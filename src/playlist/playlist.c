#include "playlist.h"
#ifdef _WIN32
#include "../compat/win32.h"
#endif
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

void pl_init(Playlist *p) { p->items = NULL; p->count = p->cap = 0; }

void pl_clear(Playlist *p) {
    free(p->items);
    p->items = NULL; p->count = p->cap = 0;
}

static int ensure(Playlist *p, int need) {
    if (need <= p->cap) return 0;
    int cap = p->cap ? p->cap + p->cap / 2 + 1 : 64;
    while (cap < need) {
        /* clamp before doubling can wrap the int counter; a Track is ~6 KB
           so the realloc below fails long before — this just documents it */
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

static int audio_ext(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !lv_strcasecmp(e, ".mp3") || !lv_strcasecmp(e, ".ogg") ||
           !lv_strcasecmp(e, ".oga") || !lv_strcasecmp(e, ".opus") ||
           !lv_strcasecmp(e, ".flac") || !lv_strcasecmp(e, ".wav") ||
           !lv_strcasecmp(e, ".aac") || !lv_strcasecmp(e, ".m4a") ||
           !lv_strcasecmp(e, ".wma") || !lv_strcasecmp(e, ".mod") ||
           !lv_strcasecmp(e, ".s3m") || !lv_strcasecmp(e, ".xm") ||
           !lv_strcasecmp(e, ".it") || !lv_strcasecmp(e, ".spc");
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
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        struct stat st;
        /* lstat: never descend into a symlinked directory (cycle guard).
           On Windows lstat is plain stat (compat/win32.h) and follows
           junctions, so refuse reparse points outright: a junction loop
           would recurse until the stack overflows. */
#ifdef _WIN32
        if (GetFileAttributesA(full) & FILE_ATTRIBUTE_REPARSE_POINT) continue;
#endif
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
            pl_add_dir(p, full);
        else if (audio_ext(names[i]))
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
    if (ensure(p, p->count + 1) != 0) return -1;
    Track *t = &p->items[p->count++];
    if (lv_is_abs(path) || lv_is_url(path)) {
        /* URLs are already fully qualified — cwd-absolutizing them would
           mangle the scheme into a bogus relative path (P0 gate caught
           exactly this: dec_open would never see the URL) */
        lv_strlcpy(t->path, path, sizeof t->path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd))
            snprintf(t->path, sizeof t->path, "%s/%s", cwd, path);
        else
            lv_strlcpy(t->path, path, sizeof t->path);
    }
    tag_init(&t->tags);
    t->duration = 0;
    t->notag = 0;
    t->start_sec = t->end_sec = 0;
    return 0;
}

void pl_insert(Playlist *p, int idx, const char *path) {
    if (idx < 0) idx = 0;
    if (idx > p->count) idx = p->count;
    if (ensure(p, p->count + 1) != 0) return;
    memmove(&p->items[idx + 1], &p->items[idx], (size_t)(p->count - idx) * sizeof(Track));
    p->count++;
    Track *t = &p->items[idx];
    lv_strlcpy(t->path, path, sizeof t->path);
    tag_init(&t->tags);
    t->duration = 0;
    t->notag = 0;
    t->start_sec = t->end_sec = 0;
}

void pl_remove(Playlist *p, int idx) {
    if (idx < 0 || idx >= p->count) return;
    memmove(&p->items[idx], &p->items[idx + 1], (size_t)(p->count - idx - 1) * sizeof(Track));
    p->count--;
}

int pl_load_m3u(Playlist *p, const char *m3u) {
    FILE *f = fopen(m3u, "r");
    if (!f) return -1;
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, m3u, sizeof dir);
    if (!lv_dirname(dir)) lv_strlcpy(dir, ".", sizeof dir);

    /* pre-size from the file size (~96 bytes per entry): multi-KB
       playlists otherwise churn realloc through every doubling. cap it:
       Track is ~6 KB, so a multi-MB m3u (or a junk huge file sniffed as
       one) would pre-allocate gigabytes */
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
            pending_dur = atoi(s + 8);
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
   index is max_index * sizeof(PlsEnt) (~4.3 KB) — the same transient the
   pre-fix reader already spent on a real 65k list; only a FILE that
   literally numbers an entry that high pays it, so this is not corruption,
   and .pls files are only parsed on explicit user load. */
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
            int d = atoi(eq);   /* LengthN -1 = unknown -> 0 */
            ent[at].dur = d > 0 ? d : 0;
        }
    }
    fclose(f);
    for (int i = 0; i < ent_n; i++) {
        if (!ent[i].path[0]) continue;   /* TitleN/LengthN without FileN */
        if (pl_add_file(p, ent[i].path) == 0) {
            Track *t = &p->items[p->count - 1];
            t->duration = ent[i].dur;
            if (ent[i].title[0])
                lv_strlcpy(t->tags.title, ent[i].title, sizeof t->tags.title);
        }
    }
    free(ent);
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
                lv_strlcpy(p->items[idx].tags.title, q1 + 1, sizeof p->items[idx].tags.title);
            }
        } else if (!strncmp(s, "PERFORMER ", 10) && idx >= 0 && idx < p->count) {
            char *q1 = strchr(s, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) *q2 = 0;
                lv_strlcpy(p->items[idx].tags.artist, q1 + 1, sizeof p->items[idx].tags.artist);
            }
        } else if (!strncmp(s, "INDEX 01 ", 9)) {
            int mm, ss, ff;
            if (sscanf(s + 9, "%d:%d:%d", &mm, &ss, &ff) == 3) {
                double sec = mm * 60 + ss + ff / 75.0;
                if (idx >= 0 && idx < p->count) {
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
        if (t->end_sec > t->start_sec)
            t->duration = (int)(t->end_sec - t->start_sec + 0.5);
    }
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
        if (i < p->count && !strcmp(p->items[i].path, path))
            p->items[i].tags = tags;
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
   selection size; OOM refuses the copy whole and keeps the old clipboard */
static Track *clip;
static int clip_n;

int pl_clip_copy(Playlist *p, const int *sel, int n) {
    if (n <= 0) return 0;              /* empty selection: clipboard intact */
    for (int i = 0; i < n; i++)
        if (sel[i] < 0 || sel[i] >= p->count) return -1;  /* stale set: refuse */
    Track *nc = malloc((size_t)n * sizeof *nc);
    if (!nc) return -1;
    for (int i = 0; i < n; i++) nc[i] = p->items[sel[i]];
    free(clip);
    clip = nc; clip_n = n;
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
    memmove(&p->items[at + clip_n], &p->items[at],
            (size_t)(p->count - at) * sizeof(Track));
    for (int i = 0; i < clip_n; i++) p->items[at + i] = clip[i];
    p->count += clip_n;
    /* paste BELOW cur leaves cur alone; paste at/above it shifts it */
    if (cur && *cur >= 0 && at <= *cur) *cur += clip_n;
    return 0;
}

int pl_clip_count(void) { return clip_n; }
