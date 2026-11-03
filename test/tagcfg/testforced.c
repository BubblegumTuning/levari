/* unit test: tag_set_legacy_default (tag.legacy_encoding) override for
   legacy 8-bit fields. Standalone — tag.c never reads cfg:
     gcc -std=gnu11 -Isrc -Isrc/tag -o /tmp/tf test/tagcfg/testforced.c src/tag/tag.c
*/
#include <stdio.h>
#include <string.h>
#include "tag.h"

static int fails;
static void chk(const char *what, const char *got, const char *want) {
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got '%s' want '%s'\n", what, got, want); }
    else printf("PASS %s\n", what);
}

/* id3v2.3: TIT2 'Ні'(cp1251 CD B3) + TALB 'Київ'(cp1251 CA E8 BF E2);
   frame size = payload INCLUDING the enc byte: 1+2 and 1+4; tag = 13+15 = 28 */
static const unsigned char cyr[] =
    "ID3\x03\x00\x00\x00\x00\x00\x1c"
    "TIT2\x00\x00\x00\x03\x00\x00\x00\xcd\xb3"
    "TALB\x00\x00\x00\x05\x00\x00\x00\xca\xe8\xbf\xe2";
/* id3v2.3: TIT2 'Caf\xe9\xb0' + TPE1 'Beyonc\xe9 Knowles' (latin-1 shapes) */
static const unsigned char lat[] =
    "ID3\x03\x00\x00\x00\x00\x00\x2a"
    "TIT2\x00\x00\x00\x06\x00\x00\x00\x43\x61\x66\xe9\xb0"
    "TPE1\x00\x00\x00\x10\x00\x00\x00\x42\x65\x79\x6f\x6e\x63\xe9\x20\x4b\x6e\x6f\x77\x6c\x65\x73";

int main(void) {
    FILE *f = fopen("/tmp/scratch-p2/tf-cyr.mp3", "wb");
    fwrite(cyr, 1, sizeof cyr - 1, f); fclose(f);
    f = fopen("/tmp/scratch-p2/tf-lat.mp3", "wb");
    fwrite(lat, 1, sizeof lat - 1, f); fclose(f);
    Tags t;

    tag_init(&t); tag_read("/tmp/scratch-p2/tf-cyr.mp3", &t);
    chk("auto cyr block title", t.title, "Ні");
    chk("auto cyr block album", t.album, "Київ");

    tag_set_legacy_default(LV_ENC_LATIN1);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-cyr.mp3", &t);
    chk("forced latin1 reads cyr bytes latin-1", t.title, "Í³"); /* Í ³ */
    chk("forced latin1 album", t.album, "Êè¿â");

    tag_set_legacy_default(LV_ENC_CP1251);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-lat.mp3", &t);
    chk("forced cp1251 reads latin block cyrillic", t.title, "Cafй\xc2\xb0");
    chk("forced cp1251 artist", t.artist, "Beyoncй Knowles");

    tag_set_legacy_default(LV_ENC_UTF8);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-cyr.mp3", &t);
    chk("forced utf8 title (valid pair passes through)", t.title, "\xcd\xb3"); /* ͳ U+0373 */
    chk("forced utf8 album falls back to guesser", t.album, "Київ");

    tag_set_legacy_default(LV_ENC_AUTO);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-lat.mp3", &t);
    chk("auto restored (block latin-1 verdict)", t.title, "Caf\xc3\xa9\xc2\xb0");

    printf(fails ? "FAILURES: %d\n" : "all forced-encoding checks pass\n", fails);
    return fails != 0;
}
