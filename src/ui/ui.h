#ifndef LEVARI_UI_H
#define LEVARI_UI_H

#include "../player/player.h"
#include <SDL2/SDL.h>

int ui_run(Player *p, int show_pl);
/* side-snap for dragged windows: if (x,y) puts w within DOCK_SNAP px of a side
   of the main window, snap it flush; while snapped, align tops when they are
   within DOCK_SNAP px of each other */
void ui_snap_next_to_main(SDL_Window *w, int *x, int *y);
void ui_open_playlist(void);
/* clipboard helpers shared by the search + options text fields */
void ui_clip_copy(const char *text);
int ui_clip_paste(char *dst, size_t n); /* appends sanitized text; returns changed */
extern unsigned ui_deadline_ms; /* 0 = none */
extern const char *ui_dump_path; /* save main+pl window BMPs once, then NULL */
extern int ui_show_eq;           /* start with EQ window visible */
extern int ui_show_vis;          /* spectrum visualization on */
extern int ui_show_search;       /* start with search window docked left */

#endif
