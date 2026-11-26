/* ReplayGain extraction gate (WP1): tag_rg_read over hand-built tag
   blocks. The fixtures need NO playable audio — tag_rg_read walks only
   the tag block — so every file is deterministic bytes written into the
   cwd (ctest_home.sh runs us inside a fresh scratch dir).
   Pinned: ID3v2.3 TXXX (both spellings of the description), ID3v2.4
   RVA2 (16-bit signed adjustment*512, spec 4.11), native FLAC via the
   Vorbis comment, absent values (no tag, "n/a" garbage), and the
   '_'~' ' description equivalence.
   Build/run: CTest target testrg (tools/ctest_home.sh wrapper). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tag/tag.h"

static int fails;

static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void wr(const char *name, const unsigned char *b, size_t n) {
    FILE *f = fopen(name, "wb");
    if (!f) { printf("FAIL open %s\n", name); fails++; return; }
    fwrite(b, 1, n, f);
    fclose(f);
}

/* append helpers over a generous static buffer */
static unsigned char fx[4096];
static size_t fp_;
static void ap(const void *p, size_t n) { memcpy(fx + fp_, p, n); fp_ += n; }
static void apc(int c) { fx[fp_++] = (unsigned char)c; }
static void apbe32(unsigned v) {
    apc(v >> 24); apc(v >> 16); apc(v >> 8); apc(v);
}

static void rg_read(const char *name, int *t, int *a) {
    *t = *a = 12345;   /* poison: tag_rg_read must overwrite both */
    tag_rg_read(name, t, a);
}

/* APEv2 builders: item = vs LE32 + flags LE32 + key + NUL + value;
   footer = "APETAGEX" + ver LE32 + size LE32 (items+footer) +
   count LE32 + flags LE32 + 8 reserved zero bytes */
static size_t ape_item(const char *k, const char *v, unsigned fl) {
    size_t kl = strlen(k), vl = strlen(v);
    apc((unsigned)vl); apc(0); apc(0); apc(0);
    apc(fl & 0xFF); apc((fl >> 8) & 0xFF); apc((fl >> 16) & 0xFF); apc((fl >> 24) & 0xFF);
    ap(k, kl); apc(0);
    ap(v, vl);
    return 8 + kl + 1 + vl;
}
static void ape_footer(unsigned cnt, size_t items, unsigned flags) {
    size_t size = items + 32;
    ap("APETAGEX", 8);
    apc(0xD0); apc(0x07); apc(0); apc(0);              /* 2000 LE32 */
    apc((unsigned)size); apc((unsigned)(size >> 8)); apc(0); apc(0);
    apc(cnt); apc(0); apc(0); apc(0);
    apc(flags & 0xFF); apc((flags >> 8) & 0xFF); apc((flags >> 16) & 0xFF); apc((flags >> 24) & 0xFF);
    for (int i = 0; i < 8; i++) apc(0);                /* reserved */
}

/* ---- ID3v2.3 TXXX ---- */
static size_t id3_txxx_frame(const char *desc, const char *val) {
    size_t dl = strlen(desc), vl = strlen(val);
    ap("TXXX", 4);
    apbe32((unsigned)(1 + dl + 1 + vl + 1));
    apc(0); apc(0);                      /* frame flags */
    apc(0);                              /* enc: latin-1 (ASCII-safe) */
    ap(desc, dl); apc(0);
    ap(val, vl); apc(0);
    return 10 + 1 + dl + 1 + vl + 1;
}

int main(void) {
    /* 1) ID3v2.3 TXXX, underscore spelling, " dB" unit */
    fp_ = 0;
    size_t b1 = id3_txxx_frame("REPLAYGAIN_TRACK_GAIN", "-8.35 dB") +
                id3_txxx_frame("REPLAYGAIN_ALBUM_GAIN", "-6.02 dB");
    /* header first: rebuild with body size known */
    {
        unsigned char tmp[4096];
        memcpy(tmp, fx, fp_);
        fp_ = 0;
        ap("ID3", 3); apc(3); apc(0); apc(0);
        apc(0); apc(0); apc((unsigned)((b1 >> 14) & 0x7F)); apc((unsigned)(b1 & 0x7F));
        memcpy(fx + fp_, tmp, b1); fp_ += b1;
        wr("rg-id3v23.bin", fx, fp_);
    }
    int t, a;
    rg_read("rg-id3v23.bin", &t, &a);
    chk("id3v23 TXXX track gain", t, -835);
    chk("id3v23 TXXX album gain", a, -602);

    /* 2) ID3v2.4 RVA2: adjustment = round(db*512), 16-bit signed BE */
    fp_ = 0;
    struct { const char *id; int adj; } rv[] = { { "track", -4275 }, { "album", -3082 } };
    size_t b2 = 0;
    for (int i = 0; i < 2; i++) {
        size_t il = strlen(rv[i].id);
        size_t fsz = il + 1 + 1 + 2 + 1;   /* id NUL chan adj(2) peakbits */
        ap("RVA2", 4);
        apc(0); apc(0); apc(0); apc((unsigned)fsz);   /* v4 syncsafe size */
        apc(0); apc(0);
        ap(rv[i].id, il); apc(0);
        apc(0x01);                                     /* channel: master */
        apc((unsigned)(rv[i].adj >> 8) & 0xFF); apc(rv[i].adj & 0xFF);
        apc(0);                                        /* no peak field */
        b2 += 10 + fsz;
    }
    {
        unsigned char tmp[4096];
        memcpy(tmp, fx, fp_);
        fp_ = 0;
        ap("ID3", 3); apc(4); apc(0); apc(0);
        apc(0); apc(0); apc((unsigned)((b2 >> 14) & 0x7F)); apc((unsigned)(b2 & 0x7F));
        memcpy(fx + fp_, tmp, b2); fp_ += b2;
        wr("rg-id3v24.bin", fx, fp_);
    }
    rg_read("rg-id3v24.bin", &t, &a);
    chk("id3v24 RVA2 track gain (-4275/512 dB)", t, -835);
    chk("id3v24 RVA2 album gain (-3082/512 dB)", a, -602);

    /* 3) native FLAC: fLaC + zero STREAMINFO + VORBIS_COMMENT */
    fp_ = 0;
    ap("fLaC", 4);
    apc(0x00); apc(0); apc(0); apc(42);          /* STREAMINFO, len 42 */
    for (int i = 0; i < 42; i++) apc(0);
    {
        const char *e1 = "REPLAYGAIN_TRACK_GAIN=-8.35";
        const char *e2 = "REPLAYGAIN_ALBUM_GAIN=-6.02";
        size_t body = 4 + 4 + (4 + strlen(e1)) + (4 + strlen(e2));
        apc(0x84); apc((unsigned)(body >> 16)); apc((unsigned)(body >> 8)); apc((unsigned)body);
        apc(0); apc(0); apc(0); apc(0);          /* vendor length 0 */
        apc(2); apc(0); apc(0); apc(0);          /* 2 comments */
        apc((unsigned)strlen(e1)); apc(0); apc(0); apc(0);
        ap(e1, strlen(e1));
        apc((unsigned)strlen(e2)); apc(0); apc(0); apc(0);
        ap(e2, strlen(e2));
        wr("rg-flac.bin", fx, fp_);
    }
    rg_read("rg-flac.bin", &t, &a);
    chk("flac vorbis-comment track gain", t, -835);
    chk("flac vorbis-comment album gain", a, -602);

    /* 4) space-spelling description (TXXX) is equivalent */
    fp_ = 0;
    b1 = id3_txxx_frame("Replaygain Track Gain", "+3") +
         id3_txxx_frame("REPLAYGAIN ALBUM GAIN", "4.5dB");
    {
        unsigned char tmp[4096];
        memcpy(tmp, fx, fp_);
        fp_ = 0;
        ap("ID3", 3); apc(3); apc(0); apc(0);
        apc(0); apc(0); apc((unsigned)((b1 >> 14) & 0x7F)); apc((unsigned)(b1 & 0x7F));
        memcpy(fx + fp_, tmp, b1); fp_ += b1;
        wr("rg-id3-spell.bin", fx, fp_);
    }
    rg_read("rg-id3-spell.bin", &t, &a);
    chk("TXXX case/space spelling track", t, 300);
    chk("TXXX space spelling album", a, 450);

    /* 5) no tag at all -> both absent */
    fp_ = 0;
    ap("RIFFjunkjunk", 12);
    wr("rg-none.bin", fx, fp_);
    rg_read("rg-none.bin", &t, &a);
    chk("untagged file track absent", t, LV_RG_NONE);
    chk("untagged file album absent", a, LV_RG_NONE);

    /* 6) "n/a" value -> absent (garbage must never become a gain) */
    fp_ = 0;
    {
        const char *e1 = "REPLAYGAIN_TRACK_GAIN=n/a";
        size_t body = 4 + 4 + (4 + strlen(e1));
        ap("fLaC", 4);
        apc(0x84); apc((unsigned)(body >> 16)); apc((unsigned)(body >> 8)); apc((unsigned)body);
        apc(0); apc(0); apc(0); apc(0);
        apc(1); apc(0); apc(0); apc(0);
        apc((unsigned)strlen(e1)); apc(0); apc(0); apc(0);
        ap(e1, strlen(e1));
        wr("rg-na.bin", fx, fp_);
    }
    rg_read("rg-na.bin", &t, &a);
    chk("n/a value track absent", t, LV_RG_NONE);
    chk("n/a value album absent", a, LV_RG_NONE);

    /* 7) missing file -> both absent, no crash */
    rg_read("rg-does-not-exist.bin", &t, &a);
    chk("missing file track absent", t, LV_RG_NONE);
    chk("missing file album absent", a, LV_RG_NONE);

    /* ---- APEv2 (WP2): footer at EOF, items before it ---- */
    /* 8) plain tail tag (no header), with a dummy item to skip */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("TITLE", "x", 0) +
                    ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 0) +
                    ape_item("REPLAYGAIN_ALBUM_GAIN", "-6.02", 0);
        ape_footer(3, it, 0);
        wr("rg-ape.bin", fx, fp_);
    }
    rg_read("rg-ape.bin", &t, &a);
    chk("apev2 track gain", t, -835);
    chk("apev2 album gain", a, -602);

    /* 9) APEv2 before a trailing ID3v1 */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 0) +
                    ape_item("REPLAYGAIN_ALBUM_GAIN", "-6.02", 0);
        ape_footer(2, it, 0);
        ap("TAG", 3);
        for (int i = 0; i < 125; i++) apc(' ');        /* ID3v1, 128 total */
        wr("rg-ape-v1.bin", fx, fp_);
    }
    rg_read("rg-ape-v1.bin", &t, &a);
    chk("apev2 + ID3v1 tail track gain", t, -835);
    chk("apev2 + ID3v1 tail album gain", a, -602);

    /* 10) header present (footer flags bit 31): spec layout is
       audio + header + items + footer, and the footer's size field
       excludes the header */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 0) +
                    ape_item("REPLAYGAIN_ALBUM_GAIN", "-6.02", 0);
        /* header: same 32-byte shape, bit 29 set (this is the header) */
        ap("APETAGEX", 8);
        apc(0xD0); apc(0x07); apc(0); apc(0);
        apc((unsigned)(it + 32)); apc((unsigned)((it + 32) >> 8)); apc(0); apc(0);
        apc(2); apc(0); apc(0); apc(0);
        apc(0); apc(0); apc(0); apc(0x20);             /* bit 29 */
        for (int i = 0; i < 8; i++) apc(0);
        ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 0);
        ape_item("REPLAYGAIN_ALBUM_GAIN", "-6.02", 0);
        ape_footer(2, it, 0x80000000u);                /* bit 31: has header */
        wr("rg-ape-hdr.bin", fx, fp_);
    }
    rg_read("rg-ape-hdr.bin", &t, &a);
    chk("apev2 with header track gain", t, -835);
    chk("apev2 with header album gain", a, -602);

    /* 11) lowercase key (readers recommended case-insensitive) */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("replaygain_track_gain", "+3", 0);
        ape_footer(1, it, 0);
        wr("rg-ape-case.bin", fx, fp_);
    }
    rg_read("rg-ape-case.bin", &t, &a);
    chk("apev2 lowercase key track", t, 300);
    chk("apev2 lowercase key album absent", a, LV_RG_NONE);

    /* 12) binary item type (flags bits 2..1 = 1) is not parsed as text */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 1u << 1);
        ape_footer(1, it, 0);
        wr("rg-ape-bin.bin", fx, fp_);
    }
    rg_read("rg-ape-bin.bin", &t, &a);
    chk("apev2 binary item ignored", t, LV_RG_NONE);

    /* 13) malformed footer (version 1000) -> absent */
    fp_ = 0;
    ap("JUNKaudiodata", 13);
    {
        size_t it = ape_item("REPLAYGAIN_TRACK_GAIN", "-8.35", 0);
        ap("APETAGEX", 8);
        apc(0xE8); apc(0x03); apc(0); apc(0);          /* 1000 LE32 */
        apc((unsigned)(it + 32)); apc(0); apc(0); apc(0);
        apc(1); apc(0); apc(0); apc(0);
        apc(0); apc(0); apc(0); apc(0);
        for (int i = 0; i < 8; i++) apc(0);
        wr("rg-ape-v1tag.bin", fx, fp_);
    }
    rg_read("rg-ape-v1tag.bin", &t, &a);
    chk("apev1 footer rejected track", t, LV_RG_NONE);
    chk("apev1 footer rejected album", a, LV_RG_NONE);

    printf(fails ? "%d FAILURES\n" : "all pass\n", fails ? fails : 0);
    return fails != 0;
}
