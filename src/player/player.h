#ifndef LEVARI_PLAYER_H
#define LEVARI_PLAYER_H

#include "../playlist/playlist.h"
#include "../decode/decode.h"
#include "../dsp/dsp.h"
#include <pthread.h>
#include <stdatomic.h>

/* Wake primitive (batch 21): flagship rides plain pthreads (identical
   typedef and calls — zero flagship change); the era build swaps in a
   Win32 manual-reset event because static winpthread condvars never
   wake on the Win98 box (hardware log batch 20: pstm+pstmok for every
   post, zero pwake). Keep both macro sets call-for-call identical at
   the use sites. The era branch deliberately declares the handle as
   void* WITHOUT including <windows.h>: that header would poison every
   UI translation unit via this header (winuser.h's SB_* scrollbar
   macros collide with search.c's enums — batch-21 era build trap).
   Real type is HANDLE (void*); the operations live in
   compat/win98_wake.h, included ONLY by player.c — the one TU that
   touches p->wake. */
#if defined(LV_WIN9X) && defined(_WIN32)
typedef struct { void *ev; } lv_wake_t;
#define LV_WAKE_T          lv_wake_t
#else
typedef pthread_cond_t lv_wake_t;
#define LV_WAKE_T           lv_wake_t
#define LV_WAKE_INIT(w)     pthread_cond_init((w), NULL)
#define LV_WAKE_SIGNAL(w)   pthread_cond_signal(w)
#define LV_WAKE_BROADCAST(w) pthread_cond_broadcast(w)
#define LV_WAKE_WAIT(w, m)  pthread_cond_wait((w), (m))
#endif

enum { ST_STOP, ST_PLAY, ST_PAUSE };
enum { CMD_NONE, CMD_PLAY, CMD_TOGGLE_PAUSE, CMD_STOP, CMD_SEEK, CMD_NEXT, CMD_PREV, CMD_SPEED };
enum { RG_OFF, RG_TRACK, RG_ALBUM };   /* audio.replaygain = off|track|album */

/* the cross-thread scalars are _Atomic, not volatile: the UI, daemon and
   MPRIS threads read them lock-free while the player thread writes them,
   and a volatile 64-bit load is two 32-bit loads on the supported
   armv6/armv7 targets (LV_TUNE) — torn pos/track bounds meant wild seeks
   and wrong progress at track boundaries. seq_cst is plenty at this rate */
typedef struct {
    Playlist *pl;
    _Atomic int cur;
    _Atomic int state;
    _Atomic int shuffle;
    _Atomic int repeat;   /* 0 none, 1 all, 2 one */
    _Atomic int vol;      /* 0..128 */
    _Atomic int vol_prev; /* volume restored by unmute (daemon mute path) */
    _Atomic int fade_stop;/* 1: CMD_STOP fades out over 0.5s before stopping */
    int fading;            /* player-thread only: a stop fade is in progress */
    int url_checked;       /* player-thread only: urlIndex banked/considered
                              for the CURRENT track (set at first decoded
                              chunk; reset on every track load) */
    long long fade_total, fade_left; /* frames of the running fade */
    _Atomic int pan;      /* -32..32 left..right balance */
    _Atomic int cmd;
    _Atomic int cmd_arg;
    _Atomic long long seek_frame;
    Decoder *dec;
    _Atomic long long pos_frames;
    _Atomic long long total_frames;
    _Atomic long long track_start, track_end; /* CUE bounds in frames */
    _Atomic int rate, ch;
    EQ eq;
    WS ws;
    /* ReplayGain: rg_mode is seeded from cfg at startup and stored live
       by options SAVE; rg_trk/rg_alb are the CURRENT track's centi-dB
       (LV_RG_NONE = absent), probed off-lock at load and written under
       p->mtx; the cache is player-thread only, shaped exactly like the
       preamp cache in EQ (the comparison is the one choke point). */
    _Atomic int rg_mode;
    _Atomic int rg_trk, rg_alb;
    float rg_cache_db;
    int32_t rg_q;
    _Atomic int speed; /* percent, 100 = normal */
    _Atomic long long ab_a, ab_b; /* -1 = unset */
    int16_t viz_buf[4096];
    volatile int viz_n;
    volatile unsigned viz_seq;
    volatile int viz_feed;    /* UI sets 1 while the viz band shows, 0
                                 otherwise: the player thread feeds
                                 viz_buf only when set — with 0 there is
                                 no downmix, no memcpy, no mutex */
    pthread_mutex_t viz_mtx; /* viz_buf/n/seq: player writes, UI reads */
    int ao_open;
    pthread_mutex_t mtx; /* guards playlist mutations vs player thread */
    LV_WAKE_T wake; /* player idles here; pl_post/pl_post_seek/stop signal under mtx */
    _Atomic int quit;
#if defined(LV_WIN9X) && defined(_WIN32)
    void *th; /* era: HANDLE from CreateThread — winpthread's pthread_create
                 returns EAGAIN on the box (batch 21: plyr:thr rc=11 every
                 boot, the thread NEVER existed), so era never calls
                 pthread_* on this field at all */
#else
    pthread_t th;
#endif
} Player;

void pl_player_init(Player *p, Playlist *pl);
void pl_player_start(Player *p);
void pl_player_stop(Player *p);
/* wire the urlIndex bank (D5): main registers lib_url_index_save so the
   player never links the library module; unset = URL rows never bank */
void pl_player_set_url_save(int (*fn)(const char *url));
void pl_post(Player *p, int cmd, int arg);
void pl_post_seek(Player *p, double frac);
/* display helpers */
const char *pl_current_title(Player *p, char *buf, size_t n);

#endif
