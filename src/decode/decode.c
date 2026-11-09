/* dladdr (probe path reporting) needs the GNU feature macro on glibc
   before any system header; harmless elsewhere, macOS exposes it by
   default, and the MinGW branch never calls it */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef LEVAR_DECODE_SUFFIX
#include "tier_rename.h"
#endif
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_EX_IMPLEMENTATION
#include "../../third_party/minimp3.h"
#include "../../third_party/minimp3_ex.h"

#define DR_FLAC_IMPLEMENTATION
#include "../../third_party/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "../../third_party/dr_wav.h"

#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/stb_vorbis.c"

#include "../../third_party/libmodplug/modplug.h"
#include "../../third_party/snes_spc/spc.h"

#include "decode.h"
#include "../common.h"
#include "../config/config.h"
#include "../tag/tag.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#ifdef HAVE_FFMPEG
#include "ffabi.h"
#include "ffload.h"
#endif

enum { F_UNKNOWN, F_MP3, F_OGG, F_OPUS, F_FLAC, F_OGGFLAC, F_WAV, F_FFMPEG, F_MOD, F_SPC };

/* ---- opus via dlopen'd libopus (no dev headers needed) ---- */
static void *(*opus_create)(int Fs, int ch, int *err);
static int (*opus_dec)(void *st, const unsigned char *pkt, int len, int16_t *pcm, int frame_sz, int fec);
static void (*opus_destroy)(void *st);
static int (*opus_ctl)(void *st, int req, ...);

static int opus_syms(void) {
    if (opus_dec) return 0;
    void *h = NULL;
#ifdef _WIN32
    if (!h) h = dlopen("libopus-0.dll", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libopus.dll", RTLD_NOW | RTLD_LOCAL);
#elif defined(__APPLE__)
    static const char *OPUS_CANDS[] = {
        "libopus.0.dylib", "libopus.dylib",
        "/opt/homebrew/opt/opus/lib/libopus.0.dylib",
        "/usr/local/opt/opus/lib/libopus.0.dylib",
    };
    for (size_t i = 0; i < sizeof OPUS_CANDS / sizeof OPUS_CANDS[0] && !h; i++)
        h = dlopen(OPUS_CANDS[i], RTLD_NOW | RTLD_LOCAL);
#else
    if (!h) h = dlopen("libopus.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libopus.so", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) return -1;
    opus_create = (void *(*)(int, int, int *))dlsym(h, "opus_decoder_create");
    opus_dec = (int (*)(void *, const unsigned char *, int, int16_t *, int, int))dlsym(h, "opus_decode");
    opus_destroy = (void (*)(void *))dlsym(h, "opus_decoder_destroy");
    opus_ctl = (int (*)(void *, int, ...))dlsym(h, "opus_decoder_ctl");
    if (!opus_create || !opus_dec || !opus_destroy || !opus_ctl) return -1;
    return 0;
}

typedef struct {
    FILE *f;
    uint32_t serial;
    void *dec;
    int pre_skip, eof;
    long long data_off;   /* first audio page offset */
    long long next_off;   /* offset of next page to load */
    long long page_granule;
    unsigned char page[65307];
    int page_n, body_pos;
    unsigned char seg[255];
    int nseg, seg_i;
    int cont;
    unsigned char pkt[65536];
    int pkt_len;
    int16_t tmp[5760 * 2];
    int16_t buf[5760 * 2];
    int buf_n, buf_i;
    long long discard;
} Opus;

static long long rd64(const unsigned char *p) {
    /* unsigned accumulator: signed left-shift overflow was UB (UBSan,
       night 2026-10-29); two's-complement conversion on return is the
       intended value for Ogg granule positions. */
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return (long long)v;
}
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static int rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static int ogg_load_page(Opus *o) {
    fseeko(o->f, (off_t)o->next_off, SEEK_SET);
    unsigned char hdr[27];
    if (fread(hdr, 1, 27, o->f) != 27 || memcmp(hdr, "OggS", 4)) return -1;
    int nseg = hdr[26];
    if (fread(o->seg, 1, (size_t)nseg, o->f) != (size_t)nseg) return -1;
    int body = 0;
    for (int i = 0; i < nseg; i++) body += o->seg[i];
    if (body > (int)sizeof o->page) return -1;
    size_t got = fread(o->page, 1, (size_t)body, o->f);
    o->next_off += 27 + nseg + (long long)got;
    o->page_granule = rd64(hdr + 6);
    o->serial = rd32(hdr + 14);
    o->cont = hdr[5] & 1;
    o->page_n = (int)got;
    o->body_pos = 0;
    o->nseg = nseg;
    o->seg_i = 0;
    return 0;
}

static int ogg_next_packet(Opus *o, const unsigned char **pkt, int *len) {
    o->pkt_len = 0;
    for (;;) {
        if (o->seg_i >= o->nseg) {
            if (o->eof || ogg_load_page(o) != 0) { o->eof = 1; return -1; }
            continue;
        }
        int l = o->seg[o->seg_i++];
        if (o->pkt_len + l > (int)sizeof o->pkt) { o->pkt_len = 0; return -1; }
        memcpy(o->pkt + o->pkt_len, o->page + o->body_pos, (size_t)l);
        o->pkt_len += l;
        o->body_pos += l;
        if (l < 255) { *pkt = o->pkt; *len = o->pkt_len; return 0; }
    }
}

static long long ogg_last_granule(Opus *o) {
    if (fseeko(o->f, 0, SEEK_END) != 0) return -1;
    long long sz = ftello(o->f);
    long long want = sz < 65536 ? sz : 65536;
    unsigned char *b = malloc((size_t)want);
    if (!b) return -1;
    fseeko(o->f, sz - want, SEEK_SET);
    long long got = (long long)fread(b, 1, (size_t)want, o->f);
    long long gran = -1;
    for (long long i = got - 27; i >= 0; i--) {
        if (memcmp(b + i, "OggS", 4)) continue;
        if (rd32(b + i + 14) != o->serial) continue;
        gran = rd64(b + i + 6);
        if (gran >= 0) break;
    }
    free(b);
    return gran;
}

/* offset of the last metadata block header in the chain b[begin..end) */
static size_t flac_last_blk(const unsigned char *b, size_t begin, size_t end) {
    size_t i = begin, last = begin;
    while (i + 4 <= end) {
        last = i;
        if (b[i] & 0x80) break;
        size_t blen = ((size_t)b[i + 1] << 16) | ((size_t)b[i + 2] << 8) | b[i + 3];
        i += 4 + blen;
    }
    return last;
}

/* rebuild a native FLAC byte stream from an Ogg FLAC file (raw, in hand):
   fLaC magic + metadata blocks + raw frames, packets reassembled from the
   segment tables. The stream marker packet (0x7F "FLAC" + 4B version/count)
   is dropped; ffmpeg embeds the fLaC magic + first blocks inside it and
   those are kept. The last metadata block gets its last-block flag set
   before the first audio frame (0xFF sync). ffmpeg zeroes STREAMINFO's
   total samples in Ogg (the count lives in the end-of-stream granule
   position), so patch it from the last granule: drflac then has an exact
   length and can build its seek table. Returns a malloc'd stream the
   caller frees, NULL on malformed input. */
static unsigned char *oggflac_demux(const unsigned char *raw, size_t n,
                                    size_t *olen, long long *total_out) {
    unsigned char *out = malloc(n + 4);
    if (!out) return NULL;
    size_t o = 0, pstart = 4;
    memcpy(out + o, "fLaC", 4); o += 4;
    long long serial = -1, gran = -1;
    int in_audio = 0;
    size_t i = 0;
    while (i + 27 <= n) {
        if (memcmp(raw + i, "OggS", 4)) break;
        int nseg = raw[i + 26];
        if (i + 27 + (size_t)nseg > n) break;
        const unsigned char *seg = raw + i + 27;
        size_t body = i + 27 + (size_t)nseg, blen = 0;
        for (int s = 0; s < nseg; s++) blen += seg[s];
        if (body + blen > n) break;
        long long ser = (long long)rd32(raw + i + 14);
        if (serial < 0) serial = ser;
        long long pg = rd64(raw + i + 6);
        if (ser == serial && pg >= 0) gran = pg;
        size_t bo = body;
        for (int s = 0; s < nseg; s++) {
            size_t sl = seg[s];
            memcpy(out + o, raw + bo, sl);
            o += sl; bo += sl;
            if (sl < 255) { /* packet complete */
                size_t plen = o - pstart;
                if (!in_audio && plen >= 5 && out[pstart] == 0x7F &&
                    !memcmp(out + pstart + 1, "FLAC", 4)) {
                    size_t skip = plen >= 13 && !memcmp(out + pstart + 9, "fLaC", 4)
                                  ? 13 : plen;
                    if (skip < plen) memmove(out + pstart, out + pstart + skip, plen - skip);
                    o -= skip;
                } else if (!in_audio && plen >= 1 && out[pstart] == 0xFF) {
                    in_audio = 1;
                    out[flac_last_blk(out, 4, pstart)] |= 0x80;
                }
                pstart = o;
            }
        }
        i = body + blen;
    }
    if (!in_audio || o <= 4) { free(out); return NULL; }
    long long total = gran > 0 ? gran : 0;
    if (o >= 4 + 4 + 34 && (out[4] & 0x7F) == 0) {
        unsigned char *si = out + 8; /* STREAMINFO body */
        long long ns = ((long long)(si[13] & 0x0F) << 32) |
                       ((unsigned)si[14] << 24) | ((unsigned)si[15] << 16) |
                       ((unsigned)si[16] << 8) | si[17];
        if (!ns && gran > 0) {
            si[13] = (unsigned char)((si[13] & 0xF0) | ((gran >> 32) & 0x0F));
            si[14] = (unsigned char)(gran >> 24);
            si[15] = (unsigned char)(gran >> 16);
            si[16] = (unsigned char)(gran >> 8);
            si[17] = (unsigned char)gran;
            ns = gran;
        }
        if (ns > 0) total = ns;
    }
    *olen = o;
    *total_out = total;
    return out;
}

/* ---- aac / wma / anything else via libav* (optional, HAVE_FFMPEG) ----
   DISPATCH ONLY. The libav* calls and every struct-field read live in
   ffab.c, compiled once per supported libav major against that major's
   vendored headers (vendor/ffmpeg — the layout rule is documented there).
   This file dlopens solely to read avutil_version()/avcodec_version()
   (plain functions, no structs: safe against ANY runtime libav), matches
   the runtime pair against each variant's compiled pair, and drives the
   winner through its FFApi — plain ints and opaque pointers only, so no
   libav type crosses and no field is ever misread across majors. */
#ifdef HAVE_FFMPEG
#define FF_API_PTR(n) &LV_JOIN(LV_JOIN(ff, n), _api),
static const FFApi *const FF_APIS[] = { LV_FFAPI_LIST(FF_API_PTR) };
#undef FF_API_PTR
static const FFApi *ff_api;  /* picked once: libav cannot swap under a running
                                process (libav.dir changes restart by contract) */
static int ff_api_tried;     /* 0 untried, 1 matched, -1 no variant matched */

static const FFApi *ff_match(unsigned ru, unsigned rc) {
    for (size_t i = 0; i < sizeof FF_APIS / sizeof FF_APIS[0]; i++)
        if ((unsigned)FF_APIS[i]->lavu_major == ru &&
            (unsigned)FF_APIS[i]->lavc_major == rc)
            return FF_APIS[i];
    return NULL;
}
static unsigned ff_major(void *h, const char *fn) {
    unsigned (*v)(void) = (unsigned (*)(void))dlsym(h, fn);
    return v ? v() >> 16 : 0;
}
static const FFApi *ff_pick(void) {
    if (ff_api_tried) return ff_api;
    ff_api_tried = -1;
    void *hu = ff_dlopen("libavutil");
    void *hc = ff_dlopen("libavcodec");
    if (hu && hc) {
        ff_api = ff_match(ff_major(hu, "avutil_version"),
                          ff_major(hc, "avcodec_version"));
        if (ff_api) ff_api_tried = 1;
    }
    if (hu) dlclose(hu);
    if (hc) dlclose(hc);
    return ff_api;
}

/* startup availability check that does not keep libav* mapped: dlopen
   the three, then dlclose them right away (their whole dep chain gets
   unmapped again); the first real ffmpeg-format open does the full
   symbol-resolving ff_syms() load. ff_state is left untouched.
   dec_ffmpeg_probe_info additionally REPORTS (out params optional):
   ver = the release of the library actually found (av_version_info,
   which every libav we probe exports; the lavf numeric version as
   fallback), dir = the directory of the resolved libavformat file
   ("" when it cannot be reported). dladdr lives in <dlfcn.h> on both
   Linux and macOS; on the MinGW shim the module path comes from
   GetModuleFileNameA on the handle. The version string is the answer
   to "which ffmpeg is this app actually running against" — the
   options ADV line shows it, and the soname-probe range (79..54, see
   ffload.h) means a bare-name hit can be a newer major than the versioned
   candidates would ever load. State 1 requires the runtime pair to equal
   the compiled pair of one of the vendored ABI variants (ffabi.h);
   anything else found is the honest state 3. */
static void ff_probe_report(void *hu, void *hf, char *ver_out, size_t ver_n,
                            char *dir_out, size_t dir_n) {
    if (ver_out && ver_n) {
        ver_out[0] = 0;
        const char *(*avi)(void) =
            (const char *(*)(void))dlsym(hu, "av_version_info");
        int (*avf)(void) = (int (*)(void))dlsym(hf, "avformat_version");
        if (avi && avi()[0])
            lv_strlcpy(ver_out, avi(), ver_n);
        else if (avf) {
            int v = avf();
            snprintf(ver_out, ver_n, "lavf %d.%d.%d", v >> 16, (v >> 8) & 0xFF, v & 0xFF);
        }
    }
    if (dir_out && dir_n) {
        dir_out[0] = 0;
#ifdef _WIN32
        char mp[LV_MAX_PATH];
        DWORD n = GetModuleFileNameA((HMODULE)hf, mp, sizeof mp);
        if (n > 0 && n < sizeof mp) {
            if (lv_dirname(mp)) lv_strlcpy(dir_out, mp, dir_n);
        }
#else
        void (*avg)(void) = dlsym(hf, "avformat_version");
        Dl_info di;
        if (avg && dladdr(avg, &di) && di.dli_fname) {
            char mp[LV_MAX_PATH];
            lv_strlcpy(mp, di.dli_fname, sizeof mp);
            if (lv_dirname(mp)) lv_strlcpy(dir_out, mp, dir_n);
        }
#endif
    }
}

int dec_ffmpeg_probe_info(char *ver_out, size_t ver_n, char *dir_out, size_t dir_n) {
    void *hu = ff_dlopen("libavutil");
    void *hc = ff_dlopen("libavcodec");
    void *hf = ff_dlopen("libavformat");
    int ok = hu && hc && hf;
    int rc = !ok ? 2 : (ff_match(ff_major(hu, "avutil_version"),
                                 ff_major(hc, "avcodec_version")) ? 1 : 3);
    if (ok) ff_probe_report(hu, hf, ver_out, ver_n, dir_out, dir_n);
    else {
        /* never leave a previous open's report standing */
        if (ver_out && ver_n) ver_out[0] = 0;
        if (dir_out && dir_n) dir_out[0] = 0;
    }
    if (hu) dlclose(hu);
    if (hc) dlclose(hc);
    if (hf) dlclose(hf);
    return rc;
}

int dec_ffmpeg_probe(void) { return dec_ffmpeg_probe_info(NULL, 0, NULL, 0); }
#else
int dec_ffmpeg_probe_info(char *ver_out, size_t ver_n, char *dir_out, size_t dir_n) {
    if (ver_out && ver_n) ver_out[0] = 0;
    if (dir_out && dir_n) dir_out[0] = 0;
    return 0;
}
int dec_ffmpeg_probe(void) { return 0; }
#endif

/* SPC playback policy. The ID666 length tag is authoritative when present
   (the ripper timed the song): play that many seconds, then fade out over
   SPC_FADE and end. Untagged files are trimmed by silence: once audio has
   been heard, SPC_SILENCE frames of continuous near-silence with no DSP
   key-on events end the song (same fade); SPC_MAX caps runaway playback of
   files that never go quiet. Seek = reload + spc_skip: fast-forward from
   the start (skip decodes without filtering, several times faster). */
#define SPC_RATE 32000
#define SPC_FADE (2 * SPC_RATE)
#define SPC_SILENCE (3 * SPC_RATE)
#define SPC_MAX (600 * SPC_RATE)
typedef struct {
    SNES_SPC *spc;
    SPC_Filter *filter;
    unsigned char *data; /* kept: seek reloads the emulator from it */
    long size;
    long long played;  /* frames emitted since (re)load */
    long long end_at;  /* absolute frame where output stops; 0 = trim mode */
    long long fade_at; /* absolute frame where the fade-out starts; <0 none */
    long long silent;  /* consecutive silent frames (trim mode) */
    int heard;         /* non-silent output seen (arms the trim) */
} Spc;

struct Decoder {
    int fmt;
    int rate, ch;
    long long total;
    union {
        mp3dec_ex_t mp3;
        stb_vorbis *ov;
        drflac *flac;
        struct { drflac *fl; unsigned char *buf; } oflac; /* ogg flac: demuxed stream must outlive the decoder */
        drwav *wav;
        Opus *op;
        ModPlugFile *mp;
        Spc *sp;
#ifdef HAVE_FFMPEG
        void *ff;           /* FFContext * — opaque here (see ffabi.h) */
#endif
    } u;
#ifdef HAVE_FFMPEG
    const FFApi *ffapi;     /* the ABI variant that produced u.ff — MUST be
                               outside the union: it lives beside u.ff, not over it */
#endif
};

/* validate an mpeg audio frame header as a layer-iii frame and return its
   size in bytes, 0 otherwise. layer bits 00 (adts/aac) and layers i/ii are
   excluded: minimp3 decodes layer iii only, those stay with libav. */
static int mpeg_l3_size(const unsigned char *p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int ver = (p[1] >> 3) & 3, lay = (p[1] >> 1) & 3;
    int bri = (p[2] >> 4) & 0xF, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    static const int BR[][14] = {
        {  8, 16, 24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160}, /* mpeg2/2.5 l3 */
        {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
        {  8, 16, 24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160},
        { 32, 40, 48, 56, 64, 80, 96,112,128,160,192,224,256,320}, /* mpeg1 l3 */
    };
    static const int SR[][3] = {
        {11025,12000, 8000}, {0,0,0}, {22050,24000,16000}, {44100,48000,32000},
    };
    if (ver == 1 || lay != 1 || bri == 0 || bri == 15 || sri == 3) return 0;
    return (ver == 3 ? 144 : 72) * BR[ver][bri - 1] * 1000 / SR[ver][sri] + pad;
}

static int sniff(const char *path, int *fmt) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[65536] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 3) return -1;
#ifdef HAVE_FFMPEG
    if (n >= 8 && h[4]=='f'&&h[5]=='t'&&h[6]=='y'&&h[7]=='p') { *fmt = F_FFMPEG; return 0; } /* mp4/m4a */
    if (h[0]==0x30&&h[1]==0x26&&h[2]==0xB2&&h[3]==0x75) { *fmt = F_FFMPEG; return 0; }      /* asf/wma */
#endif
    if (h[0]=='O'&&h[1]=='g'&&h[2]=='g'&&h[3]=='S') {
        *fmt = F_OGG;
        unsigned char b[4096];
        FILE *g = fopen(path, "rb");
        if (g) {
            size_t m = fread(b, 1, sizeof b, g);
            fclose(g);
            /* Ogg FLAC: the 0x7F "FLAC" stream marker sits at the start of
               the first packet (checked there, not scanned: 0x7F + "FLAC"
               can occur inside vorbis codebook bytes) */
            if (m >= 28) {
                size_t bs = 27 + (size_t)b[26];
                if (bs + 5 <= m && b[bs] == 0x7F && !memcmp(b + bs + 1, "FLAC", 4))
                    *fmt = F_OGGFLAC;
            }
            for (size_t i = 0; *fmt == F_OGG && i + 8 <= m; i++)
                if (!memcmp(b + i, "OpusHead", 8)) { *fmt = F_OPUS; break; }
        }
        return 0;
    }
    if (h[0]=='f'&&h[1]=='L'&&h[2]=='a'&&h[3]=='C') { *fmt = F_FLAC; return 0; }
    if (h[0]=='R'&&h[1]=='I'&&h[2]=='F'&&h[3]=='F') { *fmt = F_WAV; return 0; }
    if (h[0]=='I'&&h[1]=='D'&&h[2]=='3') { *fmt = F_MP3; return 0; }
    if ((h[0]=='I'&&h[1]=='M'&&h[2]=='P'&&h[3]=='M') ||               /* it */
        (n >= 17 && !memcmp(h, "Extended Module: ", 17))) { *fmt = F_MOD; return 0; } /* xm */
    if (n >= 0x2C + 4 && h[0x2C]=='S'&&h[0x2D]=='C'&&h[0x2E]=='R'&&h[0x2F]=='M') { *fmt = F_MOD; return 0; } /* s3m */
    if (n >= 27 && !memcmp(h, "SNES-SPC700 Sound File Data", 27)) { *fmt = F_SPC; return 0; }
    if (n >= 1084) {                                                 /* mod: channel magic */
        static const char *modmag[] = { "M.K.", "M!K!", "M&K!", "N4C4", "FLT4", "FLT8",
                                        "4CHN", "6CHN", "8CHN", "16CH", "24CH", "32CH",
                                        "CD81", "OKTA", "FEST" };
        for (size_t i = 0; i < sizeof modmag / sizeof modmag[0]; i++)
            if (!memcmp(h + 1080, modmag[i], 4)) { *fmt = F_MOD; return 0; }
    }
    if (h[0]==0xFF && (h[1]&0xE0)==0xE0) {
#ifdef HAVE_FFMPEG
        if ((h[1]&0x06)==0x00) { *fmt = F_FFMPEG; return 0; } /* ADTS aac: layer bits are 0 */
#endif
        *fmt = F_MP3; return 0;
    }
    /* mpeg frames can hide behind junk (a zeroed id3v2 region, encoder
       pad): accept a layer-iii header whose computed frame size lands on
       the next frame sync, the same shape as ffmpeg's probe. minimp3's
       frame walker skips the junk itself once we route the file here. */
    for (size_t i = 0; i + 4 <= n; i++) {
        int fs = mpeg_l3_size(h + i);
        if (!fs) continue;
        if (i + (size_t)fs + 2 <= n) {
            if (h[i + fs] == 0xFF && (h[i + fs + 1] & 0xE0) == 0xE0) { *fmt = F_MP3; return 0; }
        } else if (n < sizeof h) { /* the frame runs to eof: whole file is in hand */
            *fmt = F_MP3; return 0;
        }
    }
#ifdef HAVE_FFMPEG
    *fmt = F_FFMPEG; return 0; /* let libav probe it */
#endif
    return -1;
}

Decoder *dec_open(const char *path) {
    int fmt = F_UNKNOWN;
    /* URL boundary (P0 gate): lv_is_url is the ONE place the string's
       shape decides; only the allowlisted schemes reach libavformat.
       A URL never touches sniff() or the large-file stat: both are
       filesystem concerns that would false-reject it. */
    int url = lv_is_url(path);
    if (url) {
#if defined(LV_URL_ENABLED) && defined(HAVE_FFMPEG)
        if (!lv_url_scheme_allowed(path, LV_URL_SCHEMES)) {
            char sch[34];
            size_t i = 0;
            while (i < sizeof sch - 1 && path[i] && path[i] != ':') { sch[i] = path[i]; i++; }
            sch[i] = 0; /* LOG THE SCHEME ONLY: never echo the full URL
                           (userinfo/credentials must not reach logs) */
            fprintf(stderr, "levari: URL scheme '%s' not allowed (http/https/icecast only)\n", sch);
            return NULL;
        }
        fmt = F_FFMPEG;
#elif defined(LV_URL_ENABLED)
        fprintf(stderr, "levari: URL playback needs the ffmpeg backend\n");
        return NULL;
#else
        /* LV_STREAMING=OFF (the per-user no-streaming build): a URL is a
           plain, nonexistent file — sniff's fopen fails it clean. The
           lv_is_url checks OUTSIDE this branch (pl_add_file, loaders,
           daemon add) are playlist hygiene and stay live in OFF builds. */
        static int url_off_warned;
        if (!url_off_warned++)
            fprintf(stderr, "levari: URL playback not compiled in (LV_STREAMING=OFF)\n");
        if (sniff(path, &fmt) != 0) return NULL;
#endif
    } else if (sniff(path, &fmt) != 0) {
        return NULL;
    }
    /* large-file ingest policy (options window ADVANCED tab, LARGE toggle):
       the slurp-based decoders (minimp3 builds a whole-file seek table,
       modplug/ogg-flac buffer the stream) would happily OOM on a
       multi-gigabyte file. Over LV_LARGE_FILE the open is refused unless
       the user opted in; tag_probe is unaffected (fixed 128KB head). */
    if (!cfg.allow_large) {
        struct stat st;
        if (stat(path, &st) == 0 && (long long)st.st_size > (256LL << 20)) {
            fprintf(stderr, "levari: %s is %lld MB, over the 256 MB ingest "
                            "cap (enable LARGE in options > advanced)\n",
                    path, (long long)st.st_size >> 20);
            return NULL;
        }
    }
    Decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    switch (fmt) {
    case F_MP3: {
        if (mp3dec_ex_open(&d->u.mp3, path, MP3D_SEEK_TO_SAMPLE) != 0) goto fail;
        d->ch = d->u.mp3.info.channels;
        d->rate = d->u.mp3.info.hz;
        /* Without MP3D_DO_NOT_SCAN the open scan-counts EXACT samples over
           the whole stream even when no Xing/VBRI tag exists. The old
           vbr_tag_found gate discarded that count for tag-less VBR files
           and fell to the first-frame CBR estimate — a stream opening on
           an 8 kbps silence frame inflated length ~9x. With no Xing there
           is no delay/padding subtraction, and the decoder emits exactly
           the samples it counts, so trust samples; keep the size-based
           estimate only when the scan produced nothing. */
        /* A frameless (tag-only / garbage-header) stream opens SUCCESSFULLY
           with samples == 0 AND channels == 0 — minimp3_ex fills info only
           from decoded frames. Integer 0/0 traps exactly like n/0: an
           unguarded division here SIGFPE'd the scan thread mid-scan
           (journal 'trap divide error' in lvscan). Skip the division and
           let the validity gate at the end of dec_open reject the decoder. */
        d->total = d->ch > 0 ? (long long)d->u.mp3.samples / d->ch : 0;
        if (d->total == 0) {
            int kbps = d->u.mp3.info.bitrate_kbps;
            long long fs = 0;
            FILE *f = fopen(path, "rb");
            if (f) { fseek(f, 0, SEEK_END); fs = ftell(f); fclose(f); }
            if (kbps > 0 && fs > 0)
                d->total = fs * 8LL * d->rate / ((long long)kbps * 1000);
        }
        break;
    }
    case F_OGG: {
        int err;
        d->u.ov = stb_vorbis_open_filename(path, &err, NULL);
        if (!d->u.ov) goto fail;
        stb_vorbis_info info = stb_vorbis_get_info(d->u.ov);
        d->ch = info.channels;
        d->rate = info.sample_rate;
        d->total = (long long)stb_vorbis_stream_length_in_samples(d->u.ov);
        break;
    }
    case F_FLAC: {
        d->u.flac = drflac_open_file(path, NULL);
        if (!d->u.flac) goto fail;
        d->ch = (int)d->u.flac->channels;
        d->rate = (int)d->u.flac->sampleRate;
        d->total = (long long)d->u.flac->totalPCMFrameCount;
        break;
    }
    case F_OGGFLAC: {
        /* demux to a native stream in memory (same slurp policy as the
           modplug path; 64MB cap) and hand it to drflac */
        FILE *f = fopen(path, "rb");
        if (!f) goto fail;
        fseeko(f, 0, SEEK_END);
        long long fs = ftello(f);
        unsigned char *raw = NULL, *stream = NULL;
        size_t olen = 0;
        long long total = 0;
        if (fs > 0 && fs <= (64LL << 20)) {
            raw = malloc((size_t)fs);
            if (raw && fseeko(f, 0, SEEK_SET) == 0 &&
                fread(raw, 1, (size_t)fs, f) == (size_t)fs)
                stream = oggflac_demux(raw, (size_t)fs, &olen, &total);
            free(raw);
        }
        fclose(f);
        if (stream) {
            drflac *dc = drflac_open_memory(stream, olen, NULL);
            if (dc) {
                d->u.oflac.fl = dc;
                d->u.oflac.buf = stream;
                d->ch = (int)dc->channels;
                d->rate = (int)dc->sampleRate;
                d->total = (long long)dc->totalPCMFrameCount
                           ? (long long)dc->totalPCMFrameCount : total;
                break;
            }
            free(stream);
        }
#ifdef HAVE_FFMPEG
        d->fmt = F_FFMPEG; /* exotic variant or over the cap: let libav try */
        goto ffmpeg;
#else
        goto fail;
#endif
        break;
    }
    case F_OPUS: {
        if (opus_syms() != 0) goto fail;
        Opus *o = calloc(1, sizeof *o);
        if (!o) goto fail;
        o->f = fopen(path, "rb");
        if (!o->f) { free(o); goto fail; }
        const unsigned char *pkt;
        int len;
        if (ogg_next_packet(o, &pkt, &len) != 0 || len < 19 || memcmp(pkt, "OpusHead", 8)) goto opusfail;
        if (pkt[8] != 1 || pkt[16] != 0) goto opusfail; /* version 1, channel mapping family 0 */
        d->ch = pkt[9];
        o->pre_skip = rd16(pkt + 10);
        if (ogg_next_packet(o, &pkt, &len) != 0 || len < 8 || memcmp(pkt, "OpusTags", 8)) goto opusfail;
        o->data_off = o->next_off; /* audio starts on the page after OpusTags */
        int err = 0;
        o->dec = opus_create(48000, d->ch, &err);
        if (!o->dec || err) goto opusfail;
        d->rate = 48000;
        o->discard = o->pre_skip;
        long long g = ogg_last_granule(o);
        d->total = g > o->pre_skip ? g - o->pre_skip : 0;
        d->u.op = o;
        break;
    opusfail:
        if (o->dec) opus_destroy(o->dec);
        if (o->f) fclose(o->f);
        free(o);
        goto fail;
    }
    case F_WAV: {
        drwav *w = malloc(sizeof *w);
        if (!w) goto fail;
        if (!drwav_init_file(w, path, NULL)) { free(w); goto fail; }
        d->u.wav = w;
        d->ch = (int)d->u.wav->channels;
        d->rate = (int)d->u.wav->sampleRate;
        d->total = (long long)d->u.wav->totalPCMFrameCount;
        break;
    }
    case F_MOD: {
        static int mp_settings_done;
        if (!mp_settings_done) {
            ModPlug_Settings st;
            ModPlug_GetSettings(&st);
            st.mChannels = 2;
            st.mBits = 16;
            st.mFrequency = 44100;
            st.mResamplingMode = MODPLUG_RESAMPLE_SPLINE;
            st.mFlags = MODPLUG_ENABLE_OVERSAMPLING;
            ModPlug_SetSettings(&st);
            mp_settings_done = 1;
        }
        long long fs = 0;
        FILE *f = fopen(path, "rb");
        if (!f) goto fail;
        fseek(f, 0, SEEK_END);
        fs = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = NULL;
        if (fs > 0 && fs <= (64LL << 20)) buf = malloc((size_t)fs);
        if (!buf || fread(buf, 1, (size_t)fs, f) != (size_t)fs) {
            free(buf);
            fclose(f);
            goto fail;
        }
        fclose(f);
        d->u.mp = ModPlug_Load(buf, (int)fs); /* libmodplug copies what it needs */
        free(buf);
        if (!d->u.mp) {
#ifdef HAVE_FFMPEG
            d->fmt = F_FFMPEG; /* exotic module variant: let libav try */
            goto ffmpeg;
#else
            goto fail;
#endif
        }
        d->ch = 2;
        d->rate = 44100;
        int ms = ModPlug_GetLength(d->u.mp);
        d->total = ms > 0 ? (long long)ms * 441 / 10 : 0;
        break;
    }
    case F_SPC: {
        long long fs = 0;
        FILE *f = fopen(path, "rb");
        if (!f) goto fail;
        fseek(f, 0, SEEK_END);
        fs = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = NULL;
        /* spc_min_file_size: the fixed layout (header + 64K RAM + DSP
           regs) must be fully present; trailer data is optional */
        if (fs >= 0x10180 && fs <= (64LL << 20)) buf = malloc((size_t)fs);
        if (!buf || fread(buf, 1, (size_t)fs, f) != (size_t)fs) {
            free(buf);
            fclose(f);
            goto fail;
        }
        fclose(f);
        SNES_SPC *spc = spc_new();
        SPC_Filter *flt = spc_filter_new();
        if (!spc || !flt || spc_load_spc(spc, buf, (long)fs) != NULL) {
            if (spc) spc_delete(spc);
            if (flt) spc_filter_delete(flt);
            free(buf);
            goto fail;
        }
        spc_clear_echo(spc); /* many rips carry garbage in the echo region */
        Spc *s = calloc(1, sizeof *s);
        if (!s) {
            spc_delete(spc);
            spc_filter_delete(flt);
            free(buf);
            goto fail;
        }
        s->spc = spc; s->filter = flt; s->data = buf; s->size = (long)fs;
        s->fade_at = -1;
        int secs = spc_tag_duration(buf, (long)fs);
        if (secs) {
            s->end_at = (long long)secs * SPC_RATE + SPC_FADE;
            s->fade_at = (long long)secs * SPC_RATE;
        }
        d->u.sp = s;
        d->ch = 2;
        d->rate = SPC_RATE;
        d->total = s->end_at;
        break;
    }
#ifdef HAVE_FFMPEG
    case F_FFMPEG: {
    ffmpeg:
        {
        /* one dispatch: the runtime pair matched an ABI variant in
           ff_pick; that variant owns every libav touch from here on
           (session shape, whitelist, interrupt cb — see ffab.c) */
        const FFApi *api = ff_pick();
        if (!api) goto fail;
        int rate = 0, ch = 0;
        long long total = 0;
        FFContext *fx = api->open(path, url, &rate, &ch, &total);
        if (!fx) goto fail;
        d->ffapi = api;
        d->u.ff = fx;
        d->rate = rate;
        d->ch = ch;
        d->total = total;
        }
        break;
    }
#endif
    default: goto fail;
    }
    /* the whole pipeline (player buffers, pan, viz, WSOLA, ALSA) assumes
       at most stereo; vorbis allows 255 channels, flac 32, wav 65535, so
       a multichannel file here would overflow the fixed sample buffers */
    if (d->ch <= 0 || d->ch > 2 || d->rate <= 0) { dec_close(d); return NULL; }
    return d;
fail:
    free(d);
    return NULL;
}

int dec_rate(const Decoder *d) { return d->rate; }
int dec_channels(const Decoder *d) { return d->ch; }
long long dec_total_frames(const Decoder *d) { return d->total; }

/* take the latest ICY StreamTitle (1 = fresh string copied out): the
   player polls this after each chunk; the read loop marks dirty only on
   a CHANGED non-empty StreamTitle, so dirty means NEW. */
int dec_take_title(Decoder *d, char *out, size_t n) {
#ifdef HAVE_FFMPEG
    if (d->fmt == F_FFMPEG && d->ffapi->take_title(d->u.ff, out, n))
        return 1;
#else
    (void)d; (void)out; (void)n;
#endif
    return 0;
}

int dec_read(Decoder *d, int16_t *out, int frames) {
    switch (d->fmt) {
    case F_MP3: {
        size_t got = mp3dec_ex_read(&d->u.mp3, (mp3d_sample_t*)out, (size_t)frames * d->ch);
        return (int)(got / (size_t)d->ch);
    }
    case F_OGG:
        return stb_vorbis_get_samples_short_interleaved(d->u.ov, d->ch, (short*)out, frames * d->ch);
    case F_FLAC:
        return (int)drflac_read_pcm_frames_s16(d->u.flac, (drflac_uint64)frames, out);
    case F_OGGFLAC:
        return (int)drflac_read_pcm_frames_s16(d->u.oflac.fl, (drflac_uint64)frames, out);
    case F_WAV:
        return (int)drwav_read_pcm_frames_s16(d->u.wav, (drwav_uint64)frames, out);
    case F_MOD: {
        int got = ModPlug_Read(d->u.mp, out, frames * 4); /* s16 stereo -> 4 bytes/frame */
        return got > 0 ? got / 4 : 0;
    }
    case F_SPC: {
        Spc *s = d->u.sp;
        if (s->end_at && s->played >= s->end_at) return 0;
        int n = frames;
        if (s->end_at && s->played + n > s->end_at)
            n = (int)(s->end_at - s->played);
        if (spc_play(s->spc, n * 2, (short *)out) != NULL) return 0;
        spc_filter_run(s->filter, (short *)out, n * 2);
        long long from = s->played;
        s->played += n;
        if (!s->end_at) {
            /* silence trim: the emulated DSP reports whether any voice
               keyed on since the last check; quiet + no key-ons for
               SPC_SILENCE frames means the song has ended */
            int loud = 0;
            for (int i = 0; i < n * 2; i++)
                if (out[i] < -32 || out[i] > 32) { loud = 1; break; }
            if (loud) { s->silent = 0; s->heard = 1; }
            else if (spc_check_kon(s->spc)) s->silent = 0;
            else if (s->heard && (s->silent += n) >= SPC_SILENCE) {
                s->end_at = s->played + SPC_FADE;
                s->fade_at = s->played;
            }
            if (!s->end_at && s->played >= SPC_MAX) {
                s->end_at = s->played + SPC_FADE;
                s->fade_at = s->played;
            }
        }
        if (s->fade_at >= 0 && s->played > s->fade_at) {
            /* linear ramp to silence across the fade window; frames
               before fade_at keep unity gain via the span clamp */
            long long span = s->end_at - s->fade_at;
            for (int i = 0; i < n; i++) {
                long long g = s->end_at - (from + i);
                if (g > span) g = span;
                if (g < 0) g = 0;
                out[i * 2]     = (int16_t)((long long)out[i * 2]     * g / span);
                out[i * 2 + 1] = (int16_t)((long long)out[i * 2 + 1] * g / span);
            }
        }
        return n;
    }
    case F_OPUS: {
        Opus *o = d->u.op;
        int copied = 0;
        while (copied < frames) {
            if (o->buf_i < o->buf_n) {
                int take = frames - copied < o->buf_n - o->buf_i ? frames - copied : o->buf_n - o->buf_i;
                memcpy(out + (size_t)copied * d->ch, o->buf + (size_t)o->buf_i * d->ch,
                       (size_t)take * d->ch * 2);
                o->buf_i += take;
                copied += take;
                continue;
            }
            const unsigned char *pkt;
            int len;
            if (ogg_next_packet(o, &pkt, &len) != 0) break;
            int n = opus_dec(o->dec, pkt, len, o->tmp, 5760, 0);
            if (n <= 0) continue;
            int off = 0;
            if (o->discard > 0) {
                int sk = (long long)n < o->discard ? n : (int)o->discard;
                o->discard -= sk;
                off = sk;
                n -= sk;
                if (n <= 0) continue;
            }
            memcpy(o->buf, o->tmp + (size_t)off * d->ch, (size_t)n * d->ch * 2);
            o->buf_n = n;
            o->buf_i = 0;
        }
        return copied;
    }
    case F_FFMPEG: {
#ifdef HAVE_FFMPEG
        return d->ffapi->read(d->u.ff, out, frames);
#endif
        return -1;
    }
    }
    return -1;
}

int dec_seek(Decoder *d, long long frame) {
    switch (d->fmt) {
    case F_MP3:
        return mp3dec_ex_seek(&d->u.mp3, (uint64_t)frame * d->ch) == 0 ? 0 : -1;
    case F_OGG:
        return stb_vorbis_seek(d->u.ov, (unsigned int)frame) ? 0 : -1;
    case F_FLAC:
        return drflac_seek_to_pcm_frame(d->u.flac, (drflac_uint64)frame) ? 0 : -1;
    case F_OGGFLAC:
        return drflac_seek_to_pcm_frame(d->u.oflac.fl, (drflac_uint64)frame) ? 0 : -1;
    case F_WAV:
        return drwav_seek_to_pcm_frame(d->u.wav, (drwav_uint64)frame) ? 0 : -1;
    case F_MOD:
        ModPlug_Seek(d->u.mp, (int)(frame * 1000 / d->rate)); /* order-approximate */
        return 0;
    case F_SPC: {
        Spc *s = d->u.sp;
        if (frame < 0) frame = 0;
        if (frame > (1LL << 30)) return -1; /* spc_skip takes an int count */
        if (spc_load_spc(s->spc, s->data, s->size) != NULL) return -1;
        spc_clear_echo(s->spc);
        spc_filter_clear(s->filter);
        s->played = 0;
        s->silent = 0;
        s->heard = 0;
        /* end_at/fade_at are absolute stream positions: they stay valid
           across a seek, no recompute needed */
        if (frame > 0 && spc_skip(s->spc, (int)(frame * 2)) != NULL) return -1;
        s->played = frame;
        return 0;
    }
    case F_OPUS: {
        Opus *o = d->u.op;
        long long target = frame + o->pre_skip;
        o->next_off = o->data_off;
        o->eof = 0;
        o->nseg = 0;
        o->seg_i = 0;
        o->buf_n = o->buf_i = 0;
        o->pkt_len = 0;
        long long prev = 0; /* first audio page emits from sample 0 */
        for (;;) {
            if (ogg_load_page(o) != 0) return -1;
            if (o->page_granule >= 0 && o->page_granule >= target) break;
            if (o->page_granule > prev) prev = o->page_granule;
        }
        /* decoding restarts at the found page's first packet, so that page
           emits samples [prev, page_granule): discard what precedes the
           target. (The old page_granule - target landed at
           prev + page_granule - 2*target: exact only at page center, up
           to a full page off at the edges.) */
        o->discard = target - prev;
        opus_ctl(o->dec, 4007 /* OPUS_RESET_STATE */);
        return 0;
    }
    case F_FFMPEG: {
#ifdef HAVE_FFMPEG
        return d->ffapi->seek(d->u.ff, frame);
#endif
        return -1;
    }
    }
    return -1;
}

void dec_close(Decoder *d) {
    if (!d) return;
    switch (d->fmt) {
    case F_MP3: mp3dec_ex_close(&d->u.mp3); break;
    case F_OGG: stb_vorbis_close(d->u.ov); break;
    case F_FLAC: drflac_close(d->u.flac); break;
    case F_OGGFLAC: drflac_close(d->u.oflac.fl); free(d->u.oflac.buf); break;
    case F_WAV: drwav_uninit(d->u.wav); free(d->u.wav); break;
    case F_MOD: ModPlug_Unload(d->u.mp); break;
    case F_SPC: {
        Spc *s = d->u.sp;
        if (s->spc) spc_delete(s->spc);
        if (s->filter) spc_filter_delete(s->filter);
        free(s->data);
        free(s);
        break;
    }
    case F_OPUS: {
        Opus *o = d->u.op;
        if (o->dec) opus_destroy(o->dec);
        if (o->f) fclose(o->f);
        free(o);
        break;
    }
    case F_FFMPEG: {
#ifdef HAVE_FFMPEG
        d->ffapi->close(d->u.ff);
#endif
        break;
    }
    }
    free(d);
}
