/* ffab.c — the libav* backend (aac/wma/URLs and every other exotic
   format), compiled ONCE PER SUPPORTED libav MAJOR (see ffabi.h for the
   dispatch contract and the three-touch sync checklist).

   WHY per-major objects: this TU reads libav STRUCT FIELDS across the
   dlopen boundary (AVCodecContext.ch_layout/sample_rate, AVStream
   .duration/.time_base/codecpar, AVFrame/AVPacket members) — layouts
   shift when a libav major bumps, and a wrong offset reads as plausible
   garbage. The real-world shape (2026-11-07): a release
   binary built against ffmpeg 7.x headers on a machine with ffmpeg 6.1 —
   avformat_open_input, avformat_find_stream_info and even avcodec_open2
   all SUCCEED (they only receive our pointers) while cc->sample_rate
   reads 0, so every ffmpeg format fails dec_open's validity gate while
   a dlopen-only probe still claimed "detected".

   The fix is structural: this TU NEVER compiles against the system's
   libav. CMake compiles it N times against vendor/ffmpeg/ff<abi>/include
   with -DLV_FFSUF=<abi>, and the dispatcher (decode.c) picks the variant
   whose compiled pair equals the runtime avutil_version()/
   avcodec_version() majors — an honest refusal (probe state 3) when no
   variant matches. All file scope here is static; each variant exports
   exactly ONE symbol: the const FFApi instance ff<abi>_api.

   The libav* shared libraries are loaded at runtime (same pattern as
   libopus in decode.c): vendored headers are only needed to BUILD, and a
   machine without the .so files simply loses the ffmpeg formats instead
   of failing to start. */

#include "ffabi.h"
#include "ffload.h"
#include "../common.h"
#include "../config/config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <dlfcn.h>
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

/* ---- streaming constants (decision D1 + D2, internal_docs/streaming-plan.md).
   LV_URL_SCHEMES (common.h — the same list the URL-window Enter gate checks)
   gates the URL string before it ever reaches libavformat;
   FF_PROTOCOLS is the protocol_whitelist handed to avformat — it must list
   every protocol LAYER a permitted URL opens through (https rides on tls
   over tcp; icecast speaks http). file/pipe/concat/subfile are deliberately
   ABSENT: a user string must never be able to name them. ---- */
#define FF_PROTOCOLS "http,https,icecast,tls,tcp"
#define FF_IO_BUDGET_MS 10000 /* max stall of one blocking libav op (D2) */

static int ff_state; /* 0 = not tried, 1 = ok, -1 = unavailable */

static long long ff_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void (*p_av_log_set_level)(int);
/* av_rescale_q returns int64_t: an int prototype truncates the value at
   2^31 — at fine stream time bases (mp3 uses 1/14112000) that is under
   half an hour of audio, corrupting duration and seek targets */
static int64_t (*p_av_rescale_q)(int64_t, AVRational, AVRational);
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
static AVDictionaryEntry *(*p_av_dict_get)(const AVDictionary *, const char *,
                                           const AVDictionaryEntry *, int);
static AVFrame *(*p_av_frame_alloc)(void);
static void (*p_av_frame_free)(AVFrame **);
static void (*p_av_frame_unref)(AVFrame *);
static int (*p_avformat_open_input)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **);
static int (*p_avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
static void (*p_avformat_close_input)(AVFormatContext **);
static int (*p_av_read_frame)(AVFormatContext *, AVPacket *);
static int (*p_av_find_best_stream)(AVFormatContext *, enum AVMediaType, int, int, const AVCodec **, int);
static int (*p_av_seek_frame)(AVFormatContext *, int, int64_t, int);
static AVFormatContext *(*p_avformat_alloc_context)(void);
static int (*p_av_dict_set)(AVDictionary **, const char *, const char *, int);
static void (*p_av_dict_free)(AVDictionary **);

static int ff_syms(void) {
    if (ff_state) return ff_state == 1 ? 0 : -1;
    void *hu = ff_dlopen("libavutil");
    void *hc = ff_dlopen("libavcodec");
    void *hf = ff_dlopen("libavformat");
    if (!hu || !hc || !hf) { ff_state = -1; return -1; }
    /* The dispatcher matched the runtime pair before delegating here, so
       any struct read in this TU is against its own compiled layout —
       the check stays as a belt for direct callers. */
    unsigned (*uv)(void) = (unsigned (*)(void))dlsym(hu, "avutil_version");
    unsigned (*cv)(void) = (unsigned (*)(void))dlsym(hc, "avcodec_version");
    if (!uv || !cv || (uv() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
        (cv() >> 16) != LIBAVCODEC_VERSION_MAJOR) { ff_state = -1; return -1; }
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
    /* streaming (D2/D1): the interrupt cb needs a pre-allocated context,
       the whitelist needs the dict api; FFSYM fails the whole load
       closed, so an old libav without these simply has no streaming */
    FFSYM(hf, avformat_alloc_context);
    FFSYM(hu, av_dict_set);
    FFSYM(hu, av_dict_free);
    FFSYM(hu, av_dict_get);
#undef FFSYM
    ff_state = 1;
    return 0;
}

typedef struct FFContext {
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
    long long io_deadline; /* ff_now_ms() cap for the CURRENT blocking op */
    int rate, ch;      /* cached at open: read/seek never need out-params */
    char meta[256];        /* latest ICY StreamTitle (D6: only this field) */
    int meta_dirty;
} FFContext;

/* D2 interrupt: libav calls this while blocked in IO; returning 1 aborts
   that op with an error. ONE deadline per op, re-armed by the caller
   right before every blocking libav call — so the budget bounds a
   STALL, never the length of a healthy stream. */
static int ff_interrupt(void *opaque) {
    FFContext *a = (FFContext *)opaque;
    return ff_now_ms() > a->io_deadline;
}
static void ff_arm(FFContext *a) { a->io_deadline = ff_now_ms() + FF_IO_BUDGET_MS; }

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
            /* ch comes from the codec context at open time; a decoder that
               shrinks its channel layout mid-stream leaves data[c] NULL for
               the extra channels: keep those samples silent instead of
               dereferencing NULL */
            if (!fr->data[planar ? c : 0]) continue;
            const unsigned char *p = planar
                ? fr->data[c] + (size_t)(from + r) * (size_t)bps
                : fr->data[0] + ((size_t)(from + r) * (size_t)ch + (size_t)c) * (size_t)bps;
            int16_t v = 0;
            switch (fmt) {
            case AV_SAMPLE_FMT_U8:  case AV_SAMPLE_FMT_U8P:  v = (int16_t)(((int)*p - 128) << 8); break;
            case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: v = *(const int16_t *)p; break;
            case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: v = (int16_t)(*(const int32_t *)p >> 16); break;
            /* float/double through memcpy: same load, but cppcheck-clean
               (char*->float* puns were flagged once this TU got analyzed —
               pre-split, decode.c was compiled without its ffmpeg headers
               visible to cppcheck, hiding this shape) */
            case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: { float f; memcpy(&f, p, sizeof f); v = f2s(f); break; }
            case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: { double g; memcpy(&g, p, sizeof g); v = f2s((float)g); break; }
            default: break;
            }
            out[(size_t)r * (size_t)ch + (size_t)c] = v;
        }
}

static FFContext *abi_open(const char *path, int url,
                           int *rate, int *ch, long long *total) {
    if (ff_syms() != 0) return NULL;
    FFContext *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    p_av_log_set_level(AV_LOG_QUIET);
    AVDictionary *opts = NULL;
    if (url) {
        /* URL session: the interrupt cb can only be installed on a
           pre-allocated context (libav would otherwise allocate its
           own inside open), and the protocol whitelist is the security
           gate for the user string. */
        a->ic = p_avformat_alloc_context();
        if (!a->ic) { free(a); return NULL; }
        a->ic->interrupt_callback.callback = ff_interrupt;
        a->ic->interrupt_callback.opaque = a;
        ff_arm(a);
        p_av_dict_set(&opts, "protocol_whitelist", FF_PROTOCOLS, 0);
    }
    int r = p_avformat_open_input(&a->ic, path, NULL, url ? &opts : NULL);
    if (url) p_av_dict_free(&opts); /* consumes any leftovers (or the
                                        unconsumed remainder on fail) */
    if (r != 0) { free(a); return NULL; }
    if (url) ff_arm(a); /* find_stream_info probes as long as data
                           arrives; the budget bounds each STALL */
    if (p_avformat_find_stream_info(a->ic, NULL) < 0) { p_avformat_close_input(&a->ic); free(a); return NULL; }
    a->si = p_av_find_best_stream(a->ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (a->si < 0) { p_avformat_close_input(&a->ic); free(a); return NULL; }
    AVStream *st = a->ic->streams[a->si];
    AVCodecContext *cc = p_avcodec_alloc_context3(p_avcodec_find_decoder(st->codecpar->codec_id));
    if (!cc || p_avcodec_parameters_to_context(cc, st->codecpar) < 0 || p_avcodec_open2(cc, NULL, NULL) < 0) {
        if (cc) p_avcodec_free_context(&cc);
        p_avformat_close_input(&a->ic);
        free(a);
        return NULL;
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
        return NULL;
    }
    *rate = cc->sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
    *ch = cc->ch_layout.nb_channels;
#else
    *ch = cc->channels;
#endif
    a->rate = *rate;
    a->ch = *ch;
    /* rate > 0: a crafted container can carry codecpar.sample_rate == 0,
       and a {1,0} rational makes av_rescale_q divide by zero before the
       validity gate at the end of dec_open ever sees the decoder */
    if (st->duration > 0 && st->time_base.num > 0 && *rate > 0)
        *total = p_av_rescale_q(st->duration, st->time_base, (AVRational){1, *rate});
    else if (a->ic->duration > 0)
        *total = a->ic->duration * *rate / AV_TIME_BASE;
    return a;
}

static int abi_read(FFContext *a, int16_t *out, int frames) {
    int copied = 0;
    while (copied < frames) {
        if (!a->fr_valid) {
            for (;;) {
                if (!a->pkt_done) {
                    ff_arm(a); /* one stall budget per read op */
                    if (p_av_read_frame(a->ic, a->pkt) < 0) {
                        a->pkt_done = 1;
                        p_avcodec_send_packet(a->cc, NULL);
                    } else {
                        /* ICY updates ride the protocol->format
                           metadata forwarding (libavformat merges the
                           http dict into ic->metadata and raises the
                           event flag — NOT packet side data, which
                           only carries demuxer-embedded tags); quotes
                           and ';' are stripped by ffmpeg itself. The
                           merged dict also holds header fields, so a
                           title-less update re-mentions the last
                           StreamTitle: only a CHANGED non-empty value
                           is dirty. Empty (StreamTitle='';) keeps the
                           last title. */
                        if (a->ic->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED) {
                            a->ic->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
                            AVDictionaryEntry *mt = p_av_dict_get(
                                a->ic->metadata, "StreamTitle", NULL,
                                AV_DICT_IGNORE_SUFFIX);
                            if (mt && mt->value[0] && strcmp(a->meta, mt->value)) {
                                lv_strlcpy(a->meta, mt->value, sizeof a->meta);
                                a->meta_dirty = 1;
                            }
                        }
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
                                                   (AVRational){1, a->rate});
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
        ff_copy(a->fr, a->fr_i, take, a->ch, out + (size_t)copied * (size_t)a->ch);
        a->fr_i += take;
        copied += take;
        if (a->fr_i >= a->fr->nb_samples) { a->fr_valid = 0; p_av_frame_unref(a->fr); }
    }
    return copied;
}

static int abi_seek(FFContext *a, long long frame) {
    AVStream *st = a->ic->streams[a->si];
    int64_t ts = p_av_rescale_q(frame, (AVRational){1, a->rate}, st->time_base);
    ff_arm(a); /* a seek on a remote stream can block like a read */
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
}

static int abi_take_title(FFContext *a, char *out, size_t n) {
    if (!a->meta_dirty) return 0;
    a->meta_dirty = 0;
    lv_strlcpy(out, a->meta, n);
    return 1;
}

static void abi_close(FFContext *a) {
    p_av_frame_free(&a->fr);
    p_av_packet_free(&a->pkt);
    p_avcodec_free_context(&a->cc);
    p_avformat_close_input(&a->ic);
    free(a);
}

#define FF_API_SYM LV_JOIN(LV_JOIN(ff, LV_FFSUF), _api)
const FFApi FF_API_SYM = {
    abi_open, abi_read, abi_seek, abi_close, abi_take_title,
    LIBAVUTIL_VERSION_MAJOR, LIBAVCODEC_VERSION_MAJOR,
};
