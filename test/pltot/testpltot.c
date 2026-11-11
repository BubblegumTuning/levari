/* unit test: the playlist footer total (fmt_pl_total in ui.c). Durations
   arrive from untrusted files (EXTINF/LengthN/index rows are raw atoi),
   so the sum must neither wrap nor print an unbounded readout: the old
   int accumulator overflowed on two 2000000000-s rows and the footer
   read 0:00. fmt_pl_total sums in long long and saturates at
   9999:59:59+. Technique (see skill): #include ui.c, link every object
   EXCEPT main.c.o, ui.c.o, decode.c.o and stb_vorbis.c.o (dispatch.c.o
   MUST stay; the two excluded TUs are pre-split leftovers that collide
   with the dec_*.dir tier objects):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/tp \
       test/pltot/testpltot.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
   Run with scratch HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR set by the caller.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static Track rows[8];
static Playlist pl;
static int fails;

static void tot(const char *what, int n, const char *want) {
    char out[32];
    pl.count = n;
    fmt_pl_total(out, sizeof out, &pl);
    if (strcmp(out, want) != 0) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, out, want); }
    else printf("PASS %s\n", what);
}

int main(void) {
    memset(rows, 0, sizeof rows);
    pl.items = rows;

    tot("empty", 0, "0:00");

    rows[0].duration = 61;
    tot("one minute", 1, "1:01");

    rows[0].duration = 3599;
    tot("sub-hour", 1, "59:59");

    rows[0].duration = 3661;
    tot("just over hour", 1, "1:01:01");

    rows[0].duration = 2000000000;
    tot("single INT_MAX-ish row stays bounded", 1, "9999:59:59+");

    rows[1].duration = 2000000000;
    tot("old overflow pair", 2, "9999:59:59+");

    rows[0].duration = 2147483647;
    rows[1].duration = 1;
    tot("INT_MAX plus one", 2, "9999:59:59+");

    rows[0].duration = PL_TOTAL_CAP;
    tot("exact cap", 1, "9999:59:59");

    rows[0].duration = PL_TOTAL_CAP + 1;   /* a single absurd row also saturates */
    tot("one over cap", 1, "9999:59:59+");

    rows[0].duration = -294967296;         /* negative cast-overflow garbage */
    rows[1].duration = 60;
    tot("negative skipped", 2, "1:00");

    rows[0].duration = 0;
    rows[1].duration = 0;
    tot("unknowns zero", 2, "0:00");

    printf(fails ? "FAILURES: %d\n" : "ALL PASS (%d failures)\n", fails);
    return fails != 0;
}
