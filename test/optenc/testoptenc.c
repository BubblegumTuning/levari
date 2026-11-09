/* unit test: the ADV-tab LEGACY cycle button (OPTB_TENC) end to end —
   hand-built SDL_Events drive handle_opt_mouse (down, up), the cycle
   advances, tag_set_legacy_default takes live effect, and opt_save
   persists cfg.tag_legacy_encoding. Technique (see skill): #include ui.c,
   link every object EXCEPT main.c.o AND ui.c.o:
     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -Isrc -Isrc/ui -Isrc/tag $SDL -o /tmp/to \
       test/optenc/testoptenc.c $(find build/CMakeFiles/levari.dir -name '*.o' \
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
    ui.opt_tab = 1;                   /* the LEGACY button lives on the ADV tab */
    ui.pressed = B_NONE;
    ui.opt_enc = cfg.tag_legacy_enc + 1;
    chk("opens at AUTO", ui.opt_enc, 0);

    Rect r = opt_enc_rect();
    click(r.x + r.w / 2, r.y + r.h / 2);  /* center */
    chk("cycle 1 -> latin1", ui.opt_enc, 1);
    Tags t; tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: latin1 decode", strcmp(t.title, "\xc3\x8d\xc2\xb3") == 0, 1);

    click(r.x + r.w / 2, r.y + r.h / 2);  /* same button, always via the rect — never hardcoded px */
    chk("cycle 2 -> cp1251", ui.opt_enc, 2);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: cp1251 decode", strcmp(t.title, "\xd0\x9d\xd1\x96") == 0, 1);

    click(r.x + r.w / 2, r.y + r.h / 2);  /* same button, always via the rect — never hardcoded px */
    chk("cycle 3 -> utf8", ui.opt_enc, 3);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: utf8 forced", strcmp(t.title, "\xcd\xb3") == 0, 1);

    click(r.x + r.w / 2, r.y + r.h / 2);  /* same button, always via the rect — never hardcoded px */
    chk("cycle 4 -> shiftjis", ui.opt_enc, 4);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    /* forced shiftjis: CD B3 sit in the half-width kana band (D2 forced) */
    chk("live effect: shiftjis forced", strcmp(t.title, "\xef\xbe\x8d\xef\xbd\xb3") == 0, 1);

    click(r.x + r.w / 2, r.y + r.h / 2);  /* same button, always via the rect — never hardcoded px */
    chk("cycle wraps to AUTO", ui.opt_enc, 0);
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live effect: auto block verdict", strcmp(t.title, "\xd0\x9d\xd1\x96") == 0, 1);

    click(r.x + r.w / 2, r.y + r.h / 2);  /* land on latin1, then SAVE */
    click(r.x + r.w / 2, r.y + r.h / 2);  /* and cp1251 */
    opt_save();
    cfg_load();
    chk("opt_save persisted cp1251", cfg.tag_legacy_enc, LV_ENC_CP1251);
    char path[LV_MAX_PATH];
    cfg_path(path, sizeof path);
    f = fopen(path, "r");
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    chk("config file carries the value", strstr(buf, "tag.legacy_encoding = cp1251") != NULL, 1);

    /* close WITHOUT save: cfg is cp1251 (just persisted); cycle live to utf8,
       then opt_close must roll tag.c's live default back to cfg (cp1251) so
       the button re-read on next open and the decoder never disagree. */
    click(r.x + r.w / 2, r.y + r.h / 2);  /* cp1251 -> utf8, applied live */
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("live before close: utf8 forced", strcmp(t.title, "\xcd\xb3") == 0, 1);
    opt_close();
    tag_init(&t); tag_read("/tmp/scratch-p2/to-cyr.mp3", &t);
    chk("close-without-save reverts live default to cfg (cp1251)",
        strcmp(t.title, "\xd0\x9d\xd1\x96") == 0, 1);

    /* ffmpeg detection indicator (ADV page status line): pure label —
       detected reads green "detected" plus the version of the library
       actually resolved (probe_info), detected-but-versionless falls
       back to the bare word honestly, anything else reads red and says
       streaming is unavailable; the prefill helper fills a BLANK
       libav.dir field with the probe's resolved directory only on a
       probe==1 with a reportable dir (a filled field is never touched). */
    {
        int red = -1;
        const char *l = opt_ff_label(1, "7.1.5", &red);
        chk("ff label detected+ver: text", strcmp(l, "ffmpeg: detected 7.1.5") == 0, 1);
        chk("ff label detected+ver: not red", red, 0);
        red = -1;
        l = opt_ff_label(1, "", &red);
        chk("ff label detected no-ver: bare text", strcmp(l, "ffmpeg: detected") == 0, 1);
        chk("ff label detected no-ver: not red", red, 0);
        red = -1;
        l = opt_ff_label(0, "9.0.2", &red);
        chk("ff label missing: names ffmpeg + unavailable",
            strstr(l, "ffmpeg") && strstr(l, "not found") && strstr(l, "streaming"), 1);
        chk("ff label missing: red (version irrelevant)", red, 1);
        chk("ff label missing: version not shown", strstr(l, "9.0.2") == NULL, 1);
        red = -1;
        l = opt_ff_label(3, "6.1.1-3ubuntu5", &red);
        chk("ff label ABI3: names the found release + incompatible",
            strstr(l, "6.1.1-3ubuntu5") && strstr(l, "incompatible"), 1);
        chk("ff label ABI3: red", red, 1);
        chk("ff label ABI3: does not claim 'detected'", strstr(l, "detected") == NULL, 1);
        red = -1;
        l = opt_ff_label(3, "", &red);
        chk("ff label ABI3 no-ver: still incompatible + red",
            strstr(l, "incompatible") && red == 1, 1);
        ui.opt_field[OPTF_LIBAV][0] = 0;
        ff_prefill_libav(1, "/opt/homebrew/opt/ffmpeg/lib");
        chk("prefill: probe ok + blank field fills",
            strcmp(ui.opt_field[OPTF_LIBAV], "/opt/homebrew/opt/ffmpeg/lib") == 0, 1);
        lv_strlcpy(ui.opt_field[OPTF_LIBAV], "~/my/ffmpeg", LV_MAX_PATH);
        ff_prefill_libav(1, "/usr/lib64");
        chk("prefill: filled field never overwritten",
            strcmp(ui.opt_field[OPTF_LIBAV], "~/my/ffmpeg") == 0, 1);
        ui.opt_field[OPTF_LIBAV][0] = 0;
        ff_prefill_libav(2, "/usr/lib64");
        chk("prefill: probe 2 (libav missing) does not fill",
            ui.opt_field[OPTF_LIBAV][0] == 0, 1);
        ff_prefill_libav(3, "/lib/x86_64-linux-gnu");
        chk("prefill: probe 3 (ABI mismatch) still shows WHERE it looked",
            strcmp(ui.opt_field[OPTF_LIBAV], "/lib/x86_64-linux-gnu") == 0, 1);
        ui.opt_field[OPTF_LIBAV][0] = 0;
        ff_prefill_libav(1, "");
        chk("prefill: empty dir report does not fill",
            ui.opt_field[OPTF_LIBAV][0] == 0, 1);
        ui.opt_field[OPTF_LIBAV][0] = 0;
        /* the live probe still agrees with the bare wrapper (contract:
           probe_info's return == probe's return on this host) */
        char vbuf[32], dbuf[LV_MAX_PATH];
        chk("ff probe_info agrees with probe",
            dec_ffmpeg_probe_info(vbuf, sizeof vbuf, dbuf, sizeof dbuf) ==
            dec_ffmpeg_probe(), 1);
    }

    printf(fails ? "FAILURES: %d\n" : "all LEGACY button checks pass\n", fails);
    return fails != 0;
}
