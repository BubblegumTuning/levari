/* tagwords probe: print each fixture's decoded title AND artist as raw hex
   bytes (a-prefixed corpus modes target the artist field via TPE1/id3v1).
   Compile against the tag.c under test:
     gcc -std=gnu11 -I<src/tag> probe.c <tag.c> -o probe
   argv[1..] are fixture paths. Output: PATH<TAB>hex(title)<TAB>hex(artist). */
#include <stdio.h>
#include "tag.h"

static void puthex(const char *s) {
    for (const char *p = s; *p; p++) printf("%02x", (unsigned char)*p);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        Tags t;
        tag_init(&t);
        tag_read(argv[i], &t);
        printf("%s\t", argv[i]);
        puthex(t.title);
        putchar('\t');
        puthex(t.artist);
        putchar('\n');
    }
    return 0;
}
