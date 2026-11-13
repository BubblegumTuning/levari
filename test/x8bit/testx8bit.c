/* testx8bit.c — headless behavior test for the 8-bit quantizer.
   Links the pure half of src/ui/x8bit.c via the include-the-.c pattern;
   the Xlib takeover half never compiles active here (LV_X8BIT off), so
   the test needs SDL + X headers only for types, no X server.

   Build (from repo root):
     cc -Isrc $(pkg-config --cflags sdl2) \
        test/x8bit/testx8bit.c -o /tmp/testx8bit $(pkg-config --libs sdl2)
   Run: /tmp/testx8bit   (exit 0 = all pins pass)
   NOTE: without -DLV_X8BIT the Xlib half of x8bit.c compiles out, so this
   test needs no X headers and no X server — the quantizer is plain C. */
#include "../../src/ui/x8bit.c"

static int fails, total;
#define PIN(cond, name) do { total++; if (!(cond)) { fails++; \
    printf("FAIL %s (line %d)\n", name, __LINE__); } } while (0)

/* quantize a 1xN row of a single color at x-phase 0..3, y=0 */
static void strip(unsigned char *dst, int r, int g, int b, int n)
{
    unsigned char src[4 * 64];
    unsigned char cube[256];
    for (int i = 0; i < 256; i++) cube[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) {
        src[i * 4 + 0] = (unsigned char)b;  /* ARGB8888 LE: B,G,R,A */
        src[i * 4 + 1] = (unsigned char)g;
        src[i * 4 + 2] = (unsigned char)r;
        src[i * 4 + 3] = 255;
    }
    x8_quant(src, 4 * n, dst, n, cube, 0, 0, n, 1);
}

int main(void)
{
    unsigned char o[64];
    printf("testx8bit\n");
    x8_q6_init();   /* x8_quant would also lazily init it; pin order demands now */

    PIN(X8_Q6[0][0] == 0 && X8_Q6[15][255] == 5, "q6 bounds");
    int bad = 0;
    for (int p = 0; p < 16; p++)
        for (int v = 0; v < 256; v++)
            if (X8_Q6[p][v] > 5) bad = 1;
    PIN(!bad, "q6 range 0..5 everywhere");

    /* saturated primaries land on the cube corner (phase 0..3 mixed in a
       4-px row: for saturated colors dither leaves no slack, the result
       is exact regardless of phase) */
    strip(o, 255, 0, 0, 4);   /* pure red   -> level (5,0,0) */
    PIN(o[0] == 5 * 36 && o[1] == 5 * 36 && o[2] == 5 * 36 && o[3] == 5 * 36,
        "pure red cube index");
    strip(o, 0, 255, 0, 4);
    PIN(o[0] == 5 * 6, "pure green cube index");
    strip(o, 0, 0, 255, 4);
    PIN(o[0] == 5, "pure blue cube index");
    strip(o, 0, 0, 0, 4);
    PIN(o[0] == 0, "black cube index");
    strip(o, 255, 255, 255, 4);
    PIN(o[0] == 215, "white cube index");

    /* mid gray must dither between 2 and 3 per channel (v=136: level 3
       for ~10/16 of the dither phases) — never clamp to one level */
    strip(o, 136, 136, 136, 64);
    int lo = 0, hi = 0, other = 0, cnt3 = 0;
    for (int i = 0; i < 64; i++) {
        if (o[i] == 2 * 36 + 2 * 6 + 2) lo++;
        else if (o[i] == 3 * 36 + 3 * 6 + 3) { hi++; cnt3++; }
        else other++;
    }
    PIN(!other, "mid gray uses only levels 2/3");
    PIN(hi > 16 && lo > 0, "mid gray dithers both levels");
    PIN(cnt3 * 4 >= 64 && cnt3 * 3 <= 64 * 2, "mid gray level-3 share sane");

    /* the cube table is honored: remapped cells reach the output */
    {
        unsigned char src[4] = { 0, 0, 255, 255 };   /* pure red pixel */
        unsigned char cube[256];
        for (int i = 0; i < 256; i++) cube[i] = 0;
        cube[5 * 36] = 42;
        x8_quant(src, 4, o, 1, cube, 0, 0, 1, 1);
        PIN(o[0] == 42, "cube remap honored");
    }

    printf("%d/%d pins pass\n", total - fails, total);
    return fails ? 1 : 0;
}
