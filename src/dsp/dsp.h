#ifndef LEVARI_DSP_H
#define LEVARI_DSP_H

#include <pthread.h>
#include <stdint.h>

#define EQ_BANDS 10

typedef struct { float b0, b1, b2, a1, a2; float x1, x2, y1, y2; } Biquad;

typedef struct {
    int enabled;
    float preamp_db;   /* -20..+12 */
    /* gain cache, refreshed at the first eq_process after ANY preamp_db
       write: config load and tests assign preamp_db directly, so only the
       consumer can be the choke point. preamp_q is the gain in Q14 and
       gates the integer preamp-only path (q == 16384: unity, skip;
       q > 65536: past the int32-safe product ceiling, keep the float
       path — legitimate sliders never reach it, the clamp is +12 dB). */
    float preamp_cache_db, preamp_gain;
    int32_t preamp_q;
    float band_db[EQ_BANDS];
    Biquad bq[EQ_BANDS][2];
    unsigned band_mask; /* bits: bands whose coefficients are not identity */
    double rate;
    /* guards gains/coefficients: the UI thread mutates while the player
       thread runs eq_process */
    pthread_mutex_t mtx;
} EQ;

/* flush-to-zero + denormals-are-zero for the calling thread: the biquad
   state fed with silence decays into subnormal floats and CPUs execute
   those ops orders of magnitude slower; call once at player-thread entry
   (the EQ is that thread's only float consumer) */
void dsp_flush_denormals(void);

void eq_init(EQ *e, double rate);
void eq_set_rate(EQ *e, double rate);  /* keep gains, redesign for new rate */
void eq_set_preamp(EQ *e, float db);
void eq_set_band(EQ *e, int band, float db);
void eq_design_locked(EQ *e);  /* caller holds e->mtx: recompute coefficients from current gains */
void eq_set_enabled(EQ *e, int on);
void eq_toggle(EQ *e);
int  eq_enabled(EQ *e);
void eq_process(EQ *e, int16_t *buf, int frames, int ch);

/* ---- ReplayGain gain stage (player loop, applied before eq_process)
   Q14 multiplier for a dB value, clamped to the preamp slider's
   [-20, +12] dB range: the same product ceiling the preamp integer
   path proves (q <= 65536 keeps |int16 * q| inside int32). */
int32_t rg_q_for_db(float db);
/* in-place Q14 scale with saturation, same rounding as the preamp
   integer path; q == 16384 (unity) is a no-op */
void rg_scale(int32_t q, int16_t *buf, int n);

/* ---- WSOLA time-scale (speed) control ---- */
#define WS_FRAME 1024
#define WS_SEARCH 192

typedef struct {
    double s;                 /* speed factor 0.5..2.0 */
    int16_t in[2][4 * WS_FRAME];
    int in_n[2];
    int16_t tail[2][WS_FRAME / 2]; /* last min(ov, Hs) samples; Hs == WS_FRAME/2 */
    int primed;
    /* per-instance scratch: these were function-static in ws_process, which
       made the routine non-reentrant and shared state across any future
       second player; they belong to the WS the caller owns. */
    int16_t tmp[2][5 * WS_FRAME];
    int16_t mix[2 * WS_SEARCH + WS_FRAME];
    int16_t mtail[WS_FRAME / 2];
} WS;

void ws_init(WS *w, double speed);
/* consume interleaved pcm, write time-scaled pcm into out.
   capacity >= ch * (frames/speed + WS_FRAME) samples (speed >= 0.5);
   returns output frames */
int  ws_process(WS *w, const int16_t *in, int frames, int ch, int16_t *out);

#endif
