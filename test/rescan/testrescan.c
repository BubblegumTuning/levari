/* unit test: lib_index_rescan — single-file index row update (playlist
   SCAN button backend). Pins: replace-in-place (row count invariant),
   append-if-absent (decision A), notag certification + clearance, duration
   preservation when the probe goes blind, missing file -> rc -1 with the
   index file byte-identical (decision F: index untouched), escaped-path
   rows replace-not-shadow (backslash filename).
   Compile (from the repo root, after `cmake --build build`); run with a
   scratch HOME/XDG_CONFIG_HOME set by the caller. LINK CAVEAT: this test
   exercises the scan/probe path, so it must NOT pull the stale pre-split
   decode.c.o leftovers the host build/ dir carries (they predate the
   ce76e8b division fixes and SIGFPE on a frameless mp3): exclude them and
   link the dispatch + tier objects instead, as a canonical fresh tree has:

     gcc -std=gnu11 -Isrc -Isrc/library -Isrc/config -Isrc/tag test/rescan/testrescan.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' ! -name main.c.o ! -name decode.c.o ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       $(pkg-config --libs sdl2) -lz -lm -lpthread -ldl -lstdc++ -o build/testrescan
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "library.h"
#include "config.h"
#include "tag.h"
#include "common.h"

static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chks(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

/* id3v2.3 TIT2-only fixture (plain-binary tag size, frame size incl. the
   enc byte — same shape as testoptenc's cyr fixture) */
static void write_tagged(const char *path, const char *title) {
    FILE *f = fopen(path, "wb");
    int tl = (int)strlen(title);
    int fs = tl + 1, ts = 10 + fs;
    unsigned char h[10] = { 'I','D','3',3,0,0, (unsigned)(ts>>21),(unsigned)(ts>>14),
                            (unsigned)(ts>>7), (unsigned)ts };
    fwrite(h, 1, 10, f);
    fwrite("TIT2", 1, 4, f);
    unsigned char g[4] = { (unsigned)(fs>>24),(unsigned)(fs>>16),(unsigned)(fs>>8),(unsigned)fs };
    fwrite(g, 1, 4, f);
    fputc(0, f); fputc(0, f);
    fputc(0, f);                        /* enc = latin-1 */
    fwrite(title, 1, tl, f);
    fclose(f);
}
static void write_junk(const char *path) {
    FILE *f = fopen(path, "wb");
    unsigned char z[16] = {0};
    fwrite(z, 1, sizeof z, f);
    fclose(f);
}

static long slurp(const char *p, char *buf, size_t n) {
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    long len = (long)fread(buf, 1, n - 1, f);
    buf[len < 0 ? 0 : len] = 0;
    fclose(f);
    return len;
}
static int row_count(const char *idx) {
    char b[1 << 17];
    if (slurp(idx, b, sizeof b) < 0) return -1;
    int n = 0;
    for (char *s = b; *s; ) {
        char *nl = strchr(s, '\n');
        if (!nl) nl = s + strlen(s);
        if (nl > s && strncmp(s, "#levari", 7)) n++;
        s = *nl ? nl + 1 : nl;
    }
    return n;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !getenv("XDG_CONFIG_HOME")) {
        printf("caller must set scratch HOME/XDG_CONFIG_HOME\n");
        return 1;
    }
    cfg_load();
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);

    char mus[LV_MAX_PATH], one[LV_MAX_PATH], two[LV_MAX_PATH], ext[LV_MAX_PATH],
         bsp[LV_MAX_PATH];
    snprintf(mus, sizeof mus, "%s/music", home);
    snprintf(one, sizeof one, "%s/one.mp3", mus);
    snprintf(two, sizeof two, "%s/two.mp3", mus);
    snprintf(ext, sizeof ext, "%s/extra.mp3", home);
    snprintf(bsp, sizeof bsp, "%s/back\\\\slash.mp3", home);
    mkdir(mus, 0755);

    /* fixture: one tagged (probe blind: id3-only -> dur 0), one untagged */
    write_tagged(one, "First");
    write_junk(two);

    char dirs[1][LV_MAX_PATH];
    lv_strlcpy(dirs[0], mus, sizeof dirs[0]);
    int added = 0, seen = 0, skipped = 0;
    chk("scan adds both fixtures", lib_scan_add(dirs, 1, 1, &added, &seen, &skipped), 0);
    chk("scan row count", row_count(idx), 2);
    char b[1 << 17];
    slurp(idx, b, sizeof b);
    chk("scan certified the untagged row", strstr(b, "\t1\n") != NULL, 1);

    /* 1) replace in place: title changes, row count invariant */
    write_tagged(one, "Second");
    Tags t;
    int dur = 0, notag = 1;
    chk("rescan ok", lib_index_rescan(one, &t, &dur, &notag), 0);
    chks("rescan probed the new title", t.title, "Second");
    chk("row count after replace", row_count(idx), 2);
    slurp(idx, b, sizeof b);
    chk("index carries the new title", strstr(b, "Second") != NULL, 1);
    chk("index dropped the old title", strstr(b, "First") == NULL, 1);

    /* 2) duration preservation: a probe that yields 0 keeps the stored dur */
    long len = slurp(idx, b, sizeof b);
    char *row1 = strstr(b, "one.mp3");
    char *p5 = row1 ? strstr(row1, "\t0\t0\n") : NULL;
    if (!p5) { fails++; printf("FAIL dur inject: no \\t0\\t0 row for one.mp3\n"); }
    else { memcpy(p5 + 1, "123", 3);
           FILE *w = fopen(idx, "w"); fwrite(b, 1, (size_t)len, w); fclose(w); }
    chk("rescan ok (dur pin)", lib_index_rescan(one, &t, &dur, &notag), 0);
    chk("probe-blind dur preserved", dur, 123);
    slurp(idx, b, sizeof b);
    chk("index row kept dur 123", strstr(b, "\t123\t") != NULL, 1);

    /* 3) notag certification then clearance */
    chk("rescan untagged ok", lib_index_rescan(two, &t, &dur, &notag), 0);
    chk("rescan certifies notag", notag, 1);
    write_tagged(two, "Found");
    chk("rescan tagged-now ok", lib_index_rescan(two, &t, &dur, &notag), 0);
    chk("rescan clears notag", notag, 0);
    slurp(idx, b, sizeof b);
    char *crow = strstr(b, "two.mp3");
    chk("cleared row has no marker",
        crow && !strncmp(crow, "two.mp3\t\tFound\t\t0\t0\n", 20), 1);

    /* 4) decision A: absent file -> row appended */
    write_tagged(ext, "Outside");
    chk("rescan absent ok", lib_index_rescan(ext, &t, &dur, &notag), 0);
    chk("row appended", row_count(idx), 3);

    /* 5) decision F: missing file -> -1, index byte-identical */
    char before[1 << 17];
    len = slurp(idx, b, sizeof b);
    memcpy(before, b, (size_t)len + 1);
    char gone[LV_MAX_PATH];
    snprintf(gone, sizeof gone, "%s/gone.mp3", home);
    chk("missing file returns -1", lib_index_rescan(gone, &t, &dur, &notag), -1);
    len = slurp(idx, b, sizeof b);
    chk("index byte-identical after -1", strcmp(before, b) == 0 && len == (long)strlen(before), 1);

    /* 6) escaped paths replace-not-shadow: rescan a backslash filename twice */
    write_tagged(bsp, "B1");
    chk("rescan backslash path ok", lib_index_rescan(bsp, &t, &dur, &notag), 0);
    int after_first = row_count(idx);
    write_tagged(bsp, "B2");
    chk("rescan backslash twice ok", lib_index_rescan(bsp, &t, &dur, &notag), 0);
    chk("escaped row replaced not shadowed", row_count(idx), after_first);
    slurp(idx, b, sizeof b);
    chk("escaped row carries B2", strstr(b, "B2") != NULL, 1);

    /* header marker must survive every rewrite (b6cb2ea) */
    slurp(idx, b, sizeof b);
    chk("header preserved", strncmp(b, "#levari-index v2 ", 17) == 0, 1);

    printf(fails ? "FAILURES: %d\n" : "all rescan checks pass\n", fails);
    return fails != 0;
}
