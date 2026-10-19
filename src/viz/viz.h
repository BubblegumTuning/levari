#ifndef LEVARI_VIZ_H
#define LEVARI_VIZ_H

#include <stdint.h>

#define VIZ_BARS 32

/* compute log-spaced spectrum magnitudes (0..1) from mono pcm;
   rate is the pcm sample rate in Hz (bin->frequency mapping depends on it) */
void viz_spectrum(const int16_t *pcm, int n, int rate, float out[VIZ_BARS]);

#endif
