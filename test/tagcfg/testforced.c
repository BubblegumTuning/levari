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
/* id3v2.3: TIT2 Shift_JIS テスト (8365 8358 8367, katakana evidence) +
   TALB half-width kana bytes B3 B9 BF (cp932: ｽｹｿ U+FF73 FF79 FF7F) —
   the SHIFT-JIS feature fixtures (internal SJIS plan D1/D2/D6) */
static const unsigned char sj[] =
    "ID3\x03\x00\x00\x00\x00\x00\x1f"
    "TIT2\x00\x00\x00\x07\x00\x00\x00\x83\x65\x83\x58\x83\x67"
    "TALB\x00\x00\x00\x04\x00\x00\x00\xb3\xb9\xbf";

int main(void) {
    FILE *f = fopen("/tmp/scratch-p2/tf-cyr.mp3", "wb");
    fwrite(cyr, 1, sizeof cyr - 1, f); fclose(f);
    f = fopen("/tmp/scratch-p2/tf-lat.mp3", "wb");
    fwrite(lat, 1, sizeof lat - 1, f); fclose(f);
    f = fopen("/tmp/scratch-p2/tf-sj.mp3", "wb");
    fwrite(sj, 1, sizeof sj - 1, f); fclose(f);
    Tags t;

    tag_init(&t); tag_read("/tmp/scratch-p2/tf-cyr.mp3", &t);
    chk("auto cyr block title", t.title, "Ні");
    chk("auto cyr block album", t.album, "Київ");

    tag_init(&t); tag_read("/tmp/scratch-p2/tf-sj.mp3", &t);
    chk("auto sjis kana pins block (テスト)", t.title, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88");
    chk("auto never decodes half-width kana (D2: unassigned FFFD)",
        t.album, "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");

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

    /* forced SHIFT-JIS waives the kana gate (the album had none — half-width
       bytes) and accepts the half-width kana band (D2). NOTE: the cp1251
       cyrillic byte range overlaps A1-DF, so under a FORCED shiftjis
       verdict cyr bytes in that band read as half-width kana — the mode
       means "trust the user". Fail-close (D6) still fires on bytes the
       scan rejects everywhere (invalid trail, 0x80/0xA0, F0-FF,
       truncated pair): the latin artist's E9+space does. */
    tag_set_legacy_default(LV_ENC_SJIS);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-sj.mp3", &t);
    chk("forced sjis title", t.title, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88");
    chk("forced sjis half-width kana decodes (D2)",
        t.album, "\xef\xbd\xb3\xef\xbd\xb9\xef\xbd\xbf");      /* ｽｹｿ */
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-cyr.mp3", &t);
    chk("forced sjis cyr bytes in hw band read half-width",
        t.title, "\xef\xbe\x8d\xef\xbd\xb3");                  /* ﾍｳ */
    chk("forced sjis cyr album fail-closes on truncated pair (D6)",
        t.album, "Київ");
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-lat.mp3", &t);
    chk("forced sjis valid pair decodes", t.title, "Caf\xe9\xad\x8f"); /* 魏 */
    chk("forced sjis latin artist fail-closes to latin-1",
        t.artist, "Beyonc\xc3\xa9 Knowles");

    tag_set_legacy_default(LV_ENC_AUTO);
    tag_init(&t); tag_read("/tmp/scratch-p2/tf-lat.mp3", &t);
    chk("auto restored (block latin-1 verdict)", t.title, "Caf\xc3\xa9\xc2\xb0");

    printf(fails ? "FAILURES: %d\n" : "all forced-encoding checks pass\n", fails);
    return fails != 0;
}
