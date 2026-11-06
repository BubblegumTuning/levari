/* search window scrollbar grab pins (2026-11-05).
   The search list's scrollbar thumb is painted INSIDE the list rect's
   right edge; before the fix, BUTTONDOWN had no bar hit-test, so a click
   on the bar selected+played the row underneath (bug report). The
   fix mirrors the playlist drag==5 house pattern: press anywhere on the
   bar track grabs it (search drag == 2), motion maps pointer y
   proportionally over the scroll range, release clears.

   Compile (from the repo root, after `cmake --build build`):

     gcc -std=gnu11 test/searchsb/testsearchsb.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' ! -name main.c.o ! -name search.c.o ! -name dispatch.c.o) \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       $(pkg-config --cflags --libs sdl2) -lz -lm -lpthread -ldl -lstdc++ -o build/testsearchsb

   No SDL_Init and no real windows: every tested path is pure arithmetic
   over statics (row/do_jump and window-drag branches are deliberately
   never triggered — do_jump dereferences the player pointer). RED at
   HEAD~1: the bar press leaves drag == 0 (the click fell through to the
   row branch).
*/
#include "../../src/ui/search.c"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static SDL_Event mev(Uint32 type, int x, int y, Uint32 state) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    /* union aliasing trap: SDL_MouseButtonEvent.button (offset 16) and
       .clicks (18) sit INSIDE motion.state (offset 16, 4 bytes) — measured
       on this host's SDL2. For MOTION events the motion fields must be
       written LAST (motion.state overwrites the button bytes; writing
       button first would fabricate "button held" into every motion);
       for button events, button fields LAST and motion.state untouched
       (x/y at 20/24 do not alias). */
    if (type == SDL_MOUSEMOTION) {
        e.button.button = SDL_BUTTON_LEFT;
        e.button.clicks = 1;
        e.motion.x = (Sint32)x; e.motion.y = (Sint32)y; e.motion.state = state;
    } else {
        e.motion.x = (Sint32)x; e.motion.y = (Sint32)y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.clicks = 1;
    }
    return e;
}

static void press(int x, int y) {
    SDL_Event e = mev(SDL_MOUSEBUTTONDOWN, x, y, SDL_BUTTON_LMASK);
    search_mouse(&e);
}
static void motion(int x, int y, Uint32 state) {
    SDL_Event e = mev(SDL_MOUSEMOTION, x, y, state);
    search_mouse(&e);
}
static void up(int x, int y) {
    SDL_Event e = mev(SDL_MOUSEBUTTONUP, x, y, 0);
    search_mouse(&e);
}

int main(void) {
    SRCH_LIST_RECT;
    int lines = list.h / SRCH_LINEH;
    int barx = list.x + list.w - 4;      /* inside the 7px bar column */
    int midy = list.y + list.h / 2;

    /* bar press grabs the scrollbar, not a row */
    nres = 100; scroll = 0; drag = 0; sel = -1;
    press(barx, midy);
    chk("bar press sets scroll-drag (not a row click)", drag, 2);
    chk("bar press leaves scroll", scroll, 0);

    /* motion maps pointer y proportionally over the scroll range */
    int y2 = list.y + 3 * list.h / 4;
    motion(barx, y2, SDL_BUTTON_LMASK);
    long want = (long)(y2 - list.y) * (100 - lines) / list.h;
    chk("drag maps y to scroll", scroll, want);

    /* clamped at the bottom of the track */
    motion(barx, list.y + list.h + 64, SDL_BUTTON_LMASK);
    chk("scroll clamped to nres-lines", scroll, 100 - lines);

    /* release clears the grab */
    up(barx, list.y + list.h + 64);
    chk("release clears drag", drag, 0);

    /* left edge of the bar column grabs too, and never disturbs scroll */
    scroll = 5;
    press(list.x + list.w - 7, midy);
    chk("bar left edge grabs", drag, 2);
    chk("press alone leaves scroll", scroll, 5);

    /* a motion with the button already gone drops the grab */
    motion(barx, midy, 0);
    chk("released mid-drag drops drag", drag, 0);

    /* few results: no bar drawn, no grab (row branch, idx >= nres => inert) */
    nres = 3; scroll = 0;
    press(barx, list.y + list.h - 2);
    chk("no bar grab when results fit", drag, 0);

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails);
    return fails != 0;
}
