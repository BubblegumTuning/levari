/* ffload.h — the libav* dlopen shape, shared by the dispatcher (decode.c:
   version reading only) and every compiled ABI variant (ffab.c: full
   symbol load). Layout-free by construction (dlopen + string handling —
   no libav types), so it is safe whichever runtime libav gets opened.
   config libav.dir (and the macOS Homebrew keg paths) are searched before
   the bare soname; the soname candidate range must stay ABOVE every
   supported variant's libavformat major (a bare-name hit can be a newer
   major than the versioned candidates would ever load — that is why the
   probe reads avutil_version() rather than trusting the first hit). */
#ifndef FFLOAD_H
#define FFLOAD_H

#include "../common.h"
#include "../config/config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <dlfcn.h>
#endif
#include <stdio.h>

#define FF_SONAME_HI 79  /* one release-cycle ahead of the highest known major */
#define FF_SONAME_LO 54

/* one candidate set for one library in one directory.
 * Linux: libfoo.so, libfoo.so.N. macOS: libfoo.dylib, libfoo.N.dylib. */
static inline void *ff_try_dir(const char *dir, const char *base) {
    char nm[LV_MAX_PATH];
    int p = (dir && *dir) ? snprintf(nm, sizeof nm, "%s/", dir) : 0;
    if (p < 0 || (size_t)p >= sizeof nm) return NULL;
    void *h;
#ifdef __APPLE__
    snprintf(nm + p, sizeof nm - p, "%s.dylib", base);
    h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    for (int v = FF_SONAME_HI; v >= FF_SONAME_LO; v--) {
        snprintf(nm + p, sizeof nm - p, "%s.%d.dylib", base, v);
        h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
#else
    snprintf(nm + p, sizeof nm - p, "%s.so", base);
    h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    for (int v = FF_SONAME_HI; v >= FF_SONAME_LO; v--) {
        snprintf(nm + p, sizeof nm - p, "%s.so.%d", base, v);
        h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
#endif
    return NULL;
}

static inline void *ff_dlopen(const char *base) {
    if (cfg.libav_dir[0]) { /* config-provided directory first */
        char ld[LV_MAX_PATH];
        lv_expand_home(cfg.libav_dir, ld, sizeof ld);
        void *h = ff_try_dir(ld, base);
        if (h) return h;
    }
#ifdef __APPLE__
    /* Homebrew's ffmpeg keg is not on dyld's bare-soname search path */
    static const char *KEGS[] = { "/opt/homebrew/opt/ffmpeg/lib", "/usr/local/opt/ffmpeg/lib" };
    for (size_t i = 0; i < sizeof KEGS / sizeof KEGS[0]; i++) {
        void *h = ff_try_dir(KEGS[i], base);
        if (h) return h;
    }
#endif
    return ff_try_dir("", base);
}

#endif /* FFLOAD_H */
