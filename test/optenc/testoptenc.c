/* unit test: the ADV-tab LEGACY cycle button (OPTB_TENC) end to end —
   hand-built SDL_Events drive handle_opt_mouse (down, up), the cycle
   advances, tag_set_legacy_default takes live effect, and opt_save
   persists cfg.tag_legacy_encoding. Technique (see skill): #include ui.c,
   link every object EXCEPT main.c.o AND ui.c.o:
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/to \
       test/optenc/testoptenc.c $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o) \
       $(find build -name 'libmodplug.a' | head -1) -lz -lm -lpthread -ldl -lstdc++
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

/* id3v2.3 cyr block (same fixture as test/tagcfg/testforced.c):
   frame size includes the enc byte; tag payload = 13 + 15 = 28 */
static const unsigned char cyr[] =
    "ID3\x03\x00\x00\x00\x00\x00\x1c"
    "TIT2\x00\x00\x00\x03\x00\x00\x00\xcd\xb3"
    "TALB\x00\x00\x00\x05\x00\x00\x00\xca\xe8\xbf\xe2";

static void click(int x, int y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.motion.x = x; e.motion.y = y;   /* handle_opt_mouse reads motion.* */
    handle_opt_mouse(&e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.state = SDL_RELEASED;
    handle_opt_mouse(&e);
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) { printf("caller must set scratch HOME/XDG_CONFIG_HOME\n"); return 1; }
    FILE *f = fopen("/tmp/scratch-p2/to-cyr.mp3", "wb");
    fwrite(cyr, 1, sizeof cyr - 1, f); fclose(f);

    cfg_load();
    ui.pl = &fake_pl;                 /* opt_save writes fade_stop through it */
    ui.opt_visible = 1;
    ui.opt_adv = 1;                   /* the LEGACY button lives on the ADV tab */
    ui.pressed = B_NONE;
    ui.opt_enc = cfg.tag_legacy_enc + 1;
    chk("opens at AUTO", ui.opt_enc, 0);

    Rect r = opt_enc_rect();
    click(r.x + r.w / 2, r.y + r.y / 2 + r.h / 2 - r.y / 2);  /* center */
    chk("cycle 1 -> latin1", ui.opt_enc, 1);
    Tags t; tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: latin1 decode", strcmp(t.title, "\xc3\x8d\xc2\xb3") == 0, 1);

    click(186 + 36, 150 + 7);
    chk("cycle 2 -> cp1251", ui.opt_enc, 2);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: cp1251 decode", strcmp(t.title, "\xd0\x9d\xd1\x96") == 0, 1);

    click(186 + 36, 150 + 7);
    chk("cycle 3 -> utf8", ui.opt_enc, 3);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: utf8 forced", strcmp(t.title, "\xcd\xb3") == 0, 1);

    click(186 + 36, 150 + 7);
    chk("cycle wraps to AUTO", ui.opt_enc, 0);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: auto block verdict", strcmp(t.title, "\xd0\x9d\xd1\x96") == 0, 1);

    click(186 + 36, 150 + 7);          /* land on latin1, then SAVE */
    click(186 + 36, 150 + 7);          /* and cp1251 */
    opt_save();
    cfg_load();
    chk("opt_save persisted cp1251", cfg.tag_legacy_enc, LV_ENC_CP1251);
    char path[LV_MAX_PATH];
    cfg_path(path, sizeof path);
    f = fopen(path, "r");
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    chk("config file carries the value", strstr(buf, "tag.legacy_encoding = cp1251") != NULL, 1);

    printf(fails ? "FAILURES: %d\n" : "all LEGACY button checks pass\n", fails);
    return fails != 0;
}
