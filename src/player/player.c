#include "player.h"
#include "../audio/audio.h"
#include <unistd.h>
#include <time.h>

#define CHUNK 2048

static int pick_next(Player *p, int cur) {
    pthread_mutex_lock(&p->mtx);
    int n = p->pl->count;
    int r = -1;
    if (n > 0) {
        if (p->shuffle && n > 1) {
            do { r = rand() % n; } while (r == cur);
        } else if (cur + 1 < n) r = cur + 1;
        else if (p->repeat) r = 0;
    }
    pthread_mutex_unlock(&p->mtx);
    return r;
}

static int pick_prev(Player *p, int cur) {
    pthread_mutex_lock(&p->mtx);
    int n = p->pl->count;
    int r = -1;
    if (n > 0) {
        if (p->shuffle) r = -2; /* sentinel: use pick_next logic */
        else if (cur > 0) r = cur - 1;
        else if (p->repeat) r = n - 1;
        else r = 0;
    }
    pthread_mutex_unlock(&p->mtx);
    if (r == -2) return pick_next(p, cur);
    return r;
}

static void close_dec(Player *p) {
    dec_close(p->dec);
    p->dec = NULL;
    p->total_frames = 0;
    p->pos_frames = 0;
}

static void load_track_locked(Player *p, int idx) {
    Playlist *pl = p->pl;
    if (idx < 0 || idx >= pl->count) { close_dec(p); p->state = ST_STOP; return; }
    close_dec(p);
    p->ab_a = p->ab_b = -1;
    Decoder *d = dec_open(pl->items[idx].path);
    if (!d) {
        fprintf(stderr, "levari: cannot decode %s\n", pl->items[idx].path);
        p->state = ST_STOP;
        return;
    }
    /* gapless: reuse the open output stream when format matches */
    if (!p->ao_open || p->rate != dec_rate(d) || p->ch != dec_channels(d)) {
        ao_flush();
        ao_close();
        if (ao_open(dec_rate(d), dec_channels(d)) != 0) {
            fprintf(stderr, "levari: no audio output (ALSA unavailable)\n");
            dec_close(d);
            p->state = ST_STOP;
            return;
        }
        p->ao_open = 1;
    }
    p->dec = d;
    p->cur = idx;
    p->rate = dec_rate(d);
    p->ch = dec_channels(d);
    p->total_frames = dec_total_frames(d);
    p->pos_frames = 0;
    p->track_start = 0;
    p->track_end = 0;
    p->state = ST_PLAY;
    eq_set_rate(&p->eq, p->rate);
    ws_init(&p->ws, p->speed / 100.0);
    Track *t = &pl->items[idx];
    if (t->start_sec > 0 && p->total_frames > 0) {
        p->track_start = (long long)(t->start_sec * p->rate);
        if (p->track_start > 0) {
            dec_seek(d, p->track_start);
            p->pos_frames = p->track_start;
        }
    }
    if (t->end_sec > 0 && p->total_frames > 0)
        p->track_end = (long long)(t->end_sec * p->rate);
    if (!t->tags.title[0]) tag_read(t->path, &t->tags);
    if (!t->duration) {
        long long len = (p->track_end ? p->track_end : p->total_frames) - p->track_start;
        if (len > 0 && p->rate > 0) t->duration = (int)(len / p->rate);
    }
}

static void load_track(Player *p, int idx) {
    pthread_mutex_lock(&p->mtx);
    load_track_locked(p, idx);
    pthread_mutex_unlock(&p->mtx);
}

static void apply_vol(int16_t *buf, int frames, int ch, int vol) {
    if (vol >= 128) return;
    size_t n = (size_t)frames * ch;
    for (size_t i = 0; i < n; i++) buf[i] = (int16_t)((int32_t)buf[i] * vol / 128);
}

static void apply_pan(int16_t *buf, int frames, int ch, int pan) {
    if (ch != 2 || pan == 0) return;
    if (pan < -32) pan = -32;
    if (pan > 32) pan = 32;
    int lg = pan <= 0 ? 128 : 128 - pan * 4;
    int rg = pan >= 0 ? 128 : 128 + pan * 4;
    for (int i = 0; i < frames; i++) {
        buf[i * 2]     = (int16_t)((int32_t)buf[i * 2] * lg / 128);
        buf[i * 2 + 1] = (int16_t)((int32_t)buf[i * 2 + 1] * rg / 128);
    }
}

static void *player_thread(void *arg) {
    Player *p = arg;
    static int16_t buf[CHUNK * 2];
    while (!p->quit) {
        /* take cmd + its payload atomically: a plain read-then-clear can
           swallow a command posted in between, or pair one poster's arg
           with another poster's cmd (UI and daemon both post) */
        pthread_mutex_lock(&p->mtx);
        int cmd = p->cmd;
        int cmd_arg = p->cmd_arg;
        long long seek_frame = p->seek_frame;
        if (cmd != CMD_NONE) p->cmd = CMD_NONE;
        pthread_mutex_unlock(&p->mtx);
        if (cmd != CMD_NONE) {
            switch (cmd) {
            case CMD_PLAY:
                load_track(p, cmd_arg);
                break;
            case CMD_TOGGLE_PAUSE:
                if (p->state == ST_PLAY) { p->state = ST_PAUSE; ao_pause(); }
                else if (p->state == ST_PAUSE) { p->state = ST_PLAY; ao_resume(); }
                else if (p->pl->count) load_track(p, p->cur >= 0 ? p->cur : 0);
                break;
            case CMD_STOP:
                p->state = ST_STOP;
                close_dec(p);
                ao_close();
                p->ao_open = 0;
                break;
            case CMD_SEEK:
                if (p->dec) {
                    dec_seek(p->dec, seek_frame);
                    p->pos_frames = seek_frame;
                    ao_flush();
                    if (p->state == ST_PAUSE) { p->state = ST_PLAY; ao_resume(); }
                }
                break;
            case CMD_NEXT: {
                int n = pick_next(p, p->cur);
                if (n >= 0) load_track(p, n);
                break;
            }
            case CMD_PREV: {
                int n = pick_prev(p, p->cur < 0 ? 0 : p->cur);
                if (n >= 0) load_track(p, n);
                break;
            }
            case CMD_SPEED:
                /* applied here, not on the posting thread: ws_process
                   runs on this thread and must never see a reset WS */
                p->speed = cmd_arg;
                ws_init(&p->ws, p->speed / 100.0);
                break;
            }
        }

        if (p->state == ST_PLAY && p->dec) {
            static int16_t obuf[CHUNK * 2 + WS_FRAME * 2];
            int got = dec_read(p->dec, buf, CHUNK);
            if (got > 0) {
                eq_process(&p->eq, buf, got, p->ch);
                for (int i = 0; i < got; i++)
                    p->viz_buf[i] = p->ch == 2
                        ? (int16_t)(((int)buf[i * 2] + (int)buf[i * 2 + 1]) / 2)
                        : buf[i];
                p->viz_n = got;
                p->viz_seq++;
                const int16_t *toput = buf;
                int tout = got;
                if (p->speed != 100) {
                    tout = ws_process(&p->ws, buf, got, p->ch, obuf);
                    toput = obuf;
                }
                apply_vol((int16_t *)toput, tout, p->ch, p->vol);
                apply_pan((int16_t *)toput, tout, p->ch, p->pan);
                ao_write(toput, tout);
                p->pos_frames += got;
                if (p->ab_b >= 0 && p->pos_frames >= p->ab_b) {
                    long long a = p->ab_a >= 0 ? p->ab_a : p->track_start;
                    dec_seek(p->dec, a);
                    p->pos_frames = a;
                }
            }
            int ended = got <= 0 || (p->track_end > 0 && p->pos_frames >= p->track_end);
            if (ended) {
                if (p->repeat == 2) {
                    dec_seek(p->dec, p->track_start);
                    p->pos_frames = p->track_start;
                    continue;
                }
                int n = pick_next(p, p->cur);
                if (n >= 0) { load_track(p, n); continue; }
                p->state = ST_STOP;
                close_dec(p);
                ao_close();
                p->ao_open = 0;
                continue;
            }
        } else {
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    close_dec(p);
    ao_close();
    return NULL;
}

void pl_player_init(Player *p, Playlist *pl) {
    memset(p, 0, sizeof *p);
    p->pl = pl;
    p->cur = -1;
    p->state = ST_STOP;
    p->vol = 100;
    p->speed = 100;
    p->ab_a = p->ab_b = -1;
    p->track_start = p->track_end = 0;
    pthread_mutex_init(&p->mtx, NULL);
    eq_init(&p->eq, 44100);
    srand((unsigned)time(NULL));
}

void pl_player_start(Player *p) {
    pthread_create(&p->th, NULL, player_thread, p);
}

void pl_player_stop(Player *p) {
    p->quit = 1;
    pthread_join(p->th, NULL);
}

void pl_post(Player *p, int cmd, int arg) {
    pthread_mutex_lock(&p->mtx);
    p->cmd_arg = arg;
    p->cmd = cmd;
    pthread_mutex_unlock(&p->mtx);
}

void pl_post_seek(Player *p, double frac) {
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    if (endf <= base) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    long long frame = base + (long long)(frac * (double)(endf - base));
    pthread_mutex_lock(&p->mtx);
    p->seek_frame = frame;
    p->cmd = CMD_SEEK;
    pthread_mutex_unlock(&p->mtx);
}

const char *pl_current_title(Player *p, char *buf, size_t n) {
    if (p->cur < 0 || p->cur >= p->pl->count) { buf[0] = 0; return buf; }
    Track *t = &p->pl->items[p->cur];
    if (t->tags.title[0]) {
        if (t->tags.artist[0]) snprintf(buf, n, "%s - %s", t->tags.artist, t->tags.title);
        else lv_strlcpy(buf, t->tags.title, n);
    } else {
        const char *base = strrchr(t->path, '/');
        lv_strlcpy(buf, base ? base + 1 : t->path, n);
    }
    return buf;
}
