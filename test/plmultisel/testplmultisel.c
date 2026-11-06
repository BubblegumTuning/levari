/* unit test: playlist multi-selection (ctrl-click toggle / shift-click range)
   with multi REM and multi SCAN. Pins the selection state machine driven
   through the real handle_pl_mouse (modifiers read via a shadowed
   SDL_GetKeyboardState — SDL2 button events carry no modifier mask), the
   multi-REM p->cur invariant (stop iff the current row is removed; cur
   drops by the removed rows below it), and the multi-SCAN loop end to end
   against a scratch library index (append-if-absent, per-row write-back,
   summary message on a failed row, select-all/background-scan refusal),
   the P1 edit-menu mouse state machine and menu-action dispatch, and the
   P4 row drag-and-drop (drag-12): gap mapping with half-row bias, the 4px
   arm threshold, deferred double-click play (eaten by a drag, surviving
   jitter), in-set press deferring the collapse so a drag carries the whole
   set, edge auto-scroll, and selection/cur riding the landed rows.
   Technique (see skill): #include ui.c, link every object EXCEPT main.c.o
   AND ui.c.o AND dispatch.c.o (duplicate dec_* symbols otherwise):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o build/testplmultisel \
       test/plmultisel/testplmultisel.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name dispatch.c.o) \
       $(find build -name 'libmodplug.a' | head -1) \
       $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
   Run with scratch HOME/XDG_CONFIG_HOME set by the caller.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>
#include <sys/stat.h>

static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chks(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

/* shadow of SDL_GetKeyboardState (executable definitions win over the
   shared library at link time — same PLT trick as test/pldock's fake
   SetWindowPosition): the click handler reads the physical key state */
static Uint8 fake_keys[SDL_NUM_SCANCODES];
const Uint8 *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return fake_keys;
}

static Player p;
static Playlist pl;

/* click row `row` of the playlist text area (pl_layout: x=14, y=40, 17px
   rows; x=100 and pl_h=200 keep the click clear of the close button, the
   resize corner and PL_BTNS). motion fields first, button fields LAST:
   button.button/state/clicks alias motion.state, button.x/y share the
   motion.x/y offsets (measured on the host SDL 2.28.5) */
static void pl_click(int row, int ctrl, int shift, int clicks) {
    fake_keys[SDL_SCANCODE_LCTRL] = (Uint8)(ctrl ? 1 : 0);
    fake_keys[SDL_SCANCODE_LSHIFT] = (Uint8)(shift ? 1 : 0);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.motion.x = 100;
    e.motion.y = 40 + row * 17 + 8;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.button.clicks = (Uint8)clicks;
    handle_pl_mouse(&e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.state = SDL_RELEASED;
    handle_pl_mouse(&e);
    fake_keys[SDL_SCANCODE_LCTRL] = fake_keys[SDL_SCANCODE_LSHIFT] = 0;
}

/* manual press/motion/release for the row-drag pins: pl_click bundles
   DOWN+UP, a drag needs motion between them. MOTION events: motion
   fields LAST, button fields never (union trap, see header) */
static void pl_down(int row, int clicks) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.motion.x = 100; e.motion.y = 40 + row * 17 + 8;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.button.clicks = (Uint8)clicks;
    handle_pl_mouse(&e);
}
static void pl_motion(int y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.x = 100; e.motion.y = y;
    e.motion.state = SDL_BUTTON_LMASK;   /* motion fields last */
    handle_pl_mouse(&e);
}
static void pl_up(int row) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONUP;
    e.motion.x = 100; e.motion.y = 40 + row * 17 + 8;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_RELEASED;
    handle_pl_mouse(&e);
}
static const char *path_at(int i) { return pl.items[i].path; }

static int pl_key_del(void) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = SDLK_DELETE;
    return pl_key(&e);
}
static int pl_key_ctrla(void) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = SDLK_a;
    e.key.keysym.mod = KMOD_CTRL;
    return pl_key(&e);
}
static int pl_key_homeend(int end) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = end ? SDLK_END : SDLK_HOME;
    return pl_key(&e);
}

static char dir[LV_MAX_PATH], pa[LV_MAX_PATH], pb[LV_MAX_PATH], pmiss[LV_MAX_PATH];

/* id3v2.3 TIT2-only fixture (same builder as test/rescan/testrescan.c) */
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

static void pl_reset(int nrows) {
    pl_clear(&pl);
    for (int i = 0; i < nrows; i++) {
        char row[LV_MAX_PATH];
        snprintf(row, sizeof row, "%s/r%d.mp3", dir, i);
        pl_add_file(&pl, row);
    }
    ui.pl_sel = -1;
    ui.pl_selall = 0;
    pl_sel_reset();
    ui.pl_scroll = 0;
    p.cur = -1;
    p.cmd = CMD_NONE;
    ui.msg[0] = 0;
    ui.opt_scanning = 0;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) { printf("caller must set scratch HOME/XDG_CONFIG_HOME\n"); return 1; }
    snprintf(dir, sizeof dir, "%s/plmultisel", home);
    mkdir(dir, 0755);
    snprintf(pa, sizeof pa, "%s/a.mp3", dir);
    snprintf(pb, sizeof pb, "%s/b.mp3", dir);
    snprintf(pmiss, sizeof pmiss, "%s/missing.mp3", dir);

    cfg_load();
    pl_init(&pl);
    pthread_mutex_init(&p.mtx, NULL);
    pthread_cond_init(&p.wake, NULL);
    p.pl = &pl;
    ui.pl = &p;
    ui.pl_w = 300; ui.pl_h = 200;   /* rows at x=100, y=48+17*row: clear of
                                       close rect, PL_BTNS and the corner */
    ui.pl_sel = -1;
    ui.pl_cur = -1;

    /* ---- selection state machine -------------------------------------- */
    pl_reset(5);
    pl_click(2, 0, 0, 1);
    chk("plain click: one row in set", ui.pl_multi_n, 1);
    chk("plain click: row 2 selected", pl_sel_has(2), 1);
    chk("plain click: anchor", ui.pl_sel, 2);
    chk("plain click: no play cmd on single click", (int)p.cmd, CMD_NONE);

    pl_click(4, 1, 0, 1);
    chk("ctrl-click adds: two rows", ui.pl_multi_n, 2);
    chk("ctrl-click keeps row 2", pl_sel_has(2), 1);
    chk("ctrl-click adds row 4", pl_sel_has(4), 1);

    pl_click(2, 1, 0, 1);
    chk("ctrl-click toggles off", ui.pl_multi_n, 1);
    chk("ctrl-click removed row 2", pl_sel_has(2), 0);
    chk("ctrl-click kept row 4", pl_sel_has(4), 1);
    chk("ctrl-click moves anchor", ui.pl_sel, 2);

    pl_click(0, 0, 0, 1);
    pl_click(3, 0, 1, 1);
    chk("shift-click: range 0..3", ui.pl_multi_n, 4);
    chk("shift range has 0", pl_sel_has(0), 1);
    chk("shift range has 3", pl_sel_has(3), 1);
    chk("shift range excludes 4", pl_sel_has(4), 0);
    chk("shift-click keeps anchor", ui.pl_sel, 0);

    pl_click(1, 0, 1, 1);
    chk("second shift-click replaces range 0..1", ui.pl_multi_n, 2);
    chk("range 0..1 excludes 2", pl_sel_has(2), 0);

    pl_click(0, 0, 0, 1);
    chk("plain click after shift: single", ui.pl_multi_n, 1);
    chk("plain click anchor", ui.pl_sel, 0);

    /* ctrl-a collapses to the flag; the first ctrl-click expands it */
    chk("ctrl-a handled", pl_key_ctrla(), 1);
    chk("ctrl-a sets flag", ui.pl_selall, 1);
    chk("ctrl-a empties set", ui.pl_multi_n, 0);
    pl_click(1, 1, 0, 1);
    chk("ctrl-click on select-all: all minus one", ui.pl_multi_n, 4);
    chk("ctrl-click on select-all: flag cleared", ui.pl_selall, 0);
    chk("ctrl-click on select-all: clicked off", pl_sel_has(1), 0);
    chk("ctrl-click on select-all: row 0 on", pl_sel_has(0), 1);

    /* double-click plays only without modifiers */
    pl_click(2, 1, 0, 2);
    chk("ctrl double-click does not play", (int)p.cmd, CMD_NONE);
    p.cmd = CMD_NONE;
    pl_click(2, 0, 0, 2);
    chk("plain double-click plays", (int)p.cmd, CMD_PLAY);
    chk("double-click arg", p.cmd_arg, 2);

    /* ---- multi REM ----------------------------------------------------- */
    pl_reset(5);
    pl_click(0, 0, 0, 1);
    pl_click(2, 1, 0, 1);
    pl_click(3, 1, 0, 1);
    p.cur = 3;
    p.cmd = CMD_NONE;
    action(PB_REM);
    chk("multi REM: count 5 -> 2", pl.count, 2);
    {
        char want[LV_MAX_PATH];
        snprintf(want, sizeof want, "%s/r1.mp3", dir);
        chks("multi REM: survivor 0 is r1", pl.items[0].path, want);
        snprintf(want, sizeof want, "%s/r4.mp3", dir);
        chks("multi REM: survivor 1 is r4", pl.items[1].path, want);
    }
    chk("multi REM: stopped (cur was in set)", (int)p.cmd, CMD_STOP);
    chk("multi REM: cur shifted by removals below", (long)p.cur, 1);
    chk("multi REM: selection cleared", ui.pl_multi_n, 0);
    chk("multi REM: anchor cleared", ui.pl_sel, -1);

    pl_reset(5);
    p.cur = 3;
    p.cmd = CMD_NONE;
    ui.pl_multi[0] = 0; ui.pl_multi_n = 1;   /* direct: REM is what is under test */
    action(PB_REM);
    chk("REM away from cur: no stop", (int)p.cmd, CMD_NONE);
    chk("REM away from cur: cur shifts", (long)p.cur, 2);

    pl_reset(5);
    action(PB_REM);
    chk("REM with empty selection: untouched", pl.count, 5);

    pl_reset(5);
    ui.pl_selall = 1;
    action(PB_REM);
    chk("REM select-all: clears", pl.count, 0);
    chk("REM select-all: cur -1", (long)p.cur, -1);

    /* keyboard gate fires on a set selection, not on none */
    pl_reset(5);
    chk("DEL with no selection: unhandled", pl_key_del(), 0);
    pl_click(1, 0, 0, 1);
    chk("DEL with selection: handled", pl_key_del(), 1);
    chk("DEL removed the row", pl.count, 4);

    /* ---- HOME / END ----------------------------------------------------- */
    pl_reset(0);
    chk("HOME on empty: unhandled (global keys stay)", pl_key_homeend(0), 0);
    chk("END on empty: unhandled", pl_key_homeend(1), 0);

    pl_reset(20);                    /* pl_h=200 -> 8 visible rows */
    pl_click(3, 0, 0, 1);
    chk("END handled", pl_key_homeend(1), 1);
    chk("END selects last row", ui.pl_sel, 19);
    chk("END single selection", ui.pl_multi_n, 1);
    chk("END set holds row 19", pl_sel_has(19), 1);
    chk("END scrolls to bottom", ui.pl_scroll, 12);
    ui.pl_selall = 1;
    chk("HOME handled", pl_key_homeend(0), 1);
    chk("HOME clears select-all flag", ui.pl_selall, 0);
    chk("HOME selects first row", ui.pl_sel, 0);
    chk("HOME set holds only row 0", ui.pl_multi_n + (pl_sel_has(0) ? 0 : 100), 1);
    chk("HOME scrolls to top", ui.pl_scroll, 0);

    pl_reset(3);                     /* fits entirely: END must not scroll negative */
    chk("END short list handled", pl_key_homeend(1), 1);
    chk("END short list selects last", ui.pl_sel, 2);
    chk("END short list stays scrolled at top", ui.pl_scroll, 0);

    /* ---- multi SCAN ---------------------------------------------------- */
    /* background-scan refusal (index must not appear) */
    pl_reset(3);
    snprintf(pl.items[0].path, sizeof pl.items[0].path, "%s", pa);
    snprintf(pl.items[1].path, sizeof pl.items[1].path, "%s", pb);
    snprintf(pl.items[2].path, sizeof pl.items[2].path, "%s", pmiss);
    ui.pl_multi[0] = 0; ui.pl_multi[1] = 1; ui.pl_multi[2] = 2; ui.pl_multi_n = 3;
    ui.opt_scanning = 1;
    action(PB_SCAN);
    chk("SCAN during background scan refuses", strcmp(ui.msg, "  scan running, wait") == 0, 1);
    chk("refusal wrote no index", lib_index_load().n, 0);
    ui.opt_scanning = 0;

    /* select-all remains the deliberate no-op (decision D) */
    ui.pl_selall = 1;
    ui.msg[0] = 0;
    action(PB_SCAN);
    chk("SCAN select-all: no-op", lib_index_load().n, 0);
    chk("SCAN select-all: silent", ui.msg[0], 0);
    ui.pl_selall = 0;

    write_tagged(pa, "AAA");
    write_tagged(pb, "BBB");
    /* a CUE virtual row must be skipped, its parent-less path never indexed */
    pl.items[2].end_sec = 30;              /* missing.mp3 as virtual row: skip */
    ui.pl_multi_n = 3;
    action(PB_SCAN);                        /* {a, b} index, virtual skips */
    LibIndex ix = lib_index_load();
    chk("multi SCAN indexed both files", ix.n, 2);
    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < ix.n; i++) {
        if (!strcmp(ix.rows[i].path, pa) && !strcmp(ix.rows[i].title, "AAA")) seen_a = 1;
        if (!strcmp(ix.rows[i].path, pb) && !strcmp(ix.rows[i].title, "BBB")) seen_b = 1;
    }
    lib_index_free(&ix);
    chk("index row for a.mp3 title AAA", seen_a, 1);
    chk("index row for b.mp3 title BBB", seen_b, 1);
    chk("SCAN wrote tags to the playlist row", strcmp(pl.items[0].tags.title, "AAA"), 0);
    chk("SCAN silent when no row failed", ui.msg[0], 0);

    /* now a real failure in the batch: summary message, others still done */
    pl.items[2].end_sec = 0;
    ui.pl_multi_n = 3;
    ui.msg[0] = 0;
    action(PB_SCAN);
    chk("SCAN batch with a dead row: summary msg", strcmp(ui.msg, "  cannot rescan") == 0, 1);
    chk("SCAN batch still kept index at 2 rows (missing not appended)", lib_index_load().n, 2);

    /* re-SCAN after retag lands fresh values on the row and the index */
    write_tagged(pa, "CCC");
    ui.pl_multi[0] = 0; ui.pl_multi_n = 1;
    ui.msg[0] = 0;
    action(PB_SCAN);
    chk("re-SCAN applies fresh tags to the row", strcmp(pl.items[0].tags.title, "CCC"), 0);
    ix = lib_index_load();
    int ccc = 0;
    for (int i = 0; i < ix.n; i++)
        if (!strcmp(ix.rows[i].path, pa) && !strcmp(ix.rows[i].title, "CCC")) ccc = 1;
    chk("re-SCAN kept row count", ix.n, 2);
    lib_index_free(&ix);
    chk("re-SCAN replaced the index row", ccc, 1);

    /* ---- playlist edit menu (P1): mouse state machine ------------------ */
    pl_reset(5);
    pl_click(3, 0, 0, 1);                    /* selection = {3} */
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEBUTTONDOWN;
        e.motion.x = 250; e.motion.y = 200;
        e.button.button = SDL_BUTTON_RIGHT;
        e.button.state = SDL_PRESSED;
        handle_pl_mouse(&e);
        chk("right-click opens the edit menu", ui.pl_menu, 1);
        chk("right-click selects nothing", ui.pl_multi_n, 1);
        chk("right-click still selected row 3", pl_sel_has(3), 1);
        chk("right-click presses no button", ui.pressed, B_NONE);
        chk("right-click latches no drag", ui.drag, 0);

        /* menu open: the next left click is EATEN outside the items */
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEBUTTONDOWN;
        e.motion.x = 200; e.motion.y = 60;   /* over row 1, right of the menu */
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = SDL_PRESSED;
        handle_pl_mouse(&e);
        chk("outside click closes menu", ui.pl_menu, 0);
        chk("outside click did NOT select the row under it", ui.pl_multi_n, 1);
        chk("selection unchanged by eaten click", pl_sel_has(3), 1);
        chk("eaten click pressed no button", ui.pressed, B_NONE);

        /* Esc closes while open, and claims; when closed it passes through */
        SDL_Event k;
        memset(&k, 0, sizeof k);
        k.type = SDL_KEYDOWN;
        k.key.keysym.sym = SDLK_ESCAPE;
        e.button.button = SDL_BUTTON_RIGHT;  /* reopen */
        handle_pl_mouse(&e);
        chk("reopen menu", ui.pl_menu, 1);
        chk("Esc claims while menu open", pl_key(&k), 1);
        chk("Esc closed the menu", ui.pl_menu, 0);
        chk("Esc passes through when closed", pl_key(&k), 0);
    }
    {
        /* click ON an item fires its action and closes: titles E D C B A,
           SORT TITLE (item 0, center 47/8) must reorder rows */
        for (int i = 0; i < pl.count; i++)
            snprintf(pl.items[i].tags.title, sizeof pl.items[i].tags.title,
                     "%c", 'E' - i);
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEBUTTONDOWN;
        e.motion.x = 250; e.motion.y = 200;
        e.button.button = SDL_BUTTON_RIGHT;
        e.button.state = SDL_PRESSED;
        handle_pl_mouse(&e);
        e.motion.x = 47; e.motion.y = 8;     /* SORT TITLE item center */
        e.button.button = SDL_BUTTON_LEFT;
        handle_pl_mouse(&e);
        chk("menu item closes menu", ui.pl_menu, 0);
        char want[LV_MAX_PATH];
        snprintf(want, sizeof want, "%s/r4.mp3", dir);   /* title "A" */
        chk("menu item fired the sort", strcmp(pl.items[0].path, want) == 0, 1);
    }

    /* ---- menu-action dispatch ----------------------------------------- */
    pl_reset(5);
    p.cur = 0;
    for (int i = 0; i < pl.count; i++)
        snprintf(pl.items[i].tags.title, sizeof pl.items[i].tags.title, "%c", 'E' - i);
    pl_menu_action(PM_STITLE);
    chk("sort permuted rows", strcmp(pl.items[4].tags.title, "E") == 0, 1);
    char r0s[LV_MAX_PATH];
    snprintf(r0s, sizeof r0s, "%s/r0.mp3", dir);
    chk("sort remapped p.cur onto same music",
        strcmp(pl.items[p.cur].path, r0s) == 0, 1);
    chk("sort cleared selection", ui.pl_multi_n + ui.pl_selall, 0);
    pl_reset(5);

    /* copy -> paste below the selected single row */
    ui.pl_multi[0] = 1; ui.pl_multi_n = 1;
    pl_menu_action(PM_COPY);
    chk("copy filled the clipboard", pl_clip_count(), 1);
    pl_menu_action(PM_PASTE);
    chk("paste grew the list", pl.count, 6);
    char r1[LV_MAX_PATH];
    snprintf(r1, sizeof r1, "%s/r1.mp3", dir);
    chk("paste landed below the selected row", strcmp(pl.items[2].path, r1) == 0, 1);
    chk("paste cleared the stale selection", ui.pl_multi_n + ui.pl_selall, 0);

    /* cut the current row: stop is posted, rows fall like multi-REM */
    pl_reset(5);
    p.cur = 2; p.cmd = CMD_NONE;
    ui.pl_multi[0] = 2; ui.pl_multi_n = 1;
    pl_menu_action(PM_CUT);
    chk("cut posts CMD_STOP when cur is cut", (int)p.cmd, CMD_STOP);
    chk("cut removed one row", pl.count, 4);
    char r3[LV_MAX_PATH];
    snprintf(r3, sizeof r3, "%s/r3.mp3", dir);
    chk("cur falls onto the successor row", strcmp(pl.items[p.cur].path, r3) == 0, 1);
    chk("cut kept the rows in the clipboard", pl_clip_count(), 1);

    /* move: the selection rides the moved row */
    pl_reset(5);
    ui.pl_multi[0] = 2; ui.pl_multi_n = 1; ui.pl_sel = 2;
    pl_menu_action(PM_MDOWN);
    char r2[LV_MAX_PATH], r2w[LV_MAX_PATH];
    snprintf(r2, sizeof r2, "%s/r2.mp3", dir);
    snprintf(r2w, sizeof r2w, "%s/r3.mp3", dir);
    chk("move down swapped the rows", strcmp(pl.items[2].path, r2w) == 0 &&
        strcmp(pl.items[3].path, r2) == 0, 1);
    chk("move remapped the selection", ui.pl_multi[0] == 3 && ui.pl_multi_n == 1, 1);
    chk("move kept the anchor", ui.pl_sel, 3);
    pl_menu_action(PM_MUP);
    chk("move up undid it", strcmp(pl.items[2].path, r2) == 0 && ui.pl_multi[0] == 2, 1);

    /* crop: keep one row; stop iff the current row is cropped away */
    pl_reset(5);
    p.cur = 0; p.cmd = CMD_NONE;
    ui.pl_multi[0] = 1; ui.pl_multi_n = 1;
    pl_menu_action(PM_CROP);
    chk("crop stops when cur is cropped", (int)p.cmd, CMD_STOP);
    chk("crop kept only the selection", pl.count, 1);
    chk("crop kept the right row", strcmp(pl.items[0].path, r1) == 0, 1);
    /* crop with NO selection is a total no-op — it must not stop play */
    pl_reset(5);
    p.cur = 1; p.cmd = CMD_NONE;
    ui.msg[0] = 0;
    pl_menu_action(PM_CROP);
    chk("crop no-selection: list intact", pl.count, 5);
    chk("crop no-selection: no stop", (int)p.cmd, CMD_NONE);

    /* clear reuses the select-all REM path */
    pl_reset(3);
    p.cur = 1; p.cmd = CMD_NONE;
    pl_menu_action(PM_CLEAR);
    chk("clear emptied the playlist", pl.count, 0);
    chk("clear reset cur", p.cur, -1);
    chk("clear stopped playback", (int)p.cmd, CMD_STOP);

    /* save-as refuses an empty list with the title-strip message (D7);
       the non-empty path opens the SDL browser and is not testable
       headless — SAVE-mode browser internals are pinned below instead */
    pl_reset(0);
    ui.msg[0] = 0;
    pl_menu_action(PM_SAVEAS);
    chk("save-as refuses empty", strcmp(ui.msg, "  playlist empty") == 0, 1);
    chk("save-as left the browser closed", ui.file_saving, 0);
    SDL_Event kk;
    memset(&kk, 0, sizeof kk);
    kk.type = SDL_KEYDOWN;
    kk.key.keysym.sym = SDLK_k;
    kk.key.keysym.mod = KMOD_CTRL;
    chk("Ctrl+K claims in the playlist window", pl_key(&kk), 1);

    /* keyboard shortcuts: claim only what they act on */
    pl_reset(5);
    ui.pl_multi[0] = 0; ui.pl_multi_n = 1;
    {
        SDL_Event c;
        memset(&c, 0, sizeof c);
        c.type = SDL_KEYDOWN;
        c.key.keysym.sym = SDLK_c;
        c.key.keysym.mod = KMOD_CTRL;
        chk("Ctrl+C claims with a selection", pl_key(&c), 1);
        c.key.keysym.mod = 0;
        chk("plain c falls through to the global hotkey", pl_key(&c), 0);
        SDL_Event d;
        memset(&d, 0, sizeof d);
        d.type = SDL_KEYDOWN;
        d.key.keysym.sym = SDLK_DOWN;
        /* plain arrows walk the playlist cursor now (they used to be
           the global volume hotkey) */
        chk("plain Down claims (cursor walk)", pl_key(&d), 1);
        chk("Down landed cursor on row 1", pl_sel_has(1) && ui.pl_multi_n == 1, 1);
        d.key.keysym.sym = SDLK_UP;
        chk("plain Up claims", pl_key(&d), 1);
        chk("Up returned cursor to row 0", pl_sel_has(0) && ui.pl_multi_n == 1, 1);
        /* Ctrl+arrow stays unclaimed (modifier passes through) */
        d.key.keysym.sym = SDLK_DOWN;
        d.key.keysym.mod = KMOD_CTRL;
        chk("Ctrl+Down passes through", pl_key(&d), 0);
        /* re-arm selection {0} for the Alt row-move test */
        d.key.keysym.mod = KMOD_ALT;
        ui.pl_multi[0] = 0; ui.pl_multi_n = 1;
        char r0[LV_MAX_PATH];
        snprintf(r0, sizeof r0, "%s/r0.mp3", dir);
        chk("Alt+Down claims with a selection", pl_key(&d), 1);
        chk("Alt+Down moved row 0 down", strcmp(pl.items[1].path, r0) == 0, 1);
        ui.pl_multi_n = 0;
        chk("Alt+Down passes with no selection", pl_key(&d), 0);
        /* volume now lives on +/- (and the numpad); '=' shares the + key
           on common layouts, so it raises too; arrows never touch it */
        p.vol = 64;
        SDL_Event vk;
        memset(&vk, 0, sizeof vk);
        vk.type = SDL_KEYDOWN;
        vk.key.keysym.sym = SDLK_UP;   handle_key(&vk);
        vk.key.keysym.sym = SDLK_DOWN; handle_key(&vk);
        chk("arrows no longer change volume", p.vol, 64);
        vk.key.keysym.sym = SDLK_PLUS;   handle_key(&vk);
        chk("+ raises by 8", p.vol, 72);
        vk.key.keysym.sym = SDLK_EQUALS; handle_key(&vk);
        chk("= raises too (shares the + key)", p.vol, 80);
        vk.key.keysym.sym = SDLK_KP_PLUS; handle_key(&vk);
        chk("numpad + raises", p.vol, 88);
        vk.key.keysym.sym = SDLK_MINUS;    handle_key(&vk);
        chk("- lowers by 8", p.vol, 80);
        vk.key.keysym.sym = SDLK_KP_MINUS; handle_key(&vk);
        chk("numpad - lowers", p.vol, 72);
        p.vol = 126; vk.key.keysym.sym = SDLK_PLUS; handle_key(&vk);
        chk("+ clamps at 128", p.vol, 128);
        p.vol = 2; vk.key.keysym.sym = SDLK_MINUS; handle_key(&vk);
        chk("- clamps at 0", p.vol, 0);
    }

    /* ---- SAVE-mode browser internals (P2) ------------------------------ */
    pl_reset(5);
    ui.file_saving = 1;
    lv_strlcpy(ui.file_dir, dir, sizeof ui.file_dir);
    {
        /* Enter with a bare name: lands in the shown folder, .m3u appended */
        lv_strlcpy(ui.file_field, "saved", sizeof ui.file_field);
        ui.file_err = 0;
        file_save_enter();
        chk("save cleared SAVE mode on success", ui.file_saving, 0);
        char sp[LV_MAX_PATH];
        snprintf(sp, sizeof sp, "%s/saved.m3u", dir);
        FILE *f = fopen(sp, "r");
        chk("save wrote the file", f != NULL, 1);
        if (f) fclose(f);
        /* reload through the loader and compare rows (real round-trip) */
        Playlist rt;
        pl_init(&rt);
        chk("round-trip loads", pl_load_m3u(&rt, sp), 0);
        chk("round-trip row count", rt.count, 5);
        int same = rt.count == 5;
        for (int i = 0; same && i < 5; i++) {
            char want[LV_MAX_PATH];
            snprintf(want, sizeof want, "%s/r%d.mp3", dir, i);
            if (strcmp(rt.items[i].path, want)) same = 0;
        }
        chk("round-trip rows identical", same, 1);
        pl_clear(&rt);
        remove(sp);
    }
    {
        /* empty playlist refuses the write and flags the field */
        pl_reset(0);
        ui.file_saving = 1;
        ui.file_err = 0;
        lv_strlcpy(ui.file_field, "nope", sizeof ui.file_field);
        file_save_enter();
        chk("save refuses an empty playlist", ui.file_err, 1);
        chk("refusal keeps SAVE mode open", ui.file_saving, 1);
        {
            char no[LV_MAX_PATH];
            snprintf(no, sizeof no, "%s/nope.m3u", dir);
            struct stat st;
            chk("no file was written", stat(no, &st), -1);
        }
        ui.file_saving = 0;
        ui.file_err = 0;
    }
    {
        /* ADD DIR is inert in SAVE mode: press paints nothing */
        ui.file_saving = 1;
        ui.pressed = B_NONE;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEBUTTONDOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.motion.x = FILE_W - 60 + 20; e.motion.y = FILE_H - 16 + 6;
        handle_file_mouse(&e);
        chk("ADD DIR inert in SAVE mode", ui.pressed, B_NONE);
        ui.file_saving = 0;
    }

    /* ---- row drag-and-drop (P4): drag-12 state machine ---------------
       helpers pl_down/pl_motion/pl_up/path_at live at file scope */
    {
        /* drag row 0 to after row 2: gap = (y-ptext.y+8)/17 with the
           half-row bias; the set rides, cur rides, no play fires */
        pl_reset(5);
        p.cur = 0;
        p.cmd = CMD_NONE;
        pl_down(0, 1);
        chk("press arms the row drag", ui.drag, 12);
        chk("press selects the row", ui.pl_multi_n, 1);
        chk("no gap before motion", ui.pl_drop, -1);
        pl_motion(40 + 2 * 17 + 9);              /* row 2 bottom half */
        chk("gap lands after row 2", ui.pl_drop, 3);
        pl_up(2);
        chk("release clears the drag", ui.drag, 0);
        {
            char want0[LV_MAX_PATH], want1[LV_MAX_PATH], want2[LV_MAX_PATH];
            snprintf(want0, sizeof want0, "%s/r1.mp3", dir);
            snprintf(want1, sizeof want1, "%s/r2.mp3", dir);
            snprintf(want2, sizeof want2, "%s/r0.mp3", dir);
            chk("drag order row0", strcmp(path_at(0), want0) == 0, 1);
            chk("drag order row1", strcmp(path_at(1), want1) == 0, 1);
            chk("drag order row2", strcmp(path_at(2), want2) == 0, 1);
        }
        chk("selection rides to the landing", pl_sel_has(2), 1);
        chk("set stays one row", ui.pl_multi_n, 1);
        chk("anchor follows", ui.pl_sel, 2);
        chk("cur follows the block", p.cur, 2);
        chk("drag does not play", (int)p.cmd, CMD_NONE);

        /* jitter below the 4px arm threshold: the deferred double-click
           still plays and nothing moves */
        pl_reset(5);
        p.cmd = CMD_NONE;
        pl_down(1, 2);
        pl_motion(40 + 17 + 8 + 2);              /* 2px wobble */
        chk("jitter never arms the drop", ui.pl_drop, -1);
        pl_up(1);
        chk("jittered double-click plays", (int)p.cmd, CMD_PLAY);
        chk("jittered dbl arg", p.cmd_arg, 1);

        /* double-click that DRAGS: the play is eaten (plan P4 rule) */
        pl_reset(5);
        p.cmd = CMD_NONE;
        pl_down(1, 2);
        pl_motion(40 + 3 * 17 + 9);              /* row 3 bottom half */
        pl_up(3);
        chk("drag eats the double-click play", (int)p.cmd, CMD_NONE);
        {
            char w[LV_MAX_PATH];
            snprintf(w, sizeof w, "%s/r1.mp3", dir);
            chk("dbl-drag landed at gap", strcmp(path_at(3), w) == 0, 1);
        }
        chk("dbl-drag selection follows", pl_sel_has(3), 1);

        /* press inside a multi-row set: the set survives the press and
           the drag carries ALL of it; a released press still collapses */
        pl_reset(5);
        pl_click(1, 0, 0, 1);
        pl_click(3, 1, 0, 1);
        pl_down(1, 1);
        chk("in-set press keeps the set while armed", ui.pl_multi_n, 2);
        pl_motion(40 + 3 * 17 + 9);              /* gap 4 */
        pl_up(3);
        {
            char w0[LV_MAX_PATH], w1[LV_MAX_PATH], w2[LV_MAX_PATH], w3[LV_MAX_PATH];
            snprintf(w0, sizeof w0, "%s/r0.mp3", dir);
            snprintf(w1, sizeof w1, "%s/r2.mp3", dir);
            snprintf(w2, sizeof w2, "%s/r1.mp3", dir);
            snprintf(w3, sizeof w3, "%s/r3.mp3", dir);
            chk("set-drag order 0", strcmp(path_at(0), w0) == 0, 1);
            chk("set-drag order 1", strcmp(path_at(1), w1) == 0, 1);
            chk("set-drag order 2", strcmp(path_at(2), w2) == 0, 1);
            chk("set-drag order 3", strcmp(path_at(3), w3) == 0, 1);
        }
        chk("set-drag keeps two rows", ui.pl_multi_n, 2);
        chk("set-drag rows remapped", pl_sel_has(2) && pl_sel_has(3), 1);
        pl_reset(5);
        pl_click(1, 0, 0, 1);
        pl_click(3, 1, 0, 1);
        pl_down(1, 1);
        pl_up(1);                                /* never moved: collapse */
        chk("released in-set press collapses", ui.pl_multi_n, 1);
        chk("collapse lands on the pressed row", pl_sel_has(1), 1);

        /* press OUTSIDE the set collapses immediately (old semantics,
           so a drag from outside carries just that row) */
        pl_down(4, 1);
        chk("outside press collapses at down", ui.pl_multi_n, 1);
        chk("outside press selects row", pl_sel_has(4), 1);
        pl_up(4);                                /* no motion: no drop */

        /* edge auto-scroll: 3 visible lines, 10 rows, scrolled to 3;
           press VISIBLE row 4 (line 1 of the view), motion into the
           top edge scrolls up and the gap follows the scrolled view */
        pl_reset(10);
        ui.pl_h = 40 + 12 + 3 * 17;              /* exactly 3 lines */
        ui.pl_scroll = 3;
        {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_MOUSEBUTTONDOWN;
            e.motion.x = 100; e.motion.y = 40 + 17 + 8;   /* line 1 */
            e.button.button = SDL_BUTTON_LEFT;
            e.button.state = SDL_PRESSED;
            handle_pl_mouse(&e);
        }
        chk("auto-scroll pressed row 4", ui.pl_sel, 4);
        pl_motion(40 + 4);                       /* top-edge zone */
        chk("top edge scrolls up", ui.pl_scroll, 2);
        chk("gap follows the scrolled view", ui.pl_drop, 2);
        pl_motion(40 + 3 * 17 - 2);              /* bottom-edge zone */
        chk("bottom edge scrolls back down", ui.pl_scroll, 3);
        pl_up(1);                                /* release on line 1 */
        chk("auto-scroll release clears drag", ui.drag, 0);
        ui.pl_h = 200;
    }

    printf(fails ? "TEST FAILED: %d\n" : "ALL PASS (%d fails)\n", fails);
    return fails != 0;
}
