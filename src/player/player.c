#include "player.h"
#if defined(LV_WIN9X) && defined(_WIN32)
#include "../compat/win98_wake.h" /* the ONLY TU that includes it: era
                                     wake ops on p->wake (windows.h
                                     stays out of every other TU) */
#include <process.h>              /* _beginthreadex (batch 23 matrix) */
#endif
#include "../audio/audio.h"
#include "../config/config.h"    /* DecOpts at dec_open: caller-owned policy */
#include "../compat/win98_diag.h"
#include <unistd.h>
#include <time.h>

#define CHUNK 2048

/* shuffle picker: rand() shared global state across threads (we call it
   under p->mtx, but rand's own contract is still dicey) and % biases
   toward small indexes. xorshift64+ seeded once, Lemire's multiply-shift
   reduction keeps it unbiased without a modulo */
static uint64_t rng_state;

/* urlIndex bank (D5): the file itself is the library module's, so the
   player calls a hook and main wires lib_url_index_save into it — the
   player must not know the library exists. Unset hook = no banking. */
static int (*url_index_save_hook)(const char *url);
void pl_player_set_url_save(int (*fn)(const char *url)) { url_index_save_hook = fn; }

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

/* Seek-abort poll (DecOpts): an SPC fast-forward seek polls this between
   skip chunks; any command already posted (stop, track change, a newer
   seek) abandons the skip so the command loop reaches it within one chunk
   instead of queueing behind up to ~100 s of emulation. Read under p->mtx
   like every other cmd consumer (the cmd/seek_frame scalars are a set). */
static int seek_cmd_pending(void *ctx) {
    Player *p = (Player *)ctx;
    int pend;
    pthread_mutex_lock(&p->mtx);
    pend = (p->cmd != CMD_NONE);
    pthread_mutex_unlock(&p->mtx);
    return pend;
}

static void load_track_locked(Player *p, int idx, int report) {
    Playlist *pl = p->pl;
    if (idx < 0 || idx >= pl->count) { close_dec(p); p->state = ST_STOP; return; }
    close_dec(p);
    p->ab_a = p->ab_b = -1;
    /* the ingest policy is the caller's: read LARGE live so a settings
       change takes effect at the next track, exactly like the old
       decode-side cfg read */
    DecOpts dopts = { .allow_large = cfg.allow_large,
                      .mod_resample = cfg.mod_resample,
                      .seek_abort = seek_cmd_pending,
                      .seek_abort_ctx = p };
    Decoder *d = dec_open(pl->items[idx].path, &dopts);
    if (!d) {
        /* report=0 only inside load_track_alive's walk, where a dead row
           is a fact of life, not an error to print once per row */
        if (report) fprintf(stderr, "levari: cannot decode %s\n", pl->items[idx].path);
        lv98_tracef("plyr:dead row=%d [%s]", idx, pl->items[idx].path);
        p->state = ST_STOP;
        return;
    }
    /* gapless: reuse the open output stream when format matches */
    if (!p->ao_open || p->rate != dec_rate(d) || p->ch != dec_channels(d)) {
        ao_flush();
        ao_close();
        if (ao_open(dec_rate(d), dec_channels(d)) != 0) {
            fprintf(stderr, "levari: no audio output (ALSA unavailable)\n");
            lv98_tracef("plyr:ao-fail rate=%d ch=%d", dec_rate(d), dec_channels(d));
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
    lv98_tracef("plyr:ok row=%d rate=%d ch=%d frames=%d",
                idx, p->rate, p->ch, (int)p->total_frames);
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
        if (len > 0 && p->rate > 0) {
            long long s = len / p->rate;   /* gate before the cast: the
                                              frame total is decoder data,
                                              and an absurd one must not
                                              smuggle a wrapped int here */
            t->duration = lv_clamp_dur(s);  /* the backfill only runs when
                                               duration was 0, so an
                                               out-of-range s keeps 0 =
                                               unknown exactly as before */
        }
    }
}

static void load_track(Player *p, int idx, int report) {
    /* tag_read is file I/O (128 KB head read, slow on network shares) and
       the UI draw path blocks on p->mtx every frame: probe the tags BEFORE
       taking the lock, then write them back under it only if the row is
       still the same file (the daemon may have swapped the playlist).
       The ReplayGain probe rides the same off-lock window. */
    char path[LV_MAX_PATH] = "";
    int need_tags = 0, want_rg = 0;
    pthread_mutex_lock(&p->mtx);
    if (idx >= 0 && idx < p->pl->count) {
        Track *t = &p->pl->items[idx];
        lv_strlcpy(path, t->path, sizeof path);
        if (!t->tags.title[0] && !t->notag) need_tags = 1;
        want_rg = p->rg_mode != RG_OFF;
    }
    pthread_mutex_unlock(&p->mtx);
    Tags tags;
    tag_init(&tags);
    if (need_tags) tag_read(path, &tags);
    int rg_trk = LV_RG_NONE, rg_alb = LV_RG_NONE;
    if (want_rg && path[0]) tag_rg_read(path, &rg_trk, &rg_alb);
    pthread_mutex_lock(&p->mtx);
    if (need_tags && idx >= 0 && idx < p->pl->count &&
        !strcmp(p->pl->items[idx].path, path) &&
        !p->pl->items[idx].tags.title[0]) {
        const char *ti = pl_intern(&p->pl->pool, tags.title);
        const char *ar = pl_intern(&p->pl->pool, tags.artist);
        const char *al = pl_intern(&p->pl->pool, tags.album);
        if (ti && ar && al) {
            p->pl->items[idx].tags.title = ti;
            p->pl->items[idx].tags.artist = ar;
            p->pl->items[idx].tags.album = al;
            p->pl->items[idx].tags.track = tags.track;
            pl_touch(p->pl);   /* the search cache must see tags that arrive this late */
        }
    }
    if (idx >= 0 && idx < p->pl->count && !strcmp(p->pl->items[idx].path, path)) {
        p->rg_trk = rg_trk;
        p->rg_alb = rg_alb;
    } else {
        p->rg_trk = p->rg_alb = LV_RG_NONE;  /* row swapped mid-probe: no gain
                                                until the swap's own load runs */
    }
    load_track_locked(p, idx, report);
    pthread_mutex_unlock(&p->mtx);
}

/* dead-row recovery: a row that fails to decode (orphaned library
   index after a NAS path change) must not end the session. From the
   picked row keep loading forward until one succeeds — with shuffle
   or repeat on, forward wraps (those modes already mean "go on");
   sequential stops at the list end exactly like pick_next's -1, so a
   dead tail can never spin playback past the designed stop. The walk
   is bounded by the rows it attempts even when EVERY row is dead (no
   spin). Only the first attempt reports, so a walk over many dead
   rows prints one named failure, not a wall. Shuffle SELECTION is
   untouched: skipping is recovery, not selection. */
static void load_track_alive(Player *p, int start) {
    pthread_mutex_lock(&p->mtx);
    int n = p->pl->count;
    int wrap = p->shuffle || p->repeat;
    pthread_mutex_unlock(&p->mtx);
    if (start < 0) start = 0;
    int tries = wrap ? n : (start < n ? n - start : 0);
    for (int k = 0; k < tries; k++) {
        load_track(p, (start + k) % n, k == 0);
        if (p->state == ST_PLAY) return;
    }
    if (!tries) { load_track(p, start, 1); return; } /* index vanished mid-flight */
    if (tries > 1)
        fprintf(stderr, "levari: no playable track in %d rows tried\n", tries);
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

/* ReplayGain: consumer-side cache shaped exactly like eq_process's
   preamp cache — every writer (the mode store, the per-track values)
   is caught by the comparison, one pow per change not per chunk.
   Applied BEFORE eq_process so the EQ and its preamp ride on top of
   the RG gain; album mode falls back to track gain when the album
   value is absent. Mode OFF leaves the unity q in place. */
static void rg_apply(Player *p, int16_t *buf, int frames, int ch) {
    int mode = atomic_load_explicit(&p->rg_mode, memory_order_relaxed);
    int cdb = LV_RG_NONE;
    if (mode == RG_TRACK)
        cdb = atomic_load_explicit(&p->rg_trk, memory_order_relaxed);
    else if (mode == RG_ALBUM) {
        cdb = atomic_load_explicit(&p->rg_alb, memory_order_relaxed);
        if (cdb == LV_RG_NONE)
            cdb = atomic_load_explicit(&p->rg_trk, memory_order_relaxed);
    }
    if (cdb == LV_RG_NONE) cdb = 0;
    float db = (float)cdb / 100.0f;
    if (db != p->rg_cache_db) {
        p->rg_cache_db = db;
        p->rg_q = rg_q_for_db(db);
    }
    rg_scale(p->rg_q, buf, frames * ch);
}

static void *player_thread(void *arg) {
    Player *p = arg;
    static int16_t buf[CHUNK * 2];
    /* batch 21: thread-exists proof — if pl_player_start's rc is 0 but
       this never logs, the thread was created but never ran (era
       thread-startup metal, not the wake, is guilty) */
    static int th_n;
    if (!th_n++) lv98_tracef("plyr:thread");
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
                load_track_alive(p, cmd_arg);
                break;
            case CMD_TOGGLE_PAUSE:
                if (p->state == ST_PLAY) { p->state = ST_PAUSE; ao_pause(); }
                else if (p->state == ST_PAUSE) { p->state = ST_PLAY; ao_resume(); }
                else if (p->pl->count) load_track_alive(p, p->cur >= 0 ? p->cur : 0);
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
                    /* -1: an aborted SPC fast-forward (or a failed
                       reload). The aborted SPC decoder tracks its exact
                       real position (dec_pos), so re-stamp pos_frames
                       from it — a pause or speed change that aborted the
                       skip does NOT redefine transport, and without the
                       restamp the readout drifts behind the audio until
                       the next successful seek or track change. A failed
                       seek on other backends reports unknown (-1):
                       keep the last truthful value there.
                       The flush still drops the pre-seek stale audio. */
                    if (dec_seek(p->dec, seek_frame) == 0)
                        p->pos_frames = seek_frame;
                    else {
                        long long dp = dec_pos(p->dec);
                        if (dp >= 0) p->pos_frames = dp;
                    }
                    ao_flush();
                    if (p->state == ST_PAUSE) { p->state = ST_PLAY; ao_resume(); }
                }
                break;
            case CMD_NEXT: {
                int n = pick_next(p, p->cur);
                if (n >= 0) load_track_alive(p, n);
                break;
            }
            case CMD_PREV: {
                int n = pick_prev(p, p->cur < 0 ? 0 : p->cur);
                if (n >= 0) load_track_alive(p, n);
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
                if (p->cur >= 0 && p->cur < p->pl->count) {
                    const char *it = pl_intern(&p->pl->pool, icy);
                    if (it) {
                        p->pl->items[p->cur].tags.title = it;
                        pl_touch(p->pl);   /* live title change: re-normalizes that row */
                    }
                    /* the ONLY unbounded interner (every StreamTitle is a
                       fresh string): the pool GC rides this write */
                    pl_pool_maybe_gc(p->pl);
                }
                pthread_mutex_unlock(&p->mtx);
            }
            int got = dec_read(p->dec, buf, CHUNK);
            if (got > 0) {
                rg_apply(p, buf, got, p->ch);
                eq_process(&p->eq, buf, got, p->ch);
                /* the mono downmix + copy exists ONLY for the viz band:
                   with the band off (viz_feed=0, set by the UI at toggle
                   and startup) none of it runs — the FFT was already
                   UI-gated, this closes the engine-side tail */
                if (p->viz_feed) {
                    pthread_mutex_lock(&p->viz_mtx);
                    for (int i = 0; i < got; i++)
                        p->viz_buf[i] = p->ch == 2
                            ? (int16_t)(((int)buf[i * 2] + (int)buf[i * 2 + 1]) / 2)
                            : buf[i];
                    p->viz_n = got;
                    p->viz_seq++;
                    pthread_mutex_unlock(&p->viz_mtx);
                }
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
                    if (lv_is_url(upath) && url_index_save_hook)
                        url_index_save_hook(upath);
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
                if (n >= 0) { load_track_alive(p, n); continue; }
                hard_stop(p);
                continue;
            }
        } else {
            /* paused/stopped: sleep until a command or quit instead of a
               20 ms poll (50 wake-ups per second forever drained power
               on battery boards for nothing) */
            /* batch 21: first-park proof. plyr:thread + plyr:park +
               pstm but no pwake = the wait itself is still wedged;
               plyr:thread without plyr:park = stuck handling the first
               command (plyr:* load probes localize further) */
            static int pk_n;
            if (!pk_n++) lv98_tracef("plyr:park");
            pthread_mutex_lock(&p->mtx);
            while (!p->quit && p->cmd == CMD_NONE)
                LV_WAKE_WAIT(&p->wake, &p->mtx);
            pthread_mutex_unlock(&p->mtx);
            {   /* batch 20: parked-wait wake probe (cap 8) */
                static int pw_n;
                if (pw_n < 8) {
                    pw_n++;
                    lv98_tracef("pwake cmd=%d quit=%d", (int)p->cmd, (int)p->quit);
                }
            }
        }
    }
    close_dec(p);
    ao_close();
    return NULL;
}

#if defined(LV_WIN9X) && defined(_WIN32)
/* trampoline: LPTHREAD_START_ROUTINE shape over player_thread (batch 22) */
static unsigned __stdcall lv98_thread_entry(void *a) {
    return (unsigned)(size_t)player_thread(a);
}
#endif

void pl_player_init(Player *p, Playlist *pl) {
    memset(p, 0, sizeof *p);
    p->pl = pl;
    p->cur = -1;
    p->state = ST_STOP;
    p->vol = 100;
    p->speed = 100;
    p->ab_a = p->ab_b = -1;
    p->track_start = p->track_end = 0;
    p->rg_trk = p->rg_alb = LV_RG_NONE;   /* memset zero is 0.00 dB, not absent */
    p->rg_q = 16384;                      /* unity: the cache starts applied */
    pthread_mutex_init(&p->mtx, NULL);
    pthread_mutex_init(&p->viz_mtx, NULL);
    LV_WAKE_INIT(&p->wake);
    eq_init(&p->eq, 44100);
    rng_state = (uint64_t)time(NULL) ^ ((uint64_t)(unsigned)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
    if (!rng_state) rng_state = 0xA5A5A5A5u;
}

void pl_player_start(Player *p) {
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 23: thread-create matrix. Batch 22 hardware settled that
       raw CreateThread(NULL,512K,entry,p,0,NULL) ALSO fails on this
       box — gle=87 ERROR_INVALID_PARAMETER, so not starvation — while
       winpthread's _beginthreadex route failed as EAGAIN (batch 21;
       it passes an explicit 2 MB stack). 9x kernel32 argument-checking
       quirks are never adjudicated from memory (house law): walk the
       era-plausible shapes in order, log each, keep the first handle
       that opens. t1 isolates the explicit stack (winpthread's known
       failure ingredient) via the msvcrt classic with default stack;
       t2 isolates lpThreadId=NULL; t3 is the classic create-suspended
       + ResumeThread shape with the pinned stack. All three APIs are
       already imported; reap is identical (kernel HANDLE). */
    /* _beginthreadex takes unsigned*, CreateThread LPDWORD — two
       variables, no casts (GCC15 rejects the pointer mismatch) */
    unsigned btid = 0;
    DWORD tid = 0;
    p->th = (HANDLE)_beginthreadex(NULL, 0, lv98_thread_entry, p, 0, &btid);
    lv98_tracef("thr:t1 h=%p gle=%lu", p->th, p->th ? 0UL : GetLastError());
    if (!p->th) {
        p->th = CreateThread(NULL, 0,
                             (LPTHREAD_START_ROUTINE)lv98_thread_entry, p, 0, &tid);
        lv98_tracef("thr:t2 h=%p gle=%lu", p->th, p->th ? 0UL : GetLastError());
    }
    if (!p->th) {
        p->th = CreateThread(NULL, 512 * 1024,
                             (LPTHREAD_START_ROUTINE)lv98_thread_entry, p,
                             CREATE_SUSPENDED, &tid);
        lv98_tracef("thr:t3 h=%p gle=%lu", p->th, p->th ? 0UL : GetLastError());
        if (p->th) ResumeThread(p->th);
    }
    lv98_tracef("plyr:thr h=%p gle=%lu", p->th, p->th ? 0UL : GetLastError());
#else
    int rc = pthread_create(&p->th, NULL, player_thread, p);
    lv98_tracef("plyr:thr rc=%d", rc);
#endif
}

void pl_player_stop(Player *p) {
    /* quit under the lock + wake: the thread may be parked in the
       wake wait, and a bare store would leave it sleeping forever
       (the wait predicate re-reads quit under the same mutex). Era
       (batch 21): the broadcast macro maps to one event kick (single
       waiter); flagship keeps pthread_cond_broadcast verbatim */
    pthread_mutex_lock(&p->mtx);
    p->quit = 1;
    LV_WAKE_BROADCAST(&p->wake);
    pthread_mutex_unlock(&p->mtx);
#if defined(LV_WIN9X) && defined(_WIN32)
    if (p->th) {                 /* CreateThread failed: nothing to reap */
        WaitForSingleObject(p->th, INFINITE); /* wake guarantees the exit */
        CloseHandle(p->th);
    }
#else
    pthread_join(p->th, NULL);
#endif
}

void pl_post(Player *p, int cmd, int arg) {
    /* batch 20: wake-war probes — pstm = command posted, pstmok =
       post completed (cond_signal returned), pwake = player thread
       exited the parked wait. pstm without pwake = the wait is
       wedged (winpthread cond on era); pstm without pstmok =
       cond_signal itself hung. */
    static int pstm_n;
    int probe = pstm_n < 64 ? ++pstm_n : 0;
    if (probe) lv98_tracef("pstm %d arg=%d", cmd, arg);
    pthread_mutex_lock(&p->mtx);
    p->cmd_arg = arg;
    p->cmd = cmd;
    LV_WAKE_SIGNAL(&p->wake);
    pthread_mutex_unlock(&p->mtx);
    if (probe) lv98_tracef("pstmok %d", cmd);
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
        LV_WAKE_SIGNAL(&p->wake);
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
