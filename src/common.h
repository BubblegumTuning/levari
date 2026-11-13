#ifndef LEVARI_COMMON_H
#define LEVARI_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LV_MAX_PATH 4096

/* single hazard ceiling for per-track durations, in seconds: every probe
   path already refuses a length over one day (XING/VBRI/ogg tail/FLAC
   return 0 = unknown past 86400), so the playlist parsers, the index
   reader and the decoder fallbacks must not admit larger numbers —
   unbounded per-track values poison the library and the footer sum.
   Out-of-range parses become 0 (unknown), NEVER a dropped row (library
   admission is structural). Lived in tag.h until the clamp helper made
   it a tree-wide contract. */
#define LV_DUR_MAX 86400

/* the ONE duration clamp — "clamp the number, never drop the row":
   in range means the value, anything else means 0 = unknown. Every
   parser/reader/backfill of an untrusted duration goes through here
   (the CUE parser's INDEX line keeps its different rule on purpose:
   there an absurd row is REJECTED and the file kept — playlist.c). */
static inline int lv_clamp_dur(long long d) {
    return (d > 0 && d <= LV_DUR_MAX) ? (int)d : 0;
}

/* the ONE time readout format: H:MM:SS at and past an hour, else M:SS.
   The daemon's private fmt_t once always printed M:SS, so a 90-minute
   track read 1:30:00 in the UI and 90:00 over the daemon pipe; the
   playlist row duration column [m:ss] is deliberately NOT this format
   (compact column width). */
static inline void lv_fmt_time(char *buf, size_t n, long long sec) {
    if (sec < 0) sec = 0;
    if (sec >= 3600)
        snprintf(buf, n, "%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        snprintf(buf, n, "%lld:%02lld", sec / 60, sec % 60);
}

#ifndef LV_VERSION
#define LV_VERSION "0.3.7"
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
    if (home && *home && in[0] == '~' && (in[1] == '/' || in[1] == 0)) {
        /* same bytes snprintf("%s%s") would emit (home truncated to the
           budget, then the rest strlcpy'd, NUL always written) without the
           two-unbounded-%s format-truncation alarm */
        size_t hl = strlen(home);
        if (hl > n - 1) hl = n - 1;
        memcpy(out, home, hl);
        lv_strlcpy(out + hl, in + 1, n - hl);
        return;
    }
    lv_strlcpy(out, in, n);
}

/* drop a trailing incomplete utf-8 sequence: snprintf/strlcpy into a
   fixed-size buffer can cut an emoji mid-sequence, and the dangling bytes
   then travel as invalid utf-8 to the daemon client, D-Bus and JSON */
static inline void lv_utf8_trim(char *s) {
    size_t len = strlen(s);
    if (!len) return;
    size_t i = len;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (!i) { s[0] = 0; return; }              /* continuation-only tail */
    unsigned char c = (unsigned char)s[i - 1];
    if (c < 0x80) return;                      /* ends on an ascii byte */
    int need = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 :
               ((c & 0xF8) == 0xF0 ? 4 : 0));
    if (!need || len - (i - 1) < (size_t)need) s[i - 1] = 0;
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

/* THE audio-extension contract, one list for the whole tree: library
   scan, folder-add to the playlist, and the file browser all answer
   from here (the browser once kept a private narrower list — files
   could then be scanned or dropped but not shown or opened; do not
   re-fork). This set mirrors the shipped decoders: minimp3, stb_vorbis,
   dr_flac/dr_wav, modplug (.mod .s3m .xm .it), snes_spc (.spc) and the
   ffmpeg branch (.aac .m4a .wma on non-Windows). Add an extension here
   only together with a decoder for it. */
static inline int lv_is_audio(const char *name) {
    const char *e = strrchr(name, '.');
    static const char *const EXT[] = { ".mp3", ".ogg", ".oga", ".opus",
        ".flac", ".wav", ".aac", ".m4a", ".wma", ".mod", ".s3m", ".xm",
        ".it", ".spc" };
    if (!e) return 0;
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++)
        if (!lv_strcasecmp(e, EXT[i])) return 1;
    return 0;
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

/* URL boundary check (streaming gate, ONE place — see internal_docs/
   streaming-plan.md): 1 only for scheme://rest shapes with a non-empty
   rest free of control bytes and space. Hygiene failures (CR/LF/space/DEL
   anywhere in the string) return 0 — a clean reject, since a local path
   never carries "://" either. The protocol allowlist is a security
   constant kept in decode.c, not user config. */
static inline int lv_is_url(const char *s) {
    const char *p = s;
    if (!p || !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) return 0;
    for (; *p; p++) {
        char c = *p;
        if (c == ':') break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return 0;
    }
    if (p[0] != ':' || p[1] != '/' || p[2] != '/') return 0;
    if (p - s < 2 || p - s > 31) return 0; /* scheme length bound */
    p += 3;
    if (!*p) return 0;
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == 0x7F) return 0; /* CR/LF/tab/space/DEL class */
    }
    return 1;
}

/* true when url's scheme appears in the comma-separated allowlist
   (case-insensitive; full-field compare — no prefix passes) */
static inline int lv_url_scheme_allowed(const char *url, const char *allowed) {
    const char *colon = strstr(url, "://");
    if (!colon) return 0;
    size_t n = (size_t)(colon - url);
    const char *a = allowed;
    while (*a) {
        const char *e = a;
        while (*e && *e != ',') e++;
        if ((size_t)(e - a) == n && !lv_strncasecmp(a, url, n)) return 1;
        while (*e == ',') e++;
        a = e;
    }
    return 0;
}

/* the ONE protocol allowlist (streaming-plan D1): the URL-window Enter
   gate checks it in the UI, dec_open enforces it before libavformat.
   A security constant — never user config. */
#define LV_URL_SCHEMES "http,https,icecast"

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

/* era SDL floor (Win98 branch): SDL_HINT_APP_NAME is a 2.0.18 hint.
   Defining it identically on old SDL keeps call sites preprocessor-
   identical across floors; the hint is inert there (SDL_SetHint with an
   unknown key returns FALSE), and on new SDL this is the same token
   sequence as SDL_hints.h's own define — a benign redefinition. */
#ifndef SDL_HINT_APP_NAME
#define SDL_HINT_APP_NAME "SDL_APP_NAME"
#endif

#endif
