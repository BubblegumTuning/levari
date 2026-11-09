/* URL-window docking gate (2026-11-07 layout):
   the URL window docks flush ABOVE the main window; the edge above main
   is a stack — the URL window always inserts at the bottom (between main
   and whatever else is snapped there), pushing the rest up, and closing
   it merges the stack back down (docked EQ lands flush on main again).
   EQ opened while the URL window holds the dock goes ABOVE it. Both the
   48 px and the 64 px (no-ffmpeg notice) URL heights are pinned.

   Technique (see skill Mouse-state-machine + pldock): #include ui.c,
   shadow the SDL window-position surface with fake windows (executable
   definitions win over libSDL2 at link time), latch every drag ONLY via
   the real press path (drag_begin initializes the refusal state), and
   link every object EXCEPT main.c.o, ui.c.o and the stale tier-split
   leftovers (decode.c.o, stb_vorbis.c.o) + the dec_*.dir tier objects:

     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -DLV_URL_ENABLED=1 -Isrc -Isrc/ui -Isrc/tag $SDL \
       -o build/testsnapwin test/snapwin/testsnapwin.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o \
         ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) \
       $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++

   Run with a scratch HOME: urlwin_open's lazy urlIndex prefill reads
   $HOME/.levari/urlIndex and must stay out of the user's home.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

typedef struct { int x, y, w, h; } FakeWin;
static FakeWin fw_main, fw_pl, fw_eq, fw_url;
static int gmx, gmy;
static Uint32 gmb;
static int fails;
static int refused_calls;
static int wall_top;          /* 0 = off: when set, clamp y BELOW wall_top */

static FakeWin *slot(SDL_Window *w) {
    if (w == (SDL_Window *)&fw_main) return &fw_main;
    if (w == (SDL_Window *)&fw_pl)   return &fw_pl;
    if (w == (SDL_Window *)&fw_eq)   return &fw_eq;
    if (w == (SDL_Window *)&fw_url)  return &fw_url;
    return NULL;
}
void SDL_GetWindowPosition(SDL_Window *w, int *x, int *y) { FakeWin *f = slot(w); *x = f->x; *y = f->y; }
void SDL_SetWindowPosition(SDL_Window *w, int x, int y) {
    FakeWin *f = slot(w);
    if (wall_top && y < wall_top) { refused_calls++; y = wall_top; }  /* top-strut clamp */
    f->x = x; f->y = y;
}
void SDL_GetWindowSize(SDL_Window *w, int *W, int *H) { FakeWin *f = slot(w); *W = f->w; *H = f->h; }
Uint32 SDL_GetGlobalMouseState(int *x, int *y) { *x = gmx; *y = gmy; return gmb; }

/* urlwin_open's SDL path, headless: fake window creation + no-op chrome */
static int create_calls;
SDL_Window *SDL_CreateWindow(const char *t, int x, int y, int w, int h, Uint32 f) {
    (void)t; (void)f;
    create_calls++;
    fw_url = (FakeWin){ x, y, w, h };
    return (SDL_Window *)&fw_url;
}
void SDL_SetWindowSize(SDL_Window *w, int W, int H) { FakeWin *f = slot(w); f->w = W; f->h = H; }
void SDL_ShowWindow(SDL_Window *w) { (void)w; }
void SDL_HideWindow(SDL_Window *w) { (void)w; }
void SDL_RaiseWindow(SDL_Window *w) { (void)w; }
void SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *i) { (void)w; (void)i; }
SDL_Surface *SDL_GetWindowSurface(SDL_Window *w) { (void)w; return NULL; }

static Player fake_pl;
static Playlist list;

static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chk_min(const char *what, long got, long want_min) {
    if (got < want_min) { fails++; printf("FAIL %s: got %ld want >= %ld\n", what, got, want_min); }
    else printf("PASS %s (got %ld)\n", what, got);
}

/* the exact guard the ui_run loop runs before routing mouse events */
static void loop_guard(SDL_Event *e) {
    if (e->type == SDL_MOUSEMOTION && ui.drag &&
        !(e->motion.state & SDL_BUTTON_LMASK)) ui.drag = 0;
}
static void mkdown(SDL_Event *e, int lx, int ly) {
    memset(e, 0, sizeof *e);
    e->type = SDL_MOUSEBUTTONDOWN;
    e->button.button = SDL_BUTTON_LEFT;
    e->button.state = SDL_PRESSED;
    e->motion.x = lx; e->motion.y = ly;
    e->motion.state = SDL_BUTTON_LMASK;
}
static void mkmotion(SDL_Event *e, int lx, int ly) {
    memset(e, 0, sizeof *e);
    e->type = SDL_MOUSEMOTION;
    e->motion.x = lx; e->motion.y = ly;
    e->motion.state = SDL_BUTTON_LMASK;
}
static void mkup(SDL_Event *e, int lx, int ly) {
    memset(e, 0, sizeof *e);
    e->type = SDL_MOUSEBUTTONUP;
    e->button.button = SDL_BUTTON_LEFT;
    e->button.state = SDL_RELEASED;
    e->motion.x = lx; e->motion.y = ly;
    e->motion.state = 0;
}

/* register the startup EQ state: snapped flush above main (as ui_run
   does at creation) — the default the URL window must push */
static void eq_dock_above(void) {
    ui.snap[0].w = ui.weq; ui.snap[0].dx = 0; ui.snap[0].dy = -EQ_H;
    fw_eq.y = fw_main.y - EQ_H;
}
static void clear_snaps(void) {
    for (int i = 0; i < SNAP_NSLOT; i++) ui.snap[i].w = NULL;
}
/* canonical reset: main at (100,300), EQ snapped above (visible), URL
   real-sized (h given) but free at the far side, no URL snap */
static void reset(int url_h) {
    fw_main = (FakeWin){ 100, 300, MAIN_W, MAIN_H };
    fw_pl   = (FakeWin){ 100, 300 + MAIN_H, PL_W, PL_H };
    fw_eq   = (FakeWin){ 100, 300 - EQ_H, EQ_W, EQ_H };
    fw_url  = (FakeWin){ 600, 300, URL_W, url_h };
    clear_snaps();
    eq_dock_above();
    ui.eq_visible = 1;
    ui.pl_visible = 0; ui.pl_docked = 0;
    ui.drag = 0; ui.pressed = B_NONE;
    ui.url_visible = 0;
    refused_calls = 0; wall_top = 0;
}

int main(void) {
    pl_init(&list);
    pl_player_init(&fake_pl, &list);
    ui.pl = &fake_pl;
    ui.wmain = (SDL_Window *)&fw_main;
    ui.wpl   = (SDL_Window *)&fw_pl;
    ui.weq   = (SDL_Window *)&fw_eq;
    ui.wurl  = (SDL_Window *)&fw_url;

    /* A: URL opens into a stacked edge: EQ is snapped above main, so the
       URL window inserts flush above MAIN and EQ is pushed above it */
    reset(URL_H);
    url_dock_above_main();
    chk("A url docks flush above main x", fw_url.x, 100);
    chk("A url docks flush above main y", fw_url.y, 300 - URL_H);
    chk("A url snap slot dy", ui.snap[SNAP_URL].dy, -URL_H);
    chk("A eq pushed above url", fw_eq.y, 300 - URL_H - EQ_H);
    chk("A eq snap dy updated", ui.snap[0].dy, -(URL_H + EQ_H));

    /* B: closing the docked URL merges the stacked EQ back down flush
       onto main: they are docked to each other, so eq gets
       merged down to the main window dock */
    url_close();
    chk("B url snap slot cleared", ui.snap[SNAP_URL].w == NULL, 1);
    chk("B eq merged down onto main", fw_eq.y, 300 - EQ_H);
    chk("B eq snap dy restored", ui.snap[0].dy, -EQ_H);

    /* C: the 64 px no-ffmpeg height docks and stacks with ITS OWN height */
    reset(URL_H_NOFF);
    url_dock_above_main();
    chk("C url 64px docks flush above main", fw_url.y, 300 - URL_H_NOFF);
    chk("C url 64px snap dy", ui.snap[SNAP_URL].dy, -URL_H_NOFF);
    chk("C eq pushed by 64px url", fw_eq.y, 300 - URL_H_NOFF - EQ_H);
    url_close();
    chk("C merge-down after 64px dock", fw_eq.y, 300 - EQ_H);

    /* D: no EQ on the edge: the URL window simply sits above main and
       nothing moves; reopening (even from a side snap) re-docks above */
    reset(URL_H);
    ui.snap[0].w = NULL;                    /* EQ dragged away earlier */
    fw_eq.y = 500; fw_eq.x = 500;
    url_dock_above_main();
    chk("D url alone above main", fw_url.y, 300 - URL_H);
    chk("D free eq untouched", fw_eq.y, 500);
    fw_url = (FakeWin){ 100 + MAIN_W, 300, URL_W, URL_H };  /* side-snapped */
    url_dock_above_main();
    chk("D reopen re-docks above main", fw_url.y, 300 - URL_H);
    chk("D reopen re-docks x", fw_url.x, 100);

    /* E: EQ snapped-above when the URL opens ABOVE it later (url_close
       then a fresh dock with the EQ snap untouched) is case A; the
       reverse ORDER — url docked first, then the EQ re-shown — keeps the
       EQ on its snap offset, so the dock-first order needs no pushback. */
    reset(URL_H);
    url_dock_above_main();
    chk("E url first docks", fw_url.y, 300 - URL_H);
    chk("E eq (already above snap) pushed", fw_eq.y, 300 - URL_H - EQ_H);
    chk("E eq snap dy after url-first", ui.snap[0].dy, -(URL_H + EQ_H));

    /* F: EQ title-strip drag onto the edge stacks ABOVE the docked URL
       (raw target within DOCK_SNAP of my-EQ_H; landing spot = top - wh) */
    reset(URL_H);
    url_dock_above_main();
    ui.snap[0].w = NULL; fw_eq = (FakeWin){ 100, 500, EQ_W, EQ_H };
    SDL_Event e;
    ui.drag = 0; ui.pressed = B_NONE; gmb = 0;
    gmx = 110; gmy = 510;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_eq_mouse(&e);
    chk("F eq title press latches drag 7", ui.drag, 7);
    for (int i = 0; i < 60; i++) {          /* walk up onto the edge */
        gmy -= 6;
        mkmotion(&e, gmx - fw_eq.x, gmy - fw_eq.y);
        loop_guard(&e);
        handle_eq_mouse(&e);
    }
    chk("F eq snaps above docked url", fw_eq.y, 300 - URL_H - EQ_H);
    chk("F eq snap above x", fw_eq.x, 100);
    chk("F eq snap dy stacks over url", ui.snap[0].dy, -(URL_H + EQ_H));
    mkup(&e, gmx - fw_eq.x, gmy - fw_eq.y);
    loop_guard(&e);
    handle_eq_mouse(&e);

    /* G: URL title-strip drag onto the edge: inserts BELOW the snapped EQ
       and pushes it up (drag 13 rides drag_move_snap). The drag must STOP
       inside the DOCK_SNAP slack — dragging THROUGH the zone and past the
       slack correctly pulls the window free again (afc93b5), so the walk
       ends the moment the snap engages plus a couple of within-slack
       steps. Release with no wall must not kick it off the snap. */
    reset(URL_H);
    fw_url = (FakeWin){ 100, 500, URL_W, URL_H };   /* free BELOW main */
    ui.drag = 0; ui.pressed = B_NONE; gmb = 0;
    gmx = 110; gmy = 510;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_url_mouse(&e);
    chk("G url title press latches drag 13", ui.drag, 13);
    /* raw candidate hits the zone edge (my-wh+12 = 264) at step 59 of 4px */
    for (int i = 0; i < 62; i++) {          /* snap at 59, 3 within-slack steps */
        gmy -= 4;
        mkmotion(&e, gmx - fw_url.x, gmy - fw_url.y);
        loop_guard(&e);
        handle_url_mouse(&e);
    }
    chk("G url snaps flush above main (bottom of stack)", fw_url.y, 300 - URL_H);
    chk("G url snap x", fw_url.x, 100);
    chk("G eq pushed above dragged-in url", fw_eq.y, 300 - URL_H - EQ_H);
    mkup(&e, gmx - fw_url.x, gmy - fw_url.y);
    loop_guard(&e);
    handle_url_mouse(&e);
    chk("G release without wall keeps the snap", fw_url.y, 300 - URL_H);

    /* H: slow undock FROM the above snap (the cumulative-travel property
       must hold on the URL slot too): a fresh grab at the title strip,
       5 px steps DOWN — after 12 px of slack is eaten the window must
       follow the cursor again, and the snap slot must clear */
    gmy = fw_url.y + 10;                    /* grab the title strip again */
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_url_mouse(&e);
    chk("H fresh grab latches drag 13", ui.drag, 13);
    for (int i = 0; i < 20; i++) {          /* 100 px down, 5 px per step */
        gmy += 5;
        mkmotion(&e, gmx - fw_url.x, gmy - fw_url.y);
        loop_guard(&e);
        handle_url_mouse(&e);
    }
    mkup(&e, gmx - fw_url.x, gmy - fw_url.y);
    loop_guard(&e);
    handle_url_mouse(&e);
    chk("H slow drag breaks the url above-snap", ui.snap[SNAP_URL].w == NULL, 1);
    chk_min("H undocked url travels with the cursor", fw_url.y, 300 - URL_H + DOCK_SNAP + 1);

    /* I: main drag carries the whole above-stack (URL + EQ follow their
       snap offsets) */
    reset(URL_H);
    url_dock_above_main();
    ui.drag = 0; ui.pressed = B_NONE; gmb = 0;
    gmx = 110; gmy = 305;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_main_mouse(&e);                  /* latches drag 3 via drag_begin */
    for (int i = 0; i < 6; i++) {
        gmy = 305 + 10 * (i + 1);
        mkmotion(&e, 10, gmy - fw_main.y);
        loop_guard(&e);
        handle_main_mouse(&e);
    }
    chk("I main drag carries the url window", fw_url.y, 300 - URL_H + 60);
    chk("I main drag carries the stacked eq", fw_eq.y, 300 - URL_H - EQ_H + 60);
    chk("I carry keeps both snap slots", ui.snap[SNAP_URL].w != NULL && ui.snap[0].w != NULL, 1);
    mkup(&e, 10, gmy - fw_main.y);
    loop_guard(&e);
    handle_main_mouse(&e);

    /* J: refusal discipline on the PUSH path: main sits under a top
       strut, the snapped EQ is clamped there, and the URL window is
       dragged up into the snap zone — the reflow push hits the strut but
       must pay at most ONE refused request for the whole drag (same
       popup class as pldock F/G/H), then the pinned window holds. */
    reset(URL_H);
    wall_top = 100;
    fw_main = (FakeWin){ 100, 160, MAIN_W, MAIN_H };  /* main near the strut */
    fw_eq.y = 160 - EQ_H; fw_eq.x = 100;             /* eq flush above main */
    fw_url = (FakeWin){ 100, 400, URL_W, URL_H };
    ui.drag = 0; ui.pressed = B_NONE; gmb = 0;
    gmx = 110; gmy = 410;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_url_mouse(&e);
    /* snap target my-wh = 112, zone edge 124: enters at step 69 of 4px,
       then 3 within-slack steps keep the pin (reflow pushes every step) */
    for (int i = 0; i < 72; i++) {
        gmy -= 4;
        mkmotion(&e, gmx - fw_url.x, gmy - fw_url.y);
        loop_guard(&e);
        handle_url_mouse(&e);
    }
    chk("J url snapped under the strut", fw_url.y, 160 - URL_H);
    chk("J pushed window pins at the strut", fw_eq.y, 100);
    chk_min("J push path: refused requests bounded", refused_calls <= 1 ? 1 : 0, 1);
    mkup(&e, gmx - fw_url.x, gmy - fw_url.y);
    loop_guard(&e);
    handle_url_mouse(&e);
    wall_top = 0;

    /* K: the real open path (B_URL button): urlwin_open creates the window
       and docks it — relative pin so the host ffmpeg probe decides 48/64 */
    reset(URL_H);
    ui.wurl = NULL; ui.surl = NULL; create_calls = 0;
    urlwin_open();
    chk("K open creates the window once", create_calls, 1);
    chk("K open docks above main x", fw_url.x, 100);
    chk("K open docks above main (live height)", fw_url.y, 300 - fw_url.h);
    chk("K open registers the url snap", ui.snap[SNAP_URL].w == (SDL_Window *)&fw_url, 1);
    chk("K open pushes eq by live height", fw_eq.y, 300 - fw_url.h - EQ_H);
    chk("K open is visible", ui.url_visible, 1);
    /* the B_URL button closes it, and the stack merges down */
    url_close();
    chk("K close merges eq down", fw_eq.y, 300 - EQ_H);
    urlwin_open();
    chk("K reopen reuses + re-docks", fw_url.y, 300 - fw_url.h);
    chk("K reopen create stays one", create_calls, 1);
    url_close();

    /* L: URL closed while it was NOT on the above-stack: the EQ (snapped
       above main all along) must not be twitched by the merge logic */
    reset(URL_H);
    fw_url = (FakeWin){ 100 + MAIN_W + 40, 100, URL_W, URL_H };  /* free, unsnapped */
    url_close();
    chk("L unsnapped url close leaves eq docked", fw_eq.y, 300 - EQ_H);
    chk("L unsnapped url close clears nothing it did not own", ui.snap[0].w != NULL, 1);

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails ? fails : 0);
    return fails != 0;
}
