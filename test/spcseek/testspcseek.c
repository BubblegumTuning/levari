/* testspcseek — SPC fast-forward seek is abortable (DecOpts.seek_abort).

   Rationale (triage-2026-11-21): an SPC seek is an emulated fast-forward
   (~12 ms CPU per declared second); a crafted ID666 length (gate 0x1FFF s)
   could block the player thread for ~100 s, unreachable by STOP. decode.c
   now polls DecOpts.seek_abort between skip chunks. This harness carries
   decode.c in its own TU (the fuzz-harness shape — the tier OBJECT libs
   and dispatch.c stay out of the link), compiles WITHOUT HAVE_FFMPEG (the
   ffmpeg branch is not the subject; Windows ships that shape).

   Fixtures are format-floor SPCs (spc_min_file_size 0x10180, the
   make_spc_dirwrap.py layout with a silent all-zero DSP image); the
   ID666 length field at 0xA9 is set via the LE16 fallback path (the first
   byte is a non-digit, so the digit parse bails at once).

   argv[1] = scratch dir (ctest_home.sh --scratch-arg).
*/
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/decode/decode.c"

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); fails++; } } while (0)

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* format-floor SPC with the LE16 length at 0xA9 (see header). The all-zero
   body puts PC at 0 (reserved pcl/pch) and the NOP-walk runs off the end of
   RAM into the STOP padding: since the libgme STOP-hang port that padding
   STOP must halt the core, so a body that must stay alive across long skips
   gets an unconditional JMP $8000 at RAM 0x8000 (5F 00 80 — SPC700 opcode
   table; 4C is LSR abs, 80 is SETC) with pcl/pch pointed there. NOT at 0:
   the DSP's echo engine zeroes low RAM (dir=0) after ~170 ms, which
   overwrites any self-referential program at RAM 0 (the old core masked
   that with the padding STOP wrap-loop this security port removes). */
static void mk_spc(const char *path, int secs, int loop) {
    static const char SIG[] = "SNES-SPC700 Sound File Data\x1a\x1a";
    unsigned char *f = calloc(1, 0x10180);
    memcpy(f, SIG, 27);
    f[0x23] = 0;   /* has_id666 */
    f[0x24] = 0;   /* version 0 */
    f[0xA9] = (unsigned char)(secs & 0xFF);   /* LE16, non-digit first byte */
    f[0xAA] = (unsigned char)((secs >> 8) & 0xFF);
    if (loop) {
        f[0x25] = 0x00; f[0x26] = 0x80;                        /* PC = 0x8000 */
        f[0x100 + 0x8000] = 0x5F;                              /* JMP $8000 */
        f[0x100 + 0x8001] = 0x00; f[0x100 + 0x8002] = 0x80;
    }
    FILE *o = fopen(path, "wb");
    fwrite(f, 1, 0x10180, o);
    fclose(o);
    free(f);
}

static int polls;
static int abort_never(void *ctx)  { (void)ctx; polls++; return 0; }
static int abort_first(void *ctx)  { (void)ctx; polls++; return 1; }
static int abort_after4(void *ctx) { (void)ctx; return ++polls > 4; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]); return 2; }
    char p3[512], p60[512], pl[512], pdead[512];
    snprintf(p3, sizeof p3, "%s/spc-3s.spc", argv[1]);
    snprintf(p60, sizeof p60, "%s/spc-60s.spc", argv[1]);
    snprintf(pl, sizeof pl, "%s/spc-3600s.spc", argv[1]);
    snprintf(pdead, sizeof pdead, "%s/spc-floor.spc", argv[1]);
    mk_spc(p3, 3, 1); mk_spc(p60, 60, 1); mk_spc(pl, 3600, 1);
    mk_spc(pdead, 3600, 0);
    static int16_t out[4096 * 2];

    /* pin 1: tiny file, defaults — duration gate and a normal cheap seek */
    Decoder *d = dec_open(p3, NULL);
    CHECK(d != NULL, "3s spc opens");
    if (d) {
        CHECK(dec_total_frames(d) == 160000, "total = (3+2 fade) * 32000");
        CHECK(dec_seek(d, 1000) == 0, "cheap seek succeeds");
        CHECK(dec_read(d, out, 512) > 0, "read after seek > 0");
        dec_close(d);
    }

    /* pin 2: opts=NULL stores a NULL hook — a 62 s-window fast-forward
       still runs to completion (backward compatibility of the old shape) */
    d = dec_open(p60, NULL);
    CHECK(d != NULL, "60s spc opens");
    if (d) {
        CHECK(dec_total_frames(d) == 1984000, "total = (60+2) * 32000");
        double t = now();
        int rc = dec_seek(d, dec_total_frames(d) - 1);
        double w = now() - t;
        CHECK(rc == 0 && w < 30.0, "unhooked 60s skip completes");
        printf("      (60s skip wall %.2fs)\n", w);
        CHECK(dec_read(d, out, 512) > 0, "read after full skip > 0");
        dec_close(d);
    }

    /* pin 3: hook wired, never fires — seek completes normally */
    DecOpts o = { .seek_abort = abort_never, .seek_abort_ctx = NULL };
    d = dec_open(pl, &o);
    CHECK(d != NULL, "3600s spc opens");
    if (d) {
        CHECK(dec_total_frames(d) == 115264000, "total = (3600+2) * 32000");
        polls = 0;
        CHECK(dec_seek(d, 100000) == 0, "never-abort seek completes");
        CHECK(polls >= 13, "poll runs per 8192-frame chunk (>=13 of 13)");
        dec_close(d);
    }

    /* pin 4: THE fix — a 3600 s skip abandoned after 4 polls: must
       return -1 fast, and the decoder must stay usable where it got to */
    DecOpts o4 = { .seek_abort = abort_after4 };
    d = dec_open(pl, &o4);
    if (d) {
        polls = 0;
        double t = now();
        int rc = dec_seek(d, dec_total_frames(d) - 1);
        double w = now() - t;
        CHECK(rc == -1, "aborted skip returns -1");
        CHECK(w < 0.5, "abandoned skip bounded (<0.5 s of a ~44 s skip)");
        printf("      (abandoned skip wall %.4fs after %d polls)\n", w, polls);
        CHECK(polls < 60, "skip truly stopped early (far under full chunk count)");
        CHECK(dec_read(d, out, 512) > 0, "decoder usable after abort");
        dec_close(d);
    }

    /* pin 5: contract — any pending command pre-empts even a small seek */
    d = dec_open(p3, NULL);
    if (d) {
        DecOpts oi = { .seek_abort = abort_first };
        dec_close(d);
        d = dec_open(p3, &oi);
        CHECK(d != NULL, "3s spc reopens (immediate-abort hook)");
        if (d) {
            CHECK(dec_seek(d, 1000) == -1, "pending-command aborts even 1 chunk");
            dec_close(d);
        }
    }

    /* pin 6: STOP-hang defusal (libgme 775efcd port) — the all-zero floor
       body NOP-walks off the end of RAM into the STOP padding. The OLD core
       wrapped the PC to 0 and looped the 64K walk forever: a declared-3600s
       file pinned the player thread emulating garbage (this harness's own
       pre-fix fixtures "completed" only through that loop). With the port the
       core halts (cpu_error), the skip aborts fast with -1, and the decoder
       stays usable at the point reached. */
    d = dec_open(pdead, NULL);
    CHECK(d != NULL, "floor spc opens");
    if (d) {
        double t = now();
        int rc = dec_seek(d, dec_total_frames(d) - 1);
        double w = now() - t;
        CHECK(rc == -1, "tail-walking body halts (seek rc -1)");
        CHECK(w < 1.0, "halt is immediate (<1 s of a ~44 s emulated skip)");
        printf("      (floor skip wall %.4fs)\n", w);
        /* the emulated core's error is a latched query consumed by the skip;
           the decoder stays usable after it (same contract as an abort) */
        CHECK(dec_read(d, out, 512) > 0, "decoder usable after halt");
        dec_close(d);
    }

    if (fails) { printf("%d FAIL\n", fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
