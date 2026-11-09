/* ffver — gate harness for the multi-ABI ffmpeg backend.
   Stage 1: prints the raw dec_ffmpeg_probe_info triad (state, release,
   resolved dir) — the pick/match path.
   Stage 2 (files as args): opens each file through dec_open, reads a
   chunk, seeks, reads again — proves the DECODE path of the picked
   variant, not just the probe. Needs no SDL.
     gcc -std=gnu11 -Isrc -o build/ffver test/ffabi/ffver.c \
       build/CMakeFiles/levari.dir/src/decode/dispatch.c.o \
       build/CMakeFiles/levari.dir/src/config/config.c.o \
       build/CMakeFiles/levari.dir/src/tag/tag.c.o \
       $(find build/CMakeFiles -path '*dec_*.dir*' -name '*.o') \
       build/libmodplug.a build/libsnes_spc.a -lpthread -ldl -lm -lstdc++
*/
#include <stdio.h>
#include "decode/decode.h"

int main(int argc, char **argv) {
    char ver[64] = "", dir[512] = "";
    int rc = dec_ffmpeg_probe_info(ver, sizeof ver, dir, sizeof dir);
    printf("probe=%d ver=%s dir=%s\n", rc, ver, dir);
    for (int i = 1; i < argc; i++) {
        int16_t buf[2048 * 2];
        Decoder *d = dec_open(argv[i]);
        if (!d) { printf("%s: FAIL open\n", argv[i]); continue; }
        int n = dec_read(d, buf, 2048);
        int sk = dec_seek(d, dec_total_frames(d) / 2);
        int m = sk ? -1 : dec_read(d, buf, 2048);
        printf("%s: %dHz %dch %lld frames  read=%d seek=%s re-read=%d\n",
               argv[i], dec_rate(d), dec_channels(d), dec_total_frames(d),
               n, sk ? "FAIL" : "ok", m);
        dec_close(d);
    }
    return 0;
}
