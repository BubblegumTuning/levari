#ifndef LEVARI_SEARCH_H
#define LEVARI_SEARCH_H

#include "../player/player.h"
#include <SDL2/SDL.h>

void search_init(Player *p);
void search_open(SDL_Window *anchor);
void search_close(void);
void search_shutdown(void);
int search_is_open(void);
SDL_Window *search_window(void);
Uint32 search_window_id(void);
void search_set_focus(int f); /* 1 while the search window holds WM focus */
void search_draw(void);
void search_surface_refresh(void);
void search_mouse(SDL_Event *e);
void search_focus_drag(void);
void search_key(SDL_Event *e);
void search_text(const char *text);
void search_lib_invalidate(void); /* drop the cached library index (re-read on next draw) */
int search_filter_query(const char *text, char *out, size_t n);
/* the ONE typed-query codepoint filter (tab kept, DEL/C1/surrogates
   dropped): search_text and the merged mini field share it */

/* merged mini search (ui_pl.c hosts the field + list in the playlist
   window): shared matcher + result set. Only one host is ever live —
   entering mini closes the search window. sel/scroll are host state. */
void search_engine_run(const char *q, int scope); /* 0 = PL, 1 = LIB */
int search_engine_count(void);
const char *search_engine_disp(int i);
int search_engine_pl_idx(int i);   /* playlist row or -1 = library-only */
void search_engine_jump(int i);    /* the search window's activation path */

#endif
