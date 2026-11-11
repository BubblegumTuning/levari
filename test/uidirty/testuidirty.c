/* unit test: main-window dirty-rect sub-regions (cpu_floor_hunting item 1).
   Pins:
     a) mask math: main_draw_regions() — any full bit forces all, plain
        producers pass through, foreign window bits are invisible;
     b) region state after a pass: the strip-only pass marks ONE rect and
        widgets outside it report clean (rgn_hit), the TIME pass marks the
        time/info/seek rects and reports the strip clean, a full pass
        marks nothing (rgn_n == 0 = everything);
     c) the visual pin: with a marquee-length title, a TITLE-only pass
        repaints INSIDE R_TSCROLL ONLY (byte-compare of the whole surface
        outside the strip) and advances the marquee; a TIME-only pass
        leaves every pixel outside time+info+seek byte-identical AND does
        not creep the marquee;
     b2) native-RGB565 direct-write pins: the glyph writers land exact on
        a 2bpp surface (the Uint32-clobber class item 4's framebuffer hint
        exposed on the depth-16 floor) — RED 7 clobber px on the pre-fix
        skin.c, 0 after;
     d) the present pin (item 4, shadowed SDL_UpdateWindowSurface/Rects):
        region passes present their rects via SDL_UpdateWindowSurfaceRects
        and NEVER the whole-window call; a full pass keeps the whole-window
        call; a NULL window presents nothing.
   Technique + link recipe as in test/plmultisel (#include ui.c; every
   levari.dir object EXCEPT main.c.o and ui.c.o, dispatch.c.o IN, the two
   pre-split leftovers decode.c.o / stb_vorbis.c.o EXCLUDED, all dec_*.dir
   tier objects IN):
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -DLV_URL_ENABLED=1 -Isrc -Isrc/ui -Isrc/tag $SDL \
       -o /tmp/tuidirty test/uidirty/testuidirty.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o \
         ! -name stb_vorbis.c.o) \
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

/* ---- item 4 present pins: shadowed SDL present entry points --------------
   PLT trick as in test/pldock: the executable's definitions beat the SDL2
   shared library at link time, so the present calls inside the included
   ui.c land here and the pins see WHICH update form the pass chose and
   with WHICH rects. Signatures must match SDL_video.h (non-static). */
static int present_full, present_rects, present_rect_n;
static SDL_Rect present_rects_last[4];
Uint32 SDL_GetWindowFlags(SDL_Window *w) { (void)w; return SDL_WINDOW_SHOWN; }
int SDL_UpdateWindowSurface(SDL_Window *w) { (void)w; present_full++; return 0; }
int SDL_UpdateWindowSurfaceRects(SDL_Window *w, const SDL_Rect *rects, int n) {
    (void)w; present_rects++; present_rect_n = n;
    for (int i = 0; i < n &&
         i < (int)(sizeof present_rects_last / sizeof present_rects_last[0]); i++)
        present_rects_last[i] = rects[i];
    return 0;
}

static Uint8 *ref;          /* pixel snapshot of ui.smain */
static size_t refsz;

static void snap(void) {
    SDL_Surface *s = ui.smain;
    size_t sz = (size_t)s->pitch * s->h;
    if (sz != refsz) { refsz = sz; ref = realloc(ref, sz); }
    memcpy(ref, s->pixels, sz);
}

/* count surface pixels OUTSIDE Rect r that differ from the snapshot */
static long diff_outside(const Rect *r) {
    SDL_Surface *s = ui.smain;
    long d = 0;
    for (int y = 0; y < s->h; y++) {
        const Uint8 *row = (const Uint8 *)s->pixels + (size_t)y * s->pitch;
        const Uint8 *refrow = ref + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            int in = x >= r->x && x < r->x + r->w &&
                     y >= r->y && y < r->y + r->h;
            if (in) continue;
            size_t o = (size_t)x * 4;
            if (memcmp(row + o, refrow + o, 4)) d++;
        }
    }
    return d;
}

/* count surface pixels INSIDE Rect r that differ from the snapshot */
static long diff_inside(const Rect *r) {
    SDL_Surface *s = ui.smain;
    long d = 0;
    for (int y = r->y; y < r->y + r->h && y < s->h; y++) {
        const Uint8 *row = (const Uint8 *)s->pixels + (size_t)y * s->pitch;
        const Uint8 *refrow = ref + (size_t)y * s->pitch;
        for (int x = r->x; x < r->x + r->w && x < s->w; x++) {
            size_t o = (size_t)x * 4;
            if (memcmp(row + o, refrow + o, 4)) d++;
        }
    }
    return d;
}

/* pixels outside a SET of rects */
static long diff_outside3(const Rect *a, const Rect *b, const Rect *c) {
    SDL_Surface *s = ui.smain;
    long d = 0;
    for (int y = 0; y < s->h; y++) {
        const Uint8 *row = (const Uint8 *)s->pixels + (size_t)y * s->pitch;
        const Uint8 *refrow = ref + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            int in = (x >= a->x && x < a->x + a->w && y >= a->y && y < a->y + a->h) ||
                     (b && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) ||
                     (c && x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h);
            if (in) continue;
            size_t o = (size_t)x * 4;
            if (memcmp(row + o, refrow + o, 4)) d++;
        }
    }
    return d;
}

int main(void) {
    static Player p;
    static Playlist list;
    pl_init(&list);
    pl_player_init(&p, &list);
    ui.pl = &p;

    ui.smain = SDL_CreateRGBSurfaceWithFormat(0, MAIN_W, MAIN_H, 32,
                                              SDL_PIXELFORMAT_ARGB8888);
    if (!ui.smain) { puts("FAIL no surface"); return 1; }

    /* --- mask math ------------------------------------------------------- */
    chk("pos-poll bit stays a TIME region",
        main_draw_regions(D_M_TIME), D_M_TIME);
    chk("strip bit stays a strip region",
        main_draw_regions(D_M_TITLE), D_M_TITLE);
    chk("viz bit stays a strip region",
        main_draw_regions(D_M_VIZ), D_M_VIZ);
    chk("D_MAIN forces the full region set",
        main_draw_regions(D_MAIN), (long)D_M_FULL);
    chk("D_M_BG forces the full region set",
        main_draw_regions(D_M_BG), (long)D_M_FULL);
    chk("D_ALL (focus gain, expose) forces full",
        main_draw_regions(D_ALL), (long)D_M_FULL);
    chk("full bit alongside a producer still means full",
        main_draw_regions(D_M_TITLE | D_MAIN), (long)D_M_FULL);
    chk("foreign window bits never repaint main",
        main_draw_regions(D_PL | D_EQ | D_SRCH), 0);
    chk("producer union passes through intact",
        main_draw_regions(D_M_TITLE | D_M_VIZ | D_M_TIME),
        (long)(D_M_TITLE | D_M_VIZ | D_M_TIME));

    /* --- the visual pin: marquee-only repaint ---------------------------- */
    /* long-enough ASCII title to take the marquee path (2x 9px chars must
       exceed tw = R_TSCROLL.w - 4 = 189) */
    ui.vis_on = 0;
    ui.msg[0] = 0;
    ui.menu = 0;
    lv_strlcpy(ui.title, "THE LONGEST TRACK TITLE YOU EVER DID SEE",
               sizeof ui.title);
    p.state = ST_PLAY;
    p.rate = 44100;
    p.pos_frames = 44100 * 5;
    p.total_frames = 44100 * 200;

    draw_main(D_M_FULL);
    chk("full pass marks no rects (rgn_n == 0 = all)", (long)rgn_n, 0);
    chk("full pass: everything hits", rgn_hit(&R_SEEK) && rgn_hit(&R_TSCROLL), 1);
    snap();

    int off0 = ui.scroll_off;
    draw_main(D_M_TITLE);
    chk("marquee flagged by the strip pass", ui.marquee, 1);
    chk("marquee advanced exactly 2 px", ui.scroll_off - off0, 2);
    chk("TITLE pass: strip marked once", (long)rgn_n, 1);
    chk("TITLE pass: strip widget hits", rgn_hit(&R_TSCROLL), 1);
    chk("TITLE pass: seek clean", rgn_hit(&R_SEEK), 0);
    chk("TITLE pass: time box clean", rgn_hit(&R_TIME), 0);
    chk("TITLE pass: NOTHING outside the strip changed",
        diff_outside(&R_TSCROLL), 0);
    chk("TITLE pass: the strip itself did change",
        diff_inside(&R_TSCROLL) > 0, 1);

    /* --- the TIME pin: 1 s poll repaint ---------------------------------- */
    /* fresh reference: the strip content legitimately moved with the
       marquee pass above; the TIME pass must not touch anything outside
       time+info+seek relative to what is on screen NOW */
    snap();
    p.pos_frames = 44100 * 6;      /* the 1 s poll would have advanced it */
    off0 = ui.scroll_off;
    draw_main(D_M_TIME);
    chk("TIME pass: three rects (time+info+seek)", (long)rgn_n, 3);
    chk("TIME pass: seek hits", rgn_hit(&R_SEEK), 1);
    chk("TIME pass: strip clean", rgn_hit(&R_TSCROLL), 0);
    chk("TIME pass: marquee did NOT creep", ui.scroll_off - off0, 0);
    chk("TIME pass: nothing outside time+info+seek changed",
        diff_outside3(&R_TIME, &R_INFO, &R_SEEK), 0);

    /* --- skipped strip keeps the marquee flag frozen ---------------------- */
    chk("marquee flag survives a strip-skipping pass", ui.marquee, 1);

    /* --- item 4: present picks the update form from the region state ------ */
    static int fake_w;
    ui.wmain = (SDL_Window *)&fake_w;
    present_full = present_rects = present_rect_n = 0;
    win_present_main(NULL);
    chk("NULL main window: no present at all", present_full + present_rects, 0);

    draw_main(D_M_TITLE);
    win_present_main(ui.wmain);
    chk("TITLE pass presents rect update, never the whole-window one",
        (present_rects == 1 && present_full == 0), 1);
    chk("the presented rect IS the strip",
        present_rects_last[0].x == R_TSCROLL.x &&
        present_rects_last[0].y == R_TSCROLL.y &&
        present_rects_last[0].w == R_TSCROLL.w &&
        present_rects_last[0].h == R_TSCROLL.h, 1);

    present_full = present_rects = present_rect_n = 0;
    draw_main(D_M_TIME);
    win_present_main(ui.wmain);
    chk("TIME pass presents its three rects, no full update",
        (present_rects == 1 && present_full == 0 && present_rect_n == 3), 1);

    present_full = present_rects = present_rect_n = 0;
    draw_main(D_M_FULL);
    win_present_main(ui.wmain);
    chk("full pass keeps the whole-window update",
        (present_full == 1 && present_rects == 0), 1);

    /* --- native-565 direct-write pins (the Uint32-clobber class) ----------
       SDL_HINT_FRAMEBUFFER_ACCELERATION=0 makes a software window surface
       the display's NATIVE framebuffer — RGB565 on a depth-16 X server.
       font_putc/font_putcp16_clip used to write Uint32 unconditionally:
       on a 2bpp surface each glyph pixel also wrote the two pixel slots
       right of it AND the px*bpp stride landed half-offset. '!' is the
       discriminator: FONT['!'] = {0,0,0x5F,0,0} — bits 0..4 AND bit 6
       (five-stroke plus the dot): six lit pixels, one column. A clobber
       leaves a stray green or a non-magenta pixel. */
    SDL_Surface *s565 = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 16,
                                                       SDL_PIXELFORMAT_RGB565);
    if (s565) {
        Uint32 m565 = SDL_MapRGB(s565->format, 255, 0, 255);
        Uint32 g565 = SDL_MapRGB(s565->format, 0, 255, 0);
        SDL_FillRect(s565, NULL, m565);
        font_putc(s565, 2, 0, '!', g565);
        SDL_LockSurface(s565);
        int bad = 0, green = 0;
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                Uint16 v = *(const Uint16 *)((const Uint8 *)s565->pixels
                              + (size_t)yy * s565->pitch + 2 * (size_t)xx);
                int is_g = v == (Uint16)g565;
                int expect = (xx == 4) && (yy < 5 || yy == 6);
                if (expect) { if (is_g) green++; else bad++; }
                else if (is_g || v != (Uint16)m565) bad++;
            }
        SDL_UnlockSurface(s565);
        chk("'!' on RGB565: six exact glyph pixels (stroke + dot)", green, 6);
        chk("'!' on RGB565: zero clobbered pixels", bad, 0);

        /* the 16px writer too: any glyph lands exact, nothing else moves */
        SDL_FillRect(s565, NULL, m565);
        font_putcp16_clip(s565, 0, 0, 'A', g565, 0, 8);
        SDL_LockSurface(s565);
        bad = green = 0;
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                Uint16 v = *(const Uint16 *)((const Uint8 *)s565->pixels
                              + (size_t)yy * s565->pitch + 2 * (size_t)xx);
                if (v == (Uint16)g565) green++;
                else if (v != (Uint16)m565) bad++;
            }
        SDL_UnlockSurface(s565);
        chk("16px glyph on RGB565: glyph present", green > 0, 1);
        chk("16px glyph on RGB565: zero clobbered pixels", bad, 0);
        SDL_FreeSurface(s565);
    }

    free(ref);
    SDL_FreeSurface(ui.smain);
    printf(fails ? "FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails != 0;
}
