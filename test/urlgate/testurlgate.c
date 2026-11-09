/* P0 URL gate boundary pins — pure-function level (no ffmpeg, no net).
   build: gcc -std=gnu11 -I../../src -o testurlgate testurlgate.c && ./testurlgate */
#include <stdio.h>
#include "common.h"

static int fails;
#define PIN(cond) do { if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main(void) {
    /* accept: scheme://rest */
    PIN(lv_is_url("http://example.com/a.mp3"));
    PIN(lv_is_url("https://ex.com/x.flac"));
    PIN(lv_is_url("icecast://host:8/stream"));
    PIN(lv_is_url("HTTPS://X/Y"));            /* scheme case is free */
    PIN(lv_is_url("http://127.0.0.1:8000/a.mp3"));
    PIN(lv_is_url("http+rtmp://x/y"));        /* scheme chars */

    /* reject: not a URL shape at all */
    PIN(!lv_is_url(""));
    PIN(!lv_is_url("/home/x/a.mp3"));
    PIN(!lv_is_url("a.mp3"));
    PIN(!lv_is_url("C:\\music\\a.mp3"));
    PIN(!lv_is_url("http:/example.com"));     /* single slash */
    PIN(!lv_is_url("http:/"));
    PIN(!lv_is_url("http://"));               /* empty rest */
    PIN(!lv_is_url("://x"));                  /* empty scheme */
    PIN(!lv_is_url("1http://x"));             /* digit lead */
    PIN(!lv_is_url("ht tp://x"));             /* space in scheme */

    /* reject: hygiene — CR/LF/DEL/space in the rest (request-line
       injection class) */
    PIN(!lv_is_url("http://x\r\nX: evil"));
    PIN(!lv_is_url("http://x\nGET /evil"));
    PIN(!lv_is_url("http://x y"));            /* space */
    PIN(!lv_is_url("http://x\tz"));           /* tab */
    PIN(!lv_is_url("http://x\x7fz"));         /* DEL */
    PIN(!lv_is_url("http://x\x01z"));         /* low control */

    /* accept: percent-encoding and userinfo pass through untouched
       (avformat's job; userinfo is a DISPLAY/LOG concern) */
    PIN(lv_is_url("http://u:p@h/a%20b"));

    /* scheme allowlist: exact members only, case-insensitive */
    PIN(lv_url_scheme_allowed("http://x/y", "http,https,icecast"));
    PIN(lv_url_scheme_allowed("HTTPS://x/y", "http,https,icecast"));
    PIN(lv_url_scheme_allowed("icecast://x/y", "http,https,icecast"));
    PIN(!lv_url_scheme_allowed("file:///etc/passwd", "http,https,icecast"));
    PIN(!lv_url_scheme_allowed("pipe://x", "http,https,icecast"));
    PIN(!lv_url_scheme_allowed("concat:a|b", "http,https,icecast"));
    PIN(!lv_url_scheme_allowed("gopher://x/y", "http,https,icecast"));
    PIN(!lv_url_scheme_allowed("httpx://x/y", "http,https,icecast")); /* prefix trap */
    PIN(!lv_url_scheme_allowed("http://x", "https,icecast"));

    printf(fails ? "URLGATE %d FAIL\n" : "URLGATE ALL PASS\n", fails);
    return fails ? 1 : 0;
}
