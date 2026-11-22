/* unit test: the playlist footer total (fmt_pl_total in ui.c). Durations
   arrive from untrusted files (EXTINF/LengthN/index rows are raw atoi),
   so the sum must neither wrap nor print an unbounded readout: the old
   int accumulator overflowed on two 2000000000-s rows and the footer
   read 0:00. fmt_pl_total sums in long long and saturates at
   9999:59:59+. Technique: #include %s. Build, link and run are CTest targets now
   (refactor item 3 — the find-glob recipes died with levari.dir's
   objects): cmake --build build && ctest --test-dir build -R '^testpltot$'
   Link = filtered levari_ui archive minus ui_pl.c.o + levari_engine
   (pulls levari_core) + the dec tier/ffab objects + vendored libs.
   tools/ctest_home.sh runs every case in a fresh scratch dir (the
   name contains 'scratch'): HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR
   there, audio.backend=null, /tmp/scratch-p2 present, cwd inside,
   SDL_VIDEODRIVER=dummy.
*/
#include "../../src/ui/ui_pl.c"
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
