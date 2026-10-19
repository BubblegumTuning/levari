#ifndef LEVARI_AUDIO_H
#define LEVARI_AUDIO_H

#include <stdint.h>

/* returns 0 on success; probes ALSA (dlopen'd, no link-time dep) */
int  ao_open(int rate, int channels);
int  ao_write(const int16_t *pcm, int frames);
void ao_pause(void);
void ao_resume(void);
void ao_flush(void);
void ao_close(void);
const char *ao_backend(void);

#endif
