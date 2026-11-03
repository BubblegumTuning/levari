#include "dsp.h"
#include <math.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

void dsp_flush_denormals(void) {
#if defined(__SSE__)
    /* FTZ flushes subnormal results, DAZ reads subnormal state as zero:
       the decay tail collapses to exactly 0 instead of crawling through
       microcode-assisted denormal ops through every track gap (measured:
       flat EQ 0.17% -> ~14% of a core without this) */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _mm_setcsr(_mm_getcsr() | 0x0040u); /* MXCSR DAZ */
#elif defined(__arm__)
    /* FZ is FPSCR bit 24 (NOT 31 — bits 27-31 are the QC/V/C/Z/N condition
       flags, writing them does nothing for denormals). FZ exists from
       ARMv6 on (the Pi Zero's ARM1176JZF-S has it), and is RES0 and ignored
       on anything older. Unlike x86, ARM's single FZ bit flushes both the
       denormal operands (their read-back is zero, so no DAZ counterpart is
       needed) and the denormal result, collapsing the decay tail to 0.
       (Corrected: the old "VFPv2 evaluates doubles in software" claim was
       wrong — ARM1176JZF-S' VFPv2 does doubles in hardware; only vdiv/
       vsqrt are VFPv3+. ws's double blend therefore runs on the VFP and
       gets flushed like everything else, which is the intent.) */
    unsigned int f;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(f));
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(f | (1u << 24)));
#elif defined(__aarch64__)
    unsigned long f;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(f));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(f | (1ul << 24))); /* FPCR FZ */
#endif
}

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
    unsigned mask = 0;
    for (int i = 0; i < EQ_BANDS; i++) {
        for (int c = 0; c < 2; c++) {
            /* a peaking biquad at f0 >= ~Nyquist flips sin(w0) negative,
               which flips alpha's sign and puts a pole outside the unit
               circle (measured a2 up to 2.1 at 22050 Hz): the output
               explodes into noise. bypass bands the rate cannot reproduce
               instead — identity coefficients. */
            if (e->rate <= 0 || FREQ[i] >= 0.45 * e->rate) {
                Biquad *b = &e->bq[i][c];
                b->b0 = 1.0f; b->b1 = b->b2 = b->a1 = b->a2 = 0.0f;
            } else {
                design_peaking(&e->bq[i][c], e->rate, FREQ[i], e->band_db[i], 1.0);
            }
            e->bq[i][c].x1 = e->bq[i][c].x2 = e->bq[i][c].y1 = e->bq[i][c].y2 = 0;
        }
        /* a peaking biquad at 0 dB has numerator == denominator (A=1
           assigns the identical float to b1/a1 and b2/a2), and the
           rate-bypass case above is literal identity: skipping such a
           band is bit-exact, and otherwise all 10 filters run per
           sample even with every band flat */
        const Biquad *b = &e->bq[i][0];
        if (b->b0 != 1.0f || b->b1 != b->a1 || b->b2 != b->a2) mask |= 1u << i;
    }
    e->band_mask = mask;
}

static inline float run(Biquad *b, float x) {
    float y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1; b->x1 = x;
    b->y2 = b->y1; b->y1 = y;
    return y;
}

void eq_process(EQ *e, int16_t *buf, int frames, int ch) {
    pthread_mutex_lock(&e->mtx);
    if (!e->enabled) { pthread_mutex_unlock(&e->mtx); return; }
    unsigned mask = e->band_mask;
    float g = (float)pow(10.0, e->preamp_db / 20.0);
    if (!mask && g == 1.0f) { pthread_mutex_unlock(&e->mtx); return; }
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < ch; c++) {
            int idx = f * ch + c;
            int bc = c < 2 ? c : 0;
            float v = (float)buf[idx] * g;
            for (int b = 0; b < EQ_BANDS; b++)
                if (mask & (1u << b)) v = run(&e->bq[b][bc], v);
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

static long long ws_err(const int16_t *f, const int16_t *t, int n) {
    long long err = 0;
    for (int i = 0; i < n; i++) {
        int diff = (int)f[i] - t[i];
        err += (long long)diff * diff;
    }
    return err;
}

/* coarse-to-fine over the full search range: a step-4 grid plus the
   +-3 residues around the grid winner. The grid sees every region, the
   refine lands the splice; what can differ from brute force is strongly
   periodic material with several near-equal minima a period apart, where
   the chosen d may sit a few samples off the true best — a marginally
   softer blend, not an artifact (measured: the brute force search was
   1.9% of a core at 0.75x, more than any decoder) */
static int ws_search(const int16_t *base, const int16_t *tail, int ov) {
    const int step = 4;
    int bd = 0;
    long long best = (long long)1 << 60;
    for (int d = -WS_SEARCH; d <= WS_SEARCH; d += step) {
        long long e = ws_err(base + d, tail, ov);
        if (e < best) { best = e; bd = d; }
    }
    int lo = bd - step, hi = bd + step;
    if (lo < -WS_SEARCH) lo = -WS_SEARCH;
    if (hi > WS_SEARCH) hi = WS_SEARCH;
    for (int d = lo; d <= hi; d++) {
        if ((d + WS_SEARCH) % step == 0) continue; /* already a grid point */
        long long e = ws_err(base + d, tail, ov);
        if (e < best) { best = e; bd = d; }
    }
    return bd;
}

int ws_process(WS *w, const int16_t *in, int frames, int ch, int16_t *out) {
    if (ch > 2) ch = 2;
    const int Hs = WS_FRAME / 2;
    int Ha = (int)(Hs * w->s + 0.5);
    if (Ha < 64) Ha = 64;
    if (Ha > WS_FRAME - 64) Ha = WS_FRAME - 64;
    /* A full analysis overlap (WS_FRAME - Ha) exceeds the synthesis hop
       below 1.0x. The blend already keeps only Hs samples, and tail[] is
       that long — correlate and store that, never the longer overlap. */
    int ov = WS_FRAME - Ha;
    if (ov > Hs) ov = Hs;

    for (int c = 0; c < ch; c++) {
        int16_t *ib = w->in[c];
        int n = w->in_n[c];
        for (int i = 0; i < frames; i++) ib[n++] = in[(size_t)i * ch + c];
        w->in_n[c] = n;
    }

    /* per-channel searches cost the same decision twice; stereo channels
       are correlated, so one alignment per hop is computed on the mono
       downmix and shared, and only the blend stays per channel */
    int produced = 0;
    while (w->in_n[0] >= WS_FRAME + WS_SEARCH &&
           (ch < 2 || w->in_n[1] >= WS_FRAME + WS_SEARCH)) {
        int d = 0;
        if (w->primed) {
            const int16_t *mb = w->in[0], *mt = w->tail[0];
            if (ch == 2) {
                for (int j = 0; j < 2 * WS_SEARCH + ov; j++)
                    w->mix[j] = (int16_t)(((int)w->in[0][j] + (int)w->in[1][j] + 1) >> 1);
                for (int j = 0; j < ov; j++)
                    w->mtail[j] = (int16_t)(((int)w->tail[0][j] + (int)w->tail[1][j] + 1) >> 1);
                mb = w->mix; mt = w->mtail;
            }
            d = ws_search(mb + WS_SEARCH, mt, ov);
        }
        for (int c = 0; c < ch; c++) {
            int16_t *o = w->tmp[c] + produced;
            const int16_t *f = w->in[c] + WS_SEARCH + d;
            if (w->primed) {
                for (int i = 0; i < ov; i++) {
                    double a = (double)(ov - i) / ov;
                    double v = a * w->tail[c][i] + (1.0 - a) * f[i];
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    o[i] = (int16_t)v;
                }
                for (int i = ov; i < Hs; i++) o[i] = f[i];
            } else {
                for (int i = 0; i < Hs; i++) o[i] = f[i];
            }
            /* each hop emits Hs samples and ov <= Hs, so tail[] covers it */
            int save = ov < produced + Hs ? ov : produced + Hs;
            for (int i = 0; i < save; i++)
                w->tail[c][i] = w->tmp[c][produced + Hs - save + i];
        }
        w->primed = 1;
        produced += Hs;
        for (int c = 0; c < ch; c++) {
            memmove(w->in[c], w->in[c] + Ha, (size_t)(w->in_n[c] - Ha) * 2);
            w->in_n[c] -= Ha;
        }
    }

    for (int i = 0; i < produced; i++)
        for (int c = 0; c < ch; c++)
            out[(size_t)i * ch + c] = w->tmp[c][i];
    return produced;
}
