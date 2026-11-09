/* unit test: the ABOUT tab of the options window. Hand-built SDL_Events
   drive handle_opt_mouse: the ABOUT tab button switches ui.opt_tab, the
   ADV page's controls stay dead on the ABOUT page, and a BASIC-page
   field click cannot take focus there. draw_opt() renders every page
   (smoke) and the ABOUT page is saved as a BMP when LV_ABOUT_BMP names
   a path (layout eyeball). Technique (see skill): #include ui.c, link
   every object EXCEPT main.c.o, ui.c.o and dispatch.c.o:
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/ta \
       test/about/testabout.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name dispatch.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++
   Run with scratch HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR set by the caller.
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static Player fake_pl;
static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void down(int x, int y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.motion.x = x; e.motion.y = y;
    handle_opt_mouse(&e);
}
static void up(int x, int y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_RELEASED;
    e.motion.x = x; e.motion.y = y;
    handle_opt_mouse(&e);
}
static void click(int x, int y) { down(x, y); up(x, y); }

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    ui.sopt = SDL_CreateRGBSurfaceWithFormat(0, OPT_W, OPT_H, 32, SDL_PIXELFORMAT_RGB24);
    if (!ui.sopt) { printf("no surface\n"); return 1; }
    ui.pl = &fake_pl;                 /* opt_save writes fade_stop through it */
    ui.pressed = B_NONE;
    ui.opt_visible = 1;
    ui.opt_focus = -1;
    ui.opt_hover = -1;
    ui.opt_status[0] = 0;
    ui.opt_tab = 0;

    /* every page ACTUALLY paints: sentinel-fill the surface first and
       require draw_opt() to have touched pixels — a bare non-crash smoke
       cannot catch a page that paints nothing */
    {
        int painted = 1;
        for (int tab = 0; tab < 3; tab++) {
            ui.opt_tab = tab;
            if (SDL_LockSurface(ui.sopt) != 0) { painted = -1; break; }
            Uint8 *p = (Uint8 *)ui.sopt->pixels;
            for (int y = 0; y < ui.sopt->h; y++)
                memset(p + (size_t)y * ui.sopt->pitch, 0x5A, (size_t)ui.sopt->w * 3);
            SDL_UnlockSurface(ui.sopt);
            draw_opt();
            if (SDL_LockSurface(ui.sopt) != 0) { painted = -1; break; }
            p = (Uint8 *)ui.sopt->pixels;
            int touched = 0;
            for (int y = 0; !touched && y < ui.sopt->h; y++) {
                Uint8 *row = p + (size_t)y * ui.sopt->pitch;
                for (int x = 0; x < (int)(ui.sopt->w * 3); x++)
                    if (row[x] != 0x5A) { touched = 1; break; }
            }
            SDL_UnlockSurface(ui.sopt);
            if (!touched) painted = 0;
        }
        ui.opt_tab = 0;
        chk("all three pages paint pixels", painted == 1, 1);
    }

    /* ABOUT tab click lands, and back to BASIC and ADV */
    Rect tu = opt_tab_about_rect();
    click(tu.x + tu.w / 2, tu.y + tu.h / 2);
    chk("ABOUT tab -> 2", ui.opt_tab, 2);
    Rect tb = opt_tab_basic_rect();
    click(tb.x + tb.w / 2, tb.y + tb.h / 2);
    chk("BASIC tab -> 0", ui.opt_tab, 0);
    Rect ta = opt_tab_adv_rect();
    click(ta.x + ta.w / 2, ta.y + ta.h / 2);
    chk("ADV tab -> 1", ui.opt_tab, 1);
    click(tu.x + tu.w / 2, tu.y + tu.h / 2);

    /* ADV controls are dead on the ABOUT page */
    Rect te = opt_enc_rect();
    down(te.x + te.w / 2, te.y + te.h / 2);
    chk("ADV LEGACY dead on ABOUT", ui.pressed, B_NONE);

    /* BASIC fields cannot take focus on the ABOUT page */
    Rect f = opt_field_rect(0);
    down(f.x + 10, f.y + f.h / 2);
    chk("no field focus on ABOUT", ui.opt_focus, -1);

    /* SAVE/CLOSE remain armed on the ABOUT page (shared footer) */
    Rect sv = opt_save_rect();
    down(sv.x + sv.w / 2, sv.y + sv.h / 2);
    chk("SAVE live on ABOUT", ui.pressed, OPTB_SAVE);
    up(sv.x + sv.w / 2, sv.y + sv.h / 2);  /* fires opt_save */

    draw_opt();
    const char *bmp = getenv("LV_ABOUT_BMP");
    if (bmp) {
        ui.opt_tab = 2;
        ui.pressed = B_NONE;
        draw_opt();
        if (SDL_SaveBMP(ui.sopt, bmp) != 0) { fails++; printf("FAIL save BMP: %s\n", SDL_GetError()); }
        else printf("PASS saved %s\n", bmp);
    }
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
