#ifndef LEVARI_SKIN_H
#define LEVARI_SKIN_H

#include <SDL2/SDL.h>

/* main window layout: stacked rows, nothing overlaps anything */
#define MAIN_W 315
#define MAIN_H 150
#define PL_W MAIN_W   /* default playlist width matches the main window */
#define PL_H 116

typedef struct { int x, y, w, h; } Rect;

extern const Rect R_TITLE;
/* row 1: time box (2x font) + scrolling title beside it */
extern const Rect R_TIME, R_TSCROLL;
/* row 2: khz / kbps readout */
extern const Rect R_INFO;
/* row 3: volume */
extern const Rect R_VOL;
/* row 4: left/right balance + EQ + playlist buttons */
extern const Rect R_PAN, R_EQ, R_LIST;
/* row 5: timeline slider, on its own row below the display */
extern const Rect R_SEEK;
/* row 6: prev play pause stop next + open file/folder */
extern const Rect R_PREV, R_PLAY, R_PAUSE, R_STOP, R_NEXT, R_OPEN;
/* row 7: shuffle + repeat toggles (+ VIS / A-B on the right) */
extern const Rect R_SHUF, R_REPEAT, R_VIS, R_ABA, R_ABB, R_SEARCH, R_RPT1;

/* playlist window layout */
extern const Rect P_TITLE;
extern const Rect P_SHUF, P_REPEAT, P_ADD, P_REM, P_CLOSE, P_LIB;
extern const Rect P_TEXT, P_SCROLL;

void skin_draw_main_bg(SDL_Surface *s);
void skin_draw_pl_bg(SDL_Surface *s, int w, int h, const Rect *text, const Rect *scroll);
void skin_button(SDL_Surface *s, const Rect *r, int pressed, int active);
void skin_slider(SDL_Surface *s, const Rect *track, int knob_x, int knob_w);
void skin_knob(SDL_Surface *s, int x, int y, int w, int h);
void skin_window_bg(SDL_Surface *s, int w, int h);
void skin_vslider(SDL_Surface *s, int x, int y, int h, int knob_y);

void font_putc(SDL_Surface *s, int x, int y, int ch, Uint32 color);
void font_puts(SDL_Surface *s, int x, int y, const char *str, Uint32 color);
int  font_w(const char *str);
/* UTF-8 aware: CJK codepoints render as 16x16 JIS X 0208 glyphs */
void font_puts16(SDL_Surface *s, int x, int y, const char *str, Uint32 color);
void font_putcp16(SDL_Surface *s, int x, int y, unsigned cp, Uint32 color);
void font_putcp16_clip(SDL_Surface *s, int x, int y, unsigned cp, Uint32 color, int cx0, int cx1);
int  font_cp_w(unsigned cp);
int  font_cp_w16(unsigned cp);
int  font_w16(const char *str);
int  font_utf8_next(const char *s, unsigned *cp);
int  font_fit(const char *s, int maxw);
/* start of the trailing suffix that fits; == s when all of s fits */
const char *font_fit_tail(const char *s, int maxw);
/* double-size (10x14) variant for the play-time box */
void font_puts_big(SDL_Surface *s, int x, int y, const char *str, Uint32 color);
int  font_w_big(const char *str);

SDL_Surface *skin_app_icon(int size);

#endif
