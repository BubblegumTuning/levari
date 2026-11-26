/* unit test: EQ band_mask (flat-band skip, denormal collapse) and the
   WSOLA mono-mix coarse search. CTest: cmake --build build && ctest --test-dir build -R '^testdsp$'.
   The target compiles testdsp.c + dsp.c with -ffp-contract=off (the
   bit-identical-chain pins compare a hand-written biquad against
   eq_process: identical FP rules or the pin is meaningless). Sanitizer
   shape: configure -DLV_SANITIZE=ON — the target inherits it wholesale. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dsp.h"

static int fails;
#define chk(what, cond) ((cond) ? (printf("PASS %s\n", what)) \
                                : (fails++, printf("FAIL %s: %s\n", what, #cond)))

static EQ mk(float pre, const float bands[EQ_BANDS], double rate) {
    EQ e; eq_init(&e, rate);
    e.enabled = 1;
    e.preamp_db = pre;
    for (int i = 0; i < EQ_BANDS; i++) e.band_db[i] = bands[i];
    eq_design_locked(&e);
    return e;
}

/* reference: the pre-mask eq_process (all 10 bands always run) */
static void refproc(EQ *e, int16_t *buf, int frames, int ch) {
    float g = (float)pow(10.0, e->preamp_db / 20.0);
    for (int f = 0; f < frames; f++)
        for (int c = 0; c < ch; c++) {
            int idx = f * ch + c, bc = c < 2 ? c : 0;
            float v = (float)buf[idx] * g;
            for (int b = 0; b < EQ_BANDS; b++) {
                Biquad *q = &e->bq[b][bc];
                float y = q->b0 * v + q->b1 * q->x1 + q->b2 * q->x2
                        - q->a1 * q->y1 - q->a2 * q->y2;
                q->x2 = q->x1; q->x1 = v;
                q->y2 = q->y1; q->y1 = y;
                v = y;
            }
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[idx] = (int16_t)v;
        }
}

static int16_t rndsample(unsigned *s) { /* xorshift, stable across runs */
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (int16_t)(*s % 65536u - 32768u);
}

int main(void) {
    static int16_t in[4096 * 2], A[4096 * 2], B[4096 * 2];
    unsigned s = 12345;
    for (int i = 0; i < 4096 * 2; i++) in[i] = rndsample(&s);

    float flat[EQ_BANDS] = {0}, one[EQ_BANDS] = {0}, all[EQ_BANDS];
    one[2] = 6.0f;
    for (int i = 0; i < EQ_BANDS; i++) all[i] = i == 4 ? 0.0f : (float)(i - 4) * 2.5f;

    /* --- mask semantics --- */
    EQ e = mk(0, flat, 44100);
    chk("flat mask is empty", e.band_mask == 0);
    e = mk(0, one, 44100);
    chk("one-band mask", e.band_mask == (1u << 2));
    e = mk(0, all, 44100);
    chk("the single 0 dB band is skipped", e.band_mask == 0x3ef);
    e = mk(0, all, 22050);
    chk("12k/14k/16k bypassed at 22050", e.band_mask == 0x6f);
    e = mk(0, one, 8000);
    chk("bypass keeps a designed band out of the mask", e.band_mask == (1u << 2));

    /* --- flat EQ is an exact pass-through (mask + unity preamp skip) --- */
    e = mk(0, flat, 44100);
    memcpy(A, in, sizeof in);
    eq_process(&e, A, 4096, 2);
    chk("flat EQ output bit-identical to input", memcmp(A, in, sizeof in) == 0);

    /* --- preamp with flat bands: integer Q14 path (opt item 3, 2026-11-17).
       The pre-integer pin demanded bit-exactness against the float chain;
       the integer path rounds Q14-to-nearest where float truncates, so the
       deliberate contract is now the MEASURED envelope against the float
       reference (worst deviations printed, pinned <=2 LSB), plus exact
       saturation and cache-refresh pins. --- */
    {
        e = mk(3, flat, 44100);
        memcpy(A, in, sizeof in);
        eq_process(&e, A, 4096, 2);
        float g3 = (float)pow(10.0, 3.0 / 20.0);
        long w3 = 0;
        for (int i = 0; i < 4096 * 2; i++) {
            float v = (float)in[i] * g3;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            long d = labs((long)A[i] - (long)(int16_t)v);
            if (d > w3) w3 = d;
        }
        printf("  (deviation vs float ref, +3 dB random: %ld LSB)\n", w3);
        chk("integer preamp within 2 LSB of float ref (+3 dB)", w3 <= 2);

        /* the widest legitimate gain, EVERY int16 input (q-margin worst case) */
        static int16_t big[65536], big2[65536];
        for (int i = 0; i < 65536; i++) big2[i] = big[i] = (int16_t)(i - 32768);
        EQ ex = mk(12, flat, 44100);
        chk("+12 dB q stays inside the int32-safe gate", ex.preamp_q <= 65536);
        eq_process(&ex, big2, 65536, 1);
        float gx = (float)pow(10.0, 12.0 / 20.0);
        long wx = 0;
        for (int i = 0; i < 65536; i++) {
            float v = (float)big[i] * gx;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            long d = labs((long)big2[i] - (long)(int16_t)v);
            if (d > wx) wx = d;
        }
        printf("  (deviation vs float ref, +12 dB exhaustive 65536: %ld LSB)\n", wx);
        chk("integer preamp within 2 LSB of float ref (+12 dB, all int16)", wx <= 2);
        chk("+12 dB clamps high exactly (no wrap)", big2[65535] == 32767);
        chk("+12 dB clamps low exactly (no wrap)", big2[0] == -32768);

        /* the consumer-side refresh: direct preamp_db writes (config load,
           tests) must reach the very next chunk */
        EQ cw = mk(0, flat, 44100);
        cw.preamp_db = -6;
        static int16_t four[4] = { 1000, -1000, 32767, -32768 };
        eq_process(&cw, four, 4, 1);
        float g6 = (float)pow(10.0, -6.0 / 20.0);
        long d6 = labs((long)four[0] - (long)(int16_t)((float)1000 * g6));
        chk("direct preamp_db write is seen without the setter", d6 <= 2);
        int16_t after[4];
        memcpy(after, four, sizeof after);   /* the -6 dB state, not the input */
        cw.preamp_db = 0;
        eq_process(&cw, four, 4, 1);
        chk("unity after direct change skips untouched",
            memcmp(four, after, sizeof four) == 0);

        /* past the int32-safe gate the float path must stay (saturating,
           never wrapping): a setter-clamped user can never send this, a
           direct write or future slider could */
        EQ ov = mk(0, flat, 44100);
        ov.preamp_db = 40;
        static int16_t sat[2] = { 30000, -30000 };
        eq_process(&ov, sat, 2, 1);
        chk("out-of-gate preamp clamps on the float path (no wrap)",
            sat[0] == 32767 && sat[1] == -32768);
    }

    /* --- active bands match the reference chain on active bands --- */
    e = mk(2, one, 44100);
    EQ r = e;
    memcpy(A, in, sizeof in); memcpy(B, in, sizeof in);
    eq_process(&e, A, 4096, 2);
    refproc(&r, B, 4096, 2); /* old chain includes flat bands: differs only
                                by the old chain's own rounding noise */
    long worst = 0;
    for (int i = 0; i < 4096 * 2; i++) {
        long d = labs((long)A[i] - (long)B[i]);
        if (d > worst) worst = d;
    }
    chk("active-band EQ within the old chain's rounding noise", worst <= 8);
    /* the old chain is NOT identity on flat bands: prove the skip is the
       accurate one by checking the new path against a band-only reference */
    EQ m = mk(2, one, 44100);
    memcpy(B, in, sizeof B);
    float g2 = (float)pow(10.0, m.preamp_db / 20.0);
    for (int f = 0; f < 4096; f++)
        for (int c = 0; c < 2; c++) {
            int idx = f * 2 + c;
            float v = (float)in[idx] * g2;
            Biquad *q = &m.bq[2][c];
            float y = q->b0 * v + q->b1 * q->x1 + q->b2 * q->x2
                    - q->a1 * q->y1 - q->a2 * q->y2;
            q->x2 = q->x1; q->x1 = v; q->y2 = q->y1; q->y1 = y; v = y;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            B[idx] = (int16_t)v;
        }
    memcpy(A, in, sizeof A);
    EQ m2 = mk(2, one, 44100);
    eq_process(&m2, A, 4096, 2);
    chk("EQ with skipped flat bands bit-identical to band-only chain",
        memcmp(A, B, sizeof B) == 0);

    /* --- denormal collapse after an impulse into silence --- */
#if defined(__SSE__) || defined(__arm__) || defined(__aarch64__)
    dsp_flush_denormals();
    EQ d = mk(0, one, 44100);
    static int16_t imp[512 * 2];
    imp[0] = 30000; imp[1] = 30000;
    eq_process(&d, imp, 512, 2);
    memset(imp, 0, sizeof imp);
    for (int k = 0; k < 100; k++) eq_process(&d, imp, 512, 2);
    int collapsed = 1;
    for (int b = 0; b < EQ_BANDS; b++)
        for (int c = 0; c < 2; c++) {
            Biquad *q = &d.bq[b][c];
            if (q->y1 != 0 || q->y2 != 0 || q->x1 != 0 || q->x2 != 0) collapsed = 0;
        }
    chk("filter state collapses to exactly 0 through silence", collapsed);
#endif

    /* --- WSOLA: length, DC invariance, determinism --- */
    int speeds_pct[] = {50, 75, 150, 200};
    for (unsigned k = 0; k < sizeof speeds_pct / sizeof speeds_pct[0]; k++) {
        double sp = speeds_pct[k] / 100.0;
        char what[64];
        /* DC invariance: constant input must produce exactly that constant */
        WS w; ws_init(&w, sp);
        static int16_t chunk[1024 * 2], out[2 * (1024 * 2 + WS_FRAME)];
        for (int i = 0; i < 1024 * 2; i++) chunk[i] = 1000;
        long long got = 0; int dc_ok = 1;
        for (int r = 0; r < 8; r++) {
            int n = ws_process(&w, chunk, 1024, 2, out);
            for (int i = 0; i < n * 2; i++) if (out[i] != 1000) dc_ok = 0;
            got += n;
        }
        snprintf(what, sizeof what, "%d%%: DC in + stereo stays DC", speeds_pct[k]);
        chk(what, dc_ok);
        /* length close to frames/speed once past priming */
        double want = 8 * 1024 / sp;
        snprintf(what, sizeof what, "%d%%: output length near frames/speed", speeds_pct[k]);
        chk(what, llabs(got - (long long)want) <= 2 * WS_FRAME);
        /* determinism: identical input streams give identical output */
        WS w1, w2; ws_init(&w1, sp); ws_init(&w2, sp);
        unsigned s2 = 999;
        for (int i = 0; i < 1024 * 2; i++) chunk[i] = rndsample(&s2);
        static int16_t o1[2 * (1024 * 2 + WS_FRAME)], o2[2 * (1024 * 2 + WS_FRAME)];
        int same = 1;
        for (int r = 0; r < 6; r++) {
            int n1 = ws_process(&w1, chunk, 1024, 2, o1);
            int n2 = ws_process(&w2, chunk, 1024, 2, o2);
            if (n1 != n2 || memcmp(o1, o2, (size_t)n1 * 2 * 2)) same = 0;
        }
        snprintf(what, sizeof what, "%d%%: deterministic", speeds_pct[k]);
        chk(what, same);
    }
    /* mono path still works (no mixing scratch involved) */
    {
        WS w; ws_init(&w, 0.75);
        static int16_t mc[1024], mo[1024 * 2 + WS_FRAME];
        for (int i = 0; i < 1024; i++) mc[i] = -500;
        int okm = 1; long long gotm = 0;
        for (int r = 0; r < 4; r++) {
            int n = ws_process(&w, mc, 1024, 1, mo);
            for (int i = 0; i < n; i++) if (mo[i] != -500) okm = 0;
            gotm += n;
        }
        chk("0.75x mono DC stays DC", okm && gotm > (long long)(4096 * 1.3) - 2 * WS_FRAME);
    }
    /* trichannel input clamps to two (old contract) without blowing up,
       and the DC signal survives the clamp (a single n>=0 smoke proves
       nothing about the output) */
    {
        WS w; ws_init(&w, 1.5);
        static int16_t t3[512 * 3], to[2 * (512 * 2 + WS_FRAME)];
        for (int i = 0; i < 512 * 3; i++) t3[i] = 7;
        int dcok = 1, got = 0;
        for (int r = 0; r < 4; r++) {          /* several rounds: priming may emit 0 */
            int n = ws_process(&w, t3, 512, 3, to);
            for (int i = 0; i < n * 2; i++) if (to[i] != 7) dcok = 0;
            got += n;
        }
        chk("3-channel input clamps to stereo (DC preserved)", dcok && got > 0);
    }
    /* ---- ReplayGain gain stage (rg_q_for_db / rg_scale) ---- */
    chk("rg unity q is exactly 16384", rg_q_for_db(0.0f) == 16384);
    int32_t q835 = rg_q_for_db(-8.35f);
    chk("rg -8.35 dB q within 1 of 6265", q835 >= 6264 && q835 <= 6266);
    chk("rg clamp high equals +12 dB", rg_q_for_db(30.0f) == rg_q_for_db(12.0f));
    chk("rg clamp low equals -20 dB", rg_q_for_db(-30.0f) == rg_q_for_db(-20.0f));
    {   /* rg_scale vs float reference: the house <=2 LSB convention */
        int16_t buf[256], ref[256];
        for (int i = 0; i < 256; i++) buf[i] = ref[i] = (int16_t)(i * 511 - 65000);
        int32_t q = rg_q_for_db(-6.02f);
        rg_scale(q, buf, 256);
        float g = (float)pow(10.0, -6.02 / 20.0);
        int worst = 0;
        for (int i = 0; i < 256; i++) {
            int r = (int)(ref[i] * g);
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            int d = buf[i] - r;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        chk("rg_scale within 2 LSB of the float reference", worst <= 2);
        /* saturation: full-scale through a boost clamps, never wraps */
        int16_t hot[4] = { 32767, -32768, 30000, -30000 };
        rg_scale(rg_q_for_db(12.0f), hot, 4);
        chk("rg_scale saturates at both ends",
            hot[0] == 32767 && hot[1] == -32768 && hot[2] == 32767 && hot[3] == -32768);
        /* unity is a no-op */
        int16_t same[4] = { 1, -2, 300, -400 };
        int16_t keep[4] = { 1, -2, 300, -400 };
        rg_scale(16384, same, 4);
        chk("rg_scale unity leaves the buffer untouched",
            !memcmp(same, keep, sizeof same));
    }

    printf("%s: %d failure(s)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails != 0;
}
