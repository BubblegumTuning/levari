/* URL window pins (streaming-plan UI-entry gate): lazy inert prefill from
   urlIndex, the Enter gate (lv_is_url && LV_URL_SCHEMES) with the file_err
   rejection pattern, transient-play on accept (one-row swapped list +
   CMD_PLAY, the row never persists), Esc close with no post, the D3 leak
   fix (a URL typed into the FILE-browser field plays transient and closes
   the browser — never a persistent row), the B_URL button toggle, and the
   paste-hygiene helper the field's ctrl-V rides.

   Technique (see skill): #include ui.c, link every object EXCEPT
   main.c.o, ui.c.o and the stale tier-split leftovers (decode.c.o,
   stb_vorbis.c.o) + the dec_*.dir tier objects. -DLV_URL_ENABLED=1 is
   REQUIRED: the URL window compiles out without it (that is the
   LV_STREAMING=OFF shape — this test only exists when the define is on).

     SDL=$(pkg-config --cflags --libs sdl2)
     gcc -std=gnu11 -DLV_URL_ENABLED=1 -Isrc -Isrc/ui -Isrc/tag $SDL \
       -o build/testurlwin test/urlwin/testurlwin.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' \
         ! -name main.c.o ! -name ui.c.o ! -name decode.c.o \
         ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) \
       $(find build -name 'libsnes_spc.a' | head -1) \
       -lz -lm -lpthread -ldl -lstdc++

   Run with a scratch HOME (the urlIndex stub is written at
   $HOME/.levari/urlIndex) and SDL_VIDEODRIVER=dummy (dummy creates real
   windows, so urlwin_open's SDL path runs unmodified). NO clipboard pins:
   the dummy clipboard is flaky on this host — tf_sanitize_paste is pinned
   as a pure function instead (tf_paste is 3 lines on top of it + SDL glue
   exercised by the file field in the shipped binary).
*/
#include "../../src/ui/ui.c"
#include <stdlib.h>

static Player fp;
static Playlist list;
static int fails;

static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}
static void chks(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void write_stub(const char *body) {
    char p[LV_MAX_PATH];
    char dir[LV_MAX_PATH];
    snprintf(dir, sizeof dir, "%s/.levari", getenv("HOME"));
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/urlIndex", dir);
    FILE *f = fopen(p, "w");
    fputs(body, f);
    fclose(f);
}

static SDL_Event keyev(SDL_Keycode k, Uint16 mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    e.key.keysym.mod = mod;
    return e;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !strstr(home, "scratch")) {
        printf("run with a scratch HOME (urlIndex stub lands under it)\n");
        return 2;
    }
    pl_player_init(&fp, &list);
    ui.pl = &fp;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("FAIL sdl: %s\n", SDL_GetError()); return 2; }
    ui.wmain = SDL_CreateWindow("t", 0, 0, MAIN_W, MAIN_H, SDL_WINDOW_BORDERLESS);
    if (!ui.wmain) { printf("FAIL dummy window: %s\n", SDL_GetError()); return 2; }

    /* ---- empty state: no urlIndex file -> empty prefill ---- */
    {
        char p[LV_MAX_PATH];
        snprintf(p, sizeof p, "%s/.levari/urlIndex", home);
        remove(p);
    }
    urlwin_open();
    chk("open: visible", ui.url_visible, 1);
    chk("open: focused", ui.url_focus, 1);
    chk("open: no error", ui.url_err, 0);
    chks("open: no index -> empty field", ui.url_field, "");

    /* ---- lazy prefill: the banked row lands in the field, inert ---- */
    url_close();
    write_stub("http://host/a.flac\n");
    urlwin_open();
    chks("prefill: banked row in field", ui.url_field, "http://host/a.flac");
    chk("prefill: caret at end", ui.url_tf.caret, (int)strlen(ui.url_field));

    /* ---- tampered row: prefilled INERT, Enter-gate rejects ---- */
    url_close();
    write_stub("file:///x\n");
    urlwin_open();
    chks("tampered: prefilled untouched (inert display)", ui.url_field, "file:///x");
    SDL_Event e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("tampered: no play posted", fp.cmd, CMD_NONE);
    chk("tampered: red error raised", ui.url_err, 1);
    chk("tampered: window STAYS open", ui.url_visible, 1);
    chks("tampered: text kept for correction", ui.url_field, "file:///x");

    /* ---- Esc closes without posting ---- */
    e = keyev(SDLK_ESCAPE, 0);
    url_key(&e);
    chk("esc: window closed", ui.url_visible, 0);
    chk("esc: focus cleared", ui.url_focus, 0);
    chk("esc: no play posted", fp.cmd, CMD_NONE);

    /* ---- oversize tampered row: never enters the field ---- */
    {
        char big[LV_MAX_PATH + 8];
        memset(big, 'A', sizeof big - 1);
        big[sizeof big - 1] = 0;
        url_close();
        write_stub(big);
        urlwin_open();
        chk("oversize: field stays empty", ui.url_field[0], 0);
        chk("oversize: window still opens", ui.url_visible, 1);
    }

    /* ---- empty field Enter: no error, no post, stays open ---- */
    e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("empty Enter: no error", ui.url_err, 0);
    chk("empty Enter: no post", fp.cmd, CMD_NONE);
    chk("empty Enter: stays open", ui.url_visible, 1);

    /* ---- typed junk: Enter gate rejects, text survives ---- */
    url_text("not a url");
    e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("junk: no play posted", fp.cmd, CMD_NONE);
    chk("junk: red error raised", ui.url_err, 1);
    chks("junk: text kept", ui.url_field, "not a url");
    url_close();

    /* ---- valid URL: Enter plays transient, closes window ---- */
    url_close();
    write_stub("http://stream.example/live\n");
    urlwin_open();
    ui.url_ff_ok = 1; /* play pins host-independent */
    e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("play: CMD_PLAY posted", fp.cmd, CMD_PLAY);
    chk("play: posted at row 0", fp.cmd_arg, 0);
    chk("play: exactly one row", list.count, 1);
    chks("play: row is the URL verbatim", list.items[0].path, "http://stream.example/live");
    chk("play: window closed", ui.url_visible, 0);

    /* ---- ffmpeg availability (dec_ffmpeg_probe): the window must say
       IN ITSELF when streaming cannot ever work (no libav): Enter with
       the flag forced off stays inert (no post, window open, NO url-shape
       error — the notice is the explanation). Probe semantics: 1 loadable,
       2 missing, 0 no backend compiled in — anything != 1 is inert. ---- */
    chk("open: url_ff_ok mirrors the live probe", ui.url_ff_ok,
        dec_ffmpeg_probe() == 1 ? 1 : 0);
    chk("open: url_ff_code mirrors the raw probe", ui.url_ff_code, dec_ffmpeg_probe());
    /* ABI mismatch (probe 3) must read differently than absence (2):
       a box with an old ffmpeg must not be told "not found" */
    chks("notice(2): absence wording", url_ff_notice(2),
         "ffmpeg not found - install it or set libav.dir");
    chks("notice(3): mismatch wording", url_ff_notice(3),
         "ffmpeg found but incompatible - see options");
    url_close();
    write_stub("http://stream.example/noff.mp3");
    urlwin_open();
    fp.cmd = CMD_NONE;  /* previous play pin left CMD_PLAY posted */
    ui.url_ff_ok = 0;
    e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("no-ffmpeg: Enter posts nothing", fp.cmd, CMD_NONE);
    chk("no-ffmpeg: no shape-error flash (notice is the reason)", ui.url_err, 0);
    chk("no-ffmpeg: window stays open", ui.url_visible, 1);
    chks("no-ffmpeg: text kept", ui.url_field, "http://stream.example/noff.mp3");
    ui.url_ff_ok = 1; /* host-independent play pins below */
    e = keyev(SDLK_RETURN, 0);
    url_key(&e);
    chk("ffmpeg restored: same field now plays", fp.cmd, CMD_PLAY);
    chk("ffmpeg restored: window closed", ui.url_visible, 0);
    fp.cmd = CMD_NONE;

    /* ---- B_URL button toggle ---- */
    action(B_URL);
    chk("B_URL: opens window", ui.url_visible, 1);
    action(B_URL);
    chk("B_URL: second press closes", ui.url_visible, 0);

    /* ---- D3 leak: URL typed into the FILE-browser field -> transient
       play + browser closes; never a persistent-row path ---- */
    fp.cmd = CMD_NONE;
    ui.file_visible = 1;
    lv_strlcpy(ui.file_field, "http://typed.example/leak.mp3", sizeof ui.file_field);
    file_field_enter();
    chk("file-field URL: CMD_PLAY posted", fp.cmd, CMD_PLAY);
    chk("file-field URL: browser closed", ui.file_visible, 0);
    chk("file-field URL: still one transient row", list.count, 1);
    chks("file-field URL: row verbatim", list.items[0].path, "http://typed.example/leak.mp3");

    /* ---- paste hygiene (tf_paste is this + SDL clipboard glue) ---- */
    {
        char out[64];
        size_t n;
        n = tf_sanitize_paste("  http://a/x\r\n", out, sizeof out);
        chks("paste: trims crlf/spaces", out, "http://a/x");
        chk("paste: returns length", (long)n, 10);

        n = tf_sanitize_paste("\"C:\\path\\to.mp3\"", out, sizeof out);
        chks("paste: strips one quote pair", out, "C:\\path\\to.mp3");
        chk("paste: quote-pair length", (long)n, 14);

        tf_sanitize_paste("http://x GET / HTTP/1.1\r\nHost: evil", out, sizeof out);
        chks("paste: embedded CR/LF dropped (request-line class)", out,
             "http://x GET / HTTP/1.1Host: evil"); /* spaces are printable: kept; CRLF gone */

        tf_sanitize_paste("http://a\tb\r\nc\x7f", out, sizeof out);
        chks("paste: TAB, CRLF and DEL dropped", out, "http://abc");

        tf_sanitize_paste("stream\x7f" "ing", out, sizeof out);
        chks("paste: DEL dropped mid-string", out, "streaming");

        tf_sanitize_paste("\xc3\xa9", out, sizeof out); /* UTF-8 e-acute */
        chk("paste: UTF-8 lead/cont kept", (long)strlen(out), 2);

        tf_sanitize_paste("\"unpaired", out, sizeof out);
        chks("paste: lone quote kept (no pair)", out, "\"unpaired");

        {
            char big[128];
            memset(big, 'u', sizeof big - 1);
            big[sizeof big - 1] = 0;
            n = tf_sanitize_paste(big, out, 16);
            chk("paste: truncates to cap", (long)n, 15);
            chk("paste: NUL-terminated at cap", out[15], 0);
        }
    }

    if (list.items) pl_clear(&list);
    SDL_DestroyWindow(ui.wmain);
    SDL_Quit();
    printf(fails ? "URLWIN: %d FAIL\n" : "URLWIN: ALL PASS\n", fails);
    return fails != 0;
}
