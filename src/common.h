#ifndef LEVARI_COMMON_H
#define LEVARI_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LV_MAX_PATH 4096

#ifndef LV_VERSION
#define LV_VERSION "0.2.0"
#endif

static inline void lv_strlcpy(char *dst, const char *src, size_t n) {
    if (n == 0) return;
    size_t i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* expand a leading '~' or '~/' to the home directory; input passes through
   unchanged when no home env var is available */
static inline void lv_expand_home(const char *in, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    if (home && *home && in[0] == '~' && (in[1] == '/' || in[1] == 0))
        snprintf(out, n, "%s%s", home, in + 1);
    else
        lv_strlcpy(out, in, n);
}

static inline int lv_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int lv_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

#endif
