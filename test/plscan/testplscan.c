/* unit test: the playlist SCAN button (action(PB_SCAN)) end to end —
   re-probe updates the Track AND the index (decision A append-if-absent),
   missing file -> red title-strip message + stdout, CUE virtual row and
   select-all state are guarded no-ops, background scan -> refusal message,
   and a real title change preempts the message (refresh_title).
   Technique: #include ui.c (see testoptenc for the rationale); run with a
   scratch HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR set by the caller.
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/tps \
       test/plscan/testplscan.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name dispatch.c.o) \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static Player fake_pl;
static Playlist pl;
static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

/* id3v2.3 TIT2-only fixture (same shape as testoptenc/testrescan) */
static void write_tagged(const char *path, const char *title) {
    FILE *f = fopen(path, "wb");
    int tl = (int)strlen(title), fs = tl + 1, ts = 10 + fs;
    unsigned char h[10] = { 'I','D','3',3,0,0,(unsigned)(ts>>21),(unsigned)(ts>>14),
                            (unsigned)(ts>>7),(unsigned)ts };
    fwrite(h, 1, 10, f);
    fwrite("TIT2", 1, 4, f);
    unsigned char g[4] = { (unsigned)(fs>>24),(unsigned)(fs>>16),(unsigned)(fs>>8),(unsigned)fs };
    fwrite(g, 1, 4, f);
    fputc(0, f); fputc(0, f); fputc(0, f);
    fwrite(title, 1, tl, f);
    fclose(f);
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !getenv("XDG_CONFIG_HOME")) {
        printf("caller must set scratch HOME/XDG_CONFIG_HOME\n");
        return 1;
    }
    cfg_load();
    pthread_mutex_init(&fake_pl.mtx, NULL);
    pl_init(&pl);
    fake_pl.pl = &pl;
    fake_pl.cur = -1;
    ui.pl = &fake_pl;

    char one[LV_MAX_PATH];
    snprintf(one, sizeof one, "%s/scanme.mp3", home);
    write_tagged(one, "ScanMe");
    pl_add_file(&pl, one);
    ui.pl_sel = 0;

    /* happy path: Track updated + index row appended (no index existed) */
    action(PB_SCAN);
    chk("track title refreshed", strcmp(pl.items[0].tags.title, "ScanMe") == 0, 1);
    chk("no error message on success", ui.msg[0] == 0, 1);
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *r = fopen(idx, "r");
    chk("index row created (decision A)", r != NULL, 1);
    if (r) {
        char b[8192];
        size_t n = fread(b, 1, sizeof b - 1, r); b[n] = 0;
        fclose(r);
        chk("index row carries the title", strstr(b, "ScanMe") != NULL, 1);
    }

    /* CUE virtual row: end_sec set -> deliberate no-op, no message */
    ui.msg[0] = 0;
    pl.items[0].end_sec = 100;
    action(PB_SCAN);
    chk("virtual row: no message, untouched", ui.msg[0] == 0, 1);
    pl.items[0].end_sec = 0;

    /* select-all state: no-op (multi-select comes later) */
    ui.pl_selall = 1;
    action(PB_SCAN);
    chk("select-all is a no-op", ui.msg[0] == 0, 1);
    ui.pl_selall = 0;

    /* background scan running: refusal message, index not touched again */
    ui.opt_scanning = 1;
    action(PB_SCAN);
    chk("scan-running refusal message", strstr(ui.msg, "scan running") != NULL, 1);
    ui.opt_scanning = 0;

    /* decision F: missing file -> stdout + red message, no crash */
    char gone[LV_MAX_PATH];
    snprintf(gone, sizeof gone, "%s/gone.mp3", home);
    pl_add_file(&pl, gone);
    ui.pl_sel = 1;
    ui.msg[0] = 0;
    action(PB_SCAN);
    chk("missing file -> red message", strstr(ui.msg, "cannot rescan") != NULL, 1);

    /* preemption: a real title change clears the message */
    fake_pl.cur = 0;
    refresh_title();          /* ui.title "" -> "  ScanMe": clears the message */
    chk("title change preempts message", ui.msg[0] == 0, 1);

    printf(fails ? "FAILURES: %d\n" : "all SCAN button checks pass\n", fails);
    return fails != 0;
}
