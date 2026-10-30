#include "audio.h"
#include "../common.h"
#include "../config/config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <dlfcn.h>
#endif
#include <SDL2/SDL.h>
#include <errno.h>
#include <time.h>

/* ---------- ALSA (dlopen) ---------- */
typedef void snd_pcm_t;
static void *alsa_lib;
static snd_pcm_t *pcm;
static int (*p_snd_pcm_open)(snd_pcm_t **, const char *, int, int);
static int (*p_snd_pcm_close)(snd_pcm_t *);
static int (*p_snd_pcm_set_params)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int);
static long (*p_snd_pcm_writei)(snd_pcm_t *, const void *, unsigned long);
static int (*p_snd_pcm_prepare)(snd_pcm_t *);
static int (*p_snd_pcm_drop)(snd_pcm_t *);
static int (*p_snd_pcm_drain)(snd_pcm_t *);
static int (*p_snd_pcm_pause)(snd_pcm_t *, int);
static int (*p_snd_pcm_recover)(snd_pcm_t *, int, int);
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

/* ---------- PulseAudio (dlopen libpulse-simple) ---------- */
typedef struct { unsigned char channels; int position[32]; } lv_map;
typedef struct { int format; unsigned rate; unsigned char channels; lv_map map; } lv_spec;
/* mirror of pa_buffer_attr (5 x uint32): the server default buffers
   ~0.5-2 s on desktop setups, and the decode-side viz/time display runs
   that far ahead of the audible audio. Request a small buffer instead:
   the display then leads by ~150 ms, imperceptible, and pause/seek
   response improves by the same margin. */
typedef struct { unsigned maxlen, tlen, prebuf, minreq, fragsize; } lv_battr;
typedef void pa_simple_t;
static void *pa_lib;
static pa_simple_t *pas;
static lv_spec pas_ss;
static lv_battr pas_ba;
static pa_simple_t *(*p_pa_simple_new)(const char *, const char *, int, const char *,
                                       const char *, const lv_spec *, const lv_map *,
                                       const void *, int *);
static int (*p_pa_simple_write)(pa_simple_t *, const void *, size_t, int *);
static int (*p_pa_simple_drain)(pa_simple_t *, int *);
static void (*p_pa_simple_free)(pa_simple_t *);
#define PA_SAMPLE_S16LE 2
#define PA_STREAM_PLAYBACK 0

/* ---------- state ---------- */
static int backend; /* 0 none, 1 alsa, 2 pulse, 3 null, 4 sdl */
static int paused;
static int cur_rate, cur_ch;

/* ---------- SDL (WASAPI/directsound under the hood) ---------- */
static SDL_AudioDeviceID sdl_dev;

static int try_sdl(int rate, int channels) {
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return -1;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = 1024;
    const char *dev = cfg.audio_device[0] ? cfg.audio_device : NULL;
    sdl_dev = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
    if (!sdl_dev) return -1;
    SDL_PauseAudioDevice(sdl_dev, 0);
    backend = 4;
    return 0;
}

static int try_pulse(int rate, int channels) {
    pa_lib = dlopen("libpulse-simple.so.0", RTLD_NOW);
    if (!pa_lib) pa_lib = dlopen("libpulse-simple.so", RTLD_NOW);
    if (!pa_lib) return -1;
    p_pa_simple_new = dlsym(pa_lib, "pa_simple_new");
    p_pa_simple_write = dlsym(pa_lib, "pa_simple_write");
    p_pa_simple_drain = dlsym(pa_lib, "pa_simple_drain");
    p_pa_simple_free = dlsym(pa_lib, "pa_simple_free");
    if (!p_pa_simple_new || !p_pa_simple_write || !p_pa_simple_free) return -1;
    pas_ss.format = PA_SAMPLE_S16LE;
    pas_ss.rate = (unsigned)rate;
    pas_ss.channels = (unsigned char)channels;
    unsigned bps = (unsigned)rate * (unsigned)channels * 2u; /* s16 bytes/sec */
    pas_ba.maxlen   = bps / 2;       /* 500 ms hard ceiling */
    pas_ba.tlen     = bps * 3 / 20;  /* 150 ms server-side target */
    pas_ba.prebuf   = bps * 3 / 40;  /* start playing after 75 ms */
    pas_ba.minreq   = 0;             /* server default */
    pas_ba.fragsize = 0;             /* playback: server-chosen */
    int err = 0;
    pas = p_pa_simple_new(NULL, "levari", PA_STREAM_PLAYBACK, NULL, "levari", &pas_ss, NULL, &pas_ba, &err);
    if (!pas) return -1;
    backend = 2;
    return 0;
}

static int try_alsa(int rate, int channels) {
    alsa_lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!alsa_lib) return -1;
    p_snd_pcm_open = dlsym(alsa_lib, "snd_pcm_open");
    p_snd_pcm_close = dlsym(alsa_lib, "snd_pcm_close");
    p_snd_pcm_set_params = dlsym(alsa_lib, "snd_pcm_set_params");
    p_snd_pcm_writei = dlsym(alsa_lib, "snd_pcm_writei");
    p_snd_pcm_prepare = dlsym(alsa_lib, "snd_pcm_prepare");
    p_snd_pcm_drop = dlsym(alsa_lib, "snd_pcm_drop");
    p_snd_pcm_drain = dlsym(alsa_lib, "snd_pcm_drain");
    p_snd_pcm_pause = dlsym(alsa_lib, "snd_pcm_pause");
    p_snd_pcm_recover = dlsym(alsa_lib, "snd_pcm_recover");
    if (!p_snd_pcm_open || !p_snd_pcm_close || !p_snd_pcm_set_params || !p_snd_pcm_writei)
        return -1;
    if (p_snd_pcm_open(&pcm, cfg.audio_device[0] ? cfg.audio_device : "default", 0, 0) < 0) { pcm = NULL; return -1; }
    if (p_snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned)channels, (unsigned)rate, 1, 150000) < 0) {
        p_snd_pcm_close(pcm); pcm = NULL; return -1;
    }
    backend = 1;
    return 0;
}

int ao_open(int rate, int channels) {
    cur_rate = rate; cur_ch = channels;
    paused = 0;
    const char *want = cfg.audio_backend;
    if (!want[0] || !lv_strcasecmp(want, "auto")) {
#ifdef _WIN32
        if (try_sdl(rate, channels) == 0) return 0;
#else
        if (try_pulse(rate, channels) == 0) return 0;
        if (try_alsa(rate, channels) == 0) return 0;
        if (try_sdl(rate, channels) == 0) return 0;
#endif
    } else if (!lv_strcasecmp(want, "pulse")) {
        if (try_pulse(rate, channels) == 0) return 0;
    } else if (!lv_strcasecmp(want, "alsa")) {
        if (try_alsa(rate, channels) == 0) return 0;
    } else if (!lv_strcasecmp(want, "sdl")) {
        if (try_sdl(rate, channels) == 0) return 0;
    }
    backend = 3; /* null */
    return 0;
}

int ao_write(const int16_t *buf, int frames) {
    if (paused) return frames;
    if (backend == 2) {
        size_t bytes = (size_t)frames * cur_ch * 2;
        int err = 0;
        if (p_pa_simple_write(pas, buf, bytes, &err) < 0) return -1;
        return frames;
    }
    if (backend == 4) {
        SDL_QueueAudio(sdl_dev, buf, (Uint32)((size_t)frames * cur_ch * 2));
        size_t maxq = (size_t)cur_rate * cur_ch * 2 * 3 / 20; /* ~150ms */
        while (SDL_GetQueuedAudioSize(sdl_dev) > maxq) SDL_Delay(10);
        return frames;
    }
    if (backend == 1) {
        int done = 0;
        while (done < frames) {
            long r = p_snd_pcm_writei(pcm, buf + (size_t)done * cur_ch,
                                      (unsigned long)(frames - done));
            if (r == -EPIPE) { if (p_snd_pcm_prepare) p_snd_pcm_prepare(pcm); continue; }
            if (r < 0 && p_snd_pcm_recover) { if (p_snd_pcm_recover(pcm, (int)r, 1) == 0) continue; }
            if (r < 0) return done;
            done += (int)r;
        }
        return done;
    }
    /* null: simulate consumption */
    if (cur_rate > 0) {
        unsigned us = (unsigned)((long long)frames * 1000000 / cur_rate);
        struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
        nanosleep(&ts, NULL);
    }
    return frames;
}

void ao_pause(void) {
    if (paused) return;
    if (backend == 4) { SDL_PauseAudioDevice(sdl_dev, 1); paused = 1; return; }
    if (backend == 1 && p_snd_pcm_pause && p_snd_pcm_pause(pcm, 1) == 0) { paused = 1; return; }
    if (backend == 1 && p_snd_pcm_drop) p_snd_pcm_drop(pcm);
    paused = 1;
}

void ao_resume(void) {
    if (!paused) return;
    if (backend == 4) { SDL_PauseAudioDevice(sdl_dev, 0); paused = 0; return; }
    if (backend == 1 && p_snd_pcm_pause && p_snd_pcm_pause(pcm, 0) == 0) { paused = 0; return; }
    if (backend == 1 && p_snd_pcm_prepare) p_snd_pcm_prepare(pcm);
    paused = 0;
}

void ao_flush(void) {
    paused = 0;
    if (backend == 4) {
        SDL_ClearQueuedAudio(sdl_dev);
    } else if (backend == 1) {
        if (p_snd_pcm_drop) p_snd_pcm_drop(pcm);
        if (p_snd_pcm_prepare) p_snd_pcm_prepare(pcm);
    } else if (backend == 2) {
        /* libpulse-simple has no flush: recreate the stream */
        p_pa_simple_free(pas);
        int err = 0;
        pas = p_pa_simple_new(NULL, "levari", PA_STREAM_PLAYBACK, NULL, "levari", &pas_ss, NULL, &pas_ba, &err);
        if (!pas) backend = 3;
    }
}

void ao_close(void) {
    if (backend == 4 && sdl_dev) {
        SDL_ClearQueuedAudio(sdl_dev);
        SDL_CloseAudioDevice(sdl_dev);
        sdl_dev = 0;
    } else if (backend == 1 && pcm) {
        if (p_snd_pcm_drain) p_snd_pcm_drain(pcm);
        p_snd_pcm_close(pcm);
        pcm = NULL;
    } else if (backend == 2 && pas) {
        if (p_pa_simple_drain) p_pa_simple_drain(pas, NULL);
        p_pa_simple_free(pas);
        pas = NULL;
    }
    if (alsa_lib) { dlclose(alsa_lib); alsa_lib = NULL; }
    if (pa_lib) { dlclose(pa_lib); pa_lib = NULL; }
    backend = 0;
}

const char *ao_backend(void) {
    switch (backend) {
    case 1: return "ALSA";
    case 2: return "PulseAudio";
    case 3: return "null";
    case 4: return "SDL";
    }
    return "none";
}
