#include "library.h"
#include "../tag/tag.h"
#include "../decode/decode.h"
#include "../daemon/daemon.h"
#include "../playlist/playlist.h"
#include "../common.h"
#include "../config/config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

void lib_index_path(char *out, size_t n) {
    if (cfg.lib_index[0]) {
        lv_expand_home(cfg.lib_index, out, n);
        char dir[LV_MAX_PATH];
        lv_strlcpy(dir, out, sizeof dir);
        if (lv_dirname(dir)) mkdir(dir, 0755);
        return;
    }
    char base[LV_MAX_PATH];
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if (local && *local) snprintf(base, sizeof base, "%s/levari", local);
    else lv_strlcpy(base, ".", sizeof base);
#else
    const char *home = getenv("HOME");
    snprintf(base, sizeof base, "%s/.levari", home ? home : ".");
#endif
    mkdir(base, 0755);
    snprintf(out, n, "%s/index", base);
}

static int is_audio(const char *name) {
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

typedef struct {
    FILE *out;
    char **sorted; /* existing paths, sorted; scan skips anything already indexed */
    int n;
    int added;
    int seen;
    int tags; /* 0 = index filenames only (skip tag + decoder reads) */
    int toolong; /* paths dropped for length: reported, never silent */
} ScanCtx;

static int known_path(const ScanCtx *c, const char *p) {
    if (!c->sorted) return 0;
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int r = strcmp(c->sorted[mid], p);
        if (r == 0) return 1;
        if (r < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* the index is tab-separated with one row per line: a path containing a
   tab or newline would splice rows together. escape \t \n \\ in the path
   field (tag fields are already control-char-free from tag.c).
   the first line "#levari-index v2" marks the escaped format: headerless
   (pre-0.3.0) indexes store RAW paths and must never be unescaped — a
   Windows path C:\Users\me\new would lose backslashes (\n -> newline).
   lib_scan_add migrates a legacy index: safely-readable headerless rows
   are KEPT verbatim under a stamped header; an ambiguous one (lone
   backslash: cannot prove escaped-vs-raw) is discarded and rebuilt.
   lib_index_rescan refuses an ambiguous one outright — a partial update
   never rewrites what it cannot read.
   since 0.3.4 the header carries a creation marker: "<version> <YYMMDD>"
   (UTC, written when the index is created — appended scans never rewrite
   it). Detection anywhere is only strncmp("#levari-index", 13), and the
   whole line stays <= 31 bytes INCLUDING the newline for current version
   strings, so pre-0.3.4 readers — whose header probe is fgets(probe,32)
   — consume it in one gulp and see nothing new. If the version string
   ever outgrows the budget the old readers degrade to one tabless tail
   line, which the row parsers already skip (only cmd_list prints it);
   the new reader drains long headers outright. */
#define IDX_HEADER_FMT "#levari-index v2 %s %s\n"

static void idx_write_header(FILE *w) {
    time_t t = time(NULL);
    char stamp[8];
    stamp[0] = 0;
    struct tm *g = gmtime(&t); /* sole writer context (scan path); no _r needed */
    if (g) strftime(stamp, sizeof stamp, "%y%m%d", g);
    fprintf(w, IDX_HEADER_FMT, LV_VERSION, stamp);
}

int lib_idx_header(FILE *f) {
    /* consume the v2 header line if present; leave the stream at row 1 */
    long start = ftell(f);
    char probe[128];
    if (fgets(probe, sizeof probe, f)) {
        if (!strncmp(probe, "#levari-index", 13)) {
            /* a header longer than the probe (future version strings):
               swallow the rest so readers never see a phantom tail row */
            while (!strchr(probe, '\n') && fgets(probe, sizeof probe, f))
                ;
            return 1;
        }
        fseek(f, start, SEEK_SET);
    }
    return 0;
}

static void esc_field(char *dst, size_t dn, const char *src) {
    size_t j = 0;
    for (const char *p = src; *p && j + 2 < dn; p++) {
        if (*p == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (*p == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (*p == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else dst[j++] = *p;
    }
    dst[j] = 0;
}
/* dn is the dst capacity: the unescape only shrinks (every escape is a
   2-byte pair) but callers hand in buffers sized by the line reader, not
   by a contract — take the bound like esc_field does. In-place calls pass
   the string length as dn: output can never exceed input there. */
static void unesc_field(char *dst, size_t dn, const char *src, size_t sn) {
    if (dn == 0) return;
    char *d = dst;
    const char *de = dst + dn - 1;
    for (const char *p = src, *e = src + sn; p < e && d < de; p++) {
        if (*p == '\\' && p + 1 < e) {
            p++;
            *d++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p;
        } else *d++ = *p;
    }
    *d = 0;
}

/* the in-place string carries its own NUL, so the buffer is len+1 bytes */
void lib_unesc_path(char *p) { size_t l = strlen(p); unesc_field(p, l + 1, p, l); }

/* fgets splits an index row longer than the line buffer: the tail half
   would re-parse as a garbage row and self-corrupt the index, so drain
   to EOL and report the row as truncated (callers skip it). returns 1
   when a line was read, 2 when that line was cut mid-row. */
int lib_idx_getline(FILE *f, char *line, size_t n) {
    if (!fgets(line, (int)n, f)) return 0;
    if (!strchr(line, '\n') && !feof(f)) {
        int c;
        while ((c = fgetc(f)) != '\n' && c != EOF) {}
        return 2;
    }
    return 1;
}

/* ensure the v2 format on disk; runs before EVERY read-modify-write of
   the index. Returns 1 = already v2 (or freshly created), 0 = headerless
   but SAFE to keep: the header was stamped and every row preserved
   byte-verbatim, -1 = headerless and AMBIGUOUS: rows dropped, header-only
   file (the full-scan caller rebuilds what it scans), -2 = headerless,
   ambiguous, and partial=1: REFUSED — the original file is byte-identical
   and the caller must not rewrite (lib_index_rescan maps it to -3,
   "run a full library scan"; a single-file rescan must never silently
   become a full index reset — 2026-11-07 GH-line ruling).
   Headerless means AMBIGUOUS: 0.3.0 already wrote escaped paths without
   ever writing a header, so legacy rows are escaped (0.3.0) or raw
   (pre-0.3.0), and assuming raw re-escaped them — C:\\Music became
   C:\\\\Music on upgrade (lookups missed, rescans appended duplicates,
   play got corrupt paths: the 0.3.2 incident). Safe is decidable per
   file: no backslash byte at all (both readings are then byte-identical),
   or every backslash begins a valid escape pair — \\ \t \n, exactly what
   esc_field emits. Such a file is ALREADY the v2 wire format under either
   reading: stamping the header is the entire migration, no per-row
   translation, no re-escape. One lone backslash makes it ambiguous.
   Both write paths build the replacement in a .tmp and rename over the
   original only after the write succeeded: a crash never leaves a
   half-index where a whole one was. */
static int idx_rename_tmp(const char *tmp, const char *dst) {
    /* mingw's rename refuses an existing target: remove-retry, the
       cfg_save/lib_index_rescan pattern */
    if (rename(tmp, dst) != 0) {
        remove(dst);
        if (rename(tmp, dst) != 0) { remove(tmp); return -1; }
    }
    return 0;
}

static int idx_migrate(const char *idx, int partial) {
    FILE *r = fopen(idx, "r");
    if (!r) {
        FILE *w = fopen(idx, "w");
        if (w) { idx_write_header(w); fclose(w); }
        return 1;
    }
    if (lib_idx_header(r)) { fclose(r); return 1; } /* already v2 */

    /* headerless: classify from the raw bytes, touching nothing */
    int ambiguous = 0, c;
    while ((c = fgetc(r)) != EOF) {
        if (c != '\\') continue;
        int n = fgetc(r);
        if (n != '\\' && n != 't' && n != 'n') { ambiguous = 1; break; }
    }
    if (ferror(r)) ambiguous = 1; /* unreadable: never trust a guess */
    if (fseek(r, 0, SEEK_SET) != 0) ambiguous = 1;

    if (!ambiguous) {
        /* stamp the header ahead of the verbatim rows — the whole
           migration, atomically */
        char tmp[LV_MAX_PATH + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", idx);
        FILE *w = fopen(tmp, "w");
        int bad = !w;
        if (w) {
            idx_write_header(w);
            char buf[4096];
            size_t n;
            while (!bad && (n = fread(buf, 1, sizeof buf, r)) > 0)
                if (fwrite(buf, 1, n, w) != n) bad = 1;
            if (fclose(w) != 0) bad = 1;
        }
        fclose(r);
        if (!bad && idx_rename_tmp(tmp, idx) == 0) return 0;
        /* a failed copy left the original untouched: fall through and
           let the caller decide — refuse (partial) or discard-and-rebuild */
    } else {
        fclose(r);
    }
    if (partial) return -2;   /* byte-identical original, decision refused */
    char tmp[LV_MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", idx);
    FILE *w = fopen(tmp, "w");
    if (w) { idx_write_header(w); if (fclose(w) == 0)
                 if (idx_rename_tmp(tmp, idx) == 0) return -1; }
    remove(tmp);
    return -1; /* best effort: even the discard failed; every reader below
                  treats the still-headerless file as untrusted anyway */
}

static void scan_dir(const char *dir, ScanCtx *c) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[LV_MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        /* lstat: a symlinked directory must not be descended, or a
           symlink cycle in the tree recurses until the stack dies */
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
#ifdef _WIN32
            /* MinGW maps lstat->stat (compat/win32.h), so junctions and
               other reparse points look exactly like real directories in
               st_mode; a junction pointing at an ancestor recurses until
               the stack dies. Skipping reparse dirs is the Windows
               counterpart of not descending into symlinks above. */
            DWORD attr = GetFileAttributesA(full);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
            scan_dir(full, c);
        } else if (is_audio(de->d_name)) {
            /* the index row stores the escaped path (up to 2x): a path
               near LV_MAX_PATH full of backslashes produces a row longer
               than the readers' line buffer, which then splits mid-row.
               skip anything whose escaped form cannot fit one row, but
               COUNT the drop: a silent skip hides whole subtrees. */
            if (strlen(full) > LV_MAX_PATH / 2) { c->toolong++; continue; }
            c->seen++;
            if (known_path(c, full)) continue;
            Tags t;
            tag_init(&t);
            int dur = 0;
            if (c->tags) {
                /* one open + one head read covers tags and (for MP3) the
                   duration via XING/VBRI or a CBR estimate; the decoder is
                   only opened for formats that need it for length */
                dur = tag_probe(full, st.st_size, &t);
                if (!dur) {
                    Decoder *dec = dec_open(full);
                    if (dec) {
                        long long tot = dec_total_frames(dec);
                        /* long long before the cast: a crafted container
                           (ffmpeg's st->duration is unclamped) can report
                           a total whose seconds exceed INT_MAX, and the
                           old direct cast was implementation-defined */
                        long long s = tot > 0 ? tot / dec_rate(dec) : 0;
                        if (s > 0 && s <= LV_DUR_MAX) dur = (int)s;
                        dec_close(dec);
                    }
                }
            }
            /* only a scan that actually read tags may certify "no tags";
               a filenames-only pass must not mark anything */
            int notag = c->tags && !t.title[0] && !t.artist[0] && !t.album[0] && !t.track;
            char esc[2 * LV_MAX_PATH];
            esc_field(esc, sizeof esc, full);
            fprintf(c->out, "%s\t%s\t%s\t%s\t%d\t%d", esc, t.artist, t.title, t.album, dur, t.track);
            /* sparse 7th field: only untagged files carry the marker, so
               tagged rows keep the original 6-field shape */
            if (notag) fprintf(c->out, "\t1");
            fputc('\n', c->out);
            c->added++;
        }
    }
    closedir(d);
}

static int contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++)
        if (!lv_strncasecmp(p, needle, nl)) return 1;
    return 0;
}

static int cmd_scan(const char *dir, FILE *out) {
    char abs[LV_MAX_PATH], exp[LV_MAX_PATH];
    lv_expand_home(dir, exp, sizeof exp);
    if (!realpath(exp, abs)) { fprintf(out, "levari: no such dir: %s\n", dir); return 1; }
    char dirs[1][LV_MAX_PATH];
    lv_strlcpy(dirs[0], abs, LV_MAX_PATH);
    int added = 0, seen = 0, skipped = 0;
    if (lib_scan_add(dirs, 1, 1, &added, &seen, &skipped) != 0) {
        char idx[LV_MAX_PATH];
        lib_index_path(idx, sizeof idx);
        fprintf(out, "levari: cannot write %s\n", idx);
        return 1;
    }
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    fprintf(out, "scanned %d files into %s\n", seen, idx);
    if (skipped)
        fprintf(out, "levari: %d paths longer than %d chars were not indexed\n",
                skipped, LV_MAX_PATH / 2);
    return 0;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int lib_scan_add(const char dirs[][LV_MAX_PATH], int n, int with_tags, int *added, int *seen, int *skipped) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    idx_migrate(idx, 0);
    char **sorted = NULL;
    int sn = 0, cap = 0;
    FILE *r = fopen(idx, "r");
    if (r) {
        /* rows can reach 2*LV_MAX_PATH (escaped path) + tags + numbers:
           8192 used to split long rows mid-line and corrupt the reload */
        char line[2 * LV_MAX_PATH + 1024];
        lib_idx_header(r); /* migrate() guarantees the header is present */
        int gr;
        while ((gr = lib_idx_getline(r, line, sizeof line)) != 0) {
            if (gr == 2) continue; /* over-long row: already drained */
            char *tab = strchr(line, '\t');
            if (tab) *tab = 0;
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!line[0]) continue;
            size_t sl = strlen(line);
            unesc_field(line, sl + 1, line, sl); /* in-place: only shrinks */
            if (sn == cap) {
                cap = cap ? cap * 2 : 256;
                char **s2 = realloc(sorted, (size_t)cap * sizeof *sorted);
                if (!s2) break;
                sorted = s2;
            }
            char *dup = strdup(line);
            if (!dup) break;
            sorted[sn++] = dup;
        }
        fclose(r);
        if (sn) qsort(sorted, (size_t)sn, sizeof *sorted, cmpstr);
    }
    FILE *f = fopen(idx, "a");
    if (!f) {
        for (int i = 0; i < sn; i++) free(sorted[i]);
        free(sorted);
        return 1;
    }
    ScanCtx c = { f, sorted, sn, 0, 0, with_tags, 0 };
    for (int i = 0; i < n; i++) {
        char abs[LV_MAX_PATH], exp[LV_MAX_PATH];
        lv_expand_home(dirs[i], exp, sizeof exp);
        if (realpath(exp, abs)) scan_dir(abs, &c);
    }
    fclose(f);
    for (int i = 0; i < sn; i++) free(sorted[i]);
    free(sorted);
    if (added) *added = c.added;
    if (seen) *seen = c.seen;
    if (skipped) *skipped = c.toolong;
    return 0;
}

/* one pass over the index, sorted for binary search: fills tags and
   duration for playlist items whose title is still empty. exact path
   match only (index paths are realpath'd at scan time); anything not
   found falls through to the file-based pl_scan_tags. */
static int cmprow(const void *a, const void *b) {
    return strcmp(((const LibRow *)a)->path, ((const LibRow *)b)->path);
}

LibIndex lib_index_load(void) {
    LibIndex ix = { NULL, 0 };
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *r = fopen(idx, "r");
    if (!r) return ix;
    int cap = 0;
    char line[2 * LV_MAX_PATH + 1024];
    int v2 = lib_idx_header(r);
    int gr;
    while ((gr = lib_idx_getline(r, line, sizeof line)) != 0) {
        if (gr == 2) continue; /* over-long row: already drained */
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p1 = line;
        char *p2 = strchr(p1, '\t'); if (!p2) continue; *p2++ = 0;
        if (v2) { size_t sl = strlen(p1); unesc_field(p1, sl + 1, p1, sl); } /* match real paths */
        char *p3 = strchr(p2, '\t'); if (!p3) continue; *p3++ = 0;
        char *p4 = strchr(p3, '\t'); if (!p4) continue; *p4++ = 0;
        char *p5 = strchr(p4, '\t');
        int dur = 0, track = 0, notag = 0;
        if (p5) {
            *p5++ = 0;
            char *p6 = strchr(p5, '\t');
            if (p6) {
                *p6++ = 0;
                int d5 = atoi(p5);
                dur = (d5 > 0 && d5 <= LV_DUR_MAX) ? d5 : 0;  /* clamp the number, NEVER drop the row */
                char *p7 = strchr(p6, '\t');
                if (p7) { *p7++ = 0; track = atoi(p6); notag = atoi(p7); }
                else track = atoi(p6);
            } else {
                int d5 = atoi(p5);
                dur = (d5 > 0 && d5 <= LV_DUR_MAX) ? d5 : 0;  /* clamp the number, NEVER drop the row */
            }
        }
        if (!p1[0]) continue;
        if (ix.n == cap) {
            cap = cap ? cap * 2 : 256;
            LibRow *x = realloc(ix.rows, (size_t)cap * sizeof *ix.rows);
            if (!x) break;
            ix.rows = x;
        }
        /* an unchecked strdup leaves a NULL path/artist in the row:
           cmprow's strcmp and the hit->artist[0] test below deref it */
        char *rp = strdup(p1), *ra = strdup(p2);
        char *rt = strdup(p3), *ral = strdup(p4);
        if (!rp || !ra || !rt || !ral) {
            free(rp); free(ra); free(rt); free(ral);
            break;
        }
        LibRow *row = &ix.rows[ix.n++];
        row->path = rp; row->artist = ra; row->title = rt; row->album = ral;
        row->dur = dur; row->track = track; row->notag = notag;
    }
    fclose(r);
    if (ix.n) qsort(ix.rows, (size_t)ix.n, sizeof *ix.rows, cmprow);
    return ix;
}

void lib_index_free(LibIndex *ix) {
    for (int i = 0; i < ix->n; i++) {
        free(ix->rows[i].path);
        free(ix->rows[i].artist);
        free(ix->rows[i].title);
        free(ix->rows[i].album);
    }
    free(ix->rows);
    ix->rows = NULL;
    ix->n = 0;
}

/* index rows are only trusted as far as our own writer: a hand-edited or
   tampered index can carry control bytes and broken utf-8 that would
   otherwise flow into Track.tags and from there to MPRIS, the daemon
   status/pl output and m3u saves. tag.c guarantees cleanliness only for
   values it wrote, so sanitize on *read*. */
static void idx_copy(char *dst, size_t dn, const char *src) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && j + 1 < dn; p++)
        if (*p >= 0x20 && *p != 0x7f) dst[j++] = (char)*p;
    dst[j] = 0;
    lv_utf8_trim(dst);
}

void lib_fill_playlist_ix(Playlist *p, const LibIndex *ix) {
    for (int i = 0; i < p->count; i++) {
        Track *t = &p->items[i];
        if (t->tags.title[0]) continue;
        LibRow key = { t->path, NULL, NULL, NULL, 0, 0, 0 };
        LibRow *hit = bsearch(&key, ix->rows, (size_t)ix->n, sizeof *ix->rows, cmprow);
        if (!hit) continue;
        if (hit->artist[0]) idx_copy(t->tags.artist, sizeof t->tags.artist, hit->artist);
        if (hit->title[0]) idx_copy(t->tags.title, sizeof t->tags.title, hit->title);
        if (hit->album[0]) idx_copy(t->tags.album, sizeof t->tags.album, hit->album);
        if (hit->track) t->tags.track = hit->track;
        if (hit->dur) t->duration = hit->dur;
        t->notag = hit->notag;
    }
}

void lib_fill_playlist(Playlist *p) {
    LibIndex ix = lib_index_load();
    /* free unconditionally: on an OOM row lib_index_load can return n==0
       with rows already allocated (realloc before strdup) — the old early
       return leaked it */
    if (ix.n) lib_fill_playlist_ix(p, &ix);
    lib_index_free(&ix);
}

/* Field 5 (duration) of a raw index row: count 4 tabs, atoi. Tag fields are
   written RAW by the scanner — a literal tab inside a tag shifts this parse
   exactly as it shifts every reader (pre-existing format hazard, not
   introduced here); the value only ever preserves a duration a fresh probe
   could not determine, so a mis-parse loses a length, never corrupts a row. */
static int row_dur(const char *line) {
    const char *q = line;
    for (int f = 0; f < 4; f++) {
        q = strchr(q, '\t');
        if (!q) return 0;
        q++; /* advance PAST the tab: strchr(q,'\t') would find it again */
    }
    int d = atoi(q);
    /* same LV_DUR_MAX clamp as lib_index_load: this is the index's second
       reader (rescan fallback when the probe goes blind) and its value
       lands straight in Track.duration via out_dur */
    return (d > 0 && d <= LV_DUR_MAX) ? d : 0;
}

int lib_index_rescan(const char *path, Tags *out_tags, int *out_dur,
                     int *out_notag) {
    char rp[LV_MAX_PATH];
    if (!realpath(path, rp)) return -1;
    struct stat st;
    if (stat(rp, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    Tags t;
    tag_init(&t);
    int dur = tag_probe(rp, st.st_size, &t);
    /* the scanner's notag certification rule (tag-enabled pass): a full
       probe that finds nothing AT ALL certifies the sparse 7th field */
    int notag = !t.title[0] && !t.artist[0] && !t.album[0] && !t.track;

    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    if (idx_migrate(idx, 1) == -2) return -3; /* pre-v2 and ambiguous: the
        index is untouched — only a full scan may rewrite it */

    char line[2 * LV_MAX_PATH + 1024]; /* house row budget */
    char *hdr = NULL;                  /* header line kept byte-identically:
                                          the creation marker attributes the
                                          index's ORIGIN — a rewrite must not
                                          restamp it (b6cb2ea semantics) */
    char **rows = NULL;
    int n = 0, cap = 0, hit = -1, oom = 0;
    FILE *r = fopen(idx, "r");
    if (!r) return -2;
    if (lib_idx_getline(r, line, sizeof line) == 1 &&
        !strncmp(line, "#levari-index", 13))
        hdr = strdup(line);            /* stream now sits at row 1 */
    else
        fseek(r, 0, SEEK_SET);         /* headerless: read rows as found */
    int gr;
    while ((gr = lib_idx_getline(r, line, sizeof line)) != 0) {
        if (gr == 2) continue; /* over-long row: dropped, same rule as reload */
        if (hit < 0) {
            char key[2 * LV_MAX_PATH];
            size_t tl = strcspn(line, "\t"); /* path is field 1; never holds
                                                an UNESCAPED tab */
            if (tl >= sizeof key) tl = sizeof key - 1;
            memcpy(key, line, tl);
            key[tl] = 0;
            unesc_field(key, sizeof key, key, tl);
            if (!strcmp(key, rp)) hit = n;
        }
        if (n == cap) {
            int nc = cap ? cap * 2 : 128;
            char **p2 = realloc(rows, (size_t)nc * sizeof *rows);
            if (!p2) { oom = 1; break; }
            rows = p2;
            cap = nc;
        }
        rows[n] = strdup(line);
        if (!rows[n]) { oom = 1; break; }
        n++;
    }
    fclose(r);
    if (oom) {
        for (int i = 0; i < n; i++) free(rows[i]);
        free(rows);
        free(hdr);
        return -2;
    }
    /* probe blind on duration: a tag_probe that yields 0 must not erase a
       duration the scanner learned from the decoder */
    if (!dur && hit >= 0) dur = row_dur(rows[hit]);

    /* new row in the scanner's exact shape: escaped path, RAW tag fields,
       sparse 7th marker only when notag */
    char esc[2 * LV_MAX_PATH];
    esc_field(esc, sizeof esc, rp);
    char row[2 * LV_MAX_PATH + 1024];
    snprintf(row, sizeof row, "%s\t%s\t%s\t%s\t%d\t%d%s\n",
             esc, t.artist, t.title, t.album, dur, t.track,
             notag ? "\t1" : "");

    char tmp[LV_MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", idx);
    FILE *w = fopen(tmp, "w");
    if (!w) {
        for (int i = 0; i < n; i++) free(rows[i]);
        free(rows);
        free(hdr);
        return -2;
    }
    if (hdr) fputs(hdr, w);
    else idx_write_header(w);
    for (int i = 0; i < n; i++) {
        if (i == hit) fputs(row, w);
        else fputs(rows[i], w);
        free(rows[i]);
    }
    if (hit < 0) fputs(row, w); /* decision A: absent -> append */
    fclose(w);
    free(rows);
    free(hdr);
    /* mingw's rename refuses to replace an existing file: retry once
       after removing the target (same fallback as cfg_save) */
    if (rename(tmp, idx) != 0) {
        remove(idx);
        if (rename(tmp, idx) != 0) { remove(tmp); return -2; }
    }
    if (out_tags) *out_tags = t;
    if (out_dur) *out_dur = dur;
    if (out_notag) *out_notag = notag;
    return 0;
}

/* ---- urlIndex: one row = the last working URL (D5). Lives beside the
   library index — derive the directory from lib_index_path (whose
   branches both mkdir their base, so the folder exists after the call) */
static void url_index_path(char *out, size_t n) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    if (lv_dirname(idx)) snprintf(out, n, "%s/urlIndex", idx);
    else lv_strlcpy(out, "urlIndex", n); /* index path had no dir: same cwd */
}

int lib_url_index_load(char *out, size_t n) {
    out[0] = 0;
    char p[LV_MAX_PATH];
    url_index_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[2 * LV_MAX_PATH + 10]; /* room past the cap so an oversize
                                        tampered row is detected, not split */
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got) return -1;
    size_t len = strlen(line);
    while (len && line[len - 1] == '\n') line[--len] = 0;
    /* ONLY the trailing newline is trimmed: a stray CR/control byte is
       tampering evidence and must survive so the Enter gate rejects the
       row (inert-display contract). Oversize rows never enter the
       caller's buffer — they simply do not prefill. */
    if (!len || len >= n) return -1;
    lv_strlcpy(out, line, n);
    return 0;
}

int lib_url_index_save(const char *url) {
    /* shape + hygiene gate (lv_is_url rejects CR/LF/space/DEL anywhere,
       so the row can never become a multi-line or injection carrier) +
       scheme re-check for defence in depth: only open-verified URLs are
       ever banked (the open passed the allowlist), so this can only fire
       on a wrong caller — a file:// shape must never reach the file */
    if (!lv_is_url(url) || !lv_url_scheme_allowed(url, LV_URL_SCHEMES) ||
        strlen(url) >= LV_MAX_PATH) return -1;
    char p[LV_MAX_PATH];
    url_index_path(p, sizeof p);
    char tmp[LV_MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *w = fopen(tmp, "w");
    if (!w) return -1;
    fprintf(w, "%s\n", url);
    if (fflush(w) || ferror(w)) { fclose(w); remove(tmp); return -1; }
    if (fclose(w) != 0) { remove(tmp); return -1; }
    /* mingw's rename refuses to replace an existing file: retry once
       after removing the target (lib_index_rescan/cfg_save pattern) */
    if (rename(tmp, p) != 0) {
        remove(p);
        if (rename(tmp, p) != 0) { remove(tmp); return -1; }
    }
    return 0;
}

static int cmd_list(const char *filter, FILE *out) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *f = fopen(idx, "r");
    if (!f) { fprintf(out, "(library empty)\n"); return 0; }
    char line[2 * LV_MAX_PATH + 1024];
    int v2 = lib_idx_header(f);
    int shown = 0;
    int gr;
    while ((gr = lib_idx_getline(f, line, sizeof line)) != 0) {
        if (gr == 2) continue; /* over-long row: already drained */
        if (filter && !contains_ci(line, filter)) continue;
        /* display the real path, not the escaped field */
        char *tab = strchr(line, '\t');
        if (v2 && tab) {
            char path[2 * LV_MAX_PATH];
            unesc_field(path, sizeof path, line, (size_t)(tab - line));
            /* two fputs: snprintf'ing into line while tab points inside
               it overlaps in/out and is formally undefined */
            fputs(path, out);
            fputs(tab, out);
        } else fputs(line, out);
        shown++;
    }
    fclose(f);
    if (!shown) fprintf(out, "(no matches)\n");
    return 0;
}

static int cmd_play(const char *query, FILE *out) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *f = fopen(idx, "r");
    if (!f) { fprintf(out, "levari: library empty\n"); return 1; }
    char line[2 * LV_MAX_PATH + 1024];
    int v2 = lib_idx_header(f);
    char cmd[8192] = "play";
    int n = 0;
    int gr;
    while ((gr = lib_idx_getline(f, line, sizeof line)) != 0) {
        if (gr == 2) continue; /* over-long row: already drained */
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (query && *query && !contains_ci(line, query)) continue;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (v2) { size_t sl = strlen(line); unesc_field(line, sl + 1, line, sl); } /* queue real paths */
        /* cli_append_arg quotes (and escapes embedded quotes in) paths
           that need it: the play tokenizer splits bare args on spaces */
        if (cli_append_arg(cmd, sizeof cmd - 1, line) != 0) break;
        n++;
    }
    fclose(f);
    if (!n) { fprintf(out, "levari: no matches\n"); return 1; }
    char resp[8192];
    int rc = cli_request(cmd, resp, sizeof resp);
    if (rc == 2) { fprintf(out, "levari: no running instance\n"); return 2; }
    fputs(resp, out);
    return rc;
}

int lib_run(const char *sub, const char *arg, FILE *out) {
    if (!sub || !*sub) { fprintf(out, "usage: levari library scan|list|play [arg]\n"); return 1; }
    if (!strcmp(sub, "scan")) {
        if (arg && *arg) return cmd_scan(arg, out);
        if (cfg.lib_dir_n) {
            for (int i = 0; i < cfg.lib_dir_n; i++) cmd_scan(cfg.lib_dirs[i], out);
            return 0;
        }
        fprintf(out, "usage: levari library scan <dir>\n");
        return 1;
    }
    if (!strcmp(sub, "list")) return cmd_list(arg, out);
    if (!strcmp(sub, "play")) return cmd_play(arg, out);
    fprintf(out, "usage: levari library scan [dir] | list [filter] | play [query]\n");
    return 1;
}

int lib_cmd(int argc, char **argv) {
    if (argc < 2) { printf("usage: levari library scan|list|play [arg]\n"); return 1; }
    return lib_run(argv[1], argc > 2 ? argv[2] : NULL, stdout);
}
