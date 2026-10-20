#ifndef LEVARI_DSP_H
#define LEVARI_DSP_H

#include <pthread.h>
#include <stdint.h>

#define EQ_BANDS 10

typedef struct { double b0, b1, b2, a1, a2; double x1, x2, y1, y2; } Biquad;

typedef struct {
    int enabled;
    float preamp_db;   /* -20..+12 */
    float band_db[EQ_BANDS];
    Biquad bq[EQ_BANDS][2];
    double rate;
    /* guards gains/coefficients: the UI thread mutates while the player
       thread runs eq_process */
    pthread_mutex_t mtx;
} EQ;

void eq_init(EQ *e, double rate);
void eq_set_rate(EQ *e, double rate);  /* keep gains, redesign for new rate */
void eq_set_preamp(EQ *e, float db);
void eq_set_band(EQ *e, int band, float db);
void eq_design_locked(EQ *e);  /* caller holds e->mtx: recompute coefficients from current gains */
void eq_set_enabled(EQ *e, int on);
void eq_toggle(EQ *e);
int  eq_enabled(EQ *e);
void eq_process(EQ *e, int16_t *buf, int frames, int ch);

/* ---- WSOLA time-scale (speed) control ---- */
#define WS_FRAME 1024
#define WS_SEARCH 192

typedef struct {
    double s;                 /* speed factor 0.5..2.0 */
    int16_t in[2][4 * WS_FRAME];
    int in_n[2];
    int16_t tail[2][WS_FRAME / 2]; /* last min(ov, Hs) samples; Hs == WS_FRAME/2 */
    int tail_n;
    int primed;
} WS;

void ws_init(WS *w, double speed);
/* consume interleaved pcm, write time-scaled pcm into out.
   capacity >= ch * (frames/speed + WS_FRAME) samples (speed >= 0.5);
   returns output frames */
int  ws_process(WS *w, const int16_t *in, int frames, int ch, int16_t *out);

#endif
