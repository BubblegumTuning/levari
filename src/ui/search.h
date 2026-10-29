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

#endif
