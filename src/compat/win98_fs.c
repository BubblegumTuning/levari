/* win98 era POSIX file-layer override — batch 18.
 *
 * WHY: the box's msvcrt.dll is a VC6-generation CRT, Unicode-internal:
 * its _findfirst/_findnext/_findclose/_stati64 implementations reach the
 * OS through the *W entry points in msvcrt's OWN import table —
 * invisible to our import gate, unreachable from the win98_shim.c
 * A-interpositions, and at the mercy of this box's half-broken
 * unofficial-SP W layer. Batch 17's hardware log: the file browser
 * froze inside file_list's directory pass (fop:prelist dir=[C:\] was
 * the last line ever written; neither flst:opendir-fail nor any
 * loop probe nor flst:done followed). The standing rule from batches
 * 6/8: on 9x, NEVER trust W — own the path in A.
 *
 * WHAT: opendir/readdir/closedir over FindFirstFileA/FindNextFileA/
 * FindClose; _stati64 (the real symbol behind the FOB=64 `stat` macro
 * on this toolchain — see the _stati64 block below) over FindFirstFileA.
 * The linker mechanism is the same archive preemption as win98_shim.c:
 * these strong definitions link before libmsvcrt.a's per-symbol members
 * are ever considered, so the CRT members that import
 * _findfirst/_findnext/_findclose/_stati64 are never pulled — proven
 * by those names vanishing from the ILT (gate ALLOW updated).
 *
 * All three directory-walking consumers (ui.c browser, library.c scan,
 * playlist.c pl_add recursion) are fixed at once; they treat DIR as
 * opaque and only read d_name, so our private DIR layout is safe.
 * Thread-safe by construction: state lives per-DIR handle.
 */
#include "common.h"

#if defined(LV_WIN9X) && defined(_WIN32)

#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* _CRTIMP is pre-defined empty by the LV_WIN9X block in CMakeLists (the
   era build must call _stati64 directly, not through __imp__), so
   sys/stat.h's declarations here are definition-compatible */
#include <sys/stat.h>
#include <dirent.h>

/* ---------------- opendir / readdir / closedir ---------------- */

typedef struct {
    HANDLE h;              /* FindFirstFileA search handle */
    int started;           /* 0 = the opendir entry is still pending */
    WIN32_FIND_DATAA fd;   /* last enumeration result */
    struct dirent de;      /* returned to callers */
} lv98_dir;

/* Win9x file APIs do NOT translate '/': the box's IFSMgr chokes on
   mixed paths (batch 18 hardware: 9 instant stat fails then a stall —
   the classic invisible critical-error/redirector path). The legacy
   CRT used to normalize internally; we own the layer, we normalize. */
static void
lv98_fixsep(char *s)
{
    for (; *s; s++)
        if (*s == '/') *s = '\\';
}

DIR *
opendir(const char *dirname)
{
    lv98_dir *d;
    char pat[LV_MAX_PATH + 8];
    size_t l;

    if (!dirname) { errno = ENOENT; return NULL; }
    l = strlen(dirname);
    if (l == 0) { pat[0] = '.'; l = 1; }
    else if (l > sizeof pat - 4) { errno = ENAMETOOLONG; return NULL; }
    memcpy(pat, dirname, l);
    pat[l] = 0;
    lv98_fixsep(pat);
    l = strlen(pat);
    if (l == 0) { pat[0] = '.'; l = 1; }
    if (pat[l - 1] != '\\') pat[l++] = '\\';
    pat[l++] = '*';
    pat[l] = 0;

    d = (lv98_dir *)calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->started = 0;
    return (DIR *)d;
}

struct dirent *
readdir(DIR *dirp)
{
    lv98_dir *d = (lv98_dir *)dirp;

    if (d->started) {
        if (!FindNextFileA(d->h, &d->fd)) return NULL; /* exhaust or error */
    } else {
        d->started = 1; /* the entry FindFirstFileA already filled */
    }
    d->de.d_ino = 0;
    d->de.d_reclen = 0;
    memcpy(d->de.d_name, d->fd.cFileName, sizeof d->de.d_name - 1);
    d->de.d_name[sizeof d->de.d_name - 1] = 0;
    d->de.d_namlen = (unsigned short)strlen(d->de.d_name);
    return &d->de;
}

int
closedir(DIR *dirp)
{
    lv98_dir *d = (lv98_dir *)dirp;

    if (!d) { errno = EBADF; return -1; }
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

/* ---------------- _stati64 (what `stat` actually calls here) ----------------
 * Flag chain measured in this toolchain's headers: _FILE_OFFSET_BITS=64 +
 * _USE_32BIT_TIME_T (default on i686 mingw) give
 *   stat -> _stat32i64 -> _stati64
 * so _stati64 is the symbol every call site references (this is exactly
 * the import batch 16 carried). _stat64 is never referenced here — an
 * override of IT is a no-op (batch 18 first-draft trap, caught by the
 * linker map showing ui.c.obj -> _imp___stati64).
 * VC6-era msvcrt implements _stati64 through GetFileAttributesW +
 * FindFirstFileW in ITS OWN imports — the era rule applies: replace with
 * one FindFirstFileA (it returns directories' own entries, so attributes
 * AND size AND times arrive in one A call). Times: st_atime/st_mtime/
 * st_ctime deliberately zero — nothing in the tree reads them (grep);
 * zero costs no import (mktime/FileTime* all avoided). */
int
_stati64(const char *path, struct _stati64 *buf)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char fix[LV_MAX_PATH];

    if (!path || !path[0] || !buf) { errno = EINVAL; return -1; }
    if (strlen(path) >= sizeof fix) { errno = ENAMETOOLONG; return -1; }
    strcpy(fix, path);
    lv98_fixsep(fix);   /* same mixed-slash contract as opendir */
    h = FindFirstFileA(fix, &fd);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    FindClose(h);

    memset(buf, 0, sizeof *buf);
    buf->st_mode = S_IREAD;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
        buf->st_mode |= S_IWRITE;
    buf->st_mode |=
        (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
    buf->st_nlink = 1;
    buf->st_size = ((__int64)fd.nFileSizeHigh << 32) |
                   (__int64)fd.nFileSizeLow;
    return 0;
}

#endif /* LV_WIN9X && _WIN32 */
