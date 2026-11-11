/* unit test: EQ window polish — centered slider group, centered preset
   row with a new RESET button, ON/OFF moved to the lower right.
   Pins: geometry symmetry (margins differ by at most 1 px), the OLD ON
   spot no longer latches a press, the NEW ON spot latches and toggles on
   release, ROCK via the shared EQ_PRE table sets a curve, and RESET
   zeroes all ten bands while leaving the preamp untouched.
   Technique + link recipe as in test/pldock (#include ui.c; every object
   EXCEPT main.c.o, ui.c.o, decode.c.o AND stb_vorbis.c.o — dispatch.c.o
   MUST stay: it defines dec_open/dec_ffmpeg_probe_info that ui.c refs,
   while decode.c.o + stb_vorbis.c.o are the pre-split stale leftovers
   that collide with the dec_*.dir tier objects):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/teq \
       test/eq/testeq.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       build/libmodplug.a build/libsnes_spc.a -lz -lm -lpthread -ldl -lstdc++
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void mkdown(SDL_Event *e, int lx, int ly) {
    memset(e, 0, sizeof *e);
    e->type = SDL_MOUSEBUTTONDOWN;
    e->button.button = SDL_BUTTON_LEFT;
    e->button.state = SDL_PRESSED;
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

/* click a button center-style: down and up at (x,y); returns what the
   press latched */
static int click_eq(int x, int y) {
    SDL_Event e;
    ui.pressed = B_NONE; ui.drag = 0;
    mkdown(&e, x, y);
    handle_eq_mouse(&e);
    int latch = ui.pressed;
    mkup(&e, x, y);
    handle_eq_mouse(&e);
    return latch;
}

int main(void) {
    static Player fp;
    static Playlist list;
    pl_init(&list);
    pl_player_init(&fp, &list);
    ui.pl = &fp;

    /* geometry: slider group centered (preamp + 10 bands, span 252 in
       EQ_W=315 -> margins 31/32) */
    chk("slider group centered (margin diff <= 1 px)",
        (EQ_W - (EQ_BAND_X(9) + 12)) - EQ_PRE_X, 1);
    /* preset group: one button width (34 px) left of true center (85)
       38 px clear of RESET */
    chk("presets sit 34 px left of center", 85 - EQ_PRE[0].x, 34);
    chk("preset row gap rhythm (4 px between presets)",
        (EQ_PRE[1].x - (EQ_PRE[0].x + EQ_PRE[0].w)), 4);
    /* five controls in the shared table: FLAT ROCK POP JAZZ,
       RESET tucked beside ON (a centered RESET was too easy to
       press by accident) */
    chk("table has 5 controls", (long)(sizeof EQ_PRE / sizeof EQ_PRE[0]), 5);
    chk("RESET present", EQ_PRE[4].id, EQB_RESET);

    /* ON/OFF moved to the lower right: on the preset row, 10 px from the
       right edge, and clear of every preset button */
    Rect on = eq_on_rect();
    chk("ON sits on the preset row", on.y, EQ_PRE_Y);
    chk("ON 10 px off the right edge", EQ_W - (on.x + on.w), 10);
    chk("RESET adjacent to ON (4 px gap, same row)",
        on.x - (EQ_PRE[4].x + EQ_PRE[4].w), 4);
    chk("RESET clear of the presets (38 px after the left shift)",
        EQ_PRE[4].x - (EQ_PRE[3].x + EQ_PRE[3].w), 38);

    /* old ON spot (top-left) must latch nothing any more */
    chk("old ON spot is dead", click_eq(10, 28), B_NONE);

    /* new ON spot latches and toggles EQ on release */
    chk("new ON spot latches EQB_ON", click_eq(on.x + on.w / 2, on.y + 6), EQB_ON);
    chk("EQ enabled after one toggle", eq_enabled(&fp.eq), 1);

    /* ROCK through the shared table: band curve applied */
    click_eq(EQ_PRE[1].x + EQ_PRE[1].w / 2, EQ_PRE_Y + 6);  /* ROCK */
    chk("ROCK lifts band 0", fp.eq.band_db[0] > 0 ? 1 : 0, 1);

    /* preamp survives the reset; bands all land on 0 */
    eq_set_preamp(&fp.eq, 5.0f);
    click_eq(EQ_PRE[4].x + EQ_PRE[4].w / 2, EQ_PRE_Y + 6);  /* RESET */
    int zero = 1;
    for (int i = 0; i < EQ_BANDS; i++) if (fp.eq.band_db[i] != 0.0f) zero = 0;
    chk("RESET zeroes all ten bands", zero, 1);
    chk("RESET leaves the preamp alone", fp.eq.preamp_db > 4.99f && fp.eq.preamp_db < 5.01f, 1);

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails ? fails : 0);
    return fails != 0;
}
