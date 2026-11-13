/* search window scrollbar grab pins (2026-11-05).
   The search list's scrollbar thumb is painted INSIDE the list rect's
   right edge; before the fix, BUTTONDOWN had no bar hit-test, so a click
   on the bar selected+played the row underneath (bug report). The
   fix mirrors the playlist drag==5 house pattern: press anywhere on the
   bar track grabs it (search drag == 2), motion maps pointer y
   proportionally over the scroll range, release clears.
   Since 2026-11-13 the file also pins the LIB-scope queue-ADD button
   (draw+mouse scope-gated, best match, duplicates allowed) and the +
   keyboard action (queues, its TEXTINPUT swallowed so the query stays
   clean); those pins run against a zeroed fake Player — do_jump remains
   untriggered.

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
    chk("drag maps y to scroll", scroll, want);   /* mirrors the pinned contract
        (search.c scrollbar entry: scroll=(y-list.y)*(nres-lines)/list.h clamped) */

    /* order/range properties the single-point mirror above cannot catch:
       the mapping must be monotone and stay inside [0, nres-lines] */
    {
        int prev = -1, ok = 1;
        for (int y = list.y; y <= list.y + list.h; y += 7) {
            motion(barx, y, SDL_BUTTON_LMASK);
            if (scroll < prev || scroll > 100 - lines) ok = 0;
            prev = scroll;
        }
        chk("scrollbar maps monotonically inside [0, nres-lines]", ok, 1);
        motion(barx, list.y + list.h, SDL_BUTTON_LMASK);   /* exact bottom edge (the 7-px sweep may stop short) */
        chk("track bottom reaches the last page", scroll, 100 - lines);
    }

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

    /* ---- queue-ADD button + the + key (2026-11-13) ----
       A zeroed fake Player stands in for the live one: the queue path
       only touches sp->mtx (zero-init == PTHREAD_MUTEX_INITIALIZER on
       glibc), sp->pl and pl_add_file. do_jump remains untriggered. */
    {
        static Player ffp;
        static Playlist fpl;
        sp = &ffp; ffp.pl = &fpl; pl_init(&fpl);
        nres = 0; res_push("/tmp/qq.mp3", "Q", "A", -1);
        Rect qb = srch_queue_rect();
        int qx = qb.x + qb.w / 2, qy = qb.y + qb.h / 2;

        /* invisible control = dead control: PL scope gates the mouse too */
        scope = 0; pressed = SB_NONE;
        press(qx, qy);
        chk("queue dead in PL scope", pressed, SB_NONE);
        up(qx, qy);
        chk("PL-scope click queues nothing", fpl.count, 0);

        /* LIB scope: down arms, up appends the BEST match */
        scope = 1;
        press(qx, qy);
        chk("queue arms in LIB scope", pressed, SB_QUEUE);
        up(qx, qy);
        chk("up queues best match", fpl.count, 1);
        chk("queued path = res[0]", strcmp(fpl.items[0].path, "/tmp/qq.mp3") == 0, 1);

        /* already in the list: a second copy is the contract */
        press(qx, qy); up(qx, qy);
        chk("duplicate queued on purpose", fpl.count, 2);

        /* keyboard: + queues, and the TEXTINPUT the key emits is eaten
           so the query stays clean; KP variant included */
        SDL_Event ke;
        memset(&ke, 0, sizeof ke);
        ke.type = SDL_KEYDOWN;
        ke.key.keysym.sym = SDLK_PLUS;
        search_key(&ke);
        chk("+ key queues", fpl.count, 3);
        chk("+ key left the query empty", query[0] == 0, 1);
        search_text("+");
        chk("plus TEXTINPUT swallowed, not typed", query[0] == 0, 1);
        chk("swallow did not queue twice", fpl.count, 3);
        ke.key.keysym.sym = SDLK_KP_PLUS;
        search_key(&ke);
        chk("keypad + queues", fpl.count, 4);

        /* the swallow is one-shot: normal typing resumes untouched */
        scope = 0;
        search_text("z");
        chk("typing after the swallow works", strcmp(query, "z") == 0, 1);

        /* no results: button and key stay inert */
        nres = 0;
        press(qx, qy); up(qx, qy);
        ke.key.keysym.sym = SDLK_PLUS;
        search_key(&ke);
        chk("empty results keep queue inert", fpl.count, 4);
    }

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails);
    return fails != 0;
}
