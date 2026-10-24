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

#include "decode.h"
#include "../common.h"
#include "../config/config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <dlfcn.h>
#endif
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#endif

enum { F_UNKNOWN, F_MP3, F_OGG, F_OPUS, F_FLAC, F_OGGFLAC, F_WAV, F_FFMPEG, F_MOD };

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
    long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
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
   the libav* shared libraries are loaded at runtime (same pattern as
   libopus above): dev headers are only needed to build, and a machine
   without the .so files simply loses the ffmpeg formats instead of
   failing to start. */
#ifdef HAVE_FFMPEG
static int ff_state; /* 0 = not tried, 1 = ok, -1 = unavailable */

static void (*p_av_log_set_level)(int);
static int (*p_av_rescale_q)(int64_t, AVRational, AVRational);
static int (*p_av_sample_fmt_is_planar)(enum AVSampleFormat);
static int (*p_av_get_bytes_per_sample)(enum AVSampleFormat);
static const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID);
static AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *);
static int (*p_avcodec_parameters_to_context)(AVCodecContext *, const AVCodecParameters *);
static int (*p_avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
static void (*p_avcodec_free_context)(AVCodecContext **);
static int (*p_avcodec_send_packet)(AVCodecContext *, const AVPacket *);
static int (*p_avcodec_receive_frame)(AVCodecContext *, AVFrame *);
static void (*p_avcodec_flush_buffers)(AVCodecContext *);
static AVPacket *(*p_av_packet_alloc)(void);
static void (*p_av_packet_free)(AVPacket **);
static void (*p_av_packet_unref)(AVPacket *);
static AVFrame *(*p_av_frame_alloc)(void);
static void (*p_av_frame_free)(AVFrame **);
static void (*p_av_frame_unref)(AVFrame *);
static int (*p_avformat_open_input)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
static int (*p_avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
static void (*p_avformat_close_input)(AVFormatContext **);
static int (*p_av_read_frame)(AVFormatContext *, AVPacket *);
static int (*p_av_find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **, int);
static int (*p_av_seek_frame)(AVFormatContext *, int, int64_t, int);

/* tries <dir>/<base> plus versioned variants; empty dir = bare soname.
   Linux: libfoo.so, libfoo.so.N. macOS: libfoo.dylib, libfoo.N.dylib. */
static void *ff_try_dir(const char *dir, const char *base) {
    char nm[LV_MAX_PATH];
    int p = (dir && *dir) ? snprintf(nm, sizeof nm, "%s/", dir) : 0;
    if (p < 0 || (size_t)p >= sizeof nm) return NULL;
    void *h;
#ifdef __APPLE__
    snprintf(nm + p, sizeof nm - p, "%s.dylib", base);
    h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    for (int v = 62; v >= 54; v--) {
        snprintf(nm + p, sizeof nm - p, "%s.%d.dylib", base, v);
        h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
#else
    snprintf(nm + p, sizeof nm - p, "%s.so", base);
    h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    for (int v = 62; v >= 54; v--) {
        snprintf(nm + p, sizeof nm - p, "%s.so.%d", base, v);
        h = dlopen(nm, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
#endif
    return NULL;
}

static void *ff_dlopen(const char *base) {
    if (cfg.libav_dir[0]) { /* config-provided directory first */
        char ld[LV_MAX_PATH];
        lv_expand_home(cfg.libav_dir, ld, sizeof ld);
        void *h = ff_try_dir(ld, base);
        if (h) return h;
    }
#ifdef __APPLE__
    /* Homebrew's ffmpeg keg is not on dyld's bare-soname search path */
    static const char *KEGS[] = { "/opt/homebrew/opt/ffmpeg/lib", "/usr/local/opt/ffmpeg/lib" };
    for (size_t i = 0; i < sizeof KEGS / sizeof KEGS[0]; i++) {
        void *h = ff_try_dir(KEGS[i], base);
        if (h) return h;
    }
#endif
    return ff_try_dir("", base);
}

static int ff_syms(void) {
    if (ff_state) return ff_state == 1 ? 0 : -1;
    void *hu = ff_dlopen("libavutil");
    void *hc = ff_dlopen("libavcodec");
    void *hf = ff_dlopen("libavformat");
    if (!hu || !hc || !hf) { ff_state = -1; return -1; }
#define FFSYM(h, fn) do { p_##fn = (__typeof__(p_##fn))dlsym(h, #fn); \
    if (!p_##fn) { ff_state = -1; return -1; } } while (0)
    FFSYM(hu, av_log_set_level);
    FFSYM(hu, av_rescale_q);
    FFSYM(hu, av_sample_fmt_is_planar);
    FFSYM(hu, av_get_bytes_per_sample);
    FFSYM(hc, avcodec_find_decoder);
    FFSYM(hc, avcodec_alloc_context3);
    FFSYM(hc, avcodec_parameters_to_context);
    FFSYM(hc, avcodec_open2);
    FFSYM(hc, avcodec_free_context);
    FFSYM(hc, avcodec_send_packet);
    FFSYM(hc, avcodec_receive_frame);
    FFSYM(hc, avcodec_flush_buffers);
    FFSYM(hc, av_packet_alloc);
    FFSYM(hc, av_packet_free);
    FFSYM(hc, av_packet_unref);
    FFSYM(hc, av_frame_alloc);
    FFSYM(hc, av_frame_free);
    FFSYM(hc, av_frame_unref);
    FFSYM(hf, avformat_open_input);
    FFSYM(hf, avformat_find_stream_info);
    FFSYM(hf, avformat_close_input);
    FFSYM(hf, av_read_frame);
    FFSYM(hf, av_find_best_stream);
    FFSYM(hf, av_seek_frame);
#undef FFSYM
    ff_state = 1;
    return 0;
}

int dec_ffmpeg_status(void) {
    return ff_syms() == 0 ? 1 : 2;
}

typedef struct {
    AVFormatContext *ic;
    AVCodecContext *cc;
    AVPacket *pkt;
    AVFrame *fr;
    int si;            /* audio stream index */
    int pkt_done;      /* demuxer exhausted, decoder draining */
    int fr_valid;      /* fr holds samples, fr_i is the read cursor */
    int fr_i;
    int seek_pending;  /* next decoded frame decides the discard count */
    long long seek_target, skip;
} FF;

static int16_t f2s(float v) {
    float x = v * 32767.5f;
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

/* any libav sample format -> interleaved s16 */
static void ff_copy(AVFrame *fr, int from, int n, int ch, int16_t *out) {
    enum AVSampleFormat fmt = (enum AVSampleFormat)fr->format;
    int planar = p_av_sample_fmt_is_planar(fmt);
    int bps = p_av_get_bytes_per_sample(fmt);
    for (int r = 0; r < n; r++)
        for (int c = 0; c < ch; c++) {
            const unsigned char *p = planar
                ? fr->data[c] + (size_t)(from + r) * (size_t)bps
                : fr->data[0] + ((size_t)(from + r) * (size_t)ch + (size_t)c) * (size_t)bps;
            int16_t v = 0;
            switch (fmt) {
            case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  v = (int16_t)(((int)*p - 128) << 8); break;
            case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: v = *(const int16_t *)p; break;
            case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: v = (int16_t)(*(const int32_t *)p >> 16); break;
            case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: v = f2s(*(const float *)p); break;
            case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: v = f2s((float)*(const double *)p); break;
            default: break;
            }
            out[(size_t)r * (size_t)ch + (size_t)c] = v;
        }
}
#else
int dec_ffmpeg_status(void) { return 0; }
#endif

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
#ifdef HAVE_FFMPEG
        FF *ff;
#endif
    } u;
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
    if (sniff(path, &fmt) != 0) return NULL;
    Decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    switch (fmt) {
    case F_MP3: {
        if (mp3dec_ex_open(&d->u.mp3, path, MP3D_SEEK_TO_SAMPLE) != 0) goto fail;
        d->ch = d->u.mp3.info.channels;
        d->rate = d->u.mp3.info.hz;
        d->total = d->u.mp3.vbr_tag_found ? (long long)d->u.mp3.samples / d->ch : 0;
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
#ifdef HAVE_FFMPEG
    case F_FFMPEG: {
    ffmpeg:
        if (ff_syms() != 0) goto fail;
        FF *a = calloc(1, sizeof *a);
        if (!a) goto fail;
        p_av_log_set_level(AV_LOG_QUIET);
        if (p_avformat_open_input(&a->ic, path, NULL, NULL) != 0) { free(a); goto fail; }
        if (p_avformat_find_stream_info(a->ic, NULL) < 0) { p_avformat_close_input(&a->ic); free(a); goto fail; }
        a->si = p_av_find_best_stream(a->ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (a->si < 0) { p_avformat_close_input(&a->ic); free(a); goto fail; }
        AVStream *st = a->ic->streams[a->si];
        AVCodecContext *cc = p_avcodec_alloc_context3(p_avcodec_find_decoder(st->codecpar->codec_id));
        if (!cc || p_avcodec_parameters_to_context(cc, st->codecpar) < 0 || p_avcodec_open2(cc, NULL, NULL) < 0) {
            if (cc) p_avcodec_free_context(&cc);
            p_avformat_close_input(&a->ic);
            free(a);
            goto fail;
        }
        a->cc = cc;
        a->pkt = p_av_packet_alloc();
        a->fr = p_av_frame_alloc();
        if (!a->pkt || !a->fr) {
            p_av_packet_free(&a->pkt);
            p_av_frame_free(&a->fr);
            p_avcodec_free_context(&cc);
            p_avformat_close_input(&a->ic);
            free(a);
            goto fail;
        }
        d->rate = cc->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        d->ch = cc->ch_layout.nb_channels;
#else
        d->ch = cc->channels;
#endif
        if (st->duration > 0 && st->time_base.num > 0)
            d->total = p_av_rescale_q(st->duration, st->time_base, (AVRational){1, d->rate});
        else if (a->ic->duration > 0)
            d->total = a->ic->duration * d->rate / AV_TIME_BASE;
        d->u.ff = a;
        break;
    }
#endif
    default: goto fail;
    }
    /* the whole pipeline (player buffers, pan, viz, WSOLA, ALSA) assumes
       at most stereo; vorbis allows 255 channels, flac 32, wav 65535, so
       a multichannel file here would overflow the fixed sample buffers */
    if (d->ch <= 0 || d->ch > 2 || d->rate <= 0) goto fail;
    return d;
fail:
    free(d);
    return NULL;
}

int dec_rate(const Decoder *d) { return d->rate; }
int dec_channels(const Decoder *d) { return d->ch; }
long long dec_total_frames(const Decoder *d) { return d->total; }

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
        FF *a = d->u.ff;
        int copied = 0;
        while (copied < frames) {
            if (!a->fr_valid) {
                for (;;) {
                    if (!a->pkt_done) {
                        if (p_av_read_frame(a->ic, a->pkt) < 0) {
                            a->pkt_done = 1;
                            p_avcodec_send_packet(a->cc, NULL);
                        } else {
                            if (a->pkt->stream_index == a->si)
                                p_avcodec_send_packet(a->cc, a->pkt);
                            p_av_packet_unref(a->pkt);
                        }
                    }
                    int r = p_avcodec_receive_frame(a->cc, a->fr);
                    if (r == 0) {
                        a->fr_valid = 1;
                        a->fr_i = 0;
                        if (a->seek_pending) {
                            int64_t fpts = a->fr->pts;
                            long long start = a->seek_target;
                            if (fpts != AV_NOPTS_VALUE)
                                start = p_av_rescale_q(fpts, a->ic->streams[a->si]->time_base,
                                                       (AVRational){1, d->rate});
                            a->skip = a->seek_target - start;
                            if (a->skip < 0) a->skip = 0;
                            a->seek_pending = 0;
                        }
                        break;
                    }
                    if (r == AVERROR_EOF) return copied;
                    if (r == AVERROR(EAGAIN)) {
                        if (a->pkt_done) return copied;
                        continue;
                    }
                    return copied > 0 ? copied : -1;
                }
            }
            if (a->skip > 0) {
                int avail = a->fr->nb_samples - a->fr_i;
                int sk = a->skip < avail ? (int)a->skip : avail;
                a->fr_i += sk;
                a->skip -= sk;
                if (a->fr_i >= a->fr->nb_samples) { a->fr_valid = 0; p_av_frame_unref(a->fr); }
                continue;
            }
            int avail = a->fr->nb_samples - a->fr_i;
            int take = frames - copied < avail ? frames - copied : avail;
            ff_copy(a->fr, a->fr_i, take, d->ch, out + (size_t)copied * d->ch);
            a->fr_i += take;
            copied += take;
            if (a->fr_i >= a->fr->nb_samples) { a->fr_valid = 0; p_av_frame_unref(a->fr); }
        }
        return copied;
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
    case F_OPUS: {
        Opus *o = d->u.op;
        long long target = frame + o->pre_skip;
        o->next_off = o->data_off;
        o->eof = 0;
        o->nseg = 0;
        o->seg_i = 0;
        o->buf_n = o->buf_i = 0;
        o->pkt_len = 0;
        for (;;) {
            if (ogg_load_page(o) != 0) return -1;
            if (o->page_granule >= 0 && o->page_granule >= target) break;
        }
        o->discard = o->page_granule - target;
        opus_ctl(o->dec, 4007 /* OPUS_RESET_STATE */);
        return 0;
    }
    case F_FFMPEG: {
#ifdef HAVE_FFMPEG
        FF *a = d->u.ff;
        AVStream *st = a->ic->streams[a->si];
        int64_t ts = p_av_rescale_q(frame, (AVRational){1, d->rate}, st->time_base);
        if (p_av_seek_frame(a->ic, a->si, ts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
        p_avcodec_flush_buffers(a->cc);
        a->pkt_done = 0;
        a->fr_valid = 0;
        a->fr_i = 0;
        p_av_frame_unref(a->fr);
        a->seek_pending = 1;
        a->seek_target = frame;
        a->skip = 0;
        return 0;
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
    case F_OPUS: {
        Opus *o = d->u.op;
        if (o->dec) opus_destroy(o->dec);
        if (o->f) fclose(o->f);
        free(o);
        break;
    }
    case F_FFMPEG: {
#ifdef HAVE_FFMPEG
        FF *a = d->u.ff;
        p_av_frame_free(&a->fr);
        p_av_packet_free(&a->pkt);
        p_avcodec_free_context(&a->cc);
        p_avformat_close_input(&a->ic);
        free(a);
#endif
        break;
    }
    }
    free(d);
}
