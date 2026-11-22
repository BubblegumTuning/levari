#ifndef LEVARI_UI_H
#define LEVARI_UI_H

#include "../player/player.h"
#include <SDL2/SDL.h>

int ui_run(Player *p, int show_pl);
/* side-snap for dragged windows: if (x,y) puts w within DOCK_SNAP px of a side
   of the main window, snap it flush; while snapped, align tops when they are
   within DOCK_SNAP px of each other */
void ui_snap_next_to_main(SDL_Window *w, int *x, int *y);
/* wall release kick: at the end of a window drag that met WM walls
   (xlo/xhi/ylo/yhi = last accepted positions per refused side, 0 = no
   wall), move the window 1 px off any wall it sits on, away from the
   push direction, so the next grab starts clear of the clamp */
void ui_drag_bounce(SDL_Window *w, int xlo, int xhi, int ylo, int yhi);
/* clipboard/line editing lives in textedit.c (TField + tf_*): every
   field — file, URL, options, search — rides the same ops and the one
   paste sanitizer; there are no field-local clipboard helpers */
extern unsigned ui_deadline_ms; /* 0 = none */
extern const char *ui_dump_path; /* save main+pl window BMPs once, then NULL */
extern int ui_show_eq;           /* start with EQ window visible */
extern int ui_show_vis;          /* spectrum visualization on */
extern int ui_show_search;       /* start with search window docked left */
/* wiring main() performs so the UI needs neither decode.h nor mpris.h:
   ffprobe = dec_ffmpeg_probe_info (URL/options ffmpeg status lines;
   unset = "0 = not compiled in"), raise_hook = mpris_take_raise polled
   each event-loop tick (unset = no Raise traffic) */
void ui_set_ffprobe(int (*fn)(char *ver_out, size_t ver_n, char *dir_out, size_t dir_n));
void ui_set_raise_hook(int (*fn)(void));

#endif
