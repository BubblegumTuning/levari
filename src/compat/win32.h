#ifndef LV_COMPAT_WIN32_H
#define LV_COMPAT_WIN32_H

/* MinGW-w64 shims for the few POSIX APIs the codebase uses.
   Include instead of <dlfcn.h> on Windows; also include from files
   that call mkdir(path, mode). */

#ifdef _WIN32
#include "../common.h"
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

#define mkdir(p, m) _mkdir(p)

/* ---- dlfcn shim ---- */
#define RTLD_LAZY  0x0001
#define RTLD_NOW   0x0002
#define RTLD_LOCAL 0x0004
#define RTLD_GLOBAL 0x0008

static inline void *lv_dlopen(const char *name, int flag) {
    (void)flag;
    HMODULE h = LoadLibraryA(name);
    if (!h) { /* retry next to the exe (LoadLibraryA already searches it,
                 but a bare name may hit a stale PATH copy first) */
        char full[MAX_PATH];
        if (GetModuleFileNameA(NULL, full, sizeof full)) {
            char *slash = strrchr(full, '\\');
            if (slash) {
                lv_strlcpy(slash + 1, name, sizeof full - (size_t)(slash + 1 - full));
                h = LoadLibraryA(full);
            }
        }
    }
    return (void *)h;
}
#define dlopen(n, f) lv_dlopen((n), (f))
#define dlsym(h, s)  ((void *)GetProcAddress((HMODULE)(h), (s)))
#define dlclose(h)   FreeLibrary((HMODULE)(h))

/* ---- getline ---- */
static inline ssize_t lv_getline(char **linep, size_t *cap, FILE *f) {
    if (!*linep) { *cap = 256; *linep = (char *)malloc(*cap); if (!*linep) return -1; }
    size_t len = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 >= *cap) {
            *cap *= 2;
            char *nb = (char *)realloc(*linep, *cap);
            if (!nb) return -1;
            *linep = nb;
        }
        (*linep)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (c == EOF && len == 0) return -1;
    (*linep)[len] = 0;
    return (ssize_t)len;
}
#define getline(p, c, f) lv_getline((p), (c), (f))

/* ---- realpath ---- */
static inline char *lv_realpath(const char *p, char *out) {
    DWORD n = GetFullPathNameA(p, LV_MAX_PATH, out, NULL);
    if (!n || n >= LV_MAX_PATH) return NULL;
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return NULL;
    return out;
}
#define realpath(p, o) lv_realpath((p), (o))

/* ---- user id for per-user IPC endpoint names ---- */
static inline void lv_user_tag(char *out, size_t n) {
    DWORD len = (DWORD)n;
    if (!GetUserNameA(out, &len) || !out[0]) lv_strlcpy(out, "anon", n);
}

#endif /* _WIN32 */
#endif /* LV_COMPAT_WIN32_H */
