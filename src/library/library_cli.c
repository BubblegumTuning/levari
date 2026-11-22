/* library CLI layer: the `levari library ...` subcommands and the MCP
   library tool entry (lib_run). Deliberately OUTSIDE library.c: this is
   the only part of the library module that talks to a running instance
   (cli_request/cli_append_arg), and library.c must not know the daemon
   exists. library.c owns the index; this file drives it for a terminal
   (or a stdio MCP server) and forwards `play` matches to the live UI. */
#include "library.h"
#include "../daemon/daemon.h"
#include "../config/config.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../platform/platform.h"   /* realpath shim */
#else
#include <unistd.h>
#endif

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
        if (filter && !lv_stristr(line, filter)) continue;
        /* display the real path, not the escaped field */
        char *tab = strchr(line, '\t');
        if (v2 && tab) {
            char path[2 * LV_MAX_PATH];
            lib_unesc_field(path, sizeof path, line, (size_t)(tab - line));
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
        if (query && *query && !lv_stristr(line, query)) continue;
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (v2) { size_t sl = strlen(line); lib_unesc_field(line, sl + 1, line, sl); } /* queue real paths */
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
    if (!sub || !*sub) { fprintf(out, "usage: levari library scan|list|play|verify [arg]\n"); return 1; }
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
    if (!strcmp(sub, "verify") && !arg) {
        int kept = 0, purged = 0, unreach = 0;
        char bank[LV_MAX_PATH] = "";
        int rc = lib_index_verify(&kept, &purged, &unreach, bank, sizeof bank);
        if (rc == -3) { fprintf(out, "levari: index pre-v2 and ambiguous: run a full scan\n"); return 1; }
        if (rc != 0) { fprintf(out, "levari: verify failed (index untouched)\n"); return 1; }
        fprintf(out, "verify: kept %d, dropped %d, unreachable kept %d",
                kept, purged, unreach);
        if (purged && bank[0]) fprintf(out, " (banked in %s)", bank);
        fprintf(out, "\n");
        return 0;
    }
    fprintf(out, "usage: levari library scan [dir] | list [filter] | play [query] | verify\n");
    return 1;
}

int lib_cmd(int argc, char **argv) {
    if (argc < 2) { printf("usage: levari library scan|list|play|verify [arg]\n"); return 1; }
    return lib_run(argv[1], argc > 2 ? argv[2] : NULL, stdout);
}
