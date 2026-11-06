/* unit test: playlist edit engine (playlist editing depth P0).
   Pins the mutating primitives in src/playlist/playlist.c headlessly:
   sort/reverse/shuffle as permutations with p->cur landing on the SAME
   MUSIC (never the same index), CUE groups atomic in every operation,
   crop == multi-REM engine parity, cut/paste with the internal clipboard,
   move at the boundaries / into-group / over-group, pl_drop gap-index
   drops (P4 row drag: sel+cur remap, self-drop identity, non-adjacent
   set merges, group snap-forward + whole-unit atomicity), pl_insert_track
   full row copy, the shared extension classifier, and the .pls parser against
   hand-built byte-forward fixtures (NumberOfEntries mismatch, relative +
   absolute FileN, Windows backslash paths, CRLF, UTF-8 labels — expected
   UTF-8 literals generated from the codec, garbage keys ignored).
   Compile playlist.c + tag.c from source directly — no ui.c, no build
   objects, so a stale build/ tree can never masquerade as the fix:
     gcc -std=gnu11 -Isrc -o build/testplaylistedit \
       test/playlistedit/testplaylistedit.c \
       src/playlist/playlist.c src/tag/tag.c -lpthread -lm
*/
#include "playlist/playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chks(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static Playlist pl;
static int cur; /* the simulated Player.cur for these primitive calls */

/* absolute paths keep pl_add_file from cwd-absolutizing the pins */
static void addrow(const char *path, const char *artist, const char *title,
                   const char *album, int track, double ss, double es, int dur) {
    pl_add_file(&pl, path);
    Track *t = &pl.items[pl.count - 1];
    lv_strlcpy(t->tags.artist, artist, sizeof t->tags.artist);
    lv_strlcpy(t->tags.title, title, sizeof t->tags.title);
    lv_strlcpy(t->tags.album, album, sizeof t->tags.album);
    t->tags.track = track;
    t->start_sec = ss; t->end_sec = es;
    t->duration = dur;
}
static void expect_paths(const char *const *want, int n) {
    chk("row count", pl.count, n);
    for (int i = 0; i < n && i < pl.count; i++) {
        char w[64]; snprintf(w, sizeof w, "row %d", i);
        chks(w, pl.items[i].path, want[i]);
    }
}
static void reset(void) { pl_clear(&pl); cur = -1; }
static void expect_cur(const char *what, const char *path) {
    /* cur must land on the SAME MUSIC — compare path (and CUE bounds
       via start_sec, since a group shares one path) */
    if (cur < 0 || cur >= pl.count) { fails++; printf("FAIL %s: cur out of range %d\n", what, cur); return; }
    chks(what, pl.items[cur].path, path);
}

/* ---------- fixtures ---------- */
static char tmpdir[] = "/tmp/lvpleditXXXXXX";
static void write_file(const char *name, const unsigned char *bytes, size_t n) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", tmpdir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { printf("FATAL fixture %s\n", name); exit(2); }
    fwrite(bytes, 1, n, f);
    fclose(f);
}
static void load_pls(const char *name) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", tmpdir, name);
    chk("pl_load_pls open", pl_load_pls(&pl, p), 0);
}

int main(void) {
    if (!mkdtemp(tmpdir)) return 2;

    /* ---------- pl_ext_kind ---------- */
    chk("kind m3u", pl_ext_kind("a/b/c.M3U"), PL_M3U);
    chk("kind m3u8", pl_ext_kind("x.m3u8"), PL_M3U);
    chk("kind pls", pl_ext_kind("x.PLs"), PL_PLS);
    chk("kind cue", pl_ext_kind("dir/song.CUE"), PL_CUE);
    chk("kind none ext", pl_ext_kind("track.tar.xz"), PL_NONE);
    chk("kind no dot", pl_ext_kind("plainfile"), PL_NONE);
    chk("kind mp3", pl_ext_kind("a.mp3"), PL_NONE);

    /* ---------- .pls parser (byte-forward fixture) ----------
       expected UTF-8 title bytes generated from the codec:
       'Sóng Một' = 53 C3 B3 6E 67 20 4D E1 BB 99 74 */
    static const unsigned char pls[] =
        "[playlist]\r\n"
        "NumberOfEntries=99\r\n"                     /* mismatch: ignored, every FileN loads */
        "File1=rel/song1.mp3\r\n"
        "Title1=S\xC3\xB3ng M\xE1\xBB\x99t\r\n"
        "File2=/abs/song2.mp3\r\n"
        "Label2=Label Two\r\n"                        /* LabelN accepted */
        "Length2=123\r\n"
        "length3=-1\r\n"                              /* lowercase key; -1 = unknown */
        "File3=song3.mp3\r\n"
        "Garbage5=ignored\r\n"
        "File0=bad-index\r\n"                         /* 1-based: index 0 ignored */
        "FileX=bad\r\n"                               /* non-numeric index ignored */
        "File4=win\\path\\song4.mp3\r\n"              /* backslash = ordinary char on POSIX: joins */
        "$Version=M3U\r\n"
        "File6=no5.mp3\r\n";                          /* gap: File5 missing, 6 still loads */
    write_file("t.pls", pls, sizeof pls - 1);
    reset();
    load_pls("t.pls");
    chk("pls row count", pl.count, 5);               /* FileN mismatch/garbage keys ignored */
    if (pl.count == 5) {
        char joined[LV_MAX_PATH];
        snprintf(joined, sizeof joined, "%s/rel/song1.mp3", tmpdir);
        chks("pls rel join", pl.items[0].path, joined);
        chks("pls UTF-8 label", pl.items[0].tags.title, "S\xC3\xB3ng M\xE1\xBB\x99t");
        chks("pls abs", pl.items[1].path, "/abs/song2.mp3");
        chk("pls Length", pl.items[1].duration, 123);
        chks("pls LabelN", pl.items[1].tags.title, "Label Two");
        chk("pls Length -1 -> 0", pl.items[2].duration, 0);
        snprintf(joined, sizeof joined, "%s/win\\path\\song4.mp3", tmpdir);
        chks("pls backslash join", pl.items[3].path, joined);
        snprintf(joined, sizeof joined, "%s/no5.mp3", tmpdir);
        chks("pls index gap", pl.items[4].path, joined);
    }
    /* CRLF-less file + trailing FileN after TitleN (order flexibility) */
    static const unsigned char pls2[] =
        "[playlist]\n"
        "File1=a.mp3\n"
        "Title1=After\n"                              /* TitleN AFTER FileN */
        "File2=b.mp3\n"
        "TitleN=bad key\n";                           /* non-numeric ignored */
    write_file("t2.pls", pls2, sizeof pls2 - 1);
    reset();
    load_pls("t2.pls");
    chk("pls2 rows", pl.count, 2);
    if (pl.count == 2) chks("pls2 title after file", pl.items[0].tags.title, "After");
    /* missing file */
    chk("pls missing", pl_load_pls(&pl, "/nonexistent-dir/lv.pls"), -1);
    /* empty pls loads zero rows without error */
    write_file("empty.pls", (const unsigned char *)"[playlist]\n", 11);
    reset();
    load_pls("empty.pls");
    chk("pls empty", pl.count, 0);

    /* ---------- pl_group_span / two plain copies of one file ---------- */
    reset();
    addrow("/m/x.flac", "", "solo", "", 0, 0, 0, 10);
    addrow("/m/x.flac", "", "solo", "", 0, 0, 0, 10);   /* plain duplicate: NOT a group */
    {
        int a, b;
        pl_group_span(&pl, 0, &a, &b); chk("span plain a", a, 0); chk("span plain b", b, 1);
        pl_group_span(&pl, 1, &a, &b); chk("span plain2 a", a, 1); chk("span plain2 b", b, 2);
    }
    reset();
    addrow("/m/alb.flac", "A", "Tr1", "AL", 1, 0, 100, 100);
    addrow("/m/alb.flac", "A", "Tr2", "AL", 2, 100, 200, 100);
    addrow("/m/alb.flac", "A", "Tr3", "AL", 3, 200, 0, 99);
    {
        int a, b;
        pl_group_span(&pl, 1, &a, &b); chk("span cue a", a, 0); chk("span cue b", b, 3);
    }

    /* ---------- sort ---------- */
    reset();
    addrow("/s/b.mp3", "zeta",  "bbb", "B", 2, 0, 0, 1);
    addrow("/s/a.mp3", "alpha", "aaa", "A", 9, 0, 0, 2);
    addrow("/s/c.mp3", "beta",  "",    "", 1, 0, 0, 3);
    addrow("/s/d.mp3", "alpha", "aaa", "A", 0, 0, 0, 4);   /* tie with row 1: stable */
    cur = 0;
    pl_sort(&pl, PL_SORT_TITLE, &cur);
    {
        const char *want[] = { "/s/a.mp3", "/s/d.mp3", "/s/b.mp3", "/s/c.mp3" };
        expect_paths(want, 4);                          /* empties last, ties stable */
        expect_cur("sort title cur", "/s/b.mp3");       /* cur was /s/b.mp3 */
    }
    pl_sort(&pl, PL_SORT_ARTIST, &cur);
    {
        /* artist: alpha(a,d stable), beta(c), zeta(b); cur stays on /s/b.mp3 */
        const char *want[] = { "/s/a.mp3", "/s/d.mp3", "/s/c.mp3", "/s/b.mp3" };
        expect_paths(want, 4);
        expect_cur("sort artist cur", "/s/b.mp3");
    }
    cur = 2; /* now row 2 = /s/c.mp3 */
    pl_sort(&pl, PL_SORT_TRACK, &cur);
    {
        /* track: 1(c),2(b),9(a), empty-last(d); cur on c -> 0 */
        const char *want[] = { "/s/c.mp3", "/s/b.mp3", "/s/a.mp3", "/s/d.mp3" };
        expect_paths(want, 4);
        expect_cur("sort track cur", "/s/c.mp3");
    }
    pl_sort(&pl, PL_SORT_FILE, &cur);
    {
        const char *want[] = { "/s/a.mp3", "/s/b.mp3", "/s/c.mp3", "/s/d.mp3" };
        expect_paths(want, 4);
        expect_cur("sort file cur", "/s/c.mp3");
    }
    reset();
    addrow("/g/2.mp3", "", "second", "Z", 2, 0, 0, 1);
    addrow("/g/1.mp3", "", "first",  "Z", 1, 0, 0, 1);
    addrow("/g/3.mp3", "", "un",     "", 5, 0, 0, 1);
    addrow("/g/0.mp3", "", "zed",    "A", 7, 0, 0, 1);
    cur = 0;   /* /g/2.mp3 (album Z, track 2) */
    pl_sort(&pl, PL_SORT_ALBUM, &cur);
    {
        /* album A, then Z by (track,title): first(1), second(2); empty album last */
        const char *want[] = { "/g/0.mp3", "/g/1.mp3", "/g/2.mp3", "/g/3.mp3" };
        expect_paths(want, 4);
        expect_cur("sort album cur", "/g/2.mp3");
    }
    /* CUE group sorts as one unit, by its PARENT row, rows in order */
    reset();
    addrow("/z/mid.mp3", "", "M", "", 0, 0, 0, 1);
    addrow("/z/alb.flac", "", "Ztr1", "ZZ", 1, 0, 50, 50);    /* parent sorts by Z... */
    addrow("/z/alb.flac", "", "Ztr2", "ZZ", 2, 50, 99, 49);
    addrow("/z/ant.mp3", "", "A", "", 0, 0, 0, 1);
    cur = 2; /* Ztr2 (the group's second row) */
    pl_sort(&pl, PL_SORT_TITLE, &cur);
    {
        const char *want[] = { "/z/ant.mp3", "/z/mid.mp3",
                               "/z/alb.flac", "/z/alb.flac" };
        expect_paths(want, 4);
        chk("group order kept", pl.items[2].start_sec == 0 && pl.items[3].start_sec == 50, 1);
        expect_cur("sort group cur same music", "/z/alb.flac");
        chk("sort group cur row", pl.items[cur].start_sec == 50, 1);
    }

    /* ---------- reverse ---------- */
    reset();
    addrow("/r/1.mp3", "", "1", "", 0, 0, 0, 1);
    addrow("/r/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/r/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/r/4.mp3", "", "4", "", 0, 0, 0, 1);
    cur = 2; /* second row of the group */
    pl_reverse(&pl, &cur);
    {
        const char *want[] = { "/r/4.mp3", "/r/alb.flac", "/r/alb.flac", "/r/1.mp3" };
        expect_paths(want, 4);
        chk("reverse group intact+ordered", pl.items[1].start_sec == 0 && pl.items[2].start_sec == 60, 1);
        expect_cur("reverse cur same music", "/r/alb.flac");
        chk("reverse cur row", pl.items[cur].start_sec == 60, 1);
    }

    /* ---------- shuffle (seeded: rand() is the house RNG, ui seeds ticks) ---------- */
    reset();
    addrow("/h/1.mp3", "", "1", "", 0, 0, 0, 1);
    addrow("/h/2.mp3", "", "2", "", 0, 0, 0, 2);
    addrow("/h/3.mp3", "", "3", "", 0, 0, 0, 3);
    addrow("/h/4.mp3", "", "4", "", 0, 0, 0, 4);
    addrow("/h/5.mp3", "", "5", "", 0, 0, 0, 5);
    cur = 4; /* /h/5.mp3 */
    srand(42);
    pl_shuffle(&pl, &cur);
    {
        /* pinned to srand(42) on this host's rand() (glibc), measured */
        const char *want[] = { "/h/4.mp3", "/h/5.mp3", "/h/3.mp3", "/h/1.mp3", "/h/2.mp3" };
        expect_paths(want, 5);
        expect_cur("shuffle cur same music", "/h/5.mp3");
    }
    /* shuffle keeps a group whole, rows in internal order */
    reset();
    addrow("/h/a.mp3", "", "a", "", 0, 0, 0, 1);
    addrow("/h/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/h/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/h/z.mp3", "", "z", "", 0, 0, 0, 1);
    srand(7);
    pl_shuffle(&pl, &cur);
    {
        int g0 = -1, g1 = -1;
        for (int i = 0; i < pl.count; i++)
            if (!strcmp(pl.items[i].path, "/h/alb.flac")) { if (g0 < 0) g0 = i; else g1 = i; }
        chk("shuffle group adjacent", g1 == g0 + 1, 1);
        chk("shuffle group ordered", pl.items[g0].start_sec == 0 && pl.items[g1].start_sec == 60, 1);
    }

    /* ---------- remove_set == multi-REM engine parity ---------- */
    reset();
    addrow("/x/0.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/1.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/2.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/3.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/4.mp3", "", "", "", 0, 0, 0, 1);
    cur = 3;
    {
        /* replay the PB_REM multi path by hand... */
        int sel[] = { 0, 2, 7 };      /* 7: stale-high, clamped inside */
        int below = 0;
        for (int i = 0; i < 3 && cur >= 0; i++)
            if (sel[i] < cur) below++; else break;
        for (int i = 2; i >= 0; i--) pl_remove(&pl, sel[i]);
        cur -= below;
        const char *want[] = { "/x/1.mp3", "/x/3.mp3", "/x/4.mp3" };
        expect_paths(want, 3);
        chk("multi-REM cur", cur, 1);   /* /x/3.mp3 */
        expect_cur("multi-REM cur path", "/x/3.mp3");
    }
    reset();
    addrow("/x/0.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/1.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/2.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/3.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/x/4.mp3", "", "", "", 0, 0, 0, 1);
    cur = 3;
    {
        int sel[] = { 0, 2, 7 };
        pl_remove_set(&pl, sel, 3, &cur);
        const char *want[] = { "/x/1.mp3", "/x/3.mp3", "/x/4.mp3" };
        expect_paths(want, 3);
        chk("remove_set cur", cur, 1);
        expect_cur("remove_set cur path", "/x/3.mp3");
    }

    /* ---------- crop: keep-selection, complement through same engine ---------- */
    reset();
    addrow("/p/0.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/p/1.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/p/2.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/p/3.mp3", "", "", "", 0, 0, 0, 1);
    cur = 2;
    {
        int keep[] = { 1, 2 };
        pl_crop(&pl, keep, 2, &cur);
        const char *want[] = { "/p/1.mp3", "/p/2.mp3" };
        expect_paths(want, 2);
        expect_cur("crop cur survives (kept)", "/p/2.mp3");
        chk("crop cur index", cur, 1);
    }
    /* crop touching part of a group keeps the WHOLE group (via expand) */
    reset();
    addrow("/p/0.mp3", "", "", "", 0, 0, 0, 1);
    addrow("/p/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/p/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/p/3.mp3", "", "", "", 0, 0, 0, 1);
    cur = 0;
    {
        int sel[] = { 1 };
        int out[16];
        int en = pl_sel_expand(&pl, sel, 1, out);
        chk("expand group rows", en, 2);
        chk("expand second row", out[1], 2);
        pl_crop(&pl, out, en, &cur);
        const char *want[] = { "/p/alb.flac", "/p/alb.flac" };
        expect_paths(want, 2);
        /* cur (/p/0.mp3) was NOT kept: caller must stop; index-wise cur
           fell like the multi-REM contract (removed-below == 0) */
        chk("crop cur after losing its row", cur, 0);
    }

    /* ---------- clipboard: copy/paste with groups ----------
       A pasted copy of a CUE album landing ADJACENT to its source merges
       with it into one span (same path, adjacent, cue bounds = one group
       by definition — accepted: same file, and the pins below read row
       values, not group identity). Lists are rebuilt between phases. */
    reset();
    addrow("/src/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/src/alb.flac", "", "t2", "", 2, 60, 0, 60);
    {
        int sel[] = { 0 }, out[8];
        chk("copy clip_count before", pl_clip_count(), 0);
        int en = pl_sel_expand(&pl, sel, 1, out);
        chk("copy expand whole group", en, 2);
        chk("clip_copy", pl_clip_copy(&pl, out, en), 0);
        chk("clip_count", pl_clip_count(), 2);
    }
    /* paste MID-GROUP (at=2) must snap to the group's end (at=3) */
    reset();
    addrow("/c/A.mp3", "", "A", "", 0, 0, 0, 1);
    addrow("/c/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/c/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/c/B.mp3", "", "B", "", 0, 0, 0, 1);
    cur = 3;
    {
        chk("paste snap ok", pl_clip_paste(&pl, 2, &cur), 0);
        const char *want[] = { "/c/A.mp3", "/c/alb.flac", "/c/alb.flac",
                               "/src/alb.flac", "/src/alb.flac", "/c/B.mp3" };
        expect_paths(want, 6);
        chk("paste landed after group end",
            pl.items[3].start_sec == 0 && pl.items[4].start_sec == 60, 1);
        /* cur was 3 (B): paste landed at 3 <= cur -> shifts by 2 */
        expect_cur("paste-above cur shifted", "/c/B.mp3");
        chk("paste cur idx", cur, 5);
    }
    /* paste BELOW cur leaves cur alone */
    reset();
    addrow("/c/A.mp3", "", "A", "", 0, 0, 0, 1);
    addrow("/c/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/c/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/c/B.mp3", "", "B", "", 0, 0, 0, 1);
    cur = 0;
    chk("paste below ok", pl_clip_paste(&pl, pl.count, &cur), 0);
    chk("paste below cur stays", cur, 0);
    expect_cur("paste below cur music", "/c/A.mp3");
    chk("paste below count", pl.count, 6);
    /* cut a group: rows gone, clipboard holds the group, cur remaps */
    reset();
    addrow("/c/A.mp3", "", "A", "", 0, 0, 0, 1);
    addrow("/c/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/c/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/c/B.mp3", "", "B", "", 0, 0, 0, 1);
    cur = 3;                        /* B: two cut rows sit below it */
    {
        int sel[] = { 1 }, out[8];
        int en = pl_sel_expand(&pl, sel, 1, out);
        pl_clip_cut(&pl, out, en, &cur);
        chk("cut clip intact", pl_clip_count(), 2);
        const char *want[] = { "/c/A.mp3", "/c/B.mp3" };
        expect_paths(want, 2);
        expect_cur("cut cur music survives", "/c/B.mp3");
        chk("cut cur idx", cur, 1);
    }
    /* empty-selection copy keeps the clipboard (refuses to clear it) */
    chk("clip_copy empty", pl_clip_copy(&pl, NULL, 0), 0);
    chk("clip kept after empty copy", pl_clip_count(), 2);
    /* stale selection refused, clipboard untouched */
    chk("clip stale refuse", pl_clip_copy(&pl, (int[]){ 999 }, 1), -1);
    chk("clip after refuse", pl_clip_count(), 2);

    /* ---------- move: boundaries, into-group, over-group ---------- */
    reset();
    addrow("/v/0.mp3", "", "0", "", 0, 0, 0, 1);
    addrow("/v/1.mp3", "", "1", "", 0, 0, 0, 1);
    addrow("/v/2.mp3", "", "2", "", 0, 0, 0, 1);
    cur = 2;
    {
        int sel[] = { 2 };
        pl_move(&pl, sel, 1, -1, &cur);
        const char *want[] = { "/v/0.mp3", "/v/2.mp3", "/v/1.mp3" };
        expect_paths(want, 3);
        chk("move sel remapped", sel[0], 1);
        expect_cur("move cur follows", "/v/2.mp3");
    }
    /* top boundary: no move at all */
    {
        int sel[] = { 0 };
        pl_move(&pl, sel, 1, -1, &cur);
        const char *want[] = { "/v/0.mp3", "/v/2.mp3", "/v/1.mp3" };
        expect_paths(want, 3);
        chk("move top clamp", sel[0], 0);
    }
    /* bottom boundary */
    {
        int sel[] = { 2 };
        cur = 1;
        pl_move(&pl, sel, 1, 1, &cur);
        const char *want[] = { "/v/0.mp3", "/v/2.mp3", "/v/1.mp3" };
        expect_paths(want, 3);
        chk("move bottom clamp", sel[0], 2);
        chk("move clamp cur", cur, 1);
    }
    /* into-group: B moves up past the WHOLE group, never splitting it */
    reset();
    addrow("/w/A.mp3", "", "A", "", 0, 0, 0, 1);
    addrow("/w/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/w/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/w/B.mp3", "", "B", "", 0, 0, 0, 1);
    cur = 0;                            /* A keeps its music at row 1 */
    {
        int sel[] = { 3 };
        pl_move(&pl, sel, 1, -1, &cur);
        const char *want[] = { "/w/A.mp3", "/w/B.mp3", "/w/alb.flac", "/w/alb.flac" };
        expect_paths(want, 4);
        chk("move into-group order", pl.items[2].start_sec == 0 && pl.items[3].start_sec == 60, 1);
        expect_cur("move into-group cur", "/w/A.mp3");   /* A never moved */
        chk("move into-group cur idx", cur, 0);
    }
    /* over-group: A moves down over the whole group */
    reset();
    addrow("/w/A.mp3", "", "A", "", 0, 0, 0, 1);
    addrow("/w/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/w/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/w/B.mp3", "", "B", "", 0, 0, 0, 1);
    cur = 2;                            /* second row of the group */
    {
        int sel[] = { 0 };
        pl_move(&pl, sel, 1, 1, &cur);
        const char *want[] = { "/w/alb.flac", "/w/alb.flac", "/w/A.mp3", "/w/B.mp3" };
        expect_paths(want, 4);
        chk("move over-group order", pl.items[0].start_sec == 0 && pl.items[1].start_sec == 60, 1);
        expect_cur("move over-group cur", "/w/alb.flac");
        chk("move over-group cur row", pl.items[cur].start_sec == 60, 1);
    }
    /* run of two selected singles moves together, staying sorted */
    reset();
    addrow("/v/0.mp3", "", "0", "", 0, 0, 0, 1);
    addrow("/v/1.mp3", "", "1", "", 0, 0, 0, 1);
    addrow("/v/2.mp3", "", "2", "", 0, 0, 0, 1);
    addrow("/v/3.mp3", "", "3", "", 0, 0, 0, 1);
    cur = 3;
    {
        int sel[] = { 1, 2 };
        pl_move(&pl, sel, 2, 1, &cur);
        const char *want[] = { "/v/0.mp3", "/v/3.mp3", "/v/1.mp3", "/v/2.mp3" };
        expect_paths(want, 4);
        chk("move run sel0", sel[0], 2);
        chk("move run sel1", sel[1], 3);
        /* /v/3.mp3 (cur) rode UP as the selected run passed it */
        chk("move run cur followed music", cur, 1);
        expect_cur("move run cur music unchanged", "/v/3.mp3");
    }

    /* ---------- pl_drop (P4 row drag-and-drop): gap-index drops ---------- */
    reset();
    for (int i = 0; i < 5; i++) {
        char row[64];
        snprintf(row, sizeof row, "/d/%d.mp3", i);
        addrow(row, "", "", "", 0, 0, 0, 1);
    }
    cur = 0;
    {   /* drop one row down: rows between the gap ride up */
        int sel[] = { 0 };
        pl_drop(&pl, sel, 1, 3, &cur);
        const char *want[] = { "/d/1.mp3", "/d/2.mp3", "/d/0.mp3",
                               "/d/3.mp3", "/d/4.mp3" };
        expect_paths(want, 5);
        chk("drop sel remapped", sel[0], 2);
        chk("drop cur follows block", cur, 2);
        expect_cur("drop cur same music", "/d/0.mp3");
    }
    reset();
    for (int i = 0; i < 5; i++) {
        char row[64];
        snprintf(row, sizeof row, "/d/%d.mp3", i);
        addrow(row, "", "", "", 0, 0, 0, 1);
    }
    {   /* gap right after the block is the identity (self-drop no-op) */
        int sel[] = { 2 };
        pl_drop(&pl, sel, 1, 3, &cur);
        const char *want[] = { "/d/0.mp3", "/d/1.mp3", "/d/2.mp3",
                               "/d/3.mp3", "/d/4.mp3" };
        expect_paths(want, 5);
        chk("self-drop sel stays", sel[0], 2);
    }
    {   /* drop to the end, gap clamped past count */
        cur = 2;
        int sel[] = { 0 };
        pl_drop(&pl, sel, 1, 99, &cur);
        const char *want[] = { "/d/1.mp3", "/d/2.mp3", "/d/3.mp3",
                               "/d/4.mp3", "/d/0.mp3" };
        expect_paths(want, 5);
        chk("clamp-end sel->last", sel[0], 4);
        expect_cur("clamp-end cur same music", "/d/2.mp3");
        chk("clamp-end cur idx", cur, 1);
    }
    reset();
    for (int i = 0; i < 5; i++) {
        char row[64];
        snprintf(row, sizeof row, "/d/%d.mp3", i);
        addrow(row, "", "", "", 0, 0, 0, 1);
    }
    {   /* block crosses cur from below: cur rides down by the block */
        cur = 2;                     /* /d/2.mp3 */
        int sel[] = { 3 };
        pl_drop(&pl, sel, 1, 1, &cur);
        const char *want[] = { "/d/0.mp3", "/d/3.mp3", "/d/1.mp3",
                               "/d/2.mp3", "/d/4.mp3" };
        expect_paths(want, 5);
        expect_cur("block-crosses-cur same music", "/d/2.mp3");
        chk("block-crosses-cur idx", cur, 3);
    }
    reset();
    for (int i = 0; i < 5; i++) {
        char row[64];
        snprintf(row, sizeof row, "/d/%d.mp3", i);
        addrow(row, "", "", "", 0, 0, 0, 1);
    }
    {   /* non-adjacent set lands as ONE block (shell semantics) */
        int sel[] = { 0, 4 };
        pl_drop(&pl, sel, 2, 3, &cur);
        const char *want[] = { "/d/1.mp3", "/d/2.mp3", "/d/0.mp3",
                               "/d/4.mp3", "/d/3.mp3" };
        expect_paths(want, 5);
        chk("merge sel0", sel[0], 2);
        chk("merge sel1", sel[1], 3);
    }
    reset();
    addrow("/g/s1.mp3", "", "s1", "", 0, 0, 0, 1);
    addrow("/g/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/g/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/g/s2.mp3", "", "s2", "", 0, 0, 0, 1);
    cur = 2;                             /* second row of the group */
    {   /* a partially-listed group drags WHOLE and lands whole */
        int sel[] = { 1 };
        pl_drop(&pl, sel, 1, 0, &cur);
        const char *want[] = { "/g/alb.flac", "/g/alb.flac", "/g/s1.mp3",
                               "/g/s2.mp3" };
        expect_paths(want, 4);
        chk("drop group order", pl.items[0].start_sec == 0 &&
            pl.items[1].start_sec == 60, 1);
        chk("drop group sel->0", sel[0], 0);
        expect_cur("group-drop cur same music", "/g/alb.flac");
        chk("group-drop cur row", pl.items[cur].start_sec == 60, 1);
    }
    reset();
    addrow("/g/s1.mp3", "", "s1", "", 0, 0, 0, 1);
    addrow("/g/alb.flac", "", "t1", "", 1, 0, 60, 60);
    addrow("/g/alb.flac", "", "t2", "", 2, 60, 0, 60);
    addrow("/g/s2.mp3", "", "s2", "", 0, 0, 0, 1);
    cur = -1;
    {   /* gap INSIDE a group snaps forward (the pl_clip_paste rule):
           s1 can never wedge between a parent and its rows — dragged
           into the group's span it lands whole AFTER the group */
        int sel[] = { 0 };
        pl_drop(&pl, sel, 1, 2, &cur);
        const char *want[] = { "/g/alb.flac", "/g/alb.flac", "/g/s1.mp3",
                               "/g/s2.mp3" };
        expect_paths(want, 4);
        chk("snap-forward group stays adjacent",
            pl.items[0].start_sec == 0 && pl.items[1].start_sec == 60, 1);
        chk("snap-forward sel after group", sel[0], 2);
    }
    cur = 1;                             /* second row of the group (top after the snap pin) */
    {   /* dropping the last row to the top pushes the WHOLE group down
           (never split) and carries cur with its own music */
        int sel[] = { 3 };
        pl_drop(&pl, sel, 1, 0, &cur);
        const char *want[] = { "/g/s2.mp3", "/g/alb.flac", "/g/alb.flac",
                               "/g/s1.mp3" };
        expect_paths(want, 4);
        expect_cur("atomic shift cur same music", "/g/alb.flac");
        chk("atomic shift cur row", pl.items[cur].start_sec == 60, 1);
    }
    /* degenerate guards */
    {
        int sel[] = { 0 };
        pl_drop(&pl, sel, 0, 1, &cur);
        chk("empty-sel drop is a no-op", pl.count, 4);
    }

    /* ---------- pl_insert_track: full row copy ---------- */
    reset();
    addrow("/i/A.mp3", "art", "ttl", "alb", 7, 12.5, 34.25, 42);
    Track *src = &pl.items[0];
    Track snap = *src;
    pl_insert_track(&pl, 0, src);
    chk("insert_track count", pl.count, 2);
    chks("insert_track path", pl.items[0].path, snap.path);
    chks("insert_track title", pl.items[0].tags.title, snap.tags.title);
    chk("insert_track track#", pl.items[0].tags.track, snap.tags.track);
    chk("insert_track dur", pl.items[0].duration, snap.duration);
    chk("insert_track bounds", pl.items[0].start_sec == snap.start_sec &&
        pl.items[0].end_sec == snap.end_sec, 1);

    /* ---------- P2 gate: real m3u round-trip (save -> reload -> compare) ---------- */
    reset();
    addrow("/m3/one.mp3", "Arto", "One",   "",   1, 0, 0, 61);
    addrow("/m3/two.mp3", "Berta", "Two",  "",   2, 0, 0, 0);   /* dur 0: no EXTINF */
    addrow("/m3/three.mp3", "", "Three",   "",   0, 0, 0, 12);
    {
        char sp[512];
        snprintf(sp, sizeof sp, "%s/rt.m3u", tmpdir);
        chk("save_m3u", pl_save_m3u(&pl, sp), 0);
        reset();
        chk("reload_m3u", pl_load_m3u(&pl, sp), 0);
        const char *want[] = { "/m3/one.mp3", "/m3/two.mp3", "/m3/three.mp3" };
        expect_paths(want, 3);
        chk("round-trip EXTINF durations",
            pl.items[0].duration == 61 && pl.items[1].duration == 0 &&
            pl.items[2].duration == 12, 1);
    }

    if (fails) { printf("\n%d FAILURES\n", fails); return 1; }
    printf("\nall playlistedit pins green\n");
    return 0;
}
