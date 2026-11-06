/* unit test: dragging a docked/snapped window AWAY from the main window
   must actually undock it (regression: 2d69fac made the drag incremental
   but re-anchored to the raw mouse every step even while the snap pinned
   the window, so cursor travel against the pin was discarded and the
   12 px DOCK_SNAP could only be crossed by a single motion event > 12 px:
   slow drags never broke free — this test fails on that build).
   No display server needed: the test binary shadows SDL_GetWindowPosition,
   SDL_SetWindowPosition, SDL_GetWindowSize and SDL_GetGlobalMouseState —
   executable definitions win over libSDL2 at link time (same PLT trick as
   the I/O interposers) — and drives the real handle_pl_mouse /
   handle_file_mouse paths plus the exact latched-drag guard line from the
   ui_run loop. Technique otherwise as in test/optenc: #include ui.c, link
   every object EXCEPT main.c.o, ui.c.o AND dispatch.c.o:
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/tpd \
       test/pldock/testpldock.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name dispatch.c.o) \
       build/libmodplug.a build/libsnes_spc.a -lz -lm -lpthread -ldl -lstdc++
   Coverage note: the search window's own copy of the drag arithmetic is
   NOT exercised here (its window lives inside search.c); the fix there is
   structurally identical to drag_move_snap and compile-verified only.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

typedef struct { int x, y, w, h; } FakeWin;
static FakeWin fw_main, fw_pl, fw_file;
static int gmx, gmy;
static Uint32 gmb;
static int fails;
static int refused_calls;   /* SetWindowPosition calls the WM will refuse */
#define WALL_X 60           /* WM clamp: nothing goes left of this */
#define WALL_Y 500          /* WM clamp: nothing goes below this (H) */

static FakeWin *slot(SDL_Window *w) {
    if (w == (SDL_Window *)&fw_main) return &fw_main;
    if (w == (SDL_Window *)&fw_pl) return &fw_pl;
    if (w == (SDL_Window *)&fw_file) return &fw_file;
    return NULL;
}
void SDL_GetWindowPosition(SDL_Window *w, int *x, int *y) { FakeWin *f = slot(w); *x = f->x; *y = f->y; }
void SDL_SetWindowPosition(SDL_Window *w, int x, int y) {
    FakeWin *f = slot(w);
    if (x < WALL_X) { refused_calls++; x = WALL_X; }  /* WM clamp (F/G/I) */
    if (y > WALL_Y) { refused_calls++; y = WALL_Y; }  /* WM clamp (H) */
    f->x = x; f->y = y;
}
void SDL_GetWindowSize(SDL_Window *w, int *W, int *H) { FakeWin *f = slot(w); *W = f->w; *H = f->h; }
Uint32 SDL_GetGlobalMouseState(int *x, int *y) { *x = gmx; *y = gmy; return gmb; }

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

/* press the playlist title strip and drag it DOWN step px per motion
   event for nsteps events, release over the dragged window; returns the
   final pl offset below the dock line (0 = never escaped the snap) */
static int drag_pl_undock(int step, int nsteps, int release_over_main) {
    SDL_Event e;
    int dock_y = fw_main.y + MAIN_H;
    ui.drag = 0; ui.pressed = B_NONE;
    gmb = 0;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_pl_mouse(&e);
    if (ui.drag != 4) { fails++; printf("FAIL playlist title press latched drag (drag=%d)\n", ui.drag); return -999; }
    for (int i = 0; i < nsteps; i++) {
        gmy += step;
        mkmotion(&e, gmx - fw_pl.x, gmy - fw_pl.y);
        loop_guard(&e);
        handle_pl_mouse(&e);
    }
    int lx = gmx - fw_pl.x, ly = gmy - fw_pl.y;
    mkup(&e, lx, ly);
    loop_guard(&e);
    if (release_over_main) handle_main_mouse(&e); /* release arrives at the MAIN window */
    else handle_pl_mouse(&e);
    return fw_pl.y - dock_y;
}

/* same for the file browser snapped to main's right edge, dragged right */
static int drag_file_undock(int step, int nsteps) {
    SDL_Event e;
    int dock_x = fw_main.x + MAIN_W;
    ui.drag = 0; ui.pressed = B_NONE;
    gmb = 0;
    mkdown(&e, 10, 10);
    loop_guard(&e);
    handle_file_mouse(&e);
    if (ui.drag != 9) { fails++; printf("FAIL file title press latched drag (drag=%d)\n", ui.drag); return -999; }
    for (int i = 0; i < nsteps; i++) {
        gmx += step;
        mkmotion(&e, gmx - fw_file.x, gmy - fw_file.y);
        loop_guard(&e);
        handle_file_mouse(&e);
    }
    mkup(&e, gmx - fw_file.x, gmy - fw_file.y);
    loop_guard(&e);
    handle_file_mouse(&e);
    return fw_file.x - dock_x;
}

int main(void) {
    pl_init(&list);
    pl_player_init(&fake_pl, &list);
    ui.pl = &fake_pl;
    ui.wmain = (SDL_Window *)&fw_main;
    ui.wpl = (SDL_Window *)&fw_pl;
    ui.wfile = (SDL_Window *)&fw_file;
    ui.file_visible = 1;
    ui.pl_visible = 1;   /* the main-drag follow path gates on visibility */
    ui.pl_w = PL_W; ui.pl_h = PL_H;
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    fw_file = (FakeWin){ 100 + MAIN_W, 100, 300, MAIN_H + PL_H };
    int dock_y = 100 + MAIN_H;

    /* A: slow undock of the docked playlist: 20 steps of 5 px. The
       escape must accumulate ACROSS steps (the 2d69fac regression
       discarded them and pinned the window forever) */
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H }; ui.pl_docked = 1;
    gmx = 110; gmy = dock_y + 10;
    int off = drag_pl_undock(5, 20, 0);
    chk_min("playlist slow drag undocks (cumulative travel escapes DOCK_SNAP)", off, 1);
    chk("undocked playlist clears pl_docked", ui.pl_docked, 0);

    /* B: fast drag control — always worked, keep it working */
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H }; ui.pl_docked = 1;
    gmx = 110; gmy = dock_y + 10;
    off = drag_pl_undock(30, 5, 0);
    chk_min("playlist fast drag still undocks", off, 1);

    /* C: release routed to the MAIN window (cursor near the dock
       boundary mid-drag), then a second slow drag must still work */
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H }; ui.pl_docked = 1;
    gmx = 110; gmy = dock_y + 10;
    drag_pl_undock(5, 3, 1);
    off = drag_pl_undock(5, 20, 0);
    chk_min("second slow drag after release-over-main undocks", off, 1);

    /* D: dock back under main: dragging main must carry the playlist
       along (2d69fac's follow path stays intact). Press the title strip
       like the real path: drag_begin initializes the refusal state. */
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H }; ui.pl_docked = 1;
    ui.drag = 0; ui.pressed = B_NONE;
    gmx = 110; gmy = 105;
    SDL_Event e1;
    mkdown(&e1, 10, 10);
    loop_guard(&e1);
    handle_main_mouse(&e1);          /* latches drag 3 via drag_begin */
    for (int i = 0; i < 6; i++) {
        gmy = 105 + 10 * (i + 1);
        mkmotion(&e1, 10, gmy - fw_main.y);
        loop_guard(&e1);
        handle_main_mouse(&e1);
    }
    chk("main drag carries the docked playlist (follow)", fw_pl.y, dock_y + 60);
    chk("follow did not undock", ui.pl_docked, 1);
    mkup(&e1, 10, gmy - fw_main.y);
    loop_guard(&e1);
    handle_main_mouse(&e1);

    /* E: file browser side-snap slow undock */
    int fx = drag_file_undock(5, 20);
    chk_min("file browser slow drag undocks (side snap)", fx, 1);

    /* F: WM clamp at a screen edge (fake SetWindowPosition clamps x at
       WALL_X): pushing against the clamp must NOT accumulate phantom
       overshoot — pulling back must move the window at once. This pins
       the property 2d69fac added. It must also not keep RE-REQUESTING
       refused positions: SDL2's X11 backend polls up to 100 ms per
       refused move (SDL_x11window.c X11_SetWindowPosition), so one
       refused call per motion event while pinned starves the event loop
       and prompts the desktop's "app not responding" popup. At most ONE
      refused request per drag is tolerated (the discovery step). */
    refused_calls = 0;
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H };
    ui.pl_docked = 0; ui.drag = 0; ui.pressed = B_NONE;
    gmx = 110; gmy = 110;
    SDL_Event e2;
    mkdown(&e2, 10, 10);
    loop_guard(&e2);
    handle_main_mouse(&e2);          /* latches drag 3, anchors at (110,110) */
    for (int i = 0; i < 5; i++) {    /* push LEFT against the clamp */
        gmx -= 20;
        mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
        loop_guard(&e2);
        handle_main_mouse(&e2);
    }
    chk("clamp: window pinned at the edge", fw_main.x, WALL_X);
    gmx += 20;                       /* pull back: must move IMMEDIATELY */
    mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    chk_min("clamp: pull-back moves at once (no dead zone)", fw_main.x, WALL_X + 1);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);

    /* G: long sustained press against the wall (the popup scenario):
       30 steps of 5 px, then a single 5 px pull-back must move at once,
       and the drag must not have spammed refused requests */
    refused_calls = 0;
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    ui.drag = 0; ui.pressed = B_NONE;
    gmx = 110; gmy = 110;
    mkdown(&e2, 10, 10);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    for (int i = 0; i < 30; i++) {   /* press LEFT into the wall */
        gmx -= 5;
        mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
        loop_guard(&e2);
        handle_main_mouse(&e2);
    }
    chk("sustained press stays pinned at the wall", fw_main.x, WALL_X);
    chk_min("sustained press: refused requests bounded (kills the 100ms tax)",
            refused_calls <= 1 ? 1 : 0, 1);
    gmx += 5;                        /* ONE step back must move the window */
    mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    chk("single pull-back step after 30-step press moves (no retrace)",
        fw_main.x, WALL_X + 5);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);

    /* H: S1 regression — dragging MAIN toward the bottom wall with the
       playlist docked: the FOLLOW path used raw SetWindowPosition calls
       outside drag_step, so while the playlist was pinned at its own
       clamp every motion event fed it a fresh REFUSED request (100 ms
       poll each in SDL's X11 backend -> "app not responding"). The
       follow moves must carry the same refusal discipline: at most ONE
       refused request for the playlist across the whole drag, and the
       playlist must stay pinned at its clamp. */
    refused_calls = 0;
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H };
    ui.pl_docked = 1; ui.drag = 0; ui.pressed = B_NONE;
    gmx = 110; gmy = 110;
    mkdown(&e2, 10, 10);
    loop_guard(&e2);
    handle_main_mouse(&e2);          /* latches drag 3 */
    for (int i = 0; i < 60; i++) {   /* push DOWN: main stays free (ends
                                        at 400), the docked playlist
                                        crosses WALL_Y after step 50 */
        gmy += 5;
        mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
        loop_guard(&e2);
        handle_main_mouse(&e2);
    }
    chk("follow: main reaches 400 unpinned", fw_main.y, 400);
    chk("follow: playlist pinned at its own clamp", fw_pl.y, WALL_Y);
    chk_min("follow: refused requests bounded (kills the wall-spam popup)",
            refused_calls <= 1 ? 1 : 0, 1);
    chk("follow: pinned carry keeps pl_docked", ui.pl_docked, 1);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    /* the playlist pinned at WALL_Y means main (400) OVERLAPS it (main
       bottom 550 > pl top 500): release must pull main back flush ABOVE
       the pinned playlist instead of kicking it off its own wall */
    chk("follow: release restores main flush above pinned playlist",
        fw_main.y, WALL_Y - MAIN_H);

    /* I: wall release kick — after a long press against the left wall,
       the button-up must land the window 1 px off the wall (opposite the
       push) so the NEXT grab starts clear of the clamp: a single 5 px
       pull-back right after the release must move at once. */
    refused_calls = 0;
    fw_main = (FakeWin){ 100, 100, MAIN_W, MAIN_H };
    fw_pl = (FakeWin){ 100, dock_y, PL_W, PL_H };
    ui.pl_docked = 0; ui.drag = 0; ui.pressed = B_NONE;
    gmx = 110; gmy = 110;
    mkdown(&e2, 10, 10);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    for (int i = 0; i < 30; i++) {   /* press LEFT into the wall */
        gmx -= 5;
        mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
        loop_guard(&e2);
        handle_main_mouse(&e2);
    }
    chk("kick: pinned at the wall before release", fw_main.x, WALL_X);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    chk("kick: release lands 1 px off the wall", fw_main.x, WALL_X + 1);
    chk("kick: bounce is not a refused request", refused_calls <= 1 ? 1 : 0, 1);
    /* fresh grab: first pull-back step must move the window at once */
    mkdown(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    gmx += 5;
    mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    chk("kick: first pull-back step after fresh grab moves", fw_main.x, WALL_X + 6);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);

    /* J: the wall-facing window is a CARRIED one: file snapped flush to
       main's LEFT edge, main dragged left into the wall. The file pins at
       WALL_X (one refusal, then clamped follow); main travels into the
       file's territory until its own clamp. Release must NOT kick main 1
       px off the wall (that leaves it inside the file window) — it must
       jump main back flush against the pinned file (dock position), and
       the drag stays bounded: one refused request per window. */
    refused_calls = 0;
    fw_main = (FakeWin){ 400, 100, MAIN_W, MAIN_H };
    fw_file = (FakeWin){ 100, 100, 300, MAIN_H + PL_H };
    ui.pl_docked = 0; ui.drag = 0; ui.pressed = B_NONE;
    ui.snap[1].w = ui.wfile; ui.snap[1].dx = -fw_file.w; ui.snap[1].dy = 0;
    gmx = 410; gmy = 110;
    mkdown(&e2, 10, 10);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    for (int i = 0; i < 100; i++) {   /* push LEFT: file crosses WALL_X
                                         first, main long after */
        gmx -= 5;
        mkmotion(&e2, gmx - fw_main.x, gmy - fw_main.y);
        loop_guard(&e2);
        handle_main_mouse(&e2);
    }
    chk("file-carry: main pinned at the wall mid-drag", fw_main.x, WALL_X);
    chk("file-carry: file pinned at its clamp", fw_file.x, WALL_X);
    chk("file-carry: refused requests bounded (<=1 per window)",
        refused_calls <= 2 ? 1 : 0, 1);
    mkup(&e2, gmx - fw_main.x, gmy - fw_main.y);
    loop_guard(&e2);
    handle_main_mouse(&e2);
    chk("file-carry: release restores main flush against the pinned file",
        fw_main.x, WALL_X + fw_file.w);
    chk("file-carry: pinned file stays at its clamp", fw_file.x, WALL_X);
    ui.snap[1].w = NULL;

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails ? fails : 0);
    return fails != 0;
}
