/* testdecode — one open/read/seek row per decoder backend.

   The opus incident (e59230b) proved the class: a refactor made
   lv_fmt_opus_open's success tail fall through the opusfail cleanup and
   every .opus died at open — a green 30-test battery, because NO ctest
   test ever called dec_open on an .opus. Depth gates exist per backend
   (testopus, testspcseek, teststbseek); this is the WIDTH gate: the whole
   DEC_FMT table must open, deliver audible frames, and seek. New backend
   or refactor of an existing one -> it runs through here.

   Fixtures are committed generated sines (make_fixture.sh regenerates all
   of them where ffmpeg carries the matching encoder). Lossless/opus rows
   pin the total EXACTLY (container truth); lossy rows pin a RANGE —
   minimp3's full-scan count includes LAME's encoder delay, stb's length
   is an estimate, the ffmpeg total rescales st->duration — a legit
   vendored-parser refresh may move those without a functional change, and
   a guard must break on behavior, not on library housekeeping.

   The ffmpeg row follows the shipped dlopen shape: no libav on the host
   means the OBSERVE line and a skip (the formats are legitimately absent,
   cf. mac112), but a host WITH libav must pass. The module family
   (mod/xm/it/s3m via libmodplug) cannot ship a committed fixture —
   copyright policy keeps module files in git-ignored test-assets; they
   stay manual cut-verification, same as the module title pins in the
   release selftests.

   Same TU-carries-decode.c shape as testspcseek/testopus (tier OBJECT
   libs and dispatch filtered; the CMake target adds HAVE_FFMPEG + the
   dec_ffab objects where the shipped build has them, so the ffmpeg row
   tests the same shape as the user's binary).

   argv[1] = the repo test/ dir (ctest_home.sh --test-arg); cwd is the
   fresh scratch dir.
*/
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/decode/decode.c"

static int fails, pins;
#define CHECK(cond, name) do { \
    pins++; \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); fails++; } } while (0)

typedef struct {
    const char *name, *relpath;
    int rate;
    long long lo, hi;   /* total frames: exact when lo == hi */
} Row;

static const Row ROWS[] = {
    /* name      relpath            rate  total frames */
    { "wav",      "decode/sample.wav",  48000,  96000,  96000 },
    { "vorbis",   "decode/sample.ogg",  48000,  95000,  97000 },
    { "opus",     "opus/sample.opus",   48000,  96000,  96000 },
    { "flac",     "sample.flac",        44100, 441000, 441000 },
    { "oggflac",  "decode/sample.oga",  48000,  96000,  96000 },
    { "mp3",      "decode/sample.mp3",  48000,  95000,  99000 },
    { "ffmpeg",   "decode/sample.m4a",  48000,  95000,  98000 },
};

static char line[256];
#define CK(cond, fmt, ...) do { \
    snprintf(line, sizeof line, fmt, __VA_ARGS__); CHECK(cond, line); } while (0)

static int16_t pcm[4096 * 2];

static void run_row(const Row *r, const char *tdir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", tdir, r->relpath);

#ifdef HAVE_FFMPEG
    if (!strcmp(r->name, "ffmpeg") && !ff_pick()) {
        printf("OBSERVE: ffmpeg row skipped — no loadable libav on this host\n");
        return;
    }
#else
    if (!strcmp(r->name, "ffmpeg")) {
        printf("OBSERVE: ffmpeg row skipped — harness compiled without HAVE_FFMPEG\n");
        return;
    }
#endif

    Decoder *d = dec_open(path, NULL);
    CK(d != NULL, "%s: opens", r->name);
    if (!d) return;
    CK(dec_channels(d) == 2, "%s: channels == 2", r->name);
    CK(dec_rate(d) == r->rate, "%s: rate == %d", r->name, r->rate);
    long long tot = dec_total_frames(d);
    if (r->lo == r->hi)
        CK(tot == r->lo, "%s: total == %lld (exact)", r->name, r->lo);
    else
        CK(tot >= r->lo && tot <= r->hi, "%s: total %lld in [%lld,%lld]",
           r->name, tot, r->lo, r->hi);

    int got = dec_read(d, pcm, 4096);   /* every fixture is >= 2 s */
    CK(got > 0, "%s: read delivers frames", r->name);
    int loud = 0;
    for (int i = 0; i < got * 2; i++)
        if (pcm[i] > 1000 || pcm[i] < -1000) { loud = 1; break; }
    CK(loud, "%s: decoded output is not silence", r->name);

    long long mid = tot / 2;
    CK(dec_seek(d, mid) == 0, "%s: seek to mid-stream succeeds", r->name);
    got = dec_read(d, pcm, 1024);
    CK(got > 0, "%s: read after seek delivers", r->name);
    dec_close(d);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <repo-test-dir>\n", argv[0]); return 2; }
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) run_row(&ROWS[i], argv[1]);
    printf("OBSERVE: module family (mod/xm/it/s3m) has no committed gate — "
           "fixtures are copyright, manual test-assets verification only\n");
    printf("%d pins, %d FAIL\n", pins, fails);
    return fails ? 1 : 0;
}
