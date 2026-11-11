/* unit test: duration clamping in the playlist parsers (playlist.c).
   EXTINF and PLS LengthN are raw atoi off untrusted files; two rows of
   2000000000 s used to poison the footer sum (int wrap, footer read
   0:00). Both parsers now gate every value through the LV_DUR_MAX
   hazard ceiling (tag.h): out of range means 0 = unknown, the row is
   kept, the scan backfill supplies the real length later. Run with
   argv[1] = a fresh scratch dir for the fixtures. Technique (see skill):
   #include playlist.c, link every object EXCEPT main.c.o, playlist.c.o,
   decode.c.o and stb_vorbis.c.o (dispatch.c.o MUST stay; the two
   excluded TUs are pre-split leftovers that collide with the dec_*.dir
   tier objects):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/tag $SDL -o /tmp/td \
       test/durclamp/testdurclamp.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name playlist.c.o ! -name decode.c.o ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
*/
#include "../../src/playlist/playlist.c"
#include <assert.h>

static Playlist pl;
static int fails;
static char dirbuf[LV_MAX_PATH];

static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void chkf(const char *what, double got, double want) {
    double d = got - want; if (d < 0) d = -d;
    if (d > 0.001) { fails++; printf("FAIL %s: got %.4f want %.4f\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void wfixture(const char *name, const char *body) {
    char path[LV_MAX_PATH];
    snprintf(path, sizeof path, "%s/%s", dirbuf, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(body, f);
    fclose(f);
}

static void fxpath(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", dirbuf, name);
}

static int idx_of(const char *name) {   /* find a fixture row by path */
    char want[LV_MAX_PATH];
    fxpath(name, want, sizeof want);
    for (int i = 0; i < pl.count; i++)
        if (!strcmp(pl.items[i].path, want)) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: testdurclamp <scratch-dir>\n"); return 1; }
    lv_strlcpy(dirbuf, argv[1], sizeof dirbuf);

    /* m3u: hostile, boundary, and legitimate EXTINF values on one file.
       Relative rows join the fixture dir — that is also what the join
       path itself gets exercised on */
    wfixture("clamp.m3u",
        "#EXTM3U\n"
        "#EXTINF:2000000000,evil\nevil1.mp3\n"
        "#EXTINF:-5,neg\nevil2.mp3\n"
        "#EXTINF:0,zero\nzero.mp3\n"
        "#EXTINF:86401,barely over\nover.mp3\n"
        "#EXTINF:86400,exact ceiling\ncap.mp3\n"
        "#EXTINF:61,ok\nok.mp3\n");
    char f[LV_MAX_PATH];
    fxpath("clamp.m3u", f, sizeof f);
    chk("m3u load rc", pl_load_m3u(&pl, f), 0);
    chk("m3u row count kept (clamp is per-field, never a drop)", pl.count, 6);
    chk("m3u EXTINF 2000000000 -> 0", pl.items[idx_of("evil1.mp3")].duration, 0);
    chk("m3u EXTINF -5 -> 0",      pl.items[idx_of("evil2.mp3")].duration, 0);
    chk("m3u EXTINF 0 -> 0",       pl.items[idx_of("zero.mp3")].duration, 0);
    chk("m3u EXTINF 86401 -> 0",   pl.items[idx_of("over.mp3")].duration, 0);
    chk("m3u EXTINF 86400 kept",   pl.items[idx_of("cap.mp3")].duration, LV_DUR_MAX);
    chk("m3u EXTINF 61 kept",      pl.items[idx_of("ok.mp3")].duration, 61);
    pl_clear(&pl);

    /* pls: LengthN hostile both ways, legitimate kept, keys before FileN */
    wfixture("clamp.pls",
        "[playlist]\n"
        "NumberOfEntries=3\n"
        "Length1=2147483647\n"
        "File1=e1.mp3\n"
        "Length2=999999999999999999999\n"   /* past INT_MAX: saturates, then clamps */
        "Title2=junk\n"
        "File2=e2.mp3\n"
        "File3=o.mp3\n"
        "Length3=120\n");
    fxpath("clamp.pls", f, sizeof f);
    chk("pls load rc", pl_load_pls(&pl, f), 0);
    chk("pls row count kept", pl.count, 3);
    chk("pls Length 2147483647 -> 0", pl.items[idx_of("e1.mp3")].duration, 0);
    chk("pls Length overflow -> 0",   pl.items[idx_of("e2.mp3")].duration, 0);
    chk("pls Length 120 kept",        pl.items[idx_of("o.mp3")].duration, 120);
    pl_clear(&pl);

    /* cue: INDEX 01 is MM:SS:FF at 75 frames/s (00:01:00 = ONE second).
       The old parse was int math: mm*60 overflowed on 99999999:00:00,
       UB wrapping into a negative start_sec. Absurd INDEX rows must be
       rejected with the row kept, like every other hazard field. */
    wfixture("clamp.cue",
        "FILE \"a.wav\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 00:01:00\n"          /* 1.000 s */
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:03:00\n"          /* 3.000 s: closes track 1 */
        "  TRACK 03 AUDIO\n"
        "    INDEX 01 99999999:00:00\n");  /* old int math UB */
    fxpath("clamp.cue", f, sizeof f);
    chk("cue load rc", pl_load_cue(&pl, f), 0);
    chk("cue row count kept", pl.count, 3);
    chkf("cue t1 start = 1 s",   pl.items[0].start_sec, 1.0);
    chkf("cue t1 end = 3 s",     pl.items[0].end_sec, 3.0);
    chk("cue t1 duration",       pl.items[0].duration, 2);
    chk("cue hostile INDEX rejected (t2 left unclosed)",
        pl.items[1].end_sec <= 0, 1);
    chk("cue t3 start untouched", pl.items[2].start_sec <= 0, 1);
    pl_clear(&pl);

    wfixture("clamp2.cue",
        "FILE \"b.wav\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 999999:00:00\n"      /* 59,999,940 s: no UB, but
                                              past the one-day ceiling */
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 -1:00:00\n");        /* negative */
    fxpath("clamp2.cue", f, sizeof f);
    chk("cue2 load rc", pl_load_cue(&pl, f), 0);
    chk("cue2 row count kept", pl.count, 2);
    chk("cue2 over-ceiling INDEX rejected", pl.items[0].start_sec <= 0, 1);
    chk("cue2 negative INDEX rejected",     pl.items[1].start_sec <= 0, 1);
    pl_clear(&pl);

    printf(fails ? "FAILURES: %d\n" : "ALL PASS (%d failures)\n", fails);
    return fails != 0;
}
