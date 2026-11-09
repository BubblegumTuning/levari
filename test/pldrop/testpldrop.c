/* unit test: OS-drop batch application (m3u_name/drop_add/drop_flush).
   The desktop handshake (XDND/OLE/Cocoa) cannot run headless, but
   everything after the event loop hands us a path can: single files
   append, folders recurse (audio only), .m3u expands relative to its own
   dir, mixed batches keep arrival order, and the posted CMD_PLAY names
   the first NEW row. The event-loop windowID routing itself is compile-
   checked only — proving it needs a real drag onto a real window.
   Technique (see skill): #include ui.c, link every object EXCEPT
   main.c.o AND ui.c.o:
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/td \
       test/pldrop/testpldrop.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name dispatch.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       build/libmodplug.a build/libsnes_spc.a -lz -lm -lpthread -ldl -lstdc++
   Run with scratch HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR set by the caller
   (lib_index_load resolves the index under the config dir; a missing
   index is fine: the fill simply matches nothing). argv[1] = the repo's
   test/ dir (fixtures: a.wav b.wav example.ogg sample.flac sample.mp3
   tone.wav tone2.wav = exactly 7 audio files, plus pl.m3u/album.cue).
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>
#include <sys/stat.h>

static Player fake_pl;
static Playlist list;
static int fails;

static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static int has_path(const char *sub) {
    for (int i = 0; i < list.count; i++)
        if (strstr(list.items[i].path, sub)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    if (!home) { printf("caller must set scratch HOME/XDG_CONFIG_HOME\n"); return 1; }
    const char *tdir = argc > 1 ? argv[1] : "test";
    /* pl_add_file absolutizes relative paths with the cwd (index-path
       semantics) — pin against the ABSOLUTE fixture dir so the test is
       correct from any cwd (the old relative expectations only matched
       an accident of path handling that has since been fixed) */
    static char tdira[LV_MAX_PATH];
    if (!realpath(tdir, tdira)) { perror("resolve test dir"); return 1; }
    tdir = tdira;
    char apath[LV_MAX_PATH], mpath[LV_MAX_PATH];
    snprintf(apath, sizeof apath, "%s/a.wav", tdir);
    snprintf(mpath, sizeof mpath, "%s/pl.m3u", tdir);

    cfg_load();
    pl_init(&list);
    pl_player_init(&fake_pl, &list); /* inits mtx/wake for pl_post */
    ui.pl = &fake_pl;

    /* Shared extension classifier (5095b07 replaced m3u_name with
       pl_ext_kind, which is exactly what drop_add routes on): */
    chk("pl_ext_kind .M3U case-insensitive", pl_ext_kind("x.M3U"), PL_M3U);
    chk("pl_ext_kind .cue is not m3u", pl_ext_kind("album.cue"), PL_CUE);
    chk("pl_ext_kind no extension", pl_ext_kind("noext"), PL_NONE);

    /* 1: single audio file appends, absolute, play the new row */
    drop_open = 1;
    drop_add(apath);
    drop_flush();
    chk("file drop count", list.count, 1);
    chk("file drop row is the path", strcmp(list.items[0].path, apath), 0);
    chk("play command posted", (long)atomic_load(&fake_pl.cmd), CMD_PLAY);
    chk("play arg = first new row", (long)atomic_load(&fake_pl.cmd_arg), 0);

    /* 2: m3u appends its entries joined to the m3u's own dir */
    atomic_store(&fake_pl.cmd, CMD_NONE);
    int old = list.count;
    char bpath[LV_MAX_PATH];
    snprintf(bpath, sizeof bpath, "%s/b.wav", tdir);
    drop_add(mpath);
    drop_flush();
    chk("m3u expands to 2 rows", list.count, old + 2);
    chk("m3u row 1 joined to dir", strcmp(list.items[old].path, apath), 0);
    chk("m3u row 2 joined to dir", strcmp(list.items[old + 1].path, bpath), 0);
    chk("m3u play arg = first new row", (long)atomic_load(&fake_pl.cmd_arg), old);

    /* 3: folder drop recurses, audio extensions only */
    /* fixture-completeness guard first: the count pin below asserts the
       whole fixture dir — if a fixture file went missing the honest
       reading is 'fixtures changed', not 'the drop engine regressed' */
    {
        static const char *fx[7] = { "a.wav", "b.wav", "example.ogg",
                                     "sample.flac", "sample.mp3", "tone.wav", "tone2.wav" };
        int all = 1;
        for (int i = 0; i < 7; i++) {
            char p[LV_MAX_PATH];
            snprintf(p, sizeof p, "%s/%s", tdir, fx[i]);
            struct stat st;
            if (stat(p, &st) != 0) { all = 0; printf("MISSING fixture %s\n", fx[i]); }
        }
        chk("fixture set complete (7 audio files)", all, 1);
    }
    pl_clear(&list);
    drop_add(tdir);
    drop_flush();
    chk("dir drop finds exactly the 7 fixtures", list.count, 7);
    chk("dir drop kept example.ogg", has_path("/example.ogg"), 1);
    chk("dir drop skipped pl.m3u", has_path("pl.m3u"), 0);
    chk("dir drop skipped album.cue", has_path("album.cue"), 0);

    /* 4: one batch of file + m3u: arrival order, one play post */
    pl_clear(&list);
    atomic_store(&fake_pl.cmd, CMD_NONE);
    drop_open = 1;
    drop_add(apath);
    drop_add(mpath);
    drop_flush();
    chk("batch total rows", list.count, 3);
    chk("batch first row is the bare file", strcmp(list.items[0].path, apath), 0);
    chk("batch play arg = first new row", (long)atomic_load(&fake_pl.cmd_arg), 0);

    /* 5: empty batch posts nothing (dropping on an unrouted window or a
       cancelled drag must not stop or restart the running track) */
    atomic_store(&fake_pl.cmd, CMD_NONE);
    drop_flush();
    chk("empty batch posts no command", (long)atomic_load(&fake_pl.cmd), CMD_NONE);

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails ? fails : 0);
    return fails != 0;
}
