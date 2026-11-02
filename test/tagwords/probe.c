/* tagwords probe: print each fixture's decoded title as raw hex bytes.
   Compile against the tag.c under test:
     gcc -std=gnu11 -I<src/tag> probe.c <tag.c> -o probe
   argv[1..] are fixture paths. Output: PATH<TAB>hex(title) per line. */
#include <stdio.h>
#include "tag.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        Tags t;
        tag_init(&t);
        tag_read(argv[i], &t);
        printf("%s\t", argv[i]);
        for (const char *p = t.title; *p; p++) printf("%02x", (unsigned char)*p);
        putchar('\n');
    }
    return 0;
}
