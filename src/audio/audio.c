#include "audio.h"
#include "../common.h"
#include "../config/config.h"
#include "../compat/win98_diag.h" /* lv98_tracef (no-op off-era) */
#ifdef _WIN32
#include "../platform/platform.h"
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

#if defined(LV_WIN9X) && defined(_WIN32)
/* ---------- native waveOut (era-only, batch 25) ----------
   Batch 24 hardware settled the sound story: the box's winmm WORKS
   (our A twin read device 0's caps r=0; SDL's own waveOutOpen inside
   the winmm backend succeeded) — Windows system sounds play too.
   SDL's winmm backend died one line later: CreateSemaphore(NULL,...)
   returned NULL ("Couldn't create semaphore"), a kernel sync object
   this box refuses like it refused the CreateThread shapes. This
   backend walks the SAME proven road with ZERO sync objects: four
   prepared headers polled for WHDR_DONE from the player thread.
   waveOut is the only road the box has never said no to. */
#include <mmsystem.h>
#define WO_BUFS 4
#define WO_SIZE 8192 /* bytes/header: ~46 ms of 44.1k stereo */
static HWAVEOUT wo_dev;
static WAVEHDR wo_hdr[WO_BUFS];
static char wo_mem[WO_BUFS][WO_SIZE];
static unsigned char wo_busy[WO_BUFS]; /* 1 = queued, not yet DONE */

static int try_waveout(int rate, int channels) {
    WAVEFORMATEX wf;
    MMRESULT r;
    int i;
    if (waveOutGetNumDevs() == 0) {
        lv98_tracef("ao:wo nodevs");
        return -1;
    }
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = (DWORD)rate * wf.nBlockAlign;
    r = waveOutOpen(&wo_dev, 0, &wf, 0, 0, CALLBACK_NULL);
    lv98_tracef("ao:wo open r=%d h=%p", (int)r, (void *)wo_dev);
    if (r != MMSYSERR_NOERROR) return -1;
    for (i = 0; i < WO_BUFS; i++) {
        WAVEHDR *h = &wo_hdr[i];
        h->lpData = wo_mem[i];
        h->dwBufferLength = WO_SIZE;
        h->dwFlags = 0;
        r = waveOutPrepareHeader(wo_dev, h, sizeof *h);
        if (r != MMSYSERR_NOERROR) {
            lv98_tracef("ao:wo prep i=%d r=%d", i, (int)r);
            waveOutClose(wo_dev);
            wo_dev = NULL;
            return -1;
        }
        wo_busy[i] = 0;
    }
    lv98_tracef("ao:wo ready bufs=%d size=%d", WO_BUFS, WO_SIZE);
    backend = 5;
    return 0;
}
#endif

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
    if (!sdl_dev) {
        lv98_tracef("ao:fail err=%s", SDL_GetError());
        return -1;
    }
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 24: era audio driver probe + directsound override. First
       playback on hardware DECODED and queued (plyr:ok, timeline ran)
       yet heard nothing; SDL 2.0.8's Windows order tries wasapi (load
       fails on 98) then DIRECTSOUND (dsound.dll ships on 98 and its
       "success" on a broken MMDrv is indistinguishable from silence).
       Log the driver SDL chose; if it is directsound, close it and
       force winmm — the one audio path fully observable through our
       A interpositions — logging both attempts; if winmm cannot open,
       revert to SDL's default (never degrade to no-audio when a
       device existed). */
    {
        const char *drv = SDL_GetCurrentAudioDriver();
        lv98_tracef("ao:drv=%s f=%d c=%d s=%d", drv ? drv : "?",
                    have.freq, have.channels, have.samples);
        if (drv && strcmp(drv, "directsound") == 0) {
            lv98_tracef("ao:force winmm");
            SDL_CloseAudioDevice(sdl_dev);
            sdl_dev = 0;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            SDL_setenv("SDL_AUDIODRIVER", "winmm", 1);
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
                sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
            if (!sdl_dev) {
                lv98_tracef("ao:winmm fail err=%s revert", SDL_GetError());
                SDL_setenv("SDL_AUDIODRIVER", "", 1);
                if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
                    sdl_dev = SDL_OpenAudioDevice(dev, 0, &want, &have, 0);
            }
            if (!sdl_dev) return -1;
        }
    }
#else
    lv98_tracef("ao:drv=%s f=%d c=%d s=%d", SDL_GetCurrentAudioDriver(),
                have.freq, have.channels, have.samples);
#endif
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
    /* error out-param NULL (legal): the failure signal we act on is the
       NULL return, err was never read — and feeding the local's address
       to a call whose result lands in a static only trains cppcheck's
       danglingLifetime heuristic to scream here forever */
    pas = p_pa_simple_new(NULL, "levari", PA_STREAM_PLAYBACK, NULL, "levari", &pas_ss, NULL, &pas_ba, NULL);
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
#if defined(LV_WIN9X) && defined(_WIN32)
        /* era: the box's own winmm first — SDL's winmm backend is
           blocked on the semaphore wall this box hands kernel objects */
        if (try_waveout(rate, channels) == 0) return 0;
#endif
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
        int rc = SDL_QueueAudio(sdl_dev, buf, (Uint32)((size_t)frames * cur_ch * 2));
        {   /* batch 24: queue probes — first writes prove data flows,
               every failure names the SDL error (silent-audio split) */
            static int q_n, qf_n;
            if (q_n < 2) {
                q_n++;
                lv98_tracef("ao:q rc=%d q=%u", rc,
                            (unsigned)SDL_GetQueuedAudioSize(sdl_dev));
            }
            if (rc != 0 && qf_n < 4) {
                qf_n++;
                lv98_tracef("ao:qfail rc=%d err=%s", rc, SDL_GetError());
            }
        }
        size_t maxq = (size_t)cur_rate * cur_ch * 2 * 3 / 20; /* ~150ms */
        /* the queue drains at real time, so the excess bytes above maxq
           ARE the sleep we need: one computed delay per chunk instead of
           a 10 ms poll (100 wake-ups/second all playback long) */
        size_t bps = (size_t)cur_rate * cur_ch * 2;
        while (bps) {
            size_t q = SDL_GetQueuedAudioSize(sdl_dev);
            if (q <= maxq) break;
            size_t excess = q - maxq;
            Uint32 ms = (Uint32)(excess * 1000 / bps);
            SDL_Delay(ms ? ms : 1);
        }
        return frames;
    }
#if defined(LV_WIN9X) && defined(_WIN32)
    if (backend == 5) {
        const unsigned char *p = (const unsigned char *)buf;
        size_t total = (size_t)frames * (size_t)cur_ch * 2, left = total;
        int spins = 0;
        while (left) {
            int fi = -1, i;
            for (i = 0; i < WO_BUFS; i++)
                if (wo_busy[i] && (wo_hdr[i].dwFlags & WHDR_DONE))
                    wo_busy[i] = 0;
            for (i = 0; i < WO_BUFS; i++)
                if (!wo_busy[i]) { fi = i; break; }
            if (fi < 0) {
                /* backpressure = pacing: buffers drain at real time.
                   Watchdog 5 s: a wedged device must never hang the
                   player thread (short return; pos drift is the cost
                   of a dead device, not a freeze of the whole UI) */
                if (++spins > 500) {
                    static int wd_n;
                    if (wd_n < 2) {
                        wd_n++;
                        lv98_tracef("ao:wo watchdog left=%d", (int)left);
                    }
                    break;
                }
                Sleep(10);
                continue;
            }
            {
                size_t n = left < WO_SIZE ? left : WO_SIZE;
                MMRESULT r;
                static int w_n, wf_n;
                memcpy(wo_mem[fi], p, n);
                wo_hdr[fi].dwBufferLength = (DWORD)n;
                r = waveOutWrite(wo_dev, &wo_hdr[fi], sizeof wo_hdr[fi]);
                if (w_n < 2) {
                    w_n++;
                    lv98_tracef("ao:wo w r=%d n=%u", (int)r, (unsigned)n);
                }
                if (r != MMSYSERR_NOERROR && wf_n < 4) {
                    wf_n++;
                    lv98_tracef("ao:wo wfail r=%d", (int)r);
                }
                if (r != MMSYSERR_NOERROR) break;
                wo_busy[fi] = 1;
                p += n;
                left -= n;
            }
        }
        return (int)((total - left) / (size_t)(cur_ch * 2));
    }
#endif
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
#if defined(LV_WIN9X) && defined(_WIN32)
    if (backend == 5) { /* drop queued audio (waveOutPause needs two
                           more gate names; pause-then-resume tolerates
                           the buffer-sized restart gap) */
        waveOutReset(wo_dev);
        memset(wo_busy, 0, sizeof wo_busy);
        paused = 1;
        return;
    }
#endif
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
#if defined(LV_WIN9X) && defined(_WIN32)
    } else if (backend == 5) {
        waveOutReset(wo_dev);
        memset(wo_busy, 0, sizeof wo_busy);
#endif
    } else if (backend == 1) {
        if (p_snd_pcm_drop) p_snd_pcm_drop(pcm);
        if (p_snd_pcm_prepare) p_snd_pcm_prepare(pcm);
    } else if (backend == 2) {
        /* libpulse-simple has no flush: recreate the stream */
        p_pa_simple_free(pas);
        pas = p_pa_simple_new(NULL, "levari", PA_STREAM_PLAYBACK, NULL, "levari", &pas_ss, NULL, &pas_ba, NULL);
        if (!pas) backend = 3;
    }
}

void ao_close(void) {
    if (backend == 4 && sdl_dev) {
        SDL_ClearQueuedAudio(sdl_dev);
        SDL_CloseAudioDevice(sdl_dev);
        sdl_dev = 0;
#if defined(LV_WIN9X) && defined(_WIN32)
    } else if (backend == 5 && wo_dev) {
        int i;
        waveOutReset(wo_dev);
        for (i = 0; i < WO_BUFS; i++) {
            waveOutUnprepareHeader(wo_dev, &wo_hdr[i], sizeof wo_hdr[i]);
            wo_hdr[i].dwFlags = 0;
            wo_hdr[i].dwUser = 0;
            wo_busy[i] = 0;
        }
        waveOutClose(wo_dev);
        wo_dev = NULL;
        lv98_tracef("ao:wo closed");
#endif
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
