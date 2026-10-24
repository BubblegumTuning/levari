#ifndef LEVARI_MPRIS_H
#define LEVARI_MPRIS_H

#include "../player/player.h"

#ifdef HAVE_MPRIS

/* registers org.mpris.MediaPlayer2.levari on the session bus and runs a
   D-Bus thread; 0 = serving, nonzero = disabled (no bus / name taken) */
int mpris_start(Player *p);
void mpris_stop(void);
/* consumes the pending Raise request (UI loop calls raise_all()) */
int mpris_take_raise(void);

#else

static inline int mpris_start(Player *p) { (void)p; return -1; }
static inline void mpris_stop(void) {}
static inline int mpris_take_raise(void) { return 0; }

#endif

#endif
