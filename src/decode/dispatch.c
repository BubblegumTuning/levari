/* runtime tier dispatch for the decoder (see tier_rename.h, CMakeLists
   LEVAR_SIMD_TIERS): the suffixed tier objects carry identical source at
   different x86 codegen levels; this shim owns the plain public API and
   picks the best tier the CPU supports, once, on first use. A plain
   (tier-less) build never links this file.
    LEVAR_DECODE=sse2|avx|avx2 forces a tier — debug/verification hook: a
    forced tier runs only when it is built AND the CPU supports it; any
    other value (unknown or unavailable) matches no selector and lands on
    the SSE2 floor, with the warning naming what is actually in use.
    Tier selection runs exactly once under pthread_once, which also publishes
    the table to all later callers. */
#include "decode.h"
#include <pthread.h>
#include <stdio.h>
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
static pthread_once_t tier_once = PTHREAD_ONCE_INIT;

static void tier_pick(void) {
    DecTier c = LV_TIER_TABLE(_sse2);
#ifdef LEVAR_DECODE_HAVE_X86
    const char *force = getenv("LEVAR_DECODE");
    int got = 0;   /* 2 = avx2, 1 = avx, 0 = sse2: what the CPU/build gave us */
#ifdef LEVAR_HAVE_AVX2
    if ((!force || strcmp(force, "avx2") == 0)
        && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")
        && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2")
        && __builtin_cpu_supports("lzcnt")) {
        c = (DecTier)LV_TIER_TABLE(_avx2);
        got = 2;
    } else
#endif
#ifdef LEVAR_HAVE_AVX
    if ((!force || strcmp(force, "avx") == 0)
        && __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma")) {
        c = (DecTier)LV_TIER_TABLE(_avx);
        got = 1;
    } else
#endif
        (void)0;
    /* LEVAR_DECODE is a verification hook: never let a typo silently run the
       sse2 tier — say what was requested and what is actually in use. */
    if (force) {
        int want = strcmp(force, "avx2") == 0 ? 2
                 : strcmp(force, "avx")   == 0 ? 1
                 : strcmp(force, "sse2")  == 0 ? 0 : -1;
        if (want < 0)
            fprintf(stderr, "levari: unknown LEVAR_DECODE '%s'; using %s\n",
                    force, got == 2 ? "avx2" : got == 1 ? "avx" : "sse2");
        else if (want != got)
            fprintf(stderr, "levari: LEVAR_DECODE '%s' not available; using %s\n",
                    force, got == 2 ? "avx2" : got == 1 ? "avx" : "sse2");
    }
#endif
    t = c;
}

/* pthread_once: exactly one tier_pick, and its happens-before edge makes the
   table visible to every caller (the old !ready flag was a data race under
   the C11 model even though the values were benign). */
static DecTier *tier(void) {
    pthread_once(&tier_once, tier_pick);
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
