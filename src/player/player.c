#include "player.h"
#include "../audio/audio.h"
#include "../library/library.h"
#include <unistd.h>
#include <time.h>

#define CHUNK 2048

/* shuffle picker: rand() shared global state across threads (we call it
   under p->mtx, but rand's own contract is still dicey) and % biases
   toward small indexes. xorshift64+ seeded once, Lemire's multiply-shift
   reduction keeps it unbiased without a modulo */
static uint64_t rng_state;

static int rng_below(int n) { /* caller holds p->mtx */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    uint32_t r = (uint32_t)(rng_state >> 32);
    return (int)(((uint64_t)r * (uint32_t)n) >> 32);
}

static int pick_next(Player *p, int cur) {
    pthread_mutex_lock(&p->mtx);
    int n = p->pl->count;
    int r = -1;
    if (n > 0) {
        if (p->shuffle && n > 1) {
            do { r = rng_below(n); } while (r == cur);
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
    p->url_checked = 0;
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
    if (!t->duration) {
        long long len = (p->track_end ? p->track_end : p->total_frames) - p->track_start;
        if (len > 0 && p->rate > 0) t->duration = (int)(len / p->rate);
    }
}

static void load_track(Player *p, int idx) {
    /* tag_read is file I/O (128 KB head read, slow on network shares) and
       the UI draw path blocks on p->mtx every frame: probe the tags BEFORE
       taking the lock, then write them back under it only if the row is
       still the same file (the daemon may have swapped the playlist). */
    char path[LV_MAX_PATH] = "";
    int need_tags = 0;
    pthread_mutex_lock(&p->mtx);
    if (idx >= 0 && idx < p->pl->count) {
        Track *t = &p->pl->items[idx];
        if (!t->tags.title[0] && !t->notag) {
            lv_strlcpy(path, t->path, sizeof path);
            need_tags = 1;
        }
    }
    pthread_mutex_unlock(&p->mtx);
    Tags tags;
    tag_init(&tags);
    if (need_tags) tag_read(path, &tags);
    pthread_mutex_lock(&p->mtx);
    if (need_tags && idx >= 0 && idx < p->pl->count &&
        !strcmp(p->pl->items[idx].path, path) &&
        !p->pl->items[idx].tags.title[0])
        p->pl->items[idx].tags = tags;
    load_track_locked(p, idx);
    pthread_mutex_unlock(&p->mtx);
}

static void hard_stop(Player *p) {
    p->fading = 0;
    p->state = ST_STOP;
    close_dec(p);
    ao_close();
    p->ao_open = 0;
}

static void apply_vol(int16_t *buf, int frames, int ch, int vol) {
    if (vol >= 128) return;
    size_t n = (size_t)frames * ch;
    /* quadratic taper: gain = (vol/128)^2, closer to perceived loudness
       than linear amplitude. Max intermediate 32767*128*128 fits int32. */
    for (size_t i = 0; i < n; i++)
        buf[i] = (int16_t)((int32_t)buf[i] * vol * vol / 16384);
}

/* stop fade: linear ramp of the slider value toward silence. apply_vol's
   quadratic taper turns the linear slider ramp into a smooth perceived fade. */
static int fade_vol(int vol, long long left, long long total) {
    if (total <= 0) return 0;
    if (left >= total) return vol;
    if (left <= 0) return 0;
    return (int)((long long)vol * left / total);
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
    dsp_flush_denormals(); /* eq_process runs here; silence decays its
                              state into denormals and the whole thread
                              crawls without FTZ/DAZ */
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
            /* any command but stop interrupts a running stop fade */
            if (cmd != CMD_STOP) p->fading = 0;
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
                /* second stop while fading, or fade off/paused: stop at once */
                if (p->fade_stop && p->dec && p->state == ST_PLAY && !p->fading) {
                    p->fading = 1;
                    p->fade_total = p->rate / 2; /* 0.5 s */
                    p->fade_left = p->fade_total;
                } else {
                    hard_stop(p);
                }
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
            /* stereo at the 0.5x floor: ch * (CHUNK/0.5 + WS_FRAME) samples */
            static int16_t obuf[2 * (CHUNK * 2 + WS_FRAME)];
            char icy[256];
            if (dec_take_title(p->dec, icy, sizeof icy)) {
                /* live StreamTitle onto the playing row: every title
                   reader (marquee, status verb, mpris) re-reads the row
                   under p->mtx, so the write under it is all the plumbing
                   needed */
                pthread_mutex_lock(&p->mtx);
                if (p->cur >= 0 && p->cur < p->pl->count)
                    lv_strlcpy(p->pl->items[p->cur].tags.title, icy,
                               sizeof p->pl->items[p->cur].tags.title);
                pthread_mutex_unlock(&p->mtx);
            }
            int got = dec_read(p->dec, buf, CHUNK);
            if (got > 0) {
                eq_process(&p->eq, buf, got, p->ch);
                pthread_mutex_lock(&p->viz_mtx);
                for (int i = 0; i < got; i++)
                    p->viz_buf[i] = p->ch == 2
                        ? (int16_t)(((int)buf[i * 2] + (int)buf[i * 2 + 1]) / 2)
                        : buf[i];
                p->viz_n = got;
                p->viz_seq++;
                pthread_mutex_unlock(&p->viz_mtx);
                int16_t *toput = buf;
                int tout = got;
                if (p->speed != 100) {
                    tout = ws_process(&p->ws, buf, got, p->ch, obuf);
                    toput = obuf;
                }
                apply_vol(toput, tout, p->ch,
                          p->fading ? fade_vol(p->vol, p->fade_left, p->fade_total) : p->vol);
                apply_pan(toput, tout, p->ch, p->pan);
                ao_write(toput, tout);
                p->pos_frames += got;
                if (!p->url_checked) {
                    /* the urlIndex bank point (D5): open succeeded AND the
                       first decoded chunk advanced pos — failed opens never
                       reach here, so they can never clobber the row. The
                       path is read under the lock; the file write is off it */
                    p->url_checked = 1;
                    char upath[LV_MAX_PATH] = "";
                    pthread_mutex_lock(&p->mtx);
                    if (p->cur >= 0 && p->cur < p->pl->count)
                        lv_strlcpy(upath, p->pl->items[p->cur].path, sizeof upath);
                    pthread_mutex_unlock(&p->mtx);
                    if (lv_is_url(upath))
                        lib_url_index_save(upath);
                }
                if (p->ab_b >= 0 && p->pos_frames >= p->ab_b) {
                    long long a = p->ab_a >= 0 ? p->ab_a : p->track_start;
                    dec_seek(p->dec, a);
                    p->pos_frames = a;
                }
                if (p->fading) {
                    p->fade_left -= got;
                    if (p->fade_left <= 0) { hard_stop(p); continue; }
                }
            }
            int ended = got <= 0 || (p->track_end > 0 && p->pos_frames >= p->track_end);
            if (ended) {
                if (p->fading) { hard_stop(p); continue; } /* stop wins over next track */
                if (p->repeat == 2) {
                    dec_seek(p->dec, p->track_start);
                    p->pos_frames = p->track_start;
                    continue;
                }
                int n = pick_next(p, p->cur);
                if (n >= 0) { load_track(p, n); continue; }
                hard_stop(p);
                continue;
            }
        } else {
            /* paused/stopped: sleep until a command or quit instead of a
               20 ms poll (50 wake-ups per second forever drained power
               on battery boards for nothing) */
            pthread_mutex_lock(&p->mtx);
            while (!p->quit && p->cmd == CMD_NONE)
                pthread_cond_wait(&p->wake, &p->mtx);
            pthread_mutex_unlock(&p->mtx);
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
    pthread_mutex_init(&p->viz_mtx, NULL);
    pthread_cond_init(&p->wake, NULL);
    eq_init(&p->eq, 44100);
    rng_state = (uint64_t)time(NULL) ^ ((uint64_t)(unsigned)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
    if (!rng_state) rng_state = 0xA5A5A5A5u;
}

void pl_player_start(Player *p) {
    pthread_create(&p->th, NULL, player_thread, p);
}

void pl_player_stop(Player *p) {
    /* quit under the lock + broadcast: the thread may be parked in
       pthread_cond_wait, and a bare store would leave it sleeping
       forever (the wait predicate re-reads quit under the same mutex) */
    pthread_mutex_lock(&p->mtx);
    p->quit = 1;
    pthread_cond_broadcast(&p->wake);
    pthread_mutex_unlock(&p->mtx);
    pthread_join(p->th, NULL);
}

void pl_post(Player *p, int cmd, int arg) {
    pthread_mutex_lock(&p->mtx);
    p->cmd_arg = arg;
    p->cmd = cmd;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->mtx);
}

void pl_post_seek(Player *p, double frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    /* the scalars are read under p->mtx: during a track load the player
       thread updates them under that lock, and a bare read could mix the
       old track's bounds with the new track's rate (atomics fix tearing,
       not consistency across fields) */
    pthread_mutex_lock(&p->mtx);
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    if (endf > base) {
        long long frame = base + (long long)(frac * (double)(endf - base));
        p->seek_frame = frame;
        p->cmd = CMD_SEEK;
        pthread_cond_signal(&p->wake);
    }
    pthread_mutex_unlock(&p->mtx);
}

const char *pl_current_title(Player *p, char *buf, size_t n) {
    /* items[] can be realloc'd/freed at any moment by the daemon's
       play/add handlers under p->mtx; read the row under the same lock */
    pthread_mutex_lock(&p->mtx);
    if (p->cur < 0 || p->cur >= p->pl->count) {
        buf[0] = 0;
        pthread_mutex_unlock(&p->mtx);
        return buf;
    }
    Track *t = &p->pl->items[p->cur];
    if (t->tags.title[0]) {
        if (t->tags.artist[0]) snprintf(buf, n, "%s - %s", t->tags.artist, t->tags.title);
        else lv_strlcpy(buf, t->tags.title, n);
    } else {
        lv_pretty_name(t->path, buf, n);
    }
    pthread_mutex_unlock(&p->mtx);
    return buf;
}
