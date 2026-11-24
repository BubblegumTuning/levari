/* testopus — the opus open path stays open (e59230b).

   Rationale: the DEC_FMT table refactor (48eab0e) left lv_fmt_opus_open's
   success tail falling through the opusfail: label into destroy/close/free/
   return -1, with the success return stranded as dead code after the
   return -1. Every .opus file "cannot decode" for three releases — the
   headers parsed, libopus dlopened, and the open silently "failed" with no
   trace. ctest had zero opus-open coverage; this harness is the gate.

   Same TU-carries-decode.c shape as testspcseek (fuzz-harness style: the
   tier OBJECT libs stay out and dispatch.c is filtered from core, compiled
   WITHOUT HAVE_FFMPEG — the ffmpeg branch is not the subject).

   The committed fixture test/opus/sample.opus is a generated 440 Hz sine
   (ffmpeg -c:a libopus, stereo 48k, 2.0065 s: final granule 96312,
   preskip 312, so dec_total_frames == 96000 exactly). Regenerate with
   make_fixture.sh where ffmpeg carries libopus. Negatives are derived from
   the fixture in the scratch cwd.

   Without a loadable libopus the pins OBSERVE-skip: the backend under
   test cannot exist on such a host (dlopen'd, like every shipped build).

   argv[1] = the repo test/ dir (ctest_home.sh --test-arg); cwd is the
   fresh scratch dir.
*/
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/decode/decode.c"

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); fails++; } } while (0)

/* Read the whole fixture; returns size or -1. */
static long slurp(const char *path, unsigned char *buf, long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static void dump(const char *path, const unsigned char *b, long n) {
    FILE *o = fopen(path, "wb");
    fwrite(b, 1, (size_t)n, o);
    fclose(o);
}

/* Offset of the second page ("OggS" after the first page ends). The
   fixture's first page carries exactly the 19-byte OpusHead. */
static long page2_off(const unsigned char *b, long n) {
    if (n < 28) return -1;
    long body = 27 + 1 + b[26];            /* fixed hdr + 1 lacing + body */
    for (long i = body; i + 4 <= n; i++)
        if (!memcmp(b + i, "OggS", 4)) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <repo-test-dir>\n", argv[0]); return 2; }
    if (opus_syms() != 0) {
        printf("OBSERVE: libopus not loadable on this host — opus pins skipped\n");
        return 0;
    }

    char fix[4096];
    snprintf(fix, sizeof fix, "%s/opus/sample.opus", argv[1]);
    static unsigned char head[65536];
    long n = slurp(fix, head, (long)sizeof head);
    if (n < 0) { fprintf(stderr, "fixture missing: %s\n", fix); return 2; }
    if (n == (long)sizeof head) { fprintf(stderr, "fixture larger than the harness buffer\n"); return 2; }

    /* --- the regression itself: a valid Opus file must OPEN --- */
    Decoder *d = dec_open(fix, NULL);
    CHECK(d != NULL, "open: valid opus opens (fall-through red-pre)");
    if (!d) { printf("%d FAIL\n", fails); return fails ? 1 : 0; }
    CHECK(dec_channels(d) == 2, "open: channels == 2");
    CHECK(dec_rate(d) == 48000, "open: rate == 48000");
    CHECK(dec_total_frames(d) == 96000,
          "open: total == 96312 granules - 312 preskip == 96000");

    /* --- the read path produces real samples --- */
    static int16_t pcm[4096 * 2];
    int got = dec_read(d, pcm, 4096);
    CHECK(got == 4096, "read: first 4096 frames arrive");
    int loud = 0;
    for (int i = 0; i < got * 2; i++) if (pcm[i] > 1000 || pcm[i] < -1000) { loud = 1; break; }
    CHECK(loud, "read: decoded output is not silence (440 Hz sine)");

    /* --- the seek path --- */
    CHECK(dec_seek(d, 48000) == 0, "seek: mid-stream succeeds");
    got = dec_read(d, pcm, 1024);
    CHECK(got == 1024, "seek: read after seek delivers");

    /* --- seek discard lands INSIDE the stream: seek near the end,
           the tail must still produce frames (not overshoot to EOF) --- */
    CHECK(dec_seek(d, dec_total_frames(d) - 1024) == 0, "seek: near-end succeeds");
    got = dec_read(d, pcm, 1024);
    CHECK(got > 0, "seek: near-end read delivers the tail");
    dec_close(d);

    /* --- negatives derived from the fixture --- */

    /* head page only: the OpusTags packet never arrives -> NULL */
    long p2 = page2_off(head, n);
    CHECK(p2 > 0, "fixture: second page located");
    dump("headonly.opus", head, p2);
    CHECK(dec_open("headonly.opus", NULL) == NULL, "reject: missing OpusTags");

    /* head page with a mangled magic -> NULL */
    static unsigned char bad[65536];
    memcpy(bad, head, (size_t)p2);
    /* the OpusHead magic sits right after the 27-byte header + lacing */
    long magic = 27 + 1 + 0; /* fixed hdr + 1 lacing byte */
    CHECK(!memcmp(bad + magic, "OpusHead", 8), "fixture: OpusHead at expected offset");
    bad[magic] = 'X';
    dump("badmagic.opus", bad, p2);
    CHECK(dec_open("badmagic.opus", NULL) == NULL, "reject: mangled OpusHead magic");

    /* head page truncated mid-packet -> NULL */
    dump("short.opus", head, 28);
    CHECK(dec_open("short.opus", NULL) == NULL, "reject: truncated head page");

    /* head + tags but zero audio pages: opens clean with total 0 —
       the success path must not fall into the cleanup here either */
    long p2end = -1;
    for (long i = p2 + 27; i + 4 <= n; i++)
        if (!memcmp(head + i, "OggS", 4)) { p2end = i; break; }
    CHECK(p2end > 0, "fixture: third page located");
    dump("noaudio.opus", head, p2end);
    Decoder *z = dec_open("noaudio.opus", NULL);
    CHECK(z != NULL, "open: head+tags with zero audio opens");
    if (z) {
        CHECK(dec_total_frames(z) == 0, "open: zero-audio total == 0");
        CHECK(dec_read(z, pcm, 1024) == 0, "read: zero-audio yields nothing");
        dec_close(z);
    }

    printf("%d FAIL\n", fails);
    return fails ? 1 : 0;
}
