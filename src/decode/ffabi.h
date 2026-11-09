/* ffabi.h — the narrow boundary between the libav* backend variants and
   the dispatcher (decode.c). The field-layout rule (and the reason this
   file exists) is documented in ffab.c: the backend reads libav STRUCT
   FIELDS across the dlopen boundary, so every supported runtime
   libavutil/libavcodec major PAIR must be served by a variant compiled
   against that pair's public headers. Each variant (vendor/ffmpeg/ff<abi>
   headers, object lib dec_ffab<abi>) exports ONE const FFApi below;
   decode.c dlopens only to read avutil_version()/avcodec_version()
   (plain functions, safe across majors) and dispatches through the
   variant whose compiled pair matches — everything else stays ints and
   pointers-to-incomplete, so NO libav type crosses this header.
   Adding a libav major = three touches: vendor/ffmpeg/ff<abi>/include,
   LEVAR_FF_ABIS in CMakeLists.txt, LV_FFAPI_LIST below. */
#ifndef FFABI_H
#define FFABI_H

#include <stdint.h>
#include <stddef.h>

typedef struct FFContext FFContext; /* opaque: libav types stay in ffab.c */

typedef struct {
    /* open: path or URL (url flag selects the security-gated session
       shape); plain out-params carry everything decode.c needs (sample
       rate, channels, total frames — *total may be 0), so no libav
       type crosses. NULL on any failure — nothing to close then. */
    FFContext *(*open)(const char *path, int url,
                       int *rate, int *ch, long long *total);
    int  (*read)(FFContext *ctx, int16_t *out, int frames); /* frames copied; <0 error, 0 EOF */
    int  (*seek)(FFContext *ctx, long long frame);          /* 0 ok, <0 error */
    void (*close)(FFContext *ctx);
    int  (*take_title)(FFContext *ctx, char *out, size_t n); /* 1 = fresh ICY StreamTitle */
    const int lavu_major, lavc_major; /* the runtime pair this variant drives */
} FFApi;

#define LV_JOIN2(a, b) a##b
#define LV_JOIN(a, b) LV_JOIN2(a, b)

/* supported libav majors — keep in sync with LEVAR_FF_ABIS in CMakeLists.txt */
#define LV_FFAPI_LIST(X) X(5) X(6) X(7) X(8) X(9)

#define LV_FFAPI_DECL(n) extern const FFApi LV_JOIN(LV_JOIN(ff, n), _api);
LV_FFAPI_LIST(LV_FFAPI_DECL)
#undef LV_FFAPI_DECL

#endif /* FFABI_H */
