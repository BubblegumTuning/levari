#ifndef LEVARI_DECODE_H
#define LEVARI_DECODE_H

#include <stdint.h>
#include <stddef.h>

typedef struct Decoder Decoder;

/* ingest policy handed in by the CALLER at open: decode owns no global
   config state, so the large-file opt-in (options > advanced LARGE) is a
   value the call site reads, not a global the decoder sniffs. NULL opts
   = defaults (cap enforced). */
typedef struct {
    int allow_large;
    /* optional abort poll for seeks that emulate a fast-forward (SPC):
       called between skip chunks; a non-zero return abandons the seek,
       which returns -1 with the decoder still valid at however far the
       skip advanced. NULL = run the seek to completion (every other
       format's seek is already cheap). */
    int (*seek_abort)(void *ctx);
    void *seek_abort_ctx;
} DecOpts;

Decoder *dec_open(const char *path, const DecOpts *opts);
int      dec_rate(const Decoder *d);
int      dec_channels(const Decoder *d);
long long dec_total_frames(const Decoder *d);
int      dec_take_title(Decoder *d, char *out, size_t n); /* 1 = fresh ICY StreamTitle */
/* read up to `frames` pcm frames, interleaved s16; returns frames read or -1 */
int      dec_read(Decoder *d, int16_t *out, int frames);
/* seek to absolute pcm frame; returns 0 on success */
int      dec_seek(Decoder *d, long long frame);
void     dec_close(Decoder *d);

/* ffmpeg backend availability: 0 = not built, 1 = available,
   2 = built but libav* shared libraries missing at runtime,
   3 = libraries found but outside the set of libav ABIs this binary
   carries a variant for (see vendor/ffmpeg + ffabi.h) — struct layouts
   shift across majors, so such runtimes are refused (an honest no
   instead of garbage field reads); the found version is still
   reportable (probe_info) */
int      dec_ffmpeg_probe(void); /* 1/2/3; no lasting dlopen */
int      dec_ffmpeg_probe_info(char *ver_out, size_t ver_n,
                               char *dir_out, size_t dir_n); /* same return; reports the
                               ffmpeg release actually found + the libavformat dir it
                               resolved from ("" = unknown/not reportable); optional outs */

#endif
