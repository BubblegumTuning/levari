/* tagwords probe: print each fixture's decoded title, artist AND album as
   raw hex bytes (a-prefixed corpus modes target the artist field via
   TPE1/id3v1; mf/vmf modes assert per field).
   Env LEVARI_FORCE_LEGACY = latin1|cp1251|utf8|shiftjis applies a
   tag_set_legacy_default() override for the whole run (the corpus 'fs'
   mode re-runs fixtures under forced SHIFT-JIS).
   Compile against the tag.c under test:
     gcc -std=gnu11 -I<src/tag> probe.c <tag.c> -o probe
   argv[1..] are fixture paths. Output: PATH<TAB>hex(title)<TAB>hex(artist)<TAB>hex(album). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tag.h"

static void puthex(const char *s) {
    for (const char *p = s; *p; p++) printf("%02x", (unsigned char)*p);
}

int main(int argc, char **argv) {
    const char *fx = getenv("LEVARI_FORCE_LEGACY");
    if (fx && *fx) {                            /* unknown values stay AUTO */
        if (!strcmp(fx, "latin1"))      tag_set_legacy_default(LV_ENC_LATIN1);
        else if (!strcmp(fx, "cp1251")) tag_set_legacy_default(LV_ENC_CP1251);
        else if (!strcmp(fx, "utf8"))   tag_set_legacy_default(LV_ENC_UTF8);
        else if (!strcmp(fx, "shiftjis"))
            tag_set_legacy_default(LV_ENC_SJIS);
    }
    for (int i = 1; i < argc; i++) {
        Tags t;
        tag_init(&t);
        tag_read(argv[i], &t);
        printf("%s\t", argv[i]);
        puthex(t.title);
        putchar('\t');
        puthex(t.artist);
        putchar('\t');
        puthex(t.album);
        putchar('\n');
    }
    return 0;
}
