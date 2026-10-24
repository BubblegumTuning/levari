#include "skin.h"
#include "cjk_font.h"
#include <math.h>

const Rect R_TITLE   = {0, 0, MAIN_W, 22};
/* time box sized for HH:MM:SS in the 2x font (8 chars x 12px) */
const Rect R_TIME    = {8, 26, 102, 18};
const Rect R_TSCROLL = {114, 26, 193, 18};
const Rect R_INFO    = {8, 48, MAIN_W - 16, 10};
/* row 3: "VOL" + volume slider, "BIAS" + balance slider, equal width */
const Rect R_VOL     = {29, 62, 121, 12};
const Rect R_PAN     = {185, 62, 122, 12};
const Rect R_EQ      = {8, 78, 26, 12};
const Rect R_LIST    = {38, 78, 26, 12};
/* row 5: timeline slider, full width (8px margins like R_INFO) */
const Rect R_SEEK    = {8, 94, MAIN_W - 16, 12};
const Rect R_PREV    = {8, 110, 45, 18};
const Rect R_PLAY    = {58, 110, 45, 18};
const Rect R_PAUSE   = {108, 110, 45, 18};
const Rect R_STOP    = {158, 110, 45, 18};
const Rect R_NEXT    = {208, 110, 45, 18};
const Rect R_OPEN    = {258, 110, 45, 18};
const Rect R_SHUF    = {8, 132, 50, 12};
const Rect R_REPEAT  = {62, 132, 50, 12};
const Rect R_VIS     = {237, 132, 26, 12};
const Rect R_ABA     = {266, 132, 14, 12};
const Rect R_SEARCH  = {116, 132, 40, 12};
const Rect R_RPT1    = {160, 132, 40, 12};
const Rect R_ABB     = {283, 132, 14, 12};

const Rect P_TITLE   = {0, 0, PL_W, 22};
const Rect P_SHUF    = {12, 24, 23, 12};
const Rect P_REPEAT  = {35, 24, 23, 12};
const Rect P_ADD     = {58, 24, 23, 12};
const Rect P_REM     = {81, 24, 23, 12};
const Rect P_CLOSE   = {104, 24, 23, 12};
const Rect P_LIB     = {127, 24, 23, 12};
const Rect P_TEXT    = {14, 40, 251, 64};
const Rect P_SCROLL  = {267, 40, 8, 64};

static Uint32 C(SDL_Surface *s, int r, int g, int b) {
    return SDL_MapRGB(s->format, r, g, b);
}

static void fill(SDL_Surface *s, int x, int y, int w, int h, Uint32 c) {
    SDL_Rect r = {x, y, w, h};
    SDL_FillRect(s, &r, c);
}

static void hline(SDL_Surface *s, int x, int y, int w, Uint32 c) { fill(s, x, y, w, 1, c); }
static void vline(SDL_Surface *s, int x, int y, int h, Uint32 c) { fill(s, x, y, 1, h, c); }

static void bevel(SDL_Surface *s, const Rect *r, Uint32 hi, Uint32 lo) {
    hline(s, r->x, r->y, r->w, hi);
    vline(s, r->x, r->y, r->h, hi);
    hline(s, r->x, r->y + r->h - 1, r->w, lo);
    vline(s, r->x + r->w - 1, r->y, r->h, lo);
}

void skin_button(SDL_Surface *s, const Rect *r, int pressed, int active) {
    Uint32 face = C(s, active ? 74 : 64, active ? 74 : 64, active ? 84 : 64);
    Uint32 hi = C(s, 110, 110, 110), lo = C(s, 20, 20, 20);
    fill(s, r->x, r->y, r->w, r->h, face);
    if (pressed) bevel(s, r, lo, hi);
    else bevel(s, r, hi, lo);
}

void skin_slider(SDL_Surface *s, const Rect *track, int knob_x, int knob_w) {
    Uint32 groove = C(s, 24, 24, 24);
    fill(s, track->x, track->y, track->w, track->h, C(s, 44, 44, 44));
    int gy = track->y + track->h / 2 - 2;
    fill(s, track->x + 2, gy, track->w - 4, 4, groove);
    bevel(s, track, C(s, 90, 90, 90), C(s, 16, 16, 16));
    /* knob */
    Rect k = {knob_x, track->y + track->h / 2 - 6, knob_w, 12};
    fill(s, k.x, k.y, k.w, k.h, C(s, 120, 120, 120));
    bevel(s, &k, C(s, 180, 180, 180), C(s, 40, 40, 40));
}

void skin_knob(SDL_Surface *s, int x, int y, int w, int h) {
    Rect k = {x, y, w, h};
    fill(s, x, y, w, h, C(s, 120, 120, 120));
    bevel(s, &k, C(s, 180, 180, 180), C(s, 40, 40, 40));
}

void skin_window_bg(SDL_Surface *s, int w, int h) {
    SDL_FillRect(s, NULL, C(s, 44, 44, 44));
    hline(s, 0, 0, w, C(s, 96, 96, 98));
    fill(s, 0, 1, w, 10, C(s, 60, 60, 62));
    fill(s, 0, 11, w, 10, C(s, 50, 50, 52));
    hline(s, 0, 21, w, C(s, 20, 20, 20));
    bevel(s, &(Rect){0, 0, w, h}, C(s, 90, 90, 90), C(s, 16, 16, 16));
}

void skin_vslider(SDL_Surface *s, int x, int y, int h, int knob_y) {
    fill(s, x, y, 12, h, C(s, 40, 40, 40));
    fill(s, x + 5, y + 2, 2, h - 4, C(s, 20, 20, 20));
    bevel(s, &(Rect){x, y, 12, h}, C(s, 90, 90, 90), C(s, 16, 16, 16));
    skin_knob(s, x, knob_y, 12, 12);
}

void skin_draw_main_bg(SDL_Surface *s) {
    Uint32 bg = C(s, 44, 44, 44);
    SDL_FillRect(s, NULL, bg);
    /* title bar slightly lighter with gradient feel */
    Uint32 t1 = C(s, 60, 60, 62), t2 = C(s, 50, 50, 52);
    hline(s, 0, 0, MAIN_W, C(s, 96, 96, 98));
    fill(s, 0, 1, MAIN_W, 10, t1);
    fill(s, 0, 11, MAIN_W, 10, t2);
    hline(s, 0, 21, MAIN_W, C(s, 20, 20, 20));
    /* cling strip */
    fill(s, 0, 22, MAIN_W, 6, C(s, 36, 36, 36));
    for (int x = 2; x < MAIN_W; x += 4) vline(s, x, 23, 4, C(s, 56, 56, 56));
    /* time box: black inset, play time drawn in 2x font by the UI */
    fill(s, R_TIME.x, R_TIME.y, R_TIME.w, R_TIME.h, C(s, 16, 16, 16));
    bevel(s, &R_TIME, C(s, 20, 20, 20), C(s, 90, 90, 90));
    /* scrolling title strip beside the time box */
    fill(s, R_TSCROLL.x, R_TSCROLL.y, R_TSCROLL.w, R_TSCROLL.h, C(s, 16, 16, 16));
    bevel(s, &R_TSCROLL, C(s, 20, 20, 20), C(s, 90, 90, 90));
    /* window border */
    bevel(s, &(Rect){0, 0, MAIN_W, MAIN_H}, C(s, 90, 90, 90), C(s, 16, 16, 16));
}

void skin_draw_pl_bg(SDL_Surface *s, int w, int h, const Rect *text, const Rect *scroll) {
    Uint32 bg = C(s, 44, 44, 44);
    SDL_FillRect(s, NULL, bg);
    Uint32 t1 = C(s, 60, 60, 62), t2 = C(s, 50, 50, 52);
    hline(s, 0, 0, w, C(s, 96, 96, 98));
    fill(s, 0, 1, w, 10, t1);
    fill(s, 0, 11, w, 10, t2);
    hline(s, 0, 21, w, C(s, 20, 20, 20));
    /* text area black */
    fill(s, text->x, text->y, text->w, text->h, C(s, 0, 0, 0));
    bevel(s, text, C(s, 20, 20, 20), C(s, 90, 90, 90));
    /* scrollbar */
    fill(s, scroll->x, scroll->y, scroll->w, scroll->h, C(s, 30, 30, 30));
    bevel(s, scroll, C(s, 90, 90, 90), C(s, 16, 16, 16));
    bevel(s, &(Rect){0, 0, w, h}, C(s, 90, 90, 90), C(s, 16, 16, 16));
}

/* classic 5x7 font, column-major, bit0 = top row */
static const unsigned char FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},
    {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14},
    {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x51,0x29,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x21},
    {0x7F,0x41,0x41,0x21,0x1E},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},
    {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},
    {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x44,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x44,0x7F},
    {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},
    {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},
    {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x14,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x1C},
    {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x3C,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C},
    {0x64,0x54,0x4C,0x44,0x44},
    {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},
    {0x08,0x04,0x08,0x10,0x08},
};

void font_putc(SDL_Surface *s, int x, int y, int ch, Uint32 color) {
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = FONT[ch - 32];
    Uint8 *base = (Uint8 *)s->pixels;
    int bpp = s->format->BytesPerPixel;
    for (int col = 0; col < 5; col++) {
        unsigned char bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
            Uint32 *p = (Uint32 *)(base + (size_t)py * s->pitch + (size_t)px * bpp);
            *p = color;
        }
    }
}

/* ---- UTF-8 + CJK (16x16 JIS X 0208 via Unifont) text layer ---- */

int font_utf8_next(const char *s, unsigned *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0xF) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
        return 3;
    }
    *cp = '?';
    return 1;
}

/* unifont 8-wide codepoints (latin-1, curly quotes, dashes...) advance
   half-width like ascii; only fullwidth cjk takes the wide cell */
static int half16(unsigned cp) { return cp < 0x80 || cjk_halfwidth(cp); }
int font_cp_w(unsigned cp) { return half16(cp) ? 6 : CJK_GLYPH_W + 1; }
/* widths on the 16px grid: half-width is 8+1, CJK is 16+1 */
int font_cp_w16(unsigned cp) { return half16(cp) ? 9 : CJK_GLYPH_W + 1; }

/* draw one codepoint inside a 16px line box whose top is y,
   pixels clipped to [cx0, cx1) */
void font_putcp16_clip(SDL_Surface *s, int x, int y, unsigned cp, Uint32 color, int cx0, int cx1) {
    const uint16_t *g = cp < 0x80 ? ascii16_glyph(cp) : cjk_glyph(cp);
    int half = cp >= 0x80 && cjk_halfwidth(cp);
    int gw = cp < 0x80 || half ? 8 : 16;
    int goff = cp < 0x80 ? 0 : (half ? 4 : 0); /* 8-wide glyphs stored centered */
    if (!g) { font_putc(s, x, y + 9, '?', color); return; }
    Uint8 *base = (Uint8 *)s->pixels;
    int bpp = s->format->BytesPerPixel;
    for (int r = 0; r < 16; r++) {
        uint16_t bits = g[r];
        if (!bits) continue;
        int py = y + r;
        if (py < 0 || py >= s->h) continue;
        for (int c = 0; c < gw; c++) {
            if (!(bits & (0x8000 >> (goff + c)))) continue;
            int px = x + c;
            if (px < cx0 || px >= cx1 || px >= s->w) continue;
            Uint32 *p = (Uint32 *)(base + (size_t)py * s->pitch + (size_t)px * bpp);
            *p = color;
        }
    }
}

void font_putcp16(SDL_Surface *s, int x, int y, unsigned cp, Uint32 color) {
    font_putcp16_clip(s, x, y, cp, color, 0, s->w);
}

/* ASCII top at y (legacy); CJK bottom-aligned with ASCII */
void font_puts(SDL_Surface *s, int x, int y, const char *str, Uint32 color) {
    while (*str) {
        unsigned cp;
        int n = font_utf8_next(str, &cp);
        if (cp < 0x80) font_putc(s, x, y, (int)cp, color);
        else font_putcp16(s, x, y - 9, cp, color);
        x += font_cp_w(cp);
        str += n;
    }
}

/* text inside a 16px line box whose top is y */
void font_puts16(SDL_Surface *s, int x, int y, const char *str, Uint32 color) {
    while (*str) {
        unsigned cp;
        int n = font_utf8_next(str, &cp);
        font_putcp16(s, x, y, cp, color);
        x += font_cp_w16(cp);
        str += n;
    }
}

int font_w16(const char *str) {
    int n = 0;
    while (*str) {
        unsigned cp;
        int n2 = font_utf8_next(str, &cp);
        n += font_cp_w16(cp);
        str += n2;
    }
    return n ? n - 1 : 0;
}

/* bytes of s that fit in maxw pixels on the 16px grid (UTF-8 safe) */
int font_fit(const char *s, int maxw) {
    int used = 0, bytes = 0;
    const char *q = s;
    while (*q) {
        unsigned cp;
        int n = font_utf8_next(q, &cp);
        int w = font_cp_w16(cp);
        if (used + w > maxw + 1) break;
        used += w;
        bytes += n;
        q += n;
    }
    return bytes;
}

/* start of the longest trailing suffix of s that fits in maxw pixels on the
   16px grid; == s when all of s fits. UTF-8 safe: walks back over
   continuation bytes so a codepoint is never split mid-glyph. */
const char *font_fit_tail(const char *s, int maxw) {
    const char *p = s + strlen(s);
    int used = 0;
    while (p > s) {
        const char *q = p - 1;
        while (q > s && ((unsigned char)*q & 0xC0) == 0x80) q--;
        unsigned cp;
        int n = font_utf8_next(q, &cp);
        if (q + n != p) { p = q; continue; } /* malformed: drop it, keep the rest */
        int w = font_cp_w16(cp);
        if (used + w > maxw + 1) break;
        used += w;
        p = q;
    }
    return p;
}

int font_w(const char *str) {
    int n = 0;
    while (*str) {
        unsigned cp;
        int n2 = font_utf8_next(str, &cp);
        n += font_cp_w(cp);
        str += n2;
    }
    return n ? n - 1 : 0;
}

void font_puts_big(SDL_Surface *s, int x, int y, const char *str, Uint32 color) {
    for (int i = 0; str[i]; i++) {
        int ch = (unsigned char)str[i];
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *g = FONT[ch - 32];
        for (int c = 0; c < 5; c++)
            for (int r = 0; r < 7; r++)
                if (g[c] & (1 << r)) fill(s, x + c * 2, y + r * 2, 2, 2, color);
        x += 12;
    }
}

int font_w_big(const char *str) {
    int n = 0;
    for (const char *p = str; *p; p++) n++;
    return n ? n * 12 - 1 : 0;
}

/* app icon: the Voyager golden record, drawn procedurally at any size */
SDL_Surface *skin_app_icon(int size) {
    static const double ray_len[] = {0.62, 0.40, 0.55, 0.34, 0.60, 0.44,
                                     0.52, 0.36, 0.58, 0.42, 0.50, 0.38};
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    Uint32 *px = (Uint32 *)s->pixels;
    int pitch = s->pitch / 4;
    double c = size / 2.0, r = size / 2.0 - 0.5;
    Uint32 gold   = SDL_MapRGBA(s->format, 217, 164, 65, 255);
    Uint32 hi     = SDL_MapRGBA(s->format, 242, 201, 99, 255);
    Uint32 groove = SDL_MapRGBA(s->format, 168, 127, 47, 255);
    Uint32 etch   = SDL_MapRGBA(s->format, 122, 92, 32, 255);
    Uint32 hole   = SDL_MapRGBA(s->format, 46, 36, 16, 255);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double dx = x + 0.5 - c, dy = y + 0.5 - c;
            double d = sqrt(dx * dx + dy * dy), fr = d / r;
            if (d > r) continue;
            Uint32 col = gold;
            if (d > r - 1.5) col = hi;
            if (size >= 24) {
                const double bands[] = {0.86, 0.76, 0.66, 0.56};
                for (int b = 0; b < 4; b++)
                    if (fabs(fr - bands[b]) < 0.022) col = groove;
            }
            if (size >= 24 && fabs(fr - 0.20) < 0.022) col = etch;
            if (fr < 0.07) col = hole;
            px[y * pitch + x] = col;
        }
    /* pulsar-map starburst: 12 etched rays at golden-angle spacing */
    for (int i = 0; i < 12; i++) {
        double a = i * 2.39996;
        double ca = cos(a), sa = sin(a);
        double t0 = r * 0.24, t1 = r * ray_len[i];
        for (double t = t0; t <= t1; t += 0.5) {
            int x = (int)(c + ca * t), y = (int)(c + sa * t);
            if (x < 0 || x >= size || y < 0 || y >= size) continue;
            px[y * pitch + x] = etch;
            if (size >= 64) {
                if (x + 1 < size) px[y * pitch + x + 1] = etch;
                if (y + 1 < size) px[(y + 1) * pitch + x] = etch;
            }
        }
    }
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    return s;
}
