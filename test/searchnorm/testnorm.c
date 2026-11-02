/* search.c norm() case-folding pins.
   Compile (from the repo root, after `cmake --build build`):

     gcc -std=gnu11 test/searchnorm/testnorm.c \
       $(find build/CMakeFiles/levari.dir -name '*.o' ! -name main.c.o ! -name search.c.o) \
       $(find build -name 'libmodplug.a' | head -1) $(find build -name 'libsnes_spc.a' | head -1) \
       $(pkg-config --libs sdl2) -lz -lm -lpthread -ldl -lstdc++ -o build/testnorm

   Pins per codepoint: input folds to the expected codepoint. Guards the
   0x04C1-0x04CE block where Unicode case pairs are INVERTED (capitals
   ODD): the naive even->odd parity rule folds the SMALL letters into
   different CAPITALS (0x04C2 -> 0x04C3), so a search for a small letter
   never finds it and capitals of other letters collide with it. */
#include "../../src/ui/search.c"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void enc(unsigned cp, char *out) { utf8_enc(cp, out); }

static void pin(unsigned from, unsigned expect, const char *what) {
    char in[8], want[8], got[64];
    memset(in, 0, sizeof in);
    enc(from, in);
    memset(want, 0, sizeof want);
    enc(expect, want);
    norm(in, got, sizeof got);
    if (strcmp(got, want)) {
        printf("FAIL U+%04X -> \"%s\" want U+%04X (%s)\n", from, got, expect, what);
        fails++;
    }
}

int main(void) {
    /* inverted pairs 0x04C1-0x04CE: capitals ODD fold down +1, smalls
       even must NOT move (that is the regression this pins) */
    pin(0x04C1, 0x04C2, "zhe breve cap");
    pin(0x04C2, 0x04C2, "zhe breve small");
    pin(0x04C3, 0x04C4, "ka hook cap");
    pin(0x04C4, 0x04C4, "ka hook small");
    pin(0x04C5, 0x04C6, "el tail cap");
    pin(0x04C6, 0x04C6, "el tail small");
    pin(0x04C7, 0x04C8, "en hook cap");
    pin(0x04C8, 0x04C8, "en hook small");
    pin(0x04C9, 0x04CA, "en tail cap");
    pin(0x04CA, 0x04CA, "en tail small");
    pin(0x04CB, 0x04CC, "khakas che cap");
    pin(0x04CC, 0x04CC, "khakas che small");
    pin(0x04CD, 0x04CE, "em tail cap");
    pin(0x04CE, 0x04CE, "em tail small");

    /* passthroughs: palochka and the historic signs/marks stay unmoved */
    pin(0x04C0, 0x04C0, "palochka");
    pin(0x04CF, 0x04CF, "palochka small");
    pin(0x0482, 0x0482, "uk-fullstop");
    pin(0x0484, 0x0484, "combining mark");
    pin(0x0487, 0x0487, "combining mark");

    /* normal parity pairs keep folding even->odd */
    pin(0x0460, 0x0461, "ot cj");
    pin(0x048A, 0x048B, "short i");
    pin(0x04D0, 0x04D1, "a breve");
    pin(0x04F8, 0x04F9, "ligature we");
    pin(0x0500, 0x0501, "dje complement");
    pin(0x052E, 0x052F, "te with descender");

    /* pre-existing behavior must survive untouched */
    pin(0x0410, 0x0430, "A cyrillic");
    pin(0x042F, 0x044F, "YA cyrillic");
    pin(0x0401, 0x0451, "yo");
    pin(0x0402, 0x0452, "dje");
    pin(0x0406, 0x0456, "byelorussian-i");
    pin(0x040F, 0x045F, "dzhe");
    pin(0x0490, 0x0491, "ukrainian i");

    /* ascii + fullwidth */
    pin('A', 'a', "ascii A");
    pin('Z', 'z', "ascii Z");
    pin('5', '5', "digit");
    pin(0xFF21, 'a', "fullwidth A");
    pin(0xFF41, 'a', "fullwidth a");
    pin(0x3000, ' ', "ideographic space");

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("all norm pins green\n");
    return 0;
}
