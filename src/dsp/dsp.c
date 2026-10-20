#include "dsp.h"
#include <math.h>
#include <string.h>

static const double FREQ[EQ_BANDS] = {
    60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000
};

void eq_init(EQ *e, double rate) {
    pthread_mutex_init(&e->mtx, NULL);
    e->enabled = 0;
    e->preamp_db = 0;
    for (int i = 0; i < EQ_BANDS; i++) e->band_db[i] = 0;
    e->rate = rate;
    eq_design_locked(e);
}

void eq_set_rate(EQ *e, double rate) {
    pthread_mutex_lock(&e->mtx);
    e->rate = rate;
    eq_design_locked(e);
    pthread_mutex_unlock(&e->mtx);
}

void eq_set_preamp(EQ *e, float db) {
    if (db < -20) db = -20;
    if (db > 12) db = 12;
    pthread_mutex_lock(&e->mtx);
    e->preamp_db = db;
    pthread_mutex_unlock(&e->mtx);
}

void eq_set_band(EQ *e, int band, float db) {
    if (band < 0 || band >= EQ_BANDS) return;
    if (db < -20) db = -20;
    if (db > 12) db = 12;
    pthread_mutex_lock(&e->mtx);
    e->band_db[band] = db;
    eq_design_locked(e);
    pthread_mutex_unlock(&e->mtx);
}

static void design_peaking(Biquad *b, double fs, double f0, double db, double q) {
    double A = pow(10.0, db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double sw = sin(w0), cw = cos(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0 + alpha / A;
    b->b0 = (1.0 + alpha * A) / a0;
    b->b1 = (-2.0 * cw) / a0;
    b->b2 = (1.0 - alpha * A) / a0;
    b->a1 = (-2.0 * cw) / a0;
    b->a2 = (1.0 - alpha / A) / a0;
}

/* caller holds e->mtx (or is single-threaded); recomputes coefficients
   from the current gains and resets filter state */
void eq_design_locked(EQ *e) {
    for (int i = 0; i < EQ_BANDS; i++)
        for (int c = 0; c < 2; c++) {
            design_peaking(&e->bq[i][c], e->rate, FREQ[i], e->band_db[i], 1.0);
            e->bq[i][c].x1 = e->bq[i][c].x2 = e->bq[i][c].y1 = e->bq[i][c].y2 = 0;
        }
}

static inline double run(Biquad *b, double x) {
    double y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1; b->x1 = x;
    b->y2 = b->y1; b->y1 = y;
    return y;
}

void eq_process(EQ *e, int16_t *buf, int frames, int ch) {
    pthread_mutex_lock(&e->mtx);
    if (!e->enabled) { pthread_mutex_unlock(&e->mtx); return; }
    double g = pow(10.0, e->preamp_db / 20.0);
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < ch; c++) {
            int idx = f * ch + c;
            int bc = c < 2 ? c : 0;
            double v = (double)buf[idx] * g;
            for (int b = 0; b < EQ_BANDS; b++) v = run(&e->bq[b][bc], v);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[idx] = (int16_t)v;
        }
    }
    pthread_mutex_unlock(&e->mtx);
}

void eq_set_enabled(EQ *e, int on) {
    pthread_mutex_lock(&e->mtx);
    e->enabled = on ? 1 : 0;
    pthread_mutex_unlock(&e->mtx);
}

void eq_toggle(EQ *e) {
    pthread_mutex_lock(&e->mtx);
    e->enabled = !e->enabled;
    pthread_mutex_unlock(&e->mtx);
}

int eq_enabled(EQ *e) {
    pthread_mutex_lock(&e->mtx);
    int on = e->enabled;
    pthread_mutex_unlock(&e->mtx);
    return on;
}

/* ---------- WSOLA speed control ---------- */

void ws_init(WS *w, double speed) {
    memset(w, 0, sizeof *w);
    if (speed < 0.5) speed = 0.5;
    if (speed > 2.0) speed = 2.0;
    w->s = speed;
}

static int ws_channel(WS *w, int c, const int16_t *in, int frames, int ch, int16_t *out) {
    const int Hs = WS_FRAME / 2;
    int Ha = (int)(Hs * w->s + 0.5);
    if (Ha < 64) Ha = 64;
    if (Ha > WS_FRAME - 64) Ha = WS_FRAME - 64;
    /* A full analysis overlap (WS_FRAME - Ha) exceeds the synthesis hop
       below 1.0x. The blend already keeps only Hs samples, and tail[] is
       that long — correlate and store that, never the longer overlap. */
    int ov = WS_FRAME - Ha;
    if (ov > Hs) ov = Hs;

    int16_t *ib = w->in[c];
    int n = w->in_n[c];
    for (int i = 0; i < frames; i++) ib[n++] = in[(size_t)i * ch + c];

    int produced = 0;
    while (n >= WS_FRAME + WS_SEARCH) {
        const int16_t *base = ib + WS_SEARCH; /* frame at nominal position */
        int best_d = 0;
        if (w->primed) {
            long long best = (long long)1 << 60;
            for (int d = -WS_SEARCH; d <= WS_SEARCH; d++) {
                const int16_t *f = base + d;
                long long err = 0;
                for (int i = 0; i < ov; i++) {
                    int diff = (int)f[i] - w->tail[c][i];
                    err += (long long)diff * diff;
                }
                if (err < best) { best = err; best_d = d; }
            }
        }
        const int16_t *f = base + best_d;
        int blend = ov; /* already <= Hs */
        if (w->primed) {
            for (int i = 0; i < blend; i++) {
                double a = (double)(blend - i) / blend;
                double v = a * w->tail[c][i] + (1.0 - a) * f[i];
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[produced++] = (int16_t)v;
            }
            for (int i = blend; i < Hs; i++) out[produced++] = f[i];
        } else {
            for (int i = 0; i < Hs; i++) out[produced++] = f[i];
            w->primed = 1;
        }
        /* each iteration emits Hs samples and ov <= Hs, so this stays in out[] */
        int save = ov < produced ? ov : produced;
        for (int i = 0; i < save; i++)
            w->tail[c][i] = out[produced - save + i];
        memmove(ib, ib + Ha, (size_t)(n - Ha) * 2);
        n -= Ha;
    }
    w->in_n[c] = n;
    return produced;
}

int ws_process(WS *w, const int16_t *in, int frames, int ch, int16_t *out) {
    if (ch > 2) ch = 2;
    static int16_t tmp[2][4 * WS_FRAME + WS_FRAME];
    int per[2];
    for (int c = 0; c < ch; c++)
        per[c] = ws_channel(w, c, in, frames, ch, tmp[c]);
    int n = per[0] < per[ch - 1] ? per[0] : per[ch - 1];
    for (int i = 0; i < n; i++)
        for (int c = 0; c < ch; c++)
            out[(size_t)i * ch + c] = tmp[c][i];
    return n;
}
