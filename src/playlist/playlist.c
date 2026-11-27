#include "playlist.h"
#ifdef _WIN32
#include "../platform/platform.h"
#endif
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

/* ================= intern pool =================
   The GC high-water: a pool only grows past this through the Icy
   live-title write (paths and scan tags are bounded by the library),
   so the check is a size compare and the rebuild is rare. The
   anti-thrash factor means a legitimately large pool (30k rows) is
   never rebuilt twice for the same garbage budget. */
#define POOL_HIGH_WATER (16u << 20)
#define POOL_BLOCK (64u << 10)

void pl_pool_free(PlPool *pool) {
    for (int i = 0; i < pool->nblocks; i++) free(pool->blocks[i]);
    free(pool->blocks);
    free(pool->tab);
    memset(pool, 0, sizeof *pool);
}

static unsigned pool_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* bump-allocate n bytes: fresh block when the cursor cannot host it
   (a string larger than POOL_BLOCK gets its own block) */
static char *pool_alloc(PlPool *pool, size_t n) {
    if (pool->cur && pool->cur_used + n <= pool->cur_cap) {
        char *r = pool->cur + pool->cur_used;
        pool->cur_used += n;
        return r;
    }
    size_t bs = n > POOL_BLOCK ? n : POOL_BLOCK;
    if (bs > SIZE_MAX - pool->bytes) return NULL;
    char *blk = malloc(bs);
    if (!blk) return NULL;
    if (pool->nblocks == pool->blockcap) {
        int nc = pool->blockcap ? pool->blockcap * 2 : 8;
        char **nb = realloc(pool->blocks, (size_t)nc * sizeof *nb);
        if (!nb) { free(blk); return NULL; }
        pool->blocks = nb;
        pool->blockcap = nc;
    }
    pool->blocks[pool->nblocks++] = blk;
    pool->cur = blk;
    pool->cur_cap = bs;
    pool->bytes += bs;
    pool->cur_used = n;
    return blk;
}

/* double the hash and re-insert every interned string (walk the old
   table; the strings themselves never move) */
static int pool_grow(PlPool *pool) {
    int on = pool->tab ? pool->tab_mask + 1 : 0;
    int nn = on ? on * 2 : 256;
    char **nt = calloc((size_t)nn, sizeof *nt);
    if (!nt) return -1;
    int nm = nn - 1;
    for (int i = 0; i < on; i++) {
        char *s = pool->tab[i];
        if (!s) continue;
        int j = (int)(pool_hash(s) & (unsigned)nm);
        while (nt[j]) j = (j + 1) & nm;
        nt[j] = s;
    }
    free(pool->tab);
    pool->tab = nt;
    pool->tab_mask = nm;
    return 0;
}

const char *pl_intern(PlPool *pool, const char *s) {
    if (!s || !*s) return "";   /* one shared static slot, never in a block */
    if (pool->tab_count + 1 > (pool->tab_mask + 1) / 2)
        if (pool_grow(pool) != 0) return NULL;
    int mask = pool->tab_mask, i = (int)(pool_hash(s) & (unsigned)mask);
    while (pool->tab[i]) {
        if (!strcmp(pool->tab[i], s)) return pool->tab[i];
        i = (i + 1) & mask;
    }
    size_t n = strlen(s) + 1;
    char *d = pool_alloc(pool, n);
    if (!d) return NULL;
    memcpy(d, s, n);
    pool->tab[i] = d;
    pool->tab_count++;
    return d;
}

const char *pl_intern_cap(PlPool *pool, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (n <= cap) return pl_intern(pool, s);
    char *d = pool_alloc(pool, cap + 1);
    if (!d) return NULL;
    memcpy(d, s, cap);
    d[cap] = 0;
    /* the truncated copy may itself already be interned: look it up
       before inserting. If the grow fails, d is returned unindexed —
       valid memory (it lives in a block, reclaimed by GC), just not
       shared this once. A dedup hit orphans d in the arena: bounded
       waste, reclaimed by GC. */
    if (pool->tab_count + 1 > (pool->tab_mask + 1) / 2) {
        if (pool_grow(pool) != 0) return d;
    }
    int mask = pool->tab_mask, i = (int)(pool_hash(d) & (unsigned)mask);
    while (pool->tab[i]) {
        if (!strcmp(pool->tab[i], d)) return pool->tab[i];
        i = (i + 1) & mask;
    }
    pool->tab[i] = d;
    pool->tab_count++;
    return d;
}

void pl_pool_maybe_gc(Playlist *p) {
    if (p->pool.bytes < POOL_HIGH_WATER) return;
    if (p->pool.bytes < 2 * p->pool.last_live) return;
    /* two passes: re-intern every row's strings into the fresh pool via
       a temp array, commit only when ALL succeeded — a mid-way OOM must
       leave every row pointing at the intact old pool */
    int n = p->count;
    const char **tmp = n > 0 ? malloc((size_t)n * 4 * sizeof *tmp) : NULL;
    if (n > 0 && !tmp) return;
    PlPool np;
    memset(&np, 0, sizeof np);
    for (int i = 0; i < n; i++) {
        const Track *t = &p->items[i];
        const char **q = tmp + (size_t)i * 4;
        q[0] = pl_intern(&np, t->path);
        q[1] = pl_intern(&np, t->tags.title);
        q[2] = pl_intern(&np, t->tags.artist);
        q[3] = pl_intern(&np, t->tags.album);
        if (!q[0] || !q[1] || !q[2] || !q[3]) {
            pl_pool_free(&np);
            free(tmp);
            p->pool.last_live = p->pool.bytes;  /* anti-thrash on OOM too */
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        const char **q = tmp + (size_t)i * 4;
        p->items[i].path = q[0];
        p->items[i].tags.title = q[1];
        p->items[i].tags.artist = q[2];
        p->items[i].tags.album = q[3];
    }
    free(tmp);
    pl_pool_free(&p->pool);
    p->pool = np;
    p->pool.last_live = p->pool.bytes;
}

size_t pl_pool_bytes(const Playlist *p) { return p->pool.bytes; }

void pl_init(Playlist *p) {
    p->items = NULL; p->count = p->cap = 0;
    memset(&p->pool, 0, sizeof p->pool);
    p->gen++;
}

void pl_clear(Playlist *p) {
    free(p->items);
    pl_pool_free(&p->pool);   /* every row string rode the pool */
    p->items = NULL; p->count = p->cap = 0;
    pl_touch(p);
}

static int ensure(Playlist *p, int need) {
    if (need <= p->cap) return 0;
    int cap = p->cap ? p->cap + p->cap / 2 + 1 : 64;
    while (cap < need) {
        /* clamp before doubling can wrap the int counter; the cap*
           sizeof(Track) overflow gate below catches the rest */
        if (cap > INT_MAX / 2) { cap = INT_MAX; break; }
        cap += cap / 2 + 1;
    }
    if ((size_t)cap > SIZE_MAX / sizeof(Track)) return -1;
    Track *items = realloc(p->items, (size_t)cap * sizeof(Track));
    if (!items) return -1;
    p->items = items;
    p->cap = cap;
    return 0;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void pl_add_dir(Playlist *p, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **n2 = realloc(names, (size_t)cap * sizeof *names);
            if (!n2) break;
            names = n2;
        }
        char *dup = strdup(de->d_name);
        if (!dup) break;
        names[n++] = dup;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmpstr);
    for (int i = 0; i < n; i++) {
        char full[LV_MAX_PATH];
        lv_path_join(full, sizeof full, dir, names[i]);
        struct stat st;
        /* lstat: never descend into a symlinked directory (cycle guard).
           On Windows lstat is plain stat (platform/platform.h) and follows
           junctions, so refuse reparse points outright: a junction loop
           would recurse until the stack overflows. */
#ifdef _WIN32
        if (GetFileAttributesA(full) & FILE_ATTRIBUTE_REPARSE_POINT) continue;
#endif
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
            pl_add_dir(p, full);
        else if (lv_is_audio(names[i]))
            pl_add_file(p, full);
        free(names[i]);
    }
    free(names);
}

int pl_add(Playlist *p, const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        pl_add_dir(p, path);
        return 0;
    }
    return pl_add_file(p, path);
}

/* append without the stat-based directory probe: callers that already
   know the path is a file (m3u lines, scanned dir entries). on network
   shares the per-path stat was the dominant cost of loading big lists.
   returns 0 when the entry was appended, -1 when the grow failed:
   callers that touch items[count-1] afterwards must check it */
int pl_add_file(Playlist *p, const char *path) {
    char full[LV_MAX_PATH];
    if (lv_is_abs(path) || lv_is_url(path)) {
        /* URLs are already fully qualified — cwd-absolutizing them would
           mangle the scheme into a bogus relative path (P0 gate caught
           exactly this: dec_open would never see the URL) */
        lv_strlcpy(full, path, sizeof full);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd))
            /* era rule (batch 24): never raw-join the cwd — getcwd returns
               "C:\" at a drive root, so "%s/%s" poisoned argv paths with
               "C:\/test.mp3" (hardware: plyr:dead on argv files while the
               browser's native-joined rows played). lv_path_join trims the
               trailing separator and emits native slashes. */
            lv_path_join(full, sizeof full, cwd, path);
        else
            lv_strlcpy(full, path, sizeof full);
    }
    /* intern BEFORE the grow: a failed intern leaves the list untouched,
       matching pl_add's -1 contract */
    const char *ip = pl_intern(&p->pool, full);
    if (!ip) return -1;
    if (ensure(p, p->count + 1) != 0) return -1;
    Track *t = &p->items[p->count++];
    t->path = ip;
    t->tags.title = t->tags.artist = t->tags.album = "";
    t->duration = 0;
    t->notag = 0;
    t->start_sec = t->end_sec = 0;
    pl_touch(p);
    return 0;
}

void pl_insert(Playlist *p, int idx, const char *path) {
    if (idx < 0) idx = 0;
    if (idx > p->count) idx = p->count;
    const char *ip = pl_intern(&p->pool, path);
    if (!ip) return;
    if (ensure(p, p->count + 1) != 0) return;
    memmove(&p->items[idx + 1], &p->items[idx], (size_t)(p->count - idx) * sizeof(Track));
    p->count++;
    Track *t = &p->items[idx];
    t->path = ip;
    t->tags.title = t->tags.artist = t->tags.album = "";
    t->duration = 0;
    t->notag = 0;
    t->start_sec = t->end_sec = 0;
    pl_touch(p);
}

void pl_remove(Playlist *p, int idx) {
    if (idx < 0 || idx >= p->count) return;
    memmove(&p->items[idx], &p->items[idx + 1], (size_t)(p->count - idx - 1) * sizeof(Track));
    p->count--;
    pl_touch(p);
}

int pl_load_m3u(Playlist *p, const char *m3u) {
    FILE *f = fopen(m3u, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, m3u, sizeof dir);
    if (!lv_dirname(dir)) lv_strlcpy(dir, ".", sizeof dir);

    /* pre-size from the file size (~96 bytes per entry): multi-KB
       playlists otherwise churn realloc through every doubling. cap it
       for sanity (a junk huge file sniffed as .m3u must not pre-allocate
       on its size); with 64 B rows the old gigabyte hazard is gone, the
       cap stays as cheap conservatism */
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            int pre = (int)(sz / 96) + 16;
            if (pre > 4096) pre = 4096;
            ensure(p, p->count + pre);
        }
    }

    char line[LV_MAX_PATH + 64];
    int pending_dur = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!*s) continue;
        if (!strncmp(s, "#EXTINF:", 8)) {
            /* EXTINF is raw attacker-writable: a value past the hazard
               ceiling means unknown, exactly like a missing field */
            int d = atoi(s + 8);
            pending_dur = lv_clamp_dur(d);
            continue;
        }
        if (*s == '#') continue;
        if (lv_is_url(s)) {
            /* D3: URLs never enter playlist files. A remote .m3u would
               need fetching — not our job (explicit-playback posture);
               the row is skipped loudly, not silently */
            printf("levari: m3u skips URL row: %s\n", s);
            pending_dur = 0;
            continue;
        }
        char full[LV_MAX_PATH];
        if (lv_is_abs(s)) lv_strlcpy(full, s, sizeof full);
        else lv_path_join(full, sizeof full, dir, s);
        /* the duration belongs to the row just appended: if the append
           failed (OOM) p->count-1 would point at an older row — or -1 */
        if (pl_add_file(p, full) == 0 && pending_dur)
            p->items[p->count - 1].duration = pending_dur;
        pending_dur = 0;
    }
    fclose(f);
    return 0;
}

PlKind pl_ext_kind(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return PL_NONE;
    if (!lv_strcasecmp(e, ".m3u") || !lv_strcasecmp(e, ".m3u8")) return PL_M3U;
    if (!lv_strcasecmp(e, ".pls")) return PL_PLS;
    if (!lv_strcasecmp(e, ".cue")) return PL_CUE;
    return PL_NONE;
}

/* Winamp .pls: an INI with [playlist], NumberOfEntries and 1-based
   FileN / TitleN (LabelN accepted) / LengthN. Keys are case-insensitive;
   anything unrecognized (garbage keys, $Version, section headers) is
   ignored. NumberOfEntries does NOT truncate the list: every FileN line
   in the file loads, mismatched headers are the file's problem — this
   matches Winamp and keeps a truncated header from silently eating rows.
   Relative entries join the .pls's own dir like pl_load_m3u; a Windows
   backslash path arriving on POSIX is an ordinary filename character
   there and joins (pinned by the fixture). TitleN before or after its
   FileN both work: entries are collected by index and appended at EOF. */
typedef struct {
    char path[LV_MAX_PATH];
    char title[256];
    int dur;
} PlsEnt;

/* hard ceiling on PLS entry numbers (FileN/TitleN/LengthN): num arrives
   as an unbounded long and the index below narrows it to int — File3221225472
   casts to a negative 'at', skips the grow branch entirely and writes
   ent[at] ~4 GB below the heap (ASan-confirmed wild write, 2026-11-07 GH-line
   report), while File2147483648 wraps 'at' to a huge positive that overflows
   the int cap-doubling loop. 65536 keeps a 2x headroom over the real
   30k+ song playlists (entry numbers run to the song count — a .pls ceiling
   must NOT be a playlist-size cap; anything under it grows on demand and
   empty slots cost nothing to the playlist itself). Memory of the sparse
   index is max_index * sizeof(PlsEnt): ~4.3 KB PER ENTRY (measured 4356),
   so the ceiling's worst case is ~272 MiB (~285 MB) for a file that
   literally numbers an entry that high — the same transient the pre-fix
   reader already spent on a real 65k list; it is bounded, paid only by a
   crafted/explicit .pls, and .pls files are parsed only on explicit user
   load. */
#define PLS_MAX_ENT 65536

int pl_load_pls(Playlist *p, const char *pls) {
    FILE *f = fopen(pls, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, pls, sizeof dir);
    if (!lv_dirname(dir)) lv_strlcpy(dir, ".", sizeof dir);

    PlsEnt *ent = NULL;
    int ent_n = 0, ent_cap = 0;
    char line[LV_MAX_PATH + 128];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!*s || *s == '[' || *s == '$') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq++ = 0;
        /* split key into alpha prefix + decimal index ("File12" ->
           "file" + 12); trailing spaces on the alpha die, trailing junk
           after the digits kills the line (garbage key) */
        char alpha[24];
        int ai = 0;
        char *q = s;
        while (*q && (*q < '0' || *q > '9') && ai < (int)sizeof alpha - 1)
            alpha[ai++] = *q++;
        while (ai && (alpha[ai-1] == ' ' || alpha[ai-1] == '\t')) ai--;
        alpha[ai] = 0;
        char *endp;
        long num = strtol(q, &endp, 10);
        while (*endp == ' ' || *endp == '\t') endp++;
        if (*endp || num < 1 || num > PLS_MAX_ENT) continue;
        while (*eq == ' ' || *eq == '\t') eq++;
        int is_file   = !lv_strcasecmp(alpha, "file");
        int is_title  = !lv_strcasecmp(alpha, "title") || !lv_strcasecmp(alpha, "label");
        int is_length = !lv_strcasecmp(alpha, "length");
        if (!is_file && !is_title && !is_length) continue;   /* NumberOfEntries + garbage */
        int at = (int)num - 1;
        if (at >= ent_n) {
            int cap = ent_cap ? ent_cap : 64;
            while (cap <= at) cap *= 2;
            PlsEnt *ne = realloc(ent, (size_t)cap * sizeof *ne);
            if (!ne) break;
            memset((char *)ne + (size_t)ent_cap * sizeof *ne, 0,
                   (size_t)(cap - ent_cap) * sizeof *ne);
            ent = ne; ent_cap = cap; ent_n = at + 1;
        }
        if (is_file) {
            if (lv_is_url(eq)) {          /* D3: URL rows skipped, like m3u */
                printf("levari: pls skips URL row: %s\n", eq);
            } else if (lv_is_abs(eq)) lv_strlcpy(ent[at].path, eq, sizeof ent[at].path);
            else lv_path_join(ent[at].path, sizeof ent[at].path, dir, eq);
        } else if (is_title) {
            lv_strlcpy(ent[at].title, eq, sizeof ent[at].title);
        } else {
            int d = atoi(eq);   /* LengthN -1 = unknown -> 0, and any
                                   value past the hazard ceiling too */
            ent[at].dur = lv_clamp_dur(d);
        }
    }
    fclose(f);
    for (int i = 0; i < ent_n; i++) {
        if (!ent[i].path[0]) continue;   /* TitleN/LengthN without FileN */
        if (pl_add_file(p, ent[i].path) == 0) {
            Track *t = &p->items[p->count - 1];
            t->duration = ent[i].dur;
            if (ent[i].title[0]) {
                const char *it = pl_intern(&p->pool, ent[i].title);
                if (it) t->tags.title = it;
            }
        }
    }
    free(ent);
    pl_touch(p);   /* the TitleN fills above land AFTER the appends' bumps */
    return 0;
}

int pl_save_m3u(Playlist *p, const char *m3u) {
    FILE *f = fopen(m3u, "w");
    if (!f) return -1;
    fputs("#EXTM3U\n", f);
    for (int i = 0; i < p->count; i++) {
        Track *t = &p->items[i];
        if (lv_is_url(t->path)) {
            /* D3: transient URL rows are never persisted (the urlIndex
               file is the sanctioned single-row memory) */
            printf("levari: save skips URL row: %s\n", t->path);
            continue;
        }
        if (t->tags.title[0] || t->tags.artist[0])
            fprintf(f, "#EXTINF:%d,%s - %s\n", t->duration,
                    t->tags.artist[0] ? t->tags.artist : "?",
                    t->tags.title[0] ? t->tags.title : "?");
        fprintf(f, "%s\n", t->path);
    }
    /* a failed --save must not report success: fflush catches buffered
       write errors (disk full), fclose catches the flush-on-close case */
    if (fflush(f) != 0 || ferror(f)) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

int pl_load_cue(Playlist *p, const char *cue) {
    FILE *f = fopen(cue, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, cue, sizeof dir);
    if (!lv_dirname(dir)) lv_strlcpy(dir, ".", sizeof dir);

    char line[2048];
    char file[LV_MAX_PATH] = "";
    int idx = -1;
    int file_first = -1; /* first item belonging to the current FILE */
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
        if (!strncmp(s, "FILE ", 5)) {
            char *q1 = strchr(s, '"');
            if (!q1) continue;
            char *q2 = strchr(q1 + 1, '"');
            if (!q2) continue;
            *q2 = 0;
            if (lv_is_abs(q1 + 1)) lv_strlcpy(file, q1 + 1, sizeof file);
            else lv_path_join(file, sizeof file, dir, q1 + 1);
            file_first = p->count;
        } else if (!strncmp(s, "TRACK ", 6)) {
            /* no TRACK row before its FILE row: an empty path would add a
               garbage entry; a failed add must not leave idx dangling past
               the end of items (later TITLE/PERFORMER/INDEX write into it) */
            idx = -1;
            if (file[0]) {
                int before = p->count;
                if (pl_add(p, file) == 0 && p->count > before) idx = before;
            }
        } else if (!strncmp(s, "TITLE ", 6) && idx >= 0 && idx < p->count) {
            char *q1 = strchr(s, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) *q2 = 0;
                const char *it = pl_intern_cap(&p->pool, q1 + 1, 255);
                if (it) p->items[idx].tags.title = it;
            }
        } else if (!strncmp(s, "PERFORMER ", 10) && idx >= 0 && idx < p->count) {
            char *q1 = strchr(s, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) *q2 = 0;
                const char *ia = pl_intern_cap(&p->pool, q1 + 1, 255);
                if (ia) p->items[idx].tags.artist = ia;
            }
        } else if (!strncmp(s, "INDEX 01 ", 9)) {
            long mm, ss, ff;
            /* long, and the time built in double: these are raw sscanf
               ints off a hostile file and the old `mm * 60 + ss` was int
               math — INDEX 01 99999999:00:00 multiplied past INT_MAX,
               signed-overflow UB wrapping into a negative start_sec.
               Absurd times are rejected, the row is kept. */
            if (sscanf(s + 9, "%ld:%ld:%ld", &mm, &ss, &ff) == 3) {
                double sec = (double)mm * 60 + (double)ss + ff / 75.0;
                if (sec >= 0 && sec <= LV_DUR_MAX &&
                    idx >= 0 && idx < p->count) {
                    /* close the previous track only if it belongs to the
                       same FILE and came from this cue: an INDEX from a
                       new FILE (or a cue loaded onto a populated playlist)
                       used to truncate an unrelated item — and across
                       FILEs the seconds refer to a different file, giving
                       end_sec < start_sec */
                    if (file_first >= 0 && idx - 1 >= file_first &&
                        p->items[idx - 1].end_sec == 0)
                        p->items[idx - 1].end_sec = sec;
                    p->items[idx].start_sec = sec;
                }
            }
        }
    }
    fclose(f);
    for (int i = 0; i < p->count; i++) {
        Track *t = &p->items[i];
        /* last track leaves end_sec == 0. Subtracting that goes negative
           once start is past ~1.5s, and load_track only repairs a 0.
           Leave it unknown so the player fills it from the file length. */
        if (t->end_sec > t->start_sec) {
            double d = t->end_sec - t->start_sec + 0.5;
            /* lv_clamp_dur takes the integral seconds (the double floor's
               at most one second off at the 24h edge); keep-semantics on
               out-of-range preserved: 0 means leave the row as it is */
            int c = lv_clamp_dur((long long)d);
            if (c) t->duration = c;
        }
    }
    pl_touch(p);   /* TITLE/PERFORMER fills above wrote searchable tags after the appends' bumps */
    return 0;
}

void pl_scan_tags(Playlist *p, pthread_mutex_t *mtx) {
    /* every read of count/items happens under mtx: the daemon's add/play
       realloc or swap the array under the same lock, and an unlocked walk
       here could follow a freed items pointer (or a shifted row) */
    for (int i = 0;; i++) {
        char path[LV_MAX_PATH];
        if (mtx) pthread_mutex_lock(mtx);
        if (i >= p->count) {
            if (mtx) pthread_mutex_unlock(mtx);
            break;
        }
        Track *t = &p->items[i];
        /* notag: the index already probed this file and found no tags;
           re-reading it over a network share would find nothing again */
        int skip = t->tags.title[0] || t->notag;
        lv_strlcpy(path, t->path, sizeof path);
        if (mtx) pthread_mutex_unlock(mtx);
        if (skip) continue;
        /* per-track locking: tag_read is file I/O and one lock for the
           whole playlist would stall track loads on big lists */
        Tags tags;
        tag_init(&tags);
        tag_read(path, &tags);
        if (mtx) pthread_mutex_lock(mtx);
        /* the row may have moved or changed while we were reading: only
           write back if item i is still the file we probed */
        if (i < p->count && !strcmp(p->items[i].path, path)) {
            const char *ti = pl_intern(&p->pool, tags.title);
            const char *ar = pl_intern(&p->pool, tags.artist);
            const char *al = pl_intern(&p->pool, tags.album);
            if (ti && ar && al) {
                p->items[i].tags.title = ti;
                p->items[i].tags.artist = ar;
                p->items[i].tags.album = al;
                p->items[i].tags.track = tags.track;
                pl_touch(p);   /* a row whose tags arrived late must not keep a stale cache */
            }
        }
        if (mtx) pthread_mutex_unlock(mtx);
    }
}

/* ================= playlist edit engine =================
   All functions below touch only the items array (no I/O, no locks):
   the caller holds p->mtx across the call (the sort/copy arithmetic on
   a 10k list is microseconds — the micro-hold rule permits it).
   CUE groups (a maximal run of adjacent rows sharing one path where at
   least one row carries CUE bounds) are the atomic unit everywhere. */

static int cue_row(const Track *t) { return t->start_sec != 0 || t->end_sec != 0; }

void pl_group_span(Playlist *p, int idx, int *a, int *b) {
    if (idx < 0 || idx >= p->count) { *a = *b = 0; return; }
    int i = idx;
    while (i > 0 && !strcmp(p->items[i - 1].path, p->items[i].path) &&
           (cue_row(&p->items[i - 1]) || cue_row(&p->items[i]))) i--;
    int j = idx + 1;
    while (j < p->count && !strcmp(p->items[j - 1].path, p->items[j].path) &&
           (cue_row(&p->items[j - 1]) || cue_row(&p->items[j]))) j++;
    *a = i; *b = j;
}

/* slots array with sentinel: *starts[0..un] = row starts, un = unit count,
   or -1 on OOM. Slot u covers rows [starts[u], starts[u+1]). */
static int pl_units(Playlist *p, int **starts) {
    int *s = malloc(((size_t)p->count + 1) * sizeof *s);
    if (!s) return -1;
    int un = 0;
    for (int i = 0; i < p->count; ) {
        int a, b;
        s[un++] = i;
        pl_group_span(p, i, &a, &b);   /* a == i by construction */
        (void)a;
        i = b;
    }
    s[un] = p->count;
    *starts = s;
    return un;
}

/* Apply newpos (old row index -> new row index, a valid permutation)
   in place: one held Track and a bitmap, never a copy of the array
   (Track is ~6 KB — a temp array would be gigabytes on a 10k list).
   OOM on the bitmap (1 byte/row) aborts with the list untouched. */
static void apply_newpos(Playlist *p, const int *newpos, int *cur) {
    int n = p->count;
    char *done = calloc((size_t)n, 1);
    if (!done) return;
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        int j = i;
        Track carry = p->items[i];       /* the element awaiting its home */
        for (;;) {
            done[j] = 1;
            int t = newpos[j];           /* carry belongs at t */
            if (t == i) { p->items[t] = carry; break; }
            Track moved = p->items[t];   /* its current owner moves on */
            p->items[t] = carry;
            carry = moved;
            j = t;
        }
    }
    free(done);
    pl_touch(p);   /* the single funnel for sort/reverse/shuffle/move/drop */
    if (cur && *cur >= 0 && *cur < n) *cur = newpos[*cur];
}

/* rows with an empty primary key sort last (decision D2) */
static int emptycmp(int ea, int eb) { return ea ? (eb ? 0 : 1) : (eb ? -1 : 0); }

typedef struct { Playlist *p; const int *starts; int key; } SortCtx;

/* CUE groups sort by their PARENT row = slot's first row (D2) */
static int cmpunit(const SortCtx *c, int ua, int ub) {
    const Track *ta = &c->p->items[c->starts[ua]];
    const Track *tb = &c->p->items[c->starts[ub]];
    switch (c->key) {
    case PL_SORT_TITLE: {
        int e = emptycmp(!ta->tags.title[0], !tb->tags.title[0]);
        return e ? e : strcmp(ta->tags.title, tb->tags.title);
    }
    case PL_SORT_ARTIST: {
        int e = emptycmp(!ta->tags.artist[0], !tb->tags.artist[0]);
        return e ? e : strcmp(ta->tags.artist, tb->tags.artist);
    }
    case PL_SORT_TRACK: {
        int ea = ta->tags.track <= 0, eb = tb->tags.track <= 0;
        int e = emptycmp(ea, eb);
        return e ? e : ta->tags.track - tb->tags.track;
    }
    case PL_SORT_FILE:
        return strcmp(ta->path, tb->path);
    case PL_SORT_ALBUM: {   /* album, then track, then title (D2) */
        int e = emptycmp(!ta->tags.album[0], !tb->tags.album[0]);
        if (e) return e;
        e = strcmp(ta->tags.album, tb->tags.album);
        if (e) return e;
        int ea = ta->tags.track <= 0, eb = tb->tags.track <= 0;
        e = emptycmp(ea, eb);
        if (e) return e;
        if (ta->tags.track != tb->tags.track) return ta->tags.track - tb->tags.track;
        e = emptycmp(!ta->tags.title[0], !tb->tags.title[0]);
        return e ? e : strcmp(ta->tags.title, tb->tags.title);
    }
    }
    return 0;
}

/* stable (D2: equal keys keep playlist order) — qsort is not, and the
   unit count is small, so plain bottom-up mergesort on the index array */
static void msort_idx(int *v, int *tmp, int n, const SortCtx *c) {
    if (n < 2) return;
    int h = n / 2;
    msort_idx(v, tmp, h, c);
    msort_idx(v + h, tmp, n - h, c);
    int i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = cmpunit(c, v[i], v[j]) <= 0 ? v[i++] : v[j++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * sizeof *v);
}

/* shared skeleton for sort/reverse/shuffle: permute the UNIT order
   (groups travel intact), then apply */
static void pl_perm_units(Playlist *p, const int *ord, int un, int *cur) {
    int *starts;
    if (pl_units(p, &starts) < 0) return;
    int *newpos = malloc((size_t)p->count * sizeof *newpos);
    if (newpos) {
        int d = 0;
        for (int u = 0; u < un; u++)
            for (int r = starts[ord[u]]; r < starts[ord[u] + 1]; r++)
                newpos[r] = d++;
        apply_newpos(p, newpos, cur);
        free(newpos);
    }
    free(starts);
}

void pl_sort(Playlist *p, int key, int *cur) {
    if (p->count < 2) return;
    int *starts = NULL;
    int un = pl_units(p, &starts);
    if (un < 2) { free(starts); return; }
    int *ord = malloc((size_t)un * sizeof *ord);
    int *tmp = malloc((size_t)un * sizeof *tmp);
    if (ord && tmp) {
        for (int i = 0; i < un; i++) ord[i] = i;
        SortCtx cx = { p, starts, key };
        msort_idx(ord, tmp, un, &cx);
        /* newpos reads starts by ord — pl_units re-derives identical
           slots inside pl_perm_units (deterministic by construction) */
        int *ord_out = malloc((size_t)un * sizeof *ord_out);
        if (ord_out) { memcpy(ord_out, ord, (size_t)un * sizeof *ord); }
        free(starts); free(ord); free(tmp);
        if (ord_out) { pl_perm_units(p, ord_out, un, cur); free(ord_out); }
        return;
    }
    free(ord); free(tmp); free(starts);
}

void pl_reverse(Playlist *p, int *cur) {
    if (p->count < 2) return;
    int *starts = NULL;
    int un = pl_units(p, &starts);
    free(starts);
    if (un < 2) return;
    int *ord = malloc((size_t)un * sizeof *ord);
    if (!ord) return;
    for (int i = 0; i < un; i++) ord[i] = un - 1 - i;
    pl_perm_units(p, ord, un, cur);
    free(ord);
}

void pl_shuffle(Playlist *p, int *cur) {
    if (p->count < 2) return;
    int *starts = NULL;
    int un = pl_units(p, &starts);
    free(starts);
    if (un < 2) return;
    int *ord = malloc((size_t)un * sizeof *ord);
    if (!ord) return;
    for (int i = 0; i < un; i++) ord[i] = i;
    for (int i = un - 1; i > 0; i--) {   /* Fisher-Yates, plain shuffle (D6) */
        int j = rand() % (i + 1);
        int t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }
    pl_perm_units(p, ord, un, cur);
    free(ord);
}

void pl_remove_set(Playlist *p, const int *sel, int n, int *cur) {
    /* multi-REM engine, kept byte-for-byte compatible with the PB_REM
       invariant: walk high-to-low so removals never shift a pending
       index; cur drops by the number of removed rows below it (the
       caller posts CMD_STOP itself when cur is in the set, exactly as
       PB_REM checks pl_sel_has(p->cur) first) */
    if (cur && *cur >= 0) {
        int below = 0;
        for (int i = 0; i < n; i++) {
            if (sel[i] < 0) continue;
            if (sel[i] < *cur) below++;
            else break;                  /* sorted; at/above cur: done */
        }
        for (int i = n - 1; i >= 0; i--)
            pl_remove(p, sel[i]);        /* stale-high clamped inside */
        *cur -= below;
        return;
    }
    for (int i = n - 1; i >= 0; i--)
        pl_remove(p, sel[i]);
}

int pl_sel_expand(Playlist *p, const int *sel, int n, int *out) {
    char *mark = calloc((size_t)p->count, 1);
    if (!mark) return -1;
    for (int i = 0; i < n; i++) {
        if (sel[i] < 0 || sel[i] >= p->count) continue;
        int a, b;
        pl_group_span(p, sel[i], &a, &b);
        for (int r = a; r < b; r++) mark[r] = 1;
    }
    int k = 0;
    for (int r = 0; r < p->count; r++)
        if (mark[r]) out[k++] = r;
    free(mark);
    return k;
}

void pl_crop(Playlist *p, const int *sel, int n, int *cur) {
    /* crop = remove the complement of the (expanded) selection: the same
       walk-high-to-low engine, so crop and multi-REM cannot drift */
    char *keep = calloc((size_t)p->count, 1);
    int *del = malloc((size_t)p->count * sizeof *del);
    if (!keep || !del) { free(keep); free(del); return; }
    for (int i = 0; i < n; i++)
        if (sel[i] >= 0 && sel[i] < p->count) keep[sel[i]] = 1;
    int dn = 0;
    for (int r = 0; r < p->count; r++)
        if (!keep[r]) del[dn++] = r;
    pl_remove_set(p, del, dn, cur);
    free(keep); free(del);
}

void pl_move(Playlist *p, int *sel, int n, int steps, int *cur) {
    if (p->count < 2 || n <= 0 || steps == 0) return;
    int *starts = NULL;
    int un = pl_units(p, &starts);
    if (un < 2) { free(starts); return; }
    int *len = malloc((size_t)un * sizeof *len);
    char *S = calloc((size_t)un, 1);
    if (!len || !S) { free(starts); free(len); free(S); return; }
    for (int u = 0; u < un; u++) len[u] = starts[u + 1] - starts[u];
    /* a selected row makes its WHOLE unit selected (group atomicity);
       sel is sorted, so walk it and the units together in one pass */
    int ui = 0;
    for (int i = 0; i < n; i++) {
        while (ui < un && sel[i] >= starts[ui + 1]) ui++;
        if (ui < un && sel[i] >= starts[ui]) S[ui] = 1;
    }
    Track *tmp;                        /* scratch sized to the biggest pair */
    int maxpair = 0;
    for (int u = 0; u + 1 < un; u++)
        if (len[u] + len[u + 1] > maxpair) maxpair = len[u] + len[u + 1];
    tmp = malloc((size_t)maxpair * sizeof *tmp);
    if (!tmp) { free(starts); free(len); free(S); return; }

    int down = steps > 0;
    for (int st = 0; st < (down ? steps : -steps); st++) {
        int moved = 0;
        /* one bubble step: a selected unit swaps with one unselected
           unit in the direction of travel; scanning order keeps a run of
           selected units together and in order */
        if (!down) {
            for (int u = 1; u < un; u++) {
                if (!S[u] || S[u - 1]) continue;
                int base = starts[u - 1], l1 = len[u - 1], l2 = len[u];
                memcpy(tmp, &p->items[base], (size_t)(l1 + l2) * sizeof *tmp);
                memcpy(&p->items[base], tmp + l1, (size_t)l2 * sizeof *tmp);
                memcpy(&p->items[base + l2], tmp, (size_t)l1 * sizeof *tmp);
                /* rows of unit u rose by l1, rows of unit u-1 sank by l2 */
                for (int i = 0; i < n; i++) {
                    if (sel[i] >= base + l1 && sel[i] < base + l1 + l2) sel[i] -= l1;
                    else if (sel[i] >= base && sel[i] < base + l1) sel[i] += l2;
                }
                if (cur && *cur >= 0 && *cur < p->count) {
                    if (*cur >= base + l1 && *cur < base + l1 + l2) *cur -= l1;
                    else if (*cur >= base && *cur < base + l1) *cur += l2;
                }
                S[u - 1] = 1; S[u] = 0;
                int t = len[u - 1]; len[u - 1] = len[u]; len[u] = t;
                moved = 1;
            }
        } else {
            for (int u = un - 2; u >= 0; u--) {
                if (!S[u] || S[u + 1]) continue;
                int base = starts[u], l1 = len[u], l2 = len[u + 1];
                memcpy(tmp, &p->items[base], (size_t)(l1 + l2) * sizeof *tmp);
                memcpy(&p->items[base], tmp + l1, (size_t)l2 * sizeof *tmp);
                memcpy(&p->items[base + l2], tmp, (size_t)l1 * sizeof *tmp);
                /* rows of unit u sank by l2, rows of unit u+1 rose by l1 */
                for (int i = 0; i < n; i++) {
                    if (sel[i] >= base && sel[i] < base + l1) sel[i] += l2;
                    else if (sel[i] >= base + l1 && sel[i] < base + l1 + l2) sel[i] -= l1;
                }
                if (cur && *cur >= 0 && *cur < p->count) {
                    if (*cur >= base && *cur < base + l1) *cur += l2;
                    else if (*cur >= base + l1 && *cur < base + l1 + l2) *cur -= l1;
                }
                S[u] = 0; S[u + 1] = 1;   /* the selection rode down */
                int t = len[u]; len[u] = len[u + 1]; len[u + 1] = t;
                moved = 1;
            }
        }
        if (!moved) break;
    }
    free(tmp); free(starts); free(len); free(S);
}

void pl_drop(Playlist *p, int *sel, int n, int at, int *cur) {
    if (p->count < 2 || n <= 0) return;
    if (at < 0) at = 0;
    if (at > p->count) at = p->count;
    int *starts = NULL;
    int un = pl_units(p, &starts);
    if (un < 2) { free(starts); return; }
    char *S = calloc((size_t)un, 1);
    char *mark = calloc((size_t)p->count, 1);
    int *newpos = malloc((size_t)p->count * sizeof *newpos);
    if (!S || !mark || !newpos) {
        free(S); free(mark); free(newpos); free(starts);
        return;                                   /* OOM: list untouched */
    }
    /* a selected row drags its WHOLE unit (group atomicity), same
       monotone walk pl_move uses over the sorted selection */
    int u = 0;
    for (int i = 0; i < n; i++) {
        while (u < un && sel[i] >= starts[u + 1]) u++;
        if (u < un && sel[i] >= starts[u]) S[u] = 1;
    }
    for (u = 0; u < un; u++)
        if (S[u])
            for (int r = starts[u]; r < starts[u + 1]; r++) mark[r] = 1;
    /* gap inside a group: snap forward to its end (the pl_clip_paste
       rule — a drop can never wedge between a parent and its rows) */
    while (at > 0 && at < p->count) {
        int a, b;
        pl_group_span(p, at - 1, &a, &b);
        if (at < b) at = b;
        else break;
    }
    /* reduced-array insertion, three passes: unselected rows above the
       gap, the block (in order), unselected rows at/below the gap. A
       gap inside the block's own span comes back out the other side as
       the identity permutation, so self-drops need no special case. */
    int d = 0;
    for (int r = 0; r < p->count; r++)
        if (!mark[r] && r < at) newpos[r] = d++;
    for (int r = 0; r < p->count; r++)
        if (mark[r]) newpos[r] = d++;
    for (int r = at; r < p->count; r++)
        if (!mark[r]) newpos[r] = d++;
    apply_newpos(p, newpos, cur);
    for (int i = 0; i < n; i++) sel[i] = newpos[sel[i]];
    free(newpos); free(mark); free(S); free(starts);
}

/* internal row clipboard (decision D3): a static Track array grown to the
   selection size, with its OWN intern pool — clipboard rows never point
   into a playlist's pool, so a playlist clear/GC can never dangle them.
   OOM refuses the copy whole and keeps the old clipboard */
static Track *clip;
static int clip_n;
static PlPool clip_pool;

int pl_clip_copy(Playlist *p, const int *sel, int n) {
    if (n <= 0) return 0;              /* empty selection: clipboard intact */
    for (int i = 0; i < n; i++)
        if (sel[i] < 0 || sel[i] >= p->count) return -1;  /* stale set: refuse */
    Track *nc = malloc((size_t)n * sizeof *nc);
    if (!nc) return -1;
    PlPool np;
    memset(&np, 0, sizeof np);
    for (int i = 0; i < n; i++) {
        const Track *src = &p->items[sel[i]];
        const char *a = pl_intern(&np, src->path);
        const char *b = pl_intern(&np, src->tags.title);
        const char *c = pl_intern(&np, src->tags.artist);
        const char *d = pl_intern(&np, src->tags.album);
        if (!a || !b || !c || !d) {
            pl_pool_free(&np);
            free(nc);
            return -1;                 /* refused: old clipboard intact */
        }
        nc[i] = *src;                  /* value copy: only the pointers are replaced */
        nc[i].path = a;
        nc[i].tags.title = b;
        nc[i].tags.artist = c;
        nc[i].tags.album = d;
    }
    pl_pool_free(&clip_pool);
    free(clip);
    clip = nc; clip_n = n; clip_pool = np;
    return 0;
}

void pl_clip_cut(Playlist *p, const int *sel, int n, int *cur) {
    if (pl_clip_copy(p, sel, n) != 0) return;   /* refused copy: no removal */
    pl_remove_set(p, sel, n, cur);
}

int pl_clip_paste(Playlist *p, int at, int *cur) {
    if (clip_n <= 0) return 0;
    if (at < 0) at = 0;
    if (at > p->count) at = p->count;
    /* never land inside a CUE group: snap forward to the group's end so
       a pasted block can never wedge between a parent and its rows */
    while (at > 0 && at < p->count) {
        int a, b;
        pl_group_span(p, at - 1, &a, &b);
        if (at < b) at = b;
        else break;
    }
    if (ensure(p, p->count + clip_n) != 0) return -1;
    /* re-intern every clipboard string into the target pool BEFORE the
       memmove: an OOM mid-way leaves the list untouched (the temp array
       holds the new pointers, no row has been written yet) */
    const char **tmp = malloc((size_t)clip_n * 4 * sizeof *tmp);
    if (!tmp) return -1;
    for (int i = 0; i < clip_n; i++) {
        const Track *src = &clip[i];
        tmp[(size_t)i * 4 + 0] = pl_intern(&p->pool, src->path);
        tmp[(size_t)i * 4 + 1] = pl_intern(&p->pool, src->tags.title);
        tmp[(size_t)i * 4 + 2] = pl_intern(&p->pool, src->tags.artist);
        tmp[(size_t)i * 4 + 3] = pl_intern(&p->pool, src->tags.album);
        if (!tmp[(size_t)i * 4] || !tmp[(size_t)i * 4 + 1] ||
            !tmp[(size_t)i * 4 + 2] || !tmp[(size_t)i * 4 + 3]) {
            free(tmp);
            return -1;
        }
    }
    memmove(&p->items[at + clip_n], &p->items[at],
            (size_t)(p->count - at) * sizeof(Track));
    for (int i = 0; i < clip_n; i++) {
        p->items[at + i] = clip[i];
        p->items[at + i].path = tmp[(size_t)i * 4];
        p->items[at + i].tags.title = tmp[(size_t)i * 4 + 1];
        p->items[at + i].tags.artist = tmp[(size_t)i * 4 + 2];
        p->items[at + i].tags.album = tmp[(size_t)i * 4 + 3];
    }
    free(tmp);
    p->count += clip_n;
    pl_touch(p);
    /* paste BELOW cur leaves cur alone; paste at/above it shifts it */
    if (cur && *cur >= 0 && at <= *cur) *cur += clip_n;
    return 0;
}

int pl_clip_count(void) { return clip_n; }
