/* urlIndex writer/reader pins (streaming-plan D5 gate).

   Compile (from the repo root, after `cmake --build build`); run with a
   scratch HOME set by the caller (the file lands at $HOME/.levari/urlIndex):

     gcc -std=gnu11 -Isrc test/urlidx/testurlidx.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' ! -name main.c.o \
         ! -name decode.c.o ! -name stb_vorbis.c.o) \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       $(find build -name 'libmodplug.a' | head -1) \
       $(find build -name 'libsnes_spc.a' | head -1) \
       $(pkg-config --libs sdl2) -lz -lm -lpthread -ldl -lstdc++ \
       -o build/testurlidx

   Pins (plan: "urlIndex writer tests — ... length cap; tampered rows
   (bad scheme / CR / oversize) load inert and never auto-open nor
   crash"): save round-trips byte-exactly; save rejects CR/oversize/
   non-allowlisted-scheme (defence in depth — a file:// shape can never
   be banked); save replaces atomically; load hands a tampered row back
   UNTOUCHED (inert display: bad scheme and embedded CR survive so the
   Enter gate rejects them — this test IS the auto-open-free proof: load
   only ever fills a buffer); oversize rows never enter the buffer.
   "no write on failed open, write on working play" is the player bank
   point, e2e-pinned by gatecheck (loopback play vs 404). */
#include "library/library.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int fails;
static void chk(const char *what, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !strstr(home, "scratch")) {
        fprintf(stderr, "run with a scratch HOME (never the user's)\n");
        return 2;
    }
    char path[LV_MAX_PATH];
    snprintf(path, sizeof path, "%s/.levari/urlIndex", home);
    remove(path);

    /* 1. save round-trips byte-exactly */
    const char *u1 = "http://127.0.0.1:8000/sample.mp3";
    chk("save valid url = 0", lib_url_index_save(u1) == 0);
    FILE *f = fopen(path, "rb");
    char raw[4 * LV_MAX_PATH];
    size_t n = f ? fread(raw, 1, sizeof raw - 1, f) : 0;
    if (f) fclose(f);
    raw[n] = 0;
    char want[4 * LV_MAX_PATH];
    snprintf(want, sizeof want, "%s\n", u1);
    chk("file content is exactly the url + newline (byte-exact)",
        n == strlen(want) && !memcmp(raw, want, n));
    char out[LV_MAX_PATH];
    chk("load = 0", lib_url_index_load(out, sizeof out) == 0);
    chk("load returns the saved url verbatim", !strcmp(out, u1));

    /* 2. save replaces the single row (one row = the last working url) */
    const char *u2 = "https://example.org/stream.flac";
    chk("second save = 0", lib_url_index_save(u2) == 0);
    chk("load returns ONLY the newest",
        lib_url_index_load(out, sizeof out) == 0 && !strcmp(out, u2));

    /* 3. save rejections: hygiene bytes, oversize, off-allowlist scheme */
    char cr[64];
    snprintf(cr, sizeof cr, "http://x/%sa", "b\r");
    chk("save with embedded CR refused", lib_url_index_save(cr) != 0);
    char big[LV_MAX_PATH + 64];
    snprintf(big, sizeof big, "http://x/%s", "aaaaaaaaaa");
    memset(big + strlen("http://x/"), 'a', LV_MAX_PATH);
    big[sizeof big - 1] = 0;
    chk("oversize save refused", lib_url_index_save(big) != 0);
    chk("file:// shape save refused (scheme defence in depth)",
        lib_url_index_save("file:///etc/passwd") != 0);
    chk("plain string save refused", lib_url_index_save("not a url") != 0);
    chk("rejected saves left the banked row intact",
        lib_url_index_load(out, sizeof out) == 0 && !strcmp(out, u2));

    /* 4. tampered rows load inert: untouched bytes, no scheme validation,
       no crash — the caller displays, the Enter gate rejects */
    f = fopen(path, "w");
    fprintf(f, "file:///tmp/evil\n");
    fclose(f);
    chk("tampered bad-scheme row loads (inert display contract)",
        lib_url_index_load(out, sizeof out) == 0 &&
        !strcmp(out, "file:///tmp/evil"));
    f = fopen(path, "w");
    fprintf(f, "http://x/y\rZ\n");
    fclose(f);
    chk("tampered CR row loads with the CR PRESERVED (Enter-gate evidence)",
        lib_url_index_load(out, sizeof out) == 0 &&
        !strcmp(out, "http://x/y\rZ") && strchr(out, '\r') != NULL);

    /* 5. oversize tampered row never enters the buffer */
    f = fopen(path, "w");
    fputs("http://x/", f);
    for (int i = 0; i < LV_MAX_PATH; i++) fputc('a', f);
    fputc('\n', f);
    fclose(f);
    out[0] = 0;
    chk("oversize row rejected by load, buffer left empty",
        lib_url_index_load(out, sizeof out) != 0 && out[0] == 0);

    /* 6. empty/absent file = -1 */
    f = fopen(path, "w"); fclose(f);
    chk("empty file = no row", lib_url_index_load(out, sizeof out) != 0);
    remove(path);
    chk("absent file = no row", lib_url_index_load(out, sizeof out) != 0);

    printf(fails ? "URLIDX: %d FAIL\n" : "all urlidx pins green\n", fails);
    return fails ? 1 : 0;
}
