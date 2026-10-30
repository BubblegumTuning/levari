#ifndef LEVARI_COMMON_H
#define LEVARI_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LV_MAX_PATH 4096

#ifndef LV_VERSION
#define LV_VERSION "0.2.9"
#endif

/* build architecture, shown in the options title bar before the version
   so a release binary can be identified at a glance (x86-64 vs arm32...) */
#if defined(__aarch64__)
#define LV_ARCH "arm64"
#elif defined(__arm__)
#define LV_ARCH "arm32"
#elif defined(__x86_64__) || defined(_M_X64)
#define LV_ARCH "x86-64"
#elif defined(__i386__) || defined(_M_IX86)
#define LV_ARCH "x86"
#else
#define LV_ARCH "?"
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

/* case-insensitive substring search (no strcasestr portability gamble) */
static inline const char *lv_stristr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (const char *p = hay; *p; p++)
        if (!lv_strncasecmp(p, needle, nl)) return p;
    return NULL;
}

/* last path separator. On Windows both / and \ ; elsewhere \ is a legal
   filename character and must not split. */
static inline char *lv_strrsep(const char *s) {
#ifdef _WIN32
    char *a = strrchr(s, '/'), *b = strrchr(s, '\\');
    return (b && (!a || b > a)) ? b : a;
#else
    return strrchr(s, '/');
#endif
}

/* display name for an untagged file: basename with '_' rendered as a
   space ("artist_-_song_name.mp3" -> "artist - song name.mp3").
   real tags always win; this is only the fallback render. */
static inline void lv_pretty_name(const char *path, char *out, size_t n) {
    const char *base = lv_strrsep(path);
    base = base ? base + 1 : path;
    size_t i = 0;
    for (; i + 1 < n && base[i]; i++)
        out[i] = base[i] == '_' ? ' ' : base[i];
    out[i] = 0;
}

/* absolute path: "/", "\foo", "C:\foo", "C:foo" (drive-relative is not
   relative to the cue/m3u directory) */
static inline int lv_is_abs(const char *p) {
    if (!p || !p[0]) return 0;
#ifdef _WIN32
    if (p[0] == '/' || p[0] == '\\') return 1;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
#else
    return p[0] == '/';
#endif
}

/* cut the last component in place. "/" stays "/", "C:\file" becomes "C:\"
   (a bare "C:" is drive-relative, not the drive root). 1 if a separator
   was found. */
static inline int lv_dirname(char *path) {
    char *slash = lv_strrsep(path);
    if (!slash) return 0;
    if (slash == path) { path[1] = 0; return 1; }
    *slash = 0;
#ifdef _WIN32
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
        && path[1] == ':' && path[2] == 0) {
        path[2] = '\\';
        path[3] = 0;
    }
#endif
    return 1;
}

static inline void lv_path_join(char *out, size_t n, const char *dir, const char *rel) {
    if (lv_is_abs(rel) || !dir || !dir[0]) { lv_strlcpy(out, rel, n); return; }
    size_t dn = strlen(dir);
    if (dn && (dir[dn - 1] == '/' || dir[dn - 1] == '\\'))
        snprintf(out, n, "%s%s", dir, rel);
    else
        snprintf(out, n, "%s/%s", dir, rel);
}

/* logical drive roots from a GetLogicalDrives-style bitmask (bit 0 = "A:").
   PURE: no Win32 calls, unit-testable on any OS. *out is a malloc'd array
   of "X:\" strings in ascending order; the caller frees each entry. */
static inline int drives_from_mask(unsigned mask, char ***out) {
    int n = 0;
    for (int i = 0; i < 26; i++) if (mask & (1u << i)) n++;
    if (!n) { *out = NULL; return 0; }
    char **v = malloc((size_t)n * sizeof *v);
    if (!v) { *out = NULL; return 0; }
    int k = 0;
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        v[k] = malloc(4);
        if (!v[k]) {
            for (int j = 0; j < k; j++) free(v[j]);
            free(v);
            *out = NULL;
            return 0;
        }
        v[k][0] = (char)('A' + i);
        v[k][1] = ':';
        v[k][2] = '\\';
        v[k][3] = 0;
        k++;
    }
    *out = v;
    return k;
}

#endif
