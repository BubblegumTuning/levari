/* runtime tier dispatch for the decoder (see tier_rename.h, CMakeLists
   LEVAR_SIMD_TIERS): the suffixed tier objects carry identical source at
   different x86 codegen levels; this shim owns the plain public API and
   picks the best tier the CPU supports, once, on first use. A plain
   (tier-less) build never links this file.
   LEVAR_DECODE=sse2|avx|avx2 forces a tier — debug/verification hook.
   The ready flag is a benign race: concurrent first calls compute the same
   table, and a torn read is impossible (every field is chosen before ready
   is set). */
#include "decode.h"
#include <stdlib.h>
#include <string.h>

#define LV_TIER_EXTERN(SFX)                                                    \
    extern Decoder   *dec_open##SFX(const char *path);                         \
    extern int        dec_rate##SFX(const Decoder *d);                         \
    extern int        dec_channels##SFX(const Decoder *d);                     \
    extern long long  dec_total_frames##SFX(const Decoder *d);                 \
    extern int        dec_read##SFX(Decoder *d, int16_t *out, int frames);     \
    extern int        dec_seek##SFX(Decoder *d, long long frame);              \
    extern void       dec_close##SFX(Decoder *d);                              \
    extern int        dec_ffmpeg_probe##SFX(void);

LV_TIER_EXTERN(_sse2)
#ifdef LEVAR_HAVE_AVX
LV_TIER_EXTERN(_avx)
#endif
#ifdef LEVAR_HAVE_AVX2
LV_TIER_EXTERN(_avx2)
#endif

typedef struct {
    Decoder  *(*open)(const char *path);
    int       (*rate)(const Decoder *d);
    int       (*channels)(const Decoder *d);
    long long (*total_frames)(const Decoder *d);
    int       (*read)(Decoder *d, int16_t *out, int frames);
    int       (*seek)(Decoder *d, long long frame);
    void      (*close)(Decoder *d);
    int       (*ffmpeg_probe)(void);
} DecTier;

#define LV_TIER_TABLE(SFX)                                                     \
    { dec_open##SFX, dec_rate##SFX, dec_channels##SFX, dec_total_frames##SFX,  \
      dec_read##SFX, dec_seek##SFX, dec_close##SFX, dec_ffmpeg_probe##SFX }

static DecTier t;
static int ready;

static void tier_pick(void) {
    DecTier c = LV_TIER_TABLE(_sse2);
#ifdef LEVAR_DECODE_HAVE_X86
    const char *force = getenv("LEVAR_DECODE");
#ifdef LEVAR_HAVE_AVX2
    if ((!force || strcmp(force, "avx2") == 0)
        && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")
        && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2")
        && __builtin_cpu_supports("lzcnt"))
        c = (DecTier)LV_TIER_TABLE(_avx2);
    else
#endif
#ifdef LEVAR_HAVE_AVX
    if ((!force || strcmp(force, "avx") == 0)
        && __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma"))
        c = (DecTier)LV_TIER_TABLE(_avx);
    else
#endif
        (void)force;
#endif
    t = c;
    ready = 1;
}

static DecTier *tier(void) {
    if (!ready) tier_pick();
    return &t;
}

Decoder *dec_open(const char *path) { return tier()->open(path); }
int dec_rate(const Decoder *d) { return tier()->rate(d); }
int dec_channels(const Decoder *d) { return tier()->channels(d); }
long long dec_total_frames(const Decoder *d) { return tier()->total_frames(d); }
int dec_read(Decoder *d, int16_t *out, int frames) { return tier()->read(d, out, frames); }
int dec_seek(Decoder *d, long long frame) { return tier()->seek(d, frame); }
void dec_close(Decoder *d) { tier()->close(d); }
int dec_ffmpeg_probe(void) { return tier()->ffmpeg_probe(); }
