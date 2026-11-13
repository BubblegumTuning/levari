#ifndef LV_X8BIT_H
#define LV_X8BIT_H
#include <SDL.h>

/* Window presentation with an 8-bit takeover path.

   On X11 screens whose visual is 8-bit PseudoColor/GrayScale, SDL refuses
   window surfaces outright: SDL_GetWindowSurface returns NULL and drawing
   on it used to SIGSEGV the first frame (skin.c, the P4 NV40 8-bit root).
   lv_win_surface() detects that case and hands back a private ARGB8888
   surface instead; lv_win_present() then quantizes it (6x6x6 cube, ordered
   dither) and delivers pixels with an indexed XPutImage, honoring dirty
   rects. On every other platform/visual both functions are exactly the
   SDL calls they replace, so the call sites carry no ifdefs.

   Xlib is dlopen'd, never linked: the legacy artifact keeps zero link-time
   X11 dependencies (SDL_X11_SHARED dlopens X11 at runtime; a bare -lX11
   here would destroy that link shape). See src/ui/x8bit.c. */

SDL_Surface *lv_win_surface(SDL_Window *w);
void lv_win_present(SDL_Window *w, const SDL_Rect *rgn, int n);
void lv_win_release(SDL_Window *w); /* call before SDL_DestroyWindow */

#endif
