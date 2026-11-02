#ifndef LEVARI_PLAYER_H
#define LEVARI_PLAYER_H

#include "../playlist/playlist.h"
#include "../decode/decode.h"
#include "../dsp/dsp.h"
#include <pthread.h>
#include <stdatomic.h>

enum { ST_STOP, ST_PLAY, ST_PAUSE };
enum { CMD_NONE, CMD_PLAY, CMD_TOGGLE_PAUSE, CMD_STOP, CMD_SEEK, CMD_NEXT, CMD_PREV, CMD_SPEED };

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
    _Atomic int speed; /* percent, 100 = normal */
    _Atomic long long ab_a, ab_b; /* -1 = unset */
    int16_t viz_buf[4096];
    volatile int viz_n;
    volatile unsigned viz_seq;
    pthread_mutex_t viz_mtx; /* viz_buf/n/seq: player writes, UI reads */
    int ao_open;
    pthread_mutex_t mtx; /* guards playlist mutations vs player thread */
    _Atomic int quit;
    pthread_t th;
} Player;

void pl_player_init(Player *p, Playlist *pl);
void pl_player_start(Player *p);
void pl_player_stop(Player *p);
void pl_post(Player *p, int cmd, int arg);
void pl_post_seek(Player *p, double frac);
/* display helpers */
const char *pl_current_title(Player *p, char *buf, size_t n);

#endif
