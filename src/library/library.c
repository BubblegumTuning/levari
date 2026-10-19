#include "library.h"
#include "../tag/tag.h"
#include "../decode/decode.h"
#include "../daemon/daemon.h"
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

void lib_index_path(char *out, size_t n) {
    if (cfg.lib_index[0]) {
        lv_expand_home(cfg.lib_index, out, n);
        char dir[LV_MAX_PATH];
        lv_strlcpy(dir, out, sizeof dir);
        char *slash = strrchr(dir, '/');
        if (!slash) slash = strrchr(dir, '\\');
        if (slash) { *slash = 0; mkdir(dir, 0755); }
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
           !lv_strcasecmp(e, ".it");
}

typedef struct {
    FILE *out;
    char **sorted; /* existing paths, sorted; scan skips anything already indexed */
    int n;
    int added;
    int seen;
    int tags; /* 0 = index filenames only (skip tag + decoder reads) */
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
            scan_dir(full, c);
        } else if (is_audio(de->d_name)) {
            c->seen++;
            if (known_path(c, full)) continue;
            Tags t;
            tag_init(&t);
            int dur = 0;
            if (c->tags) {
                tag_read(full, &t);
                Decoder *dec = dec_open(full);
                if (dec) {
                    long long tot = dec_total_frames(dec);
                    if (tot > 0) dur = (int)(tot / dec_rate(dec));
                    dec_close(dec);
                }
            }
            fprintf(c->out, "%s\t%s\t%s\t%s\t%d\t%d\n", full, t.artist, t.title, t.album, dur, t.track);
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
    int added = 0, seen = 0;
    if (lib_scan_add(dirs, 1, 1, &added, &seen) != 0) {
        char idx[LV_MAX_PATH];
        lib_index_path(idx, sizeof idx);
        fprintf(out, "levari: cannot write %s\n", idx);
        return 1;
    }
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    fprintf(out, "scanned %d files into %s\n", seen, idx);
    return 0;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int lib_scan_add(const char dirs[][LV_MAX_PATH], int n, int with_tags, int *added, int *seen) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    char **sorted = NULL;
    int sn = 0, cap = 0;
    FILE *r = fopen(idx, "r");
    if (r) {
        char line[8192];
        while (fgets(line, sizeof line, r)) {
            char *tab = strchr(line, '\t');
            if (tab) *tab = 0;
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!line[0]) continue;
            if (sn == cap) {
                cap = cap ? cap * 2 : 256;
                char **s2 = realloc(sorted, (size_t)cap * sizeof *sorted);
                if (!s2) break;
                sorted = s2;
            }
            sorted[sn++] = strdup(line);
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
    ScanCtx c = { f, sorted, sn, 0, 0, with_tags };
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
    return 0;
}

int lib_foreach_entry(void (*fn)(const LibEntry *e, void *ud), void *ud) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *f = fopen(idx, "r");
    if (!f) return -1;
    char line[8192];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p1 = line;
        char *p2 = strchr(p1, '\t'); if (!p2) continue; *p2++ = 0;
        char *p3 = strchr(p2, '\t'); if (!p3) continue; *p3++ = 0;
        char *p4 = strchr(p3, '\t'); if (!p4) continue; *p4++ = 0;
        char *p5 = strchr(p4, '\t');
        int dur = 0, track = 0;
        if (p5) {
            *p5++ = 0;
            char *p6 = strchr(p5, '\t');
            if (p6) { *p6++ = 0; dur = atoi(p5); track = atoi(p6); }
            else dur = atoi(p5);
        }
        LibEntry e = { p1, p2, p3, p4, dur, track };
        fn(&e, ud);
        n++;
    }
    fclose(f);
    return n;
}

static int cmd_list(const char *filter, FILE *out) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *f = fopen(idx, "r");
    if (!f) { fprintf(out, "(library empty)\n"); return 0; }
    char line[4096];
    int shown = 0;
    while (fgets(line, sizeof line, f)) {
        if (filter && !contains_ci(line, filter)) continue;
        fputs(line, out);
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
    char line[4096];
    char cmd[8192] = "play";
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (query && *query && !contains_ci(line, query)) continue;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (strlen(cmd) + strlen(line) + 2 > sizeof cmd) break;
        strcat(cmd, " ");
        strcat(cmd, line);
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
