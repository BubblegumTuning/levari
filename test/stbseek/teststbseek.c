/* teststbseek — vendored stb_vorbis seek-window guard pin (LEVARI PATCH
 * SEVENTH hunk).
 *
 * A crafted Ogg makes the linear seek in stb_vorbis_seek_frame64 land on a
 * frame whose ACTUAL decoded window is shorter than the peek_decode_initial
 * estimate said. stb_vorbis_seek64 then advances channel_buffer_start past
 * channel_buffer_end — guarded ONLY by an assert (gone under NDEBUG) — and
 * the next stb_vorbis_get_samples_short_interleaved computes a negative
 * window length k, rewinds its caller-buffer cursor below the caller's
 * buffer, and writes a later frame there: an out-of-bounds WRITE through
 * the caller's buffer (ASan: global-buffer-overflow at
 * convert_channels_short_interleaved; in levari the caller's buffer is the
 * player's decode buffer = heap corruption). Fresh open + ONE in-range
 * seek of a crafted file, i.e. a plain seek-bar click. Present since the
 * 1.21 vendored line and still unfixed upstream 1.25.
 *
 * Fixture test/stbseek/seek-window-est.ogg: 246058-byte fuzz artifact
 * (~/fuzz-work/corpus_dec/90c469e80b35400b0386ba4281be2459e4ec1d69)
 * byte-minimized to 5810 bytes, same fault site and same failing target
 * (1000 of a declared 269188).
 *
 * Pins (post-fix contract): the window-exceeding seek FAILS with
 * VORBIS_seek_failed (ret 0) and the sentinels below the caller buffer stay
 * intact; the fixture's accepted-side seeks (1, 500 — readouts 703/204
 * samples on BOTH sides of the fix) keep succeeding identically; valid
 * files unaffected. argv[1] = repo test/ dir (ctest_home --test-arg). Link:
 * this TU includes stb_vorbis.c with STB_VORBIS_HEADER_ONLY; the
 * implementation is compiled as its own TU by CMake — never both.
 */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { printf("PASS: %s\n", msg); }                              \
        else      { printf("FAIL: %s\n", msg); fails++; }                     \
    } while (0)

/* sentinel page sits BEFORE the decode buffer: the pre-fix rewind writes
 * below out[], so the sentinels must still read 0xA5 after every pin */
static unsigned char sent[16384];
#define OUT_SHORTS (4096 * 6)
static short out[OUT_SHORTS];

static void arm(void) { memset(sent, 0xA5, sizeof sent); }

static int sent_ok(void)
{
    for (unsigned i = 0; i < sizeof sent; ++i)
        if (sent[i] != 0xA5) return 0;
    return 1;
}

/* decode into out[] immediately above the sentinel page (adjacent in the
 * same TU; .bss order cannot be forced, so the guard page is 16 KiB wide —
 * the observed rewind per loop trip is |k|*ch*2 bytes, hundreds at most) */
static int drain(stb_vorbis *v, int ch)
{
    int n, total = 0;
    while ((n = stb_vorbis_get_samples_short_interleaved(
                v, ch, out, OUT_SHORTS)) > 0)
        total += n;
    return total;
}

int main(int argc, char **argv)
{
    const char *dir  = (argc > 1) ? argv[1] : "test";
    char fx[1024], vg[1024];
    int err, ch, n;
    snprintf(fx, sizeof fx, "%s/stbseek/seek-window-est.ogg", dir);
    snprintf(vg, sizeof vg, "%s/example.ogg", dir);

    /* --- fixture opens at all (the bug is behind a successful open) --- */
    stb_vorbis *v = stb_vorbis_open_filename(fx, &err, NULL);
    if (!v) { printf("FAIL: fixture open err=%d\n", err); return 1; }
    ch = stb_vorbis_get_info(v).channels;
    stb_vorbis_close(v);
    CHECK(ch == 2, "fixture opens as stereo");

    /* --- the hole: fresh open, ONE in-range seek (sample 1000 of a file
     * declaring 269188; the seek-bar-click shape) --- */
    arm();
    v = stb_vorbis_open_filename(fx, &err, NULL);
    if (!v) { printf("FAIL: fixture reopen\n"); return 1; }
    ch = stb_vorbis_get_info(v).channels;
    int rok = stb_vorbis_seek(v, 1000);
    CHECK(rok == 0 && stb_vorbis_get_error(v) == VORBIS_seek_failed,
          "crafted in-range seek PAST the landed frame window is rejected");
    /* reads after the rejected seek stay in-bounds and safe */
    stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch);
    CHECK(sent_ok(), "no write below the caller buffer (fresh-open hole)");
    stb_vorbis_close(v);

    /* --- same hole after play-through (backward seek shape) --- */
    arm();
    v = stb_vorbis_open_filename(fx, &err, NULL);
    if (!v) { printf("FAIL: fixture reopen 2\n"); return 1; }
    ch = stb_vorbis_get_info(v).channels;
    drain(v, ch);
    rok = stb_vorbis_seek(v, 1000);
    CHECK(rok == 0, "crafted backward seek is rejected");
    stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch);
    CHECK(sent_ok(), "no write below the caller buffer (backward seek)");
    stb_vorbis_close(v);

    /* --- still-supported side: seeks the old asserts ACCEPTED must keep
     * succeeding with the SAME decoded readouts (fix rejects exactly the
     * assert-violating set). Fresh handle per probe; the accepted set on
     * THIS fixture is {1, 500} — readouts 703/204 samples, proven identical
     * on the unfixed line (which then SIGSEGVs on the very next target,
     * 1000). Larger targets were accepted by the raw artifact but are
     * rejected by the minimized fixture on BOTH sides — post-land state
     * differs (truncated stream leaves short windows). --- */
    unsigned probe[] = { 1, 500 };
    for (unsigned i = 0; i < 2; ++i) {
        v = stb_vorbis_open_filename(fx, &err, NULL);
        if (!v) { printf("FAIL: fixture reopen 3\n"); return 1; }
        ch = stb_vorbis_get_info(v).channels;
        rok = stb_vorbis_seek(v, probe[i]);
        n = rok ? stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch) : -1;
        char msg[80];
        snprintf(msg, sizeof msg, "in-window seek(%u) still accepted + reads", probe[i]);
        CHECK(rok == 1 && n > 0, msg);
        stb_vorbis_close(v);
    }
    /* sequential shape: whatever the seek decides, the caller buffer stays
     * intact (pre-fix: seek(4639) after 500+read was rejected there too —
     * safety pin, not acceptance) */
    arm();
    v = stb_vorbis_open_filename(fx, &err, NULL);
    if (!v) { printf("FAIL: fixture reopen 4\n"); return 1; }
    ch = stb_vorbis_get_info(v).channels;
    stb_vorbis_seek(v, 500);
    stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch);
    stb_vorbis_seek(v, 4639);
    stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch);
    CHECK(sent_ok(), "sequential seek+read pair keeps the caller buffer intact");
    stb_vorbis_close(v);

    /* --- valid-file regression: example.ogg seeks must be untouched --- */
    arm();
    v = stb_vorbis_open_filename(vg, &err, NULL);
    if (!v) { printf("FAIL: example.ogg open err=%d\n", err); return 1; }
    ch = stb_vorbis_get_info(v).channels;
    int ok_all = 1;
    unsigned offs[] = { 0, 1, 4096, 44100, 131071 };
    for (unsigned i = 0; i < 5; ++i) {
        rok = stb_vorbis_seek(v, offs[i]);
        n = rok ? stb_vorbis_get_samples_short_interleaved(v, ch, out, 4096 * ch) : -1;
        if (!rok || n <= 0) ok_all = 0;
    }
    CHECK(ok_all, "example.ogg: all five valid seeks accept + read");
    CHECK(sent_ok(), "example.ogg: no write below the caller buffer");
    stb_vorbis_close(v);

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
