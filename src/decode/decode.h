#ifndef LEVARI_DECODE_H
#define LEVARI_DECODE_H

#include <stdint.h>

typedef struct Decoder Decoder;

Decoder *dec_open(const char *path);
int      dec_rate(const Decoder *d);
int      dec_channels(const Decoder *d);
long long dec_total_frames(const Decoder *d);
/* read up to `frames` pcm frames, interleaved s16; returns frames read or -1 */
int      dec_read(Decoder *d, int16_t *out, int frames);
/* seek to absolute pcm frame; returns 0 on success */
int      dec_seek(Decoder *d, long long frame);
void     dec_close(Decoder *d);

/* ffmpeg backend availability: 0 = not built, 1 = available,
   2 = built but libav* shared libraries missing at runtime */
int      dec_ffmpeg_probe(void); /* 1 = libav* loadable, 2 = missing; no lasting dlopen */

#endif
