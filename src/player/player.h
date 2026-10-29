#ifndef LEVARI_PLAYER_H
#define LEVARI_PLAYER_H

#include "../playlist/playlist.h"
#include "../decode/decode.h"
#include "../dsp/dsp.h"
#include <pthread.h>

enum { ST_STOP, ST_PLAY, ST_PAUSE };
enum { CMD_NONE, CMD_PLAY, CMD_TOGGLE_PAUSE, CMD_STOP, CMD_SEEK, CMD_NEXT, CMD_PREV, CMD_SPEED };

typedef struct {
    Playlist *pl;
    volatile int cur;
    volatile int state;
    volatile int shuffle;
    volatile int repeat;   /* 0 none, 1 all, 2 one */
    volatile int vol;      /* 0..128 */
    int vol_prev;          /* volume restored by unmute */
    volatile int fade_stop;/* 1: CMD_STOP fades out over 0.5s before stopping */
    int fading;            /* player-thread only: a stop fade is in progress */
    long long fade_total, fade_left; /* frames of the running fade */
    volatile int pan;      /* -32..32 left..right balance */
    volatile int cmd;
    volatile int cmd_arg;
    volatile long long seek_frame;
    Decoder *dec;
    volatile long long pos_frames;
    volatile long long total_frames;
    volatile long long track_start, track_end; /* CUE bounds in frames */
    volatile int rate, ch;
    EQ eq;
    WS ws;
    volatile int speed; /* percent, 100 = normal */
    volatile long long ab_a, ab_b; /* -1 = unset */
    int16_t viz_buf[4096];
    volatile int viz_n;
    volatile unsigned viz_seq;
    pthread_mutex_t viz_mtx; /* viz_buf/n/seq: player writes, UI reads */
    int ao_open;
    pthread_mutex_t mtx; /* guards playlist mutations vs player thread */
    volatile int quit;
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
