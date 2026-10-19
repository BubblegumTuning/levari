#include "viz.h"
#include <math.h>
#include <string.h>

#define N 2048 /* FFT size: >= one full player chunk (CHUNK=2048) so the
                  whole chunk is analyzed, and fine enough bins (~21.6 Hz
                  at 44.1k) that the low log-spaced bars resolve */

static void fft(float *re, float *im) {
    static int rev[N];
    static int init = 0;
    if (!init) {
        int bits = 0;
        while ((1 << bits) < N) bits++;
        for (int i = 0; i < N; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) r = (r << 1) | ((i >> b) & 1);
            rev[i] = r;
        }
        init = 1;
    }
    for (int i = 0; i < N; i++)
        if (i < rev[i]) {
            float t = re[i]; re[i] = re[rev[i]]; re[rev[i]] = t;
            t = im[i]; im[i] = im[rev[i]]; im[rev[i]] = t;
        }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) {
                float ur = re[i + j], ui = im[i + j];
                float vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                float vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                re[i + j] = ur + vr; im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

void viz_spectrum(const int16_t *pcm, int n, int rate, float out[VIZ_BARS]) {
    float re[N], im[N];
    if (rate <= 0) rate = 44100; /* stopped/no track: harmless default */
    memset(im, 0, sizeof im);
    int m = n < N ? n : N;
    for (int i = 0; i < N; i++) {
        float v = i < m ? (float)pcm[i] / 32768.0f : 0.0f;
        re[i] = v * (0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1)));
    }
    fft(re, im);
    /* log-spaced edges, but guarantee >=1 bin per bar at the low end */
    int k0 = 1;
    for (int b = 0; b < VIZ_BARS; b++) {
        double f1 = 60.0 * pow(2.0, (double)(b + 1) * 8.0 / VIZ_BARS);
        int k1 = (int)(f1 * N / (double)rate);
        if (k1 < k0 + 1) k1 = k0 + 1;
        if (k1 > N / 2) k1 = N / 2;
        double peak = 0;
        for (int k = k0; k < k1; k++) {
            double m = sqrt((double)re[k] * re[k] + (double)im[k] * im[k]);
            if (m > peak) peak = m;
        }
        double mag = peak / (N / 4.0); /* full-scale sine -> ~1.0 (0.6..1.0
                                          with bin alignment; dB scale hides
                                          the scalloping dip) */
        double db = 20.0 * log10(mag + 1e-9);
        double norm = (db + 60.0) / 60.0; /* -60..0 dB -> 0..1 */
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        out[b] = (float)norm;
        k0 = k1;
        if (k0 >= N / 2) { for (int c = b + 1; c < VIZ_BARS; c++) out[c] = 0; break; }
    }
}
