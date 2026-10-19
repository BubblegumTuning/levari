#include "ui.h"
#include "search.h"
#include "../skin/skin.h"
#include "../skin/cjk_font.h"
#include "../viz/viz.h"
#include "../config/config.h"
#include "../library/library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    B_PLAY, B_PAUSE, B_STOP, B_PREV, B_NEXT, B_OPEN,
    B_SHUF, B_REPEAT, B_EQ, B_VIS, B_LIST, B_ABA, B_ABB, B_XMAIN, B_SEARCH, B_RPT1,
    PB_SHUF, PB_REPEAT, PB_ADD, PB_REM, PB_CLOSE, PB_LIB, PB_X, FB_ADDDIR, FB_X,
    EQB_ON, EQB_FLAT, EQB_ROCK, EQB_POP, EQB_JAZZ, EQB_X,
    OPTB_SAVE, OPTB_CLOSE, OPTB_TPL, OPTB_TEQ, OPTB_TVIS, OPTB_TSRCH,
    OPTB_SCAN, OPTB_CLEAR, OPTB_CHK, OPTB_X,
    B_NONE
};

/* options window: main+playlist stack width plus room for the SCAN button,
   docked right */
enum { OPTF_LIBDIRS, OPTF_LIBIDX, OPTF_LIBAV, OPTF_BACKEND, OPTF_DEVICE, OPTF_SOCK, OPTF_N };
#define OPT_SCAN_W 53
#define OPT_SCAN_GAP 4
#define OPT_CHK_W 46
#define OPT_W (MAIN_W + OPT_SCAN_W + OPT_SCAN_GAP + OPT_SCAN_GAP + OPT_CHK_W)
#define OPT_H (MAIN_H + PL_H)
#define OPT_FLD_X 100
#define OPT_FLD_W 207
#define OPT_FLD_H 18

/* equalizer: same width as main, docked above it by default */
#define EQ_W MAIN_W
#define EQ_H 156
#define EQ_SL_Y 50
#define EQ_SL_H 70
#define EQ_PRE_X 44
#define EQ_BAND_X(i) (68 + (i) * 24)

typedef struct { int id; const Rect *r; } Btn;

static const Btn MAIN_BTNS[] = {
    {B_SHUF, &R_SHUF}, {B_REPEAT, &R_REPEAT}, {B_EQ, &R_EQ}, {B_VIS, &R_VIS},
    {B_LIST, &R_LIST},
    {B_PREV, &R_PREV}, {B_PLAY, &R_PLAY}, {B_PAUSE, &R_PAUSE},
    {B_STOP, &R_STOP}, {B_NEXT, &R_NEXT}, {B_OPEN, &R_OPEN},
    {B_ABA, &R_ABA}, {B_ABB, &R_ABB}, {B_SEARCH, &R_SEARCH}, {B_RPT1, &R_RPT1},
};
static const Btn PL_BTNS[] = {
    {PB_SHUF, &P_SHUF}, {PB_REPEAT, &P_REPEAT}, {PB_ADD, &P_ADD},
    {PB_REM, &P_REM}, {PB_CLOSE, &P_CLOSE}, {PB_LIB, &P_LIB},
};

typedef struct {
    Player *pl;
    SDL_Window *wmain, *wpl, *weq, *wfile, *wopt;
    SDL_Surface *smain, *spl, *seq, *sfile, *sopt;
    /* built-in open file/folder browser */
    char file_dir[LV_MAX_PATH];
    char **file_names;   /* entries of file_dir, dirs first, then ".." at 0 */
    int file_n, file_scroll, file_sel, file_visible;
    /* options window */
    int opt_visible, opt_focus, opt_hover;
    int opt_selall;              /* ctrl-a: whole focused field selected, next edit replaces it */
    char opt_field[OPTF_N][LV_MAX_PATH];
    int opt_pl, opt_eq, opt_vis, opt_srch;
    int opt_scanning;            /* a library scan thread is running */
    int opt_tags;                /* scan reads tags+duration, not just names */
    SDL_atomic_t opt_scan_done;  /* set by the scan thread when finished */
    int opt_scan_added, opt_scan_seen;
    char opt_status[96];         /* one-line scan/clear feedback */
    int menu;            /* right-click titlebar dropdown: 0 closed 1 open */
    int pressed;
    int drag;          /* 0 none 1 seek 2 vol 3 win-main 4 win-pl 5 plscroll 6 eq-slider 7 win-eq 8 pl-resize 9 win-file 10 pan 11 win-opt */
    int eq_band;       /* 0..9 band, 10 = preamp, while dragging */
    int pl_scroll, pl_sel;
    int pl_cur;            /* last cur index the playlist was scrolled for */
    int pl_selall;           /* ctrl-a: whole playlist selected, del/rem removes it */
    int pl_visible, eq_visible;
    int pl_w, pl_h;        /* current playlist window size */
    int pl_docked;         /* playlist attached under main window */
    int eq_on, vis_on, mini;
    char title[512];
    int scroll_off;
    int drag_gx, drag_gy;   /* global mouse pos at drag start */
    int drag_wx, drag_wy;   /* window pos at drag start */
    Uint32 focus_wid;       /* window id of the focused one of ours */
    /* windows snapped beside the main one: follow it while it moves.
       slots: 0 eq, 1 file, 2 search, 3 options; w = NULL when not snapped */
    struct { SDL_Window *w; int dx, dy; } snap[4];
} UI;

static UI ui;
unsigned ui_deadline_ms;
const char *ui_dump_path;
int ui_show_eq;
int ui_show_vis;
int ui_show_search;

static int in_rect(const Rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

void ui_clip_copy(const char *text) {
    if (text && *text) SDL_SetClipboardText(text);
}

/* append the clipboard to dst, dropping control chars; returns 1 if changed */
int ui_clip_paste(char *dst, size_t n) {
    char *clip = SDL_GetClipboardText();
    if (!clip) return 0;
    size_t len = strlen(dst);
    int changed = 0;
    const char *q = clip;
    while (*q && len + 4 < n) {
        unsigned cp;
        int k = font_utf8_next(q, &cp);
        if (cp >= 0x20 && cp != 0x7F) {
            memcpy(dst + len, q, (size_t)k);
            len += (size_t)k;
            dst[len] = 0;
            changed = 1;
        }
        q += k;
    }
    SDL_free(clip);
    return changed;
}

/* top-right corner close button, present on every window */
static Rect close_rect(int win_w) {
    Rect r = { win_w - 18, 4, 12, 12 };
    return r;
}
static void draw_x(SDL_Surface *s, const Rect *r, int pressed) {
    skin_button(s, r, pressed, 0);
    font_puts(s, r->x + (r->w - font_w("X")) / 2, r->y + 3, "X", SDL_MapRGB(s->format, 220, 220, 220));
}

#define SEEK_KNOB_W 24 /* seek knob: double the 12px vol/bias knobs */
#define PL_MIN_W 275   /* keep the whole button row visible */
#define PL_MIN_H 60
#define DOCK_SNAP 12   /* px of slack when snapping the playlist back under main */

/* playlist inner layout, follows the current window size */
static void pl_layout(Rect *text, Rect *scroll) {
    text->x = 14; text->y = 40;
    text->w = ui.pl_w - 24; text->h = ui.pl_h - 40 - 12;
    scroll->x = ui.pl_w - 8; scroll->y = 40;
    scroll->w = 8; scroll->h = text->h;
}

static void pl_dock_pos(int *x, int *y) {
    SDL_GetWindowPosition(ui.wmain, x, y);
    *y += MAIN_H;
}

static void drag_begin(SDL_Window *w) {
    SDL_GetGlobalMouseState(&ui.drag_gx, &ui.drag_gy);
    SDL_GetWindowPosition(w, &ui.drag_wx, &ui.drag_wy);
}

static void drag_move(SDL_Window *w) {
    int gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_SetWindowPosition(w, ui.drag_wx + gx - ui.drag_gx, ui.drag_wy + gy - ui.drag_gy);
}

static int snap_idx(SDL_Window *w) {
    if (w == ui.weq) return 0;
    if (w == ui.wfile) return 1;
    if (w == search_window()) return 2;
    if (w == ui.wopt) return 3;
    return -1;
}

void ui_snap_next_to_main(SDL_Window *w, int *x, int *y) {
    if (!ui.wmain || !w) return;
    int mx, my, mw, mh, ww, wh;
    SDL_GetWindowPosition(ui.wmain, &mx, &my);
    SDL_GetWindowSize(ui.wmain, &mw, &mh);
    SDL_GetWindowSize(w, &ww, &wh);
    int snapped = 0;
    if (abs(*x - (mx + mw)) <= DOCK_SNAP) { *x = mx + mw; snapped = 1; }
    else if (abs((*x + ww) - mx) <= DOCK_SNAP) { *x = mx - ww; snapped = 1; }
    if (snapped && abs(*y - my) <= DOCK_SNAP) *y = my;
    else if (abs(*y - (my - wh)) <= DOCK_SNAP && abs(*x - mx) <= DOCK_SNAP) {
        *y = my - wh; *x = mx; snapped = 1;
    }
    int i = snap_idx(w);
    if (i >= 0) {
        if (snapped) { ui.snap[i].w = w; ui.snap[i].dx = *x - mx; ui.snap[i].dy = *y - my; }
        else ui.snap[i].w = NULL;
    }
}

static void drag_move_snap(SDL_Window *w) {
    int gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    int x = ui.drag_wx + gx - ui.drag_gx, y = ui.drag_wy + gy - ui.drag_gy;
    ui_snap_next_to_main(w, &x, &y);
    SDL_SetWindowPosition(w, x, y);
}

/* playlist drag: live-snap flush under the main window's bottom edge,
   x-aligned, within DOCK_SNAP px; docked state follows live like the
   side-snapped windows so it keeps following main until dragged away */
static void drag_move_pl(void) {
    int gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    int x = ui.drag_wx + gx - ui.drag_gx, y = ui.drag_wy + gy - ui.drag_gy;
    int mx, my, mw, mh;
    SDL_GetWindowPosition(ui.wmain, &mx, &my);
    SDL_GetWindowSize(ui.wmain, &mw, &mh);
    int snapped = 0;
    if (abs(y - (my + mh)) <= DOCK_SNAP) { y = my + mh; snapped = 1; }
    if (snapped && abs(x - mx) <= DOCK_SNAP) x = mx;
    ui.pl_docked = snapped;
    SDL_SetWindowPosition(ui.wpl, x, y);
}

static Uint32 col(SDL_Surface *s, int r, int g, int b) { return SDL_MapRGB(s->format, r, g, b); }

static void tri_right(SDL_Surface *s, int x, int y, int h, Uint32 c) {
    for (int i = 0; i < h; i++) {
        int w = (h - i) / 2;
        SDL_Rect r = {x + i, y + (h / 2 - w), 1, 2 * w + (h - i) % 2};
        SDL_FillRect(s, &r, c);
    }
}
static void tri_left(SDL_Surface *s, int x, int y, int h, Uint32 c) {
    for (int i = 0; i < h; i++) {
        int w = (h - i) / 2;
        SDL_Rect r = {x + (h - 1 - i), y + (h / 2 - w), 1, 2 * w + (h - i) % 2};
        SDL_FillRect(s, &r, c);
    }
}

static void draw_icon(SDL_Surface *s, int id, const Rect *r, Uint32 c) {
    int cx = r->x + r->w / 2, cy = r->y + r->h / 2;
    switch (id) {
    case B_PLAY: tri_right(s, cx - 4, cy - 5, 10, c); break;
    case B_PAUSE:
        SDL_Rect a = {cx - 4, cy - 5, 3, 10}, b = {cx + 1, cy - 5, 3, 10};
        SDL_FillRect(s, &a, c); SDL_FillRect(s, &b, c);
        break;
    case B_STOP: { SDL_Rect q = {cx - 5, cy - 5, 10, 10}; SDL_FillRect(s, &q, c); } break;
    case B_PREV:
        tri_left(s, cx - 4, cy - 5, 10, c);
        { SDL_Rect q = {cx - 6, cy - 5, 2, 10}; SDL_FillRect(s, &q, c); }
        break;
    case B_NEXT:
        tri_right(s, cx - 5, cy - 5, 10, c);
        { SDL_Rect q = {cx + 5, cy - 5, 2, 10}; SDL_FillRect(s, &q, c); }
        break;
    case B_OPEN: /* folder icon */
        SDL_Rect tab = {cx - 7, cy - 6, 6, 1};
        SDL_Rect top = {cx - 7, cy - 4, 14, 1};
        SDL_Rect lft = {cx - 7, cy - 6, 1, 11};
        SDL_Rect rgt = {cx + 6, cy - 4, 1, 9};
        SDL_Rect bot = {cx - 7, cy + 4, 14, 1};
        SDL_FillRect(s, &tab, c); SDL_FillRect(s, &top, c);
        SDL_FillRect(s, &lft, c); SDL_FillRect(s, &rgt, c); SDL_FillRect(s, &bot, c);
        break;
    }
}

static void fmt_time(char *buf, size_t n, long long sec) {
    if (sec < 0) sec = 0;
    if (sec >= 3600) snprintf(buf, n, "%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    else snprintf(buf, n, "%lld:%02lld", sec / 60, sec % 60);
}

static void draw_main(void) {
    SDL_Surface *s = ui.smain;
    Player *p = ui.pl;
    skin_draw_main_bg(s);

    font_puts(s, 10, 6, "LEVARI", col(s, 0, 224, 255));
    { Rect x = close_rect(MAIN_W); draw_x(s, &x, ui.pressed == B_XMAIN); }

    /* row 1: play time in a box, 2x font, scrolling title beside it */
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    long long pos = p->rate ? (p->pos_frames - base) / p->rate : 0;
    char tb[16];
    fmt_time(tb, sizeof tb, p->state == ST_STOP ? 0 : pos);
    font_puts_big(s, R_TIME.x + 6, R_TIME.y + 2, tb, col(s, 0, 255, 0));

    int tx = R_TSCROLL.x + 2, tw = R_TSCROLL.w - 4;
    if (ui.vis_on) {
        static float bars[VIZ_BARS], smooth[VIZ_BARS];
        static Uint32 viz_last;
        int16_t snap[4096];
        int n = p->viz_n;
        if (n > 4096) n = 4096;
        Uint32 seq = p->viz_seq;
        /* paused/stopped (or no fresh chunk): feed silence so the bars
           decay naturally instead of freezing on the last buffer */
        if (p->state != ST_PLAY || seq == viz_last)
            memset(snap, 0, (size_t)n * sizeof(snap[0]));
        else {
            memcpy(snap, p->viz_buf, (size_t)n * 2);
            viz_last = seq;
        }
        viz_spectrum(snap, n, p->rate, bars);
        for (int b = 0; b < VIZ_BARS; b++) {
            smooth[b] = bars[b] > smooth[b] ? bars[b] : smooth[b] * 0.85f;
            int h = (int)(smooth[b] * 16.0f);
            if (h > 0) {
                int x0 = b * tw / VIZ_BARS, x1 = (b + 1) * tw / VIZ_BARS;
                SDL_Rect r = {tx + x0, R_TSCROLL.y + R_TSCROLL.h - 1 - h, x1 - x0 - 1, h};
                SDL_FillRect(s, &r, col(s, 0, 200, 60));
            }
        }
    } else if (font_w16(ui.title) <= tw) {
        font_puts16(s, tx, R_TSCROLL.y + 1, ui.title, col(s, 0, 224, 255));
    } else {
        char padded[600];
        snprintf(padded, sizeof padded, "%s   ", ui.title);
        int len = font_w16(padded);
        int off = ui.scroll_off % (len + tw);
        /* draw codepoint by codepoint, hard-clipped to the title strip so
           glyphs can never bleed into the time box */
        int x = tx - off;
        const char *q = padded;
        while (*q) {
            if (x >= tx + tw) break;
            unsigned cp;
            int n = font_utf8_next(q, &cp);
            if (x + font_cp_w16(cp) > tx)
                font_putcp16_clip(s, x, R_TSCROLL.y + 1, cp, col(s, 0, 224, 255), tx, tx + tw);
            x += font_cp_w16(cp);
            q += n;
        }
        ui.scroll_off += 2;
    }

    /* row 2: khz / kbps readout; bitrate estimated from file size over the
       decoded length (whole file, so CUE virtual tracks stay correct) */
    if (p->state != ST_STOP && p->rate) {
        char rb[48];
        int kbps = 0;
        long long tf = p->total_frames;
        if (tf > 0) {
            char path[LV_MAX_PATH];
            path[0] = 0;
            pthread_mutex_lock(&p->mtx);
            int ci = p->cur;
            if (ci >= 0 && ci < p->pl->count)
                lv_strlcpy(path, p->pl->items[ci].path, sizeof path);
            pthread_mutex_unlock(&p->mtx);
            struct stat st;
            if (path[0] && stat(path, &st) == 0 && st.st_size > 0)
                kbps = (int)((double)st.st_size * 8 * p->rate /
                             (1000 * (double)tf) + 0.5);
        }
        if (kbps > 0)
            snprintf(rb, sizeof rb, "%dkhz %dkbps %s",
                     (int)(p->rate / 1000), kbps, p->ch == 2 ? "st" : "mo");
        else
            snprintf(rb, sizeof rb, "%dkhz %s",
                     (int)(p->rate / 1000), p->ch == 2 ? "st" : "mo");
        font_puts(s, R_INFO.x + 2, R_INFO.y + 1, rb, col(s, 180, 180, 180));
    }

    for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++) {
        const Btn *b = &MAIN_BTNS[i];
        int active = 0;
        if (b->id == B_SHUF) active = p->shuffle;
        if (b->id == B_REPEAT) active = p->repeat == 1;
        if (b->id == B_RPT1) active = p->repeat == 2;
        if (b->id == B_EQ) active = p->eq.enabled;
        if (b->id == B_VIS) active = ui.vis_on;
        skin_button(s, b->r, ui.pressed == b->id, active);
        Uint32 c = col(s, 220, 220, 220);
        if (b->id == B_SHUF) font_puts(s, b->r->x + (b->r->w - font_w("SHUFFLE")) / 2, b->r->y + 3, "SHUFFLE", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_REPEAT) font_puts(s, b->r->x + (b->r->w - font_w("REPEAT")) / 2, b->r->y + 3, "REPEAT", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_EQ) font_puts(s, b->r->x + 6, b->r->y + 3, "EQ", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_VIS) font_puts(s, b->r->x + 5, b->r->y + 3, "VIS", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_LIST) font_puts(s, b->r->x + (b->r->w - font_w("LST")) / 2, b->r->y + 3, "LST", c);
        if (b->id == B_SEARCH) font_puts(s, b->r->x + (b->r->w - font_w("SRCH")) / 2, b->r->y + 3,
                                         "SRCH", search_is_open() ? col(s, 0, 255, 0) : c);
        if (b->id == B_RPT1) font_puts(s, b->r->x + (b->r->w - font_w("RPT1")) / 2, b->r->y + 3,
                                      "RPT1", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_ABA) font_puts(s, b->r->x + 3, b->r->y + 3, "A",
                                      p->ab_a >= 0 ? col(s, 0, 255, 0) : c);
        if (b->id == B_ABB) font_puts(s, b->r->x + 3, b->r->y + 3, "B",
                                      p->ab_b >= 0 ? col(s, 0, 255, 0) : c);
        draw_icon(s, b->id, b->r, c);
    }

    /* row 3: labeled volume + balance sliders share the row */
    font_puts(s, 8, 65, "VOL", col(s, 150, 150, 150));
    font_puts(s, 158, 65, "BIAS", col(s, 150, 150, 150));
    int vx = R_VOL.x + 2 + p->vol * (R_VOL.w - 16) / 128;
    skin_slider(s, &R_VOL, vx, 12);
    int px = R_PAN.x + 2 + (p->pan + 32) * (R_PAN.w - 16) / 64;
    skin_slider(s, &R_PAN, px, 12);
    /* row 5: timeline slider, its own row below the display; its knob is
       double the width of the volume/bias knobs */
    double frac = endf > base ? (double)(p->pos_frames - base) / (double)(endf - base) : 0;
    int kx = R_SEEK.x + (int)(frac * (R_SEEK.w - SEEK_KNOB_W));
    skin_slider(s, &R_SEEK, kx, SEEK_KNOB_W);

    if (ui.menu) {
        Rect mi = { 2, 22, 110, 20 };
        SDL_Rect mb = { mi.x, mi.y, mi.w, mi.h };
        SDL_FillRect(s, &mb, col(s, 40, 40, 40));
        SDL_Rect t = { mi.x, mi.y, mi.w, 1 }, b = { mi.x, mi.y + mi.h - 1, mi.w, 1 };
        SDL_Rect l = { mi.x, mi.y, 1, mi.h }, r = { mi.x + mi.w - 1, mi.y, 1, mi.h };
        Uint32 bc = col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, mi.x + 8, mi.y + 6, "OPTIONS...", col(s, 220, 220, 220));
    }
}

static void draw_pl(void) {
    SDL_Surface *s = ui.spl;
    Player *p = ui.pl;
    Playlist *pl = p->pl;
    pthread_mutex_lock(&p->mtx);
    Rect ptext, pscroll;
    pl_layout(&ptext, &pscroll);
    skin_draw_pl_bg(s, ui.pl_w, ui.pl_h, &ptext, &pscroll);
    { Rect x = close_rect(ui.pl_w); draw_x(s, &x, ui.pressed == PB_X); }

    for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++) {
        const Btn *b = &PL_BTNS[i];
        int active = 0;
        if (b->id == PB_SHUF) active = p->shuffle;
        if (b->id == PB_REPEAT) active = p->repeat == 1;
        skin_button(s, b->r, ui.pressed == b->id, active);
        Uint32 c = active ? col(s, 0, 255, 0) : col(s, 220, 220, 220);
        const char *lbl = "?";
        switch (b->id) {
        case PB_SHUF: lbl = "SHUF"; break;
        case PB_REPEAT: lbl = "RPT"; break;
        case PB_ADD: lbl = "ADD"; break;
        case PB_REM: lbl = "REM"; break;
        case PB_CLOSE: lbl = "CLO"; break;
        case PB_LIB: lbl = "LIB"; break;
        }
        font_puts(s, b->r->x + (b->r->w - font_w(lbl)) / 2, b->r->y + 3, lbl, c);
    }

    /* track list: as many 17px lines as the window height allows
       (16px CJK glyphs + 1px leading) */
    int lines = ptext.h / 17;
    /* keep the playing track in view: when cur moves (next/prev, shuffle,
       search activation) scroll just enough to show it */
    if (p->cur != ui.pl_cur) {
        ui.pl_cur = p->cur;
        if (p->cur >= 0) {
            if (p->cur < ui.pl_scroll) ui.pl_scroll = p->cur;
            else if (p->cur >= ui.pl_scroll + lines) ui.pl_scroll = p->cur - lines + 1;
        }
    }
    if (ui.pl_scroll > pl->count - lines && pl->count >= lines)
        ui.pl_scroll = pl->count - lines;
    if (ui.pl_scroll < 0) ui.pl_scroll = 0;
    for (int i = 0; i < lines; i++) {
        int idx = ui.pl_scroll + i;
        if (idx >= pl->count) break;
        Track *t = &pl->items[idx];
        char line[512];
        const char *name = t->tags.title[0] ? t->tags.title : strrchr(t->path, '/') ? strrchr(t->path, '/') + 1 : t->path;
        if (t->duration) snprintf(line, sizeof line, "%02d. %s [%d:%02d]", idx + 1, name, t->duration / 60, t->duration % 60);
        else snprintf(line, sizeof line, "%02d. %s", idx + 1, name);
        /* truncate to fit, without splitting UTF-8 sequences */
        int fit = font_fit(line, ptext.w - 4);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = col(s, 170, 170, 170);
        if (idx == p->cur) c = col(s, 0, 255, 0);
        else if (idx == ui.pl_sel || ui.pl_selall) c = col(s, 255, 255, 255);
        font_puts16(s, ptext.x + 2, ptext.y + 1 + i * 17, line, c);
    }

    /* scrollbar thumb */
    if (pl->count > lines) {
        int th = pscroll.h * lines / pl->count;
        if (th < 8) th = 8;
        int ty = pscroll.y + (pscroll.h - th) * ui.pl_scroll / (pl->count - lines);
        SDL_Rect r = {pscroll.x + 1, ty, pscroll.w - 2, th};
        SDL_FillRect(s, &r, col(s, 110, 110, 110));
    }

    /* totals, pinned to the bottom edge */
    int total = 0;
    for (int i = 0; i < pl->count; i++) total += pl->items[i].duration;
    char tb[32];
    fmt_time(tb, sizeof tb, total);
    font_puts(s, 14, ui.pl_h - 10, tb, col(s, 0, 255, 0));
    snprintf(tb, sizeof tb, "%d of %d", pl->count, pl->count);
    font_puts(s, 200, ui.pl_h - 10, tb, col(s, 170, 170, 170));

    /* winamp-style resize grip in the bottom-right corner */
    Uint32 ghi = col(s, 110, 110, 110), glo = col(s, 20, 20, 20);
    for (int d = 4; d <= 10; d += 3)
        for (int t = 0; t <= d; t++) {
            SDL_Rect r = {ui.pl_w - 1 - d + t, ui.pl_h - 1 - t, 1, 1};
            SDL_FillRect(s, &r, ghi);
            if (d < 10) { SDL_Rect q = {r.x - 1, r.y + 1, 1, 1}; SDL_FillRect(s, &q, glo); }
        }
    pthread_mutex_unlock(&p->mtx);
}

static float knob_to_db(int knob_y) {
    /* knob_y in [EQ_SL_Y, EQ_SL_Y+EQ_SL_H-12] maps +12..-20 dB */
    float t = (float)(knob_y - EQ_SL_Y) / (float)(EQ_SL_H - 12);
    return 12.0f - t * 32.0f;
}
static int db_to_knob(float db) {
    if (db > 12) db = 12;
    if (db < -20) db = -20;
    return EQ_SL_Y + (int)((12.0f - db) / 32.0f * (EQ_SL_H - 12) + 0.5f);
}

static void eq_preset(Player *p, int kind) {
    static const float ROCK[10] = {4.5,3.5,2.5,1.5,0.5,-1,-1.5,-1.5,-2,-2};
    static const float POP[10]  = {-2,-1,0.5,2,2.5,1.5,0.5,0,-1,-1.5};
    static const float JAZZ[10] = {5.5,4,2,0.5,-1,-1,-1,0,1.5,2.5};
    for (int i = 0; i < EQ_BANDS; i++) {
        float v = 0;
        if (kind == EQB_ROCK) v = ROCK[i];
        else if (kind == EQB_POP) v = POP[i];
        else if (kind == EQB_JAZZ) v = JAZZ[i];
        eq_set_band(&p->eq, i, v);
    }
}

static void draw_eq(void) {
    SDL_Surface *s = ui.seq;
    Player *p = ui.pl;
    skin_window_bg(s, EQ_W, EQ_H);
    font_puts(s, 10, 6, "EQUALIZER", col(s, 0, 224, 255));
    { Rect x = close_rect(EQ_W); draw_x(s, &x, ui.pressed == EQB_X); }

    Rect on = {10, 28, 30, 12};
    skin_button(s, &on, ui.pressed == EQB_ON, p->eq.enabled);
    font_puts(s, 16, 31, p->eq.enabled ? "ON" : "OFF",
              p->eq.enabled ? col(s, 0, 255, 0) : col(s, 220, 220, 220));

    static const struct { int id; const char *l; int x, w; } PRE[] = {
        {EQB_FLAT, "FLAT", 48, 34}, {EQB_ROCK, "ROCK", 86, 34},
        {EQB_POP, "POP", 124, 30}, {EQB_JAZZ, "JAZZ", 158, 34},
    };
    for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++) {
        Rect r = {PRE[i].x, 132, PRE[i].w, 12};
        skin_button(s, &r, ui.pressed == PRE[i].id, 0);
        font_puts(s, r.x + (r.w - font_w(PRE[i].l)) / 2, 135, PRE[i].l, col(s, 220, 220, 220));
    }

    /* preamp */
    font_puts(s, EQ_PRE_X + 1, 38, "PRE", col(s, 150, 150, 150));
    skin_vslider(s, EQ_PRE_X, EQ_SL_Y, EQ_SL_H, db_to_knob(p->eq.preamp_db));
    char db[8];
    snprintf(db, sizeof db, "%+d", (int)p->eq.preamp_db);
    font_puts(s, EQ_PRE_X - 2, 124, db, col(s, 0, 255, 0));

    static const char *LBL[10] = {"60","170","310","600","1k","3k","6k","12","14","16"};
    for (int i = 0; i < EQ_BANDS; i++) {
        int x = EQ_BAND_X(i);
        skin_vslider(s, x, EQ_SL_Y, EQ_SL_H, db_to_knob(p->eq.band_db[i]));
        font_puts(s, x + (12 - font_w(LBL[i])) / 2, 124, LBL[i], col(s, 150, 150, 150));
    }
}

static void refresh_title(void) {
    Player *p = ui.pl;
    char buf[512];
    buf[0] = 0;
    pthread_mutex_lock(&p->mtx);
    int cur = p->cur;
    if (cur >= 0 && cur < p->pl->count) {
        const Track *t = &p->pl->items[cur];
        const Tags *g = &t->tags;
        if (g->title[0] || g->artist[0]) {
            /* leading spaces so the first glyph isn't clipped the instant
               the marquee starts scrolling */
            if (g->artist[0] && g->title[0]) {
                if (g->track > 0)
                    snprintf(buf, sizeof buf, "  %s - %d - %s", g->artist, g->track, g->title);
                else
                    snprintf(buf, sizeof buf, "  %s - %s", g->artist, g->title);
            } else if (g->artist[0]) {
                snprintf(buf, sizeof buf, "  %s", g->artist);
            } else if (g->track > 0) {
                snprintf(buf, sizeof buf, "  %d - %s", g->track, g->title);
            } else {
                snprintf(buf, sizeof buf, "  %s", g->title);
            }
        } else {
            const char *base = strrchr(t->path, '/');
            snprintf(buf, sizeof buf, "  %s", base ? base + 1 : t->path);
        }
    }
    pthread_mutex_unlock(&p->mtx);
    if (strcmp(buf, ui.title) != 0) {
        lv_strlcpy(ui.title, buf, sizeof ui.title);
        ui.scroll_off = 0;
    }
}

/* ---------- built-in open file/folder browser ---------- */

#define FILE_W 300
#define FILE_H 240
#define FILE_LIST_RECT Rect list = {6, 24, FILE_W - 12, FILE_H - 24 - 20}
#define FILE_ADD_RECT  Rect addb = {FILE_W - 60, FILE_H - 16, 52, 12}

static int audio_name(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !lv_strcasecmp(e, ".mp3") || !lv_strcasecmp(e, ".ogg") ||
           !lv_strcasecmp(e, ".oga") || !lv_strcasecmp(e, ".opus") ||
           !lv_strcasecmp(e, ".flac") || !lv_strcasecmp(e, ".wav") ||
           !lv_strcasecmp(e, ".aac") || !lv_strcasecmp(e, ".m4a") ||
           !lv_strcasecmp(e, ".wma");
}

static int fcmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void file_list(void) {
    for (int i = 0; i < ui.file_n; i++) free(ui.file_names[i]);
    free(ui.file_names);
    ui.file_names = NULL; ui.file_n = 0;
    DIR *d = opendir(ui.file_dir);
    if (!d) return;
    char **dirs = NULL, **files = NULL;
    int nd = 0, nf = 0, cd = 0, cf = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[LV_MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", ui.file_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        char *dup = strdup(de->d_name);
        if (S_ISDIR(st.st_mode)) {
            if (nd == cd) {
                cd = cd ? cd * 2 : 32;
                char **d2 = realloc(dirs, (size_t)cd * sizeof *dirs);
                if (!d2) { free(dup); continue; }
                dirs = d2;
            }
            dirs[nd++] = dup;
        } else if (audio_name(de->d_name)) {
            if (nf == cf) {
                cf = cf ? cf * 2 : 32;
                char **f2 = realloc(files, (size_t)cf * sizeof *files);
                if (!f2) { free(dup); continue; }
                files = f2;
            }
            files[nf++] = dup;
        } else free(dup);
    }
    closedir(d);
    qsort(dirs, (size_t)nd, sizeof *dirs, fcmp);
    qsort(files, (size_t)nf, sizeof *files, fcmp);
    int root = !strcmp(ui.file_dir, "/");
    ui.file_n = nd + nf + (root ? 0 : 1);
    ui.file_names = malloc((size_t)(ui.file_n + 1) * sizeof *ui.file_names);
    if (!ui.file_names) {
        for (int i = 0; i < nd; i++) free(dirs[i]);
        for (int i = 0; i < nf; i++) free(files[i]);
        free(dirs); free(files);
        ui.file_n = 0;
        return;
    }
    int k = 0;
    if (!root) ui.file_names[k++] = strdup("..");
    for (int i = 0; i < nd; i++) ui.file_names[k++] = dirs[i];
    for (int i = 0; i < nf; i++) ui.file_names[k++] = files[i];
    free(dirs); free(files);
    ui.file_scroll = 0; ui.file_sel = -1;
}

static void raise_all(void) {
    SDL_RaiseWindow(ui.wmain);
    if (ui.pl_visible && ui.wpl) SDL_RaiseWindow(ui.wpl);
    if (ui.eq_visible && ui.weq) SDL_RaiseWindow(ui.weq);
    if (ui.file_visible && ui.wfile) SDL_RaiseWindow(ui.wfile);
    if (ui.opt_visible && ui.wopt) SDL_RaiseWindow(ui.wopt);
    if (search_is_open()) SDL_RaiseWindow(search_window());
}

/* the WM may swallow the button press that focuses a background window,
   so on focus check if the left button is still held over the title strip
   and start the drag from there: one click activates AND starts the move */
static void focus_click_drag(Uint32 wid) {
    SDL_Window *w = NULL;
    int drag = 0;
    if (wid == SDL_GetWindowID(ui.wmain)) { w = ui.wmain; drag = 3; }
    else if (wid == SDL_GetWindowID(ui.wpl)) { w = ui.wpl; drag = 4; }
    else if (ui.weq && wid == SDL_GetWindowID(ui.weq)) { w = ui.weq; drag = 7; }
    else if (ui.wfile && wid == SDL_GetWindowID(ui.wfile)) { w = ui.wfile; drag = 9; }
    else if (ui.wopt && wid == SDL_GetWindowID(ui.wopt)) { w = ui.wopt; drag = 11; }
    if (!w) {
        if (search_is_open() && wid == search_window_id()) search_focus_drag();
        return;
    }
    if (ui.drag) return;
    int gx, gy, wx, wy, ww, wh;
    Uint32 mb = SDL_GetGlobalMouseState(&gx, &gy);
    if (!(mb & SDL_BUTTON_LMASK)) return;
    SDL_GetWindowPosition(w, &wx, &wy);
    SDL_GetWindowSize(w, &ww, &wh);
    if (gx >= wx && gx < wx + ww && gy >= wy && gy - wy < 22) {
        ui.drag = drag;
        drag_begin(w);
    }
}

static void file_close(void) {
    ui.file_visible = 0;
    if (ui.wfile) SDL_HideWindow(ui.wfile);
}

static void file_open(void) {
    if (!ui.wfile) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        ui.wfile = SDL_CreateWindow("levari - open", mx + MAIN_W + 4, my,
                                    FILE_W, FILE_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!ui.wfile) return;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.wfile, icon);
            SDL_FreeSurface(icon);
        }
        ui.sfile = SDL_GetWindowSurface(ui.wfile);
    }
    if (!ui.file_dir[0]) {
        char path[LV_MAX_PATH] = "";
        pthread_mutex_lock(&ui.pl->mtx);
        if (ui.pl->cur >= 0 && ui.pl->cur < ui.pl->pl->count)
            lv_strlcpy(path, ui.pl->pl->items[ui.pl->cur].path, sizeof path);
        pthread_mutex_unlock(&ui.pl->mtx);
        char *slash = strrchr(path, '/');
        if (slash) { *slash = 0; lv_strlcpy(ui.file_dir, path, sizeof ui.file_dir); }
        else if (!getcwd(ui.file_dir, sizeof ui.file_dir))
            lv_strlcpy(ui.file_dir, "/", sizeof ui.file_dir);
    }
    file_list();
    ui.file_visible = 1;
    SDL_ShowWindow(ui.wfile);
    raise_all();
}

static void file_adddir(void) {
    Player *p = ui.pl;
    pthread_mutex_lock(&p->mtx);
    int old = p->pl->count;
    pl_add(p->pl, ui.file_dir);
    int added = p->pl->count - old;
    pthread_mutex_unlock(&p->mtx);
    if (added > 0) pl_post(p, CMD_PLAY, old);
    file_close();
}

static void file_choose(const char *name) {
    if (!strcmp(name, "..")) {
        char *slash = strrchr(ui.file_dir, '/');
        if (slash && slash != ui.file_dir) *slash = 0;
        else if (slash) ui.file_dir[1] = 0; /* stay at "/" */
        file_list();
        return;
    }
    char full[LV_MAX_PATH];
    snprintf(full, sizeof full, "%s/%s", ui.file_dir, name);
    struct stat st;
    if (stat(full, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        lv_strlcpy(ui.file_dir, full, sizeof ui.file_dir);
        file_list();
        return;
    }
    Player *p = ui.pl;
    pthread_mutex_lock(&p->mtx);
    pl_add(p->pl, full);
    int idx = p->pl->count - 1;
    pthread_mutex_unlock(&p->mtx);
    pl_post(p, CMD_PLAY, idx);
    file_close();
}

static void draw_file(void) {
    SDL_Surface *s = ui.sfile;
    if (!s) return;
    skin_window_bg(s, FILE_W, FILE_H);
    font_puts(s, 10, 6, "OPEN FILE OR FOLDER", col(s, 0, 224, 255));
    { Rect x = close_rect(FILE_W); draw_x(s, &x, ui.pressed == FB_X); }
    FILE_LIST_RECT;
    FILE_ADD_RECT;
    SDL_Rect lb = {list.x, list.y, list.w, list.h};
    SDL_FillRect(s, &lb, col(s, 0, 0, 0));
    int lines = list.h / 17;
    if (ui.file_scroll > ui.file_n - lines && ui.file_n >= lines)
        ui.file_scroll = ui.file_n - lines;
    if (ui.file_scroll < 0) ui.file_scroll = 0;
    for (int i = 0; i < lines; i++) {
        int idx = ui.file_scroll + i;
        if (idx >= ui.file_n) break;
        char *name = ui.file_names[idx];
        char full[LV_MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", ui.file_dir, name);
        struct stat st;
        int isdir = !strcmp(name, "..") || (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        char line[LV_MAX_PATH];
        snprintf(line, sizeof line, "%s%s", name, isdir ? "/" : "");
        int fit = font_fit(line, list.w - 6);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = isdir ? col(s, 255, 255, 255) : col(s, 170, 170, 170);
        if (idx == ui.file_sel) c = col(s, 0, 255, 0);
        font_puts16(s, list.x + 2, list.y + 1 + i * 17, line, c);
    }
    if (ui.file_n > lines) {
        int th = list.h * lines / ui.file_n;
        if (th < 8) th = 8;
        int ty = list.y + (list.h - th) * ui.file_scroll / (ui.file_n - lines);
        SDL_Rect r = {list.x + list.w - 7, ty, 5, th};
        SDL_FillRect(s, &r, col(s, 110, 110, 110));
    }
    skin_button(s, &addb, ui.pressed == FB_ADDDIR, 0);
    font_puts(s, addb.x + (addb.w - font_w("ADD DIR")) / 2, addb.y + 3, "ADD DIR", col(s, 220, 220, 220));
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, ui.file_dir, sizeof dir);
    int fit = font_fit(dir, FILE_W - 76);
    if ((int)strlen(dir) > fit) {
        int fit2 = font_fit(dir, FILE_W - 94); /* leave room for ".." */
        dir[fit2] = 0;
        strncat(dir, "..", sizeof dir - (size_t)fit2 - 1);
    }
    font_puts16(s, 6, FILE_H - 17, dir, col(s, 150, 150, 150));
}

static void handle_file_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    FILE_LIST_RECT;
    FILE_ADD_RECT;
    int lines = list.h / 17;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(FILE_W);
        if (in_rect(&xc, x, y)) { ui.pressed = FB_X; return; }
        if (in_rect(&addb, x, y)) { ui.pressed = FB_ADDDIR; return; }
        if (in_rect(&list, x, y)) {
            int idx = ui.file_scroll + (y - list.y) / 17;
            if (idx < ui.file_n) { ui.file_sel = idx; file_choose(ui.file_names[idx]); }
            return;
        }
        if (y < 22) { ui.drag = 9; drag_begin(ui.wfile); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = close_rect(FILE_W);
        if (ui.pressed == FB_X && in_rect(&xc, x, y)) file_close();
        else if (ui.pressed == FB_ADDDIR && in_rect(&addb, x, y)) file_adddir();
        ui.pressed = B_NONE;
        if (ui.drag == 9) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 9) {
        drag_move_snap(ui.wfile);
    } else if (e->type == SDL_MOUSEWHEEL) {
        ui.file_scroll -= e->wheel.y;
        if (ui.file_scroll < 0) ui.file_scroll = 0;
        if (ui.file_scroll > ui.file_n - lines)
            ui.file_scroll = ui.file_n > lines ? ui.file_n - lines : 0;
    }
}

/* ---------- options window ---------- */

static const struct { const char *lbl; int y; } OPT_ROWS[OPTF_N] = {
    { "LIBRARY DIRS",  30 }, { "LIB INDEX",     56 }, { "LIBAV DIR",   82 },
    { "AUDIO BACKEND", 108 }, { "AUDIO DEVICE", 134 }, { "DAEMON SOCKET", 160 },
};

static Rect opt_field_rect(int i) {
    Rect r = { OPT_FLD_X, OPT_ROWS[i].y, OPT_FLD_W, OPT_FLD_H };
    return r;
}
static Rect opt_save_rect(void)  { Rect r = { OPT_W - 142, 224, 60, 14 }; return r; }
static Rect opt_close_rect(void) { Rect r = { OPT_W - 72,  224, 60, 14 }; return r; }
static Rect opt_clear_rect(void) { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBIDX].y, OPT_SCAN_W, OPT_FLD_H }; return r; }
static Rect opt_tog_rect(int i)  { Rect r = { OPT_FLD_X + i * 52, 190, 48, 14 }; return r; }
static Rect opt_scan_rect(void)  { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBDIRS].y, OPT_SCAN_W, OPT_FLD_H }; return r; }
static Rect opt_chk_rect(void)   { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP + OPT_SCAN_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBDIRS].y, OPT_CHK_W, OPT_FLD_H }; return r; }

static void opt_open(void) {
    if (!ui.wopt) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        int ox = mx + MAIN_W;
        SDL_Rect d;
        if (SDL_GetDisplayBounds(0, &d) == 0 && ox + OPT_W > d.x + d.w) ox = mx - OPT_W;
        ui.wopt = SDL_CreateWindow("levari - options", ox, my,
                                   OPT_W, OPT_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!ui.wopt) return;
        ui.snap[3].w = ui.wopt; ui.snap[3].dx = ox - mx; ui.snap[3].dy = 0;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.wopt, icon);
            SDL_FreeSurface(icon);
        }
        ui.sopt = SDL_GetWindowSurface(ui.wopt);
    }
    ui.opt_field[OPTF_LIBDIRS][0] = 0;
    for (int i = 0; i < cfg.lib_dir_n; i++) {
        if (i) strncat(ui.opt_field[OPTF_LIBDIRS], ":", LV_MAX_PATH - strlen(ui.opt_field[OPTF_LIBDIRS]) - 1);
        strncat(ui.opt_field[OPTF_LIBDIRS], cfg.lib_dirs[i],
                LV_MAX_PATH - strlen(ui.opt_field[OPTF_LIBDIRS]) - 1);
    }
    lv_strlcpy(ui.opt_field[OPTF_LIBIDX],
               cfg.lib_index[0] ? cfg.lib_index : "~/.levari/index", LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_LIBAV],   cfg.libav_dir,     LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_BACKEND], cfg.audio_backend, LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_DEVICE],  cfg.audio_device,  LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_SOCK],    cfg.socket_path,   LV_MAX_PATH);
    ui.opt_pl  = cfg.ui_pl  > 0;
    ui.opt_eq  = cfg.ui_eq  > 0;
    ui.opt_vis = cfg.ui_vis > 0;
    ui.opt_srch = cfg.ui_search > 0;
    ui.opt_hover = -1;
    ui.opt_focus = -1;
    ui.opt_selall = 0;
    ui.opt_status[0] = 0;
    ui.opt_tags = 1;
    ui.opt_visible = 1;
    SDL_ShowWindow(ui.wopt);
    raise_all();
}

static void opt_close(void) {
    ui.opt_visible = 0;
    ui.opt_focus = -1;
    if (ui.wopt) SDL_HideWindow(ui.wopt);
    SDL_StopTextInput();
}

static void opt_save(void) {
    cfg.lib_dir_n = 0;
    char copy[LV_MAX_PATH];
    lv_strlcpy(copy, ui.opt_field[OPTF_LIBDIRS], sizeof copy);
    char *tok = strtok(copy, ":");
    while (tok) {
        while (*tok == ' ') tok++;
        if (*tok && cfg.lib_dir_n < CFG_MAX_LIBDIRS)
            lv_strlcpy(cfg.lib_dirs[cfg.lib_dir_n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ":");
    }
    lv_strlcpy(cfg.lib_index,     ui.opt_field[OPTF_LIBIDX],  sizeof cfg.lib_index);
    lv_strlcpy(cfg.libav_dir,     ui.opt_field[OPTF_LIBAV],   sizeof cfg.libav_dir);
    lv_strlcpy(cfg.audio_backend, ui.opt_field[OPTF_BACKEND], sizeof cfg.audio_backend);
    lv_strlcpy(cfg.audio_device,  ui.opt_field[OPTF_DEVICE],  sizeof cfg.audio_device);
    lv_strlcpy(cfg.socket_path,   ui.opt_field[OPTF_SOCK],    sizeof cfg.socket_path);
    cfg.ui_pl = ui.opt_pl;
    cfg.ui_eq = ui.opt_eq;
    cfg.ui_vis = ui.opt_vis;
    cfg.ui_search = ui.opt_srch;
    cfg_save();
}

typedef struct { char (*dirs)[LV_MAX_PATH]; int n; int tags; } ScanArg;

static int scan_thread(void *p) {
    ScanArg *a = p;
    int added = 0, seen = 0;
    lib_scan_add(a->dirs, a->n, a->tags, &added, &seen);
    ui.opt_scan_added = added;
    ui.opt_scan_seen = seen;
    SDL_AtomicSet(&ui.opt_scan_done, 1);
    free(a->dirs);
    free(a);
    return 0;
}

static void opt_start_scan(void) {
    char dirs[CFG_MAX_LIBDIRS][LV_MAX_PATH];
    int n = 0;
    char copy[LV_MAX_PATH];
    lv_strlcpy(copy, ui.opt_field[OPTF_LIBDIRS], sizeof copy);
    char *tok = strtok(copy, ":");
    while (tok && n < CFG_MAX_LIBDIRS) {
        while (*tok == ' ') tok++;
        if (*tok) lv_strlcpy(dirs[n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ":");
    }
    if (!n) { lv_strlcpy(ui.opt_status, "no library dirs set", sizeof ui.opt_status); return; }
    ScanArg *a = malloc(sizeof *a);
    if (!a) { lv_strlcpy(ui.opt_status, "out of memory", sizeof ui.opt_status); return; }
    a->dirs = malloc((size_t)n * sizeof *a->dirs);
    if (!a->dirs) {
        free(a);
        lv_strlcpy(ui.opt_status, "out of memory", sizeof ui.opt_status);
        return;
    }
    a->n = n;
    a->tags = ui.opt_tags;
    for (int i = 0; i < n; i++) lv_strlcpy(a->dirs[i], dirs[i], LV_MAX_PATH);
    ui.opt_scanning = 1;
    ui.opt_status[0] = 0;
    SDL_AtomicSet(&ui.opt_scan_done, 0);
    SDL_Thread *th = SDL_CreateThread(scan_thread, "lvscan", a);
    if (!th) {
        free(a->dirs);
        free(a);
        ui.opt_scanning = 0;
        lv_strlcpy(ui.opt_status, "scan failed to start", sizeof ui.opt_status);
        return;
    }
    SDL_DetachThread(th);
}

static void opt_clear_index(void) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    remove(idx);
    search_lib_invalidate();
    lv_strlcpy(ui.opt_status, "index cleared", sizeof ui.opt_status);
}

static void draw_opt(void) {
    SDL_Surface *s = ui.sopt;
    if (!s) return;
    skin_window_bg(s, OPT_W, OPT_H);
    font_puts(s, 10, 6, "OPTIONS", col(s, 0, 224, 255));
    { Rect x = close_rect(OPT_W);
      font_puts(s, x.x - 6 - font_w("v" LV_VERSION), 6, "v" LV_VERSION, col(s, 150, 150, 150));
      draw_x(s, &x, ui.pressed == OPTB_X); }

    for (int i = 0; i < OPTF_N; i++) {
        font_puts(s, 10, OPT_ROWS[i].y + 5, OPT_ROWS[i].lbl, col(s, 150, 150, 150));
        Rect f = opt_field_rect(i);
        SDL_Rect fb = { f.x, f.y, f.w, f.h };
        SDL_FillRect(s, &fb, col(s, 0, 0, 0));
        SDL_Rect t = { f.x, f.y, f.w, 1 }, b = { f.x, f.y + f.h - 1, f.w, 1 };
        SDL_Rect l = { f.x, f.y, 1, f.h }, r = { f.x + f.w - 1, f.y, 1, f.h };
        Uint32 bc = ui.opt_focus == i ? col(s, 0, 224, 255) : col(s, 90, 90, 90);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        char val[LV_MAX_PATH];
        lv_strlcpy(val, ui.opt_field[i], sizeof val);
        int fit = font_fit(val, f.w - 8);
        if ((int)strlen(val) > fit) val[fit] = 0;
        if (ui.opt_focus == i && ui.opt_selall && val[0]) {
            SDL_Rect hl = { f.x + 3, f.y + 1, (unsigned)font_w16(val), (unsigned)(f.h - 2) };
            if (hl.x + (int)hl.w > f.x + f.w - 3)
                hl.w = f.x + f.w - 3 - hl.x;
            if (hl.w > 0) SDL_FillRect(s, &hl, col(s, 40, 60, 120));
        }
        font_puts16(s, f.x + 3, f.y + 1, val, col(s, 220, 220, 220));
        if (ui.opt_focus == i) {
            int cx = f.x + 3 + font_w16(val) + 1;
            if (cx < f.x + f.w - 2) {
                SDL_Rect c = { cx, f.y + 3, 1, f.h - 6 };
                SDL_FillRect(s, &c, col(s, 255, 255, 255));
            }
        }
    }

    Rect sc = opt_scan_rect();
    skin_button(s, &sc, ui.pressed == OPTB_SCAN && !ui.opt_scanning, 0);
    const char *sl = ui.opt_scanning ? "BUSY" : "SCAN";
    font_puts(s, sc.x + (sc.w - font_w(sl)) / 2, sc.y + 3, sl,
              ui.opt_scanning ? col(s, 150, 150, 150) : col(s, 220, 220, 220));

    /* TAGS checkbox: when on, SCAN reads tags+duration; off = names only */
    Rect ck = opt_chk_rect();
    SDL_Rect box = { ck.x, ck.y + (ck.h - 12) / 2, 12, 12 };
    SDL_FillRect(s, &box, col(s, 0, 0, 0));
    Uint32 bcol = ui.pressed == OPTB_CHK ? col(s, 0, 224, 255) : col(s, 140, 140, 140);
    SDL_Rect bt = { box.x, box.y, box.w, 1 }, bb = { box.x, box.y + box.h - 1, box.w, 1 };
    SDL_Rect bl = { box.x, box.y, 1, box.h }, br = { box.x + box.w - 1, box.y, 1, box.h };
    SDL_FillRect(s, &bt, bcol); SDL_FillRect(s, &bb, bcol);
    SDL_FillRect(s, &bl, bcol); SDL_FillRect(s, &br, bcol);
    if (ui.opt_tags) {
        Uint32 xc = col(s, 0, 255, 0);
        for (int i = 0; i < 10; i++) {
            SDL_Rect p1 = { box.x + 1 + i, box.y + 1 + i, 1, 1 };
            SDL_Rect p2 = { box.x + 10 - i, box.y + 1 + i, 1, 1 };
            SDL_FillRect(s, &p1, xc); SDL_FillRect(s, &p2, xc);
        }
    }
    font_puts(s, box.x + 16, ck.y + 5, "TAGS",
              ui.opt_tags ? col(s, 0, 255, 0) : col(s, 220, 220, 220));

    static const struct { int id; const char *l; } TOG[4] = {
        { OPTB_TPL, "UI PL" }, { OPTB_TEQ, "UI EQ" },
        { OPTB_TVIS, "UI VIS" }, { OPTB_TSRCH, "UI SRCH" },
    };
    font_puts(s, 10, 193, "STARTUP", col(s, 150, 150, 150));
    for (int i = 0; i < 4; i++) {
        Rect r = opt_tog_rect(i);
        int on = i == 0 ? ui.opt_pl : i == 1 ? ui.opt_eq : i == 2 ? ui.opt_vis : ui.opt_srch;
        skin_button(s, &r, ui.pressed == TOG[i].id, on);
        font_puts(s, r.x + (r.w - font_w(TOG[i].l)) / 2, r.y + 3, TOG[i].l,
                  on ? col(s, 0, 255, 0) : col(s, 220, 220, 220));
    }

    Rect sv = opt_save_rect(), cl = opt_close_rect();
    skin_button(s, &sv, ui.pressed == OPTB_SAVE, 0);
    font_puts(s, sv.x + (sv.w - font_w("SAVE")) / 2, sv.y + 3, "SAVE", col(s, 220, 220, 220));
    skin_button(s, &cl, ui.pressed == OPTB_CLOSE, 0);
    font_puts(s, cl.x + (cl.w - font_w("CLOSE")) / 2, cl.y + 3, "CLOSE", col(s, 220, 220, 220));

    Rect cr = opt_clear_rect();
    skin_button(s, &cr, ui.pressed == OPTB_CLEAR && !ui.opt_scanning, 0);
    font_puts(s, cr.x + (cr.w - font_w("CLEAR")) / 2, cr.y + 3, "CLEAR", col(s, 220, 220, 220));

    if (ui.opt_status[0])
        font_puts(s, 10, 240, ui.opt_status, col(s, 0, 224, 255));

    font_puts(s, 10, OPT_H - 12, "libav dir + socket apply on restart", col(s, 120, 120, 120));

    /* hover tooltip: how library dirs are scanned + multi-dir syntax */
    if (ui.opt_hover == OPTF_LIBDIRS) {
        Rect f = opt_field_rect(OPTF_LIBDIRS);
        SDL_Rect tb = { 10, f.y + f.h + 4, 210, 30 };
        SDL_FillRect(s, &tb, col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "dirs are scanned recursively", col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "separate multiple dirs with ':'", col(s, 220, 220, 220));
    }
}

static void handle_opt_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(OPT_W);
        if (in_rect(&xc, x, y)) { ui.pressed = OPTB_X; return; }
        Rect sv = opt_save_rect(), cl = opt_close_rect();
        if (in_rect(&sv, x, y)) { ui.pressed = OPTB_SAVE; return; }
        if (in_rect(&cl, x, y)) { ui.pressed = OPTB_CLOSE; return; }
        Rect sc = opt_scan_rect();
        if (in_rect(&sc, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_SCAN; return; }
        Rect cr = opt_clear_rect();
        if (in_rect(&cr, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_CLEAR; return; }
        Rect ck = opt_chk_rect();
        if (in_rect(&ck, x, y)) { ui.pressed = OPTB_CHK; return; }
        for (int i = 0; i < 4; i++) {
            Rect tg = opt_tog_rect(i);
            if (in_rect(&tg, x, y)) { ui.pressed = OPTB_TPL + i; return; }
        }
        for (int i = 0; i < OPTF_N; i++) {
            Rect f = opt_field_rect(i);
            if (in_rect(&f, x, y)) {
                ui.opt_focus = i;
                ui.opt_selall = 0;
                SDL_SetWindowInputFocus(ui.wopt);
                SDL_StartTextInput();
                return;
            }
        }
        if (y < 22) { ui.drag = 11; drag_begin(ui.wopt); ui.opt_focus = -1; }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.pressed != B_NONE) {
            Rect xc = close_rect(OPT_W);
            Rect sv = opt_save_rect(), cl = opt_close_rect();
            if (ui.pressed == OPTB_X && in_rect(&xc, x, y)) opt_close();
            else if (ui.pressed == OPTB_SAVE && in_rect(&sv, x, y)) opt_save();
            else if (ui.pressed == OPTB_CLOSE && in_rect(&cl, x, y)) opt_close();
            else if (ui.pressed >= OPTB_TPL && ui.pressed <= OPTB_TSRCH) {
                int i = ui.pressed - OPTB_TPL;
                Rect tg = opt_tog_rect(i);
                if (in_rect(&tg, x, y)) {
                    if (i == 0) ui.opt_pl = !ui.opt_pl;
                    else if (i == 1) ui.opt_eq = !ui.opt_eq;
                    else if (i == 2) ui.opt_vis = !ui.opt_vis;
                    else ui.opt_srch = !ui.opt_srch;
                }
            }
            else if (ui.pressed == OPTB_SCAN) {
                Rect sc = opt_scan_rect();
                if (in_rect(&sc, x, y)) opt_start_scan();
            } else if (ui.pressed == OPTB_CLEAR) {
                Rect cr = opt_clear_rect();
                if (in_rect(&cr, x, y)) opt_clear_index();
            } else if (ui.pressed == OPTB_CHK) {
                Rect ck = opt_chk_rect();
                if (in_rect(&ck, x, y)) ui.opt_tags = !ui.opt_tags;
            }
            ui.pressed = B_NONE;
        }
        if (ui.drag == 11) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 11) {
        drag_move_snap(ui.wopt);
    } else if (e->type == SDL_MOUSEMOTION) {
        Rect f = opt_field_rect(OPTF_LIBDIRS);
        ui.opt_hover = (x >= 8 && x < f.x + f.w && y >= f.y - 2 && y <= f.y + f.h + 2)
                       ? OPTF_LIBDIRS : -1;
    }
}

static void opt_key(SDL_Event *e) {
    if (e->key.keysym.sym == SDLK_ESCAPE) { opt_close(); return; }
    if (ui.opt_focus < 0) return;
    char *f = ui.opt_field[ui.opt_focus];
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_a:
        if (ctrl) ui.opt_selall = f[0] ? 1 : 0;
        break;
    case SDLK_c:
        if (ctrl) ui_clip_copy(f);
        break;
    case SDLK_x:
        if (ctrl) {
            if (ui.opt_selall && f[0]) { ui_clip_copy(f); f[0] = 0; ui.opt_selall = 0; }
        }
        break;
    case SDLK_v:
        if (ctrl) {
            if (ui.opt_selall) { f[0] = 0; ui.opt_selall = 0; }
            ui_clip_paste(f, sizeof ui.opt_field[0]);
        }
        break;
    case SDLK_BACKSPACE: {
        if (ui.opt_selall) { f[0] = 0; ui.opt_selall = 0; return; }
        size_t n = strlen(f);
        if (!n) return;
        /* delete the last whole utf8 codepoint */
        size_t i = n - 1;
        while (i > 0 && ((unsigned char)f[i] & 0xC0) == 0x80) i--;
        f[i] = 0;
        break;
    }
    case SDLK_DELETE:
        if (ui.opt_selall) { f[0] = 0; ui.opt_selall = 0; }
        break;
    }
}

static void opt_text(const char *text) {
    if (ui.opt_focus < 0) return;
    char *f = ui.opt_field[ui.opt_focus];
    if (ui.opt_selall) { f[0] = 0; ui.opt_selall = 0; }
    size_t n = strlen(f);
    if (n + strlen(text) < LV_MAX_PATH - 1) strcat(f, text);
}

static void pl_add_lib_cb(const LibEntry *e, void *ud) {
    Playlist *pl = ud;
    pl_add(pl, e->path);
    if (!pl->count) return;
    Track *t = &pl->items[pl->count - 1];
    lv_strlcpy(t->tags.artist, e->artist, sizeof t->tags.artist);
    lv_strlcpy(t->tags.title, e->title, sizeof t->tags.title);
    lv_strlcpy(t->tags.album, e->album, sizeof t->tags.album);
    t->duration = e->duration;
    t->tags.track = e->track;
}

static void action(int id) {
    Player *p = ui.pl;
    switch (id) {
    case B_PLAY:
        if (p->state == ST_STOP) pl_post(p, CMD_PLAY, p->cur >= 0 ? p->cur : 0);
        else if (p->state == ST_PAUSE) pl_post(p, CMD_TOGGLE_PAUSE, 0);
        break;
    case B_PAUSE: pl_post(p, CMD_TOGGLE_PAUSE, 0); break;
    case B_STOP: pl_post(p, CMD_STOP, 0); break;
    case B_PREV: pl_post(p, CMD_PREV, 0); break;
    case B_NEXT: pl_post(p, CMD_NEXT, 0); break;
    case B_SHUF: p->shuffle = !p->shuffle; break;
    case B_REPEAT: p->repeat = p->repeat == 1 ? 0 : 1; break;
    case B_RPT1: p->repeat = p->repeat == 2 ? 0 : 2; break;
    case B_ABA:
        if (p->state != ST_STOP) p->ab_a = p->pos_frames;
        break;
    case B_ABB:
        if (p->ab_b >= 0) { p->ab_a = p->ab_b = -1; } /* second click clears */
        else if (p->state != ST_STOP && p->ab_a >= 0 && p->pos_frames > p->ab_a)
            p->ab_b = p->pos_frames;
        break;
    case B_EQ:
        ui.eq_visible = !ui.eq_visible;
        if (ui.eq_visible) SDL_ShowWindow(ui.weq);
        else SDL_HideWindow(ui.weq);
        break;
    case B_VIS: ui.vis_on = !ui.vis_on; break;
    case B_LIST:
        ui.pl_visible = !ui.pl_visible;
        if (ui.pl_visible) {
            if (ui.pl_docked) {
                int dx, dy;
                pl_dock_pos(&dx, &dy);
                SDL_SetWindowPosition(ui.wpl, dx, dy);
            }
            SDL_ShowWindow(ui.wpl);
        } else SDL_HideWindow(ui.wpl);
        break;
    case PB_SHUF: p->shuffle = !p->shuffle; break;
    case PB_REPEAT: p->repeat = p->repeat == 1 ? 0 : 1; break;
    case PB_REM:
        if (ui.pl_selall) {
            if (p->cur >= 0) pl_post(p, CMD_STOP, 0);
            pthread_mutex_lock(&p->mtx);
            pl_clear(p->pl);
            p->cur = -1;
            pthread_mutex_unlock(&p->mtx);
            ui.pl_sel = -1;
            ui.pl_scroll = 0;
            ui.pl_selall = 0;
        } else if (ui.pl_sel >= 0 && ui.pl_sel < p->pl->count) {
            if (ui.pl_sel == p->cur) pl_post(p, CMD_STOP, 0);
            pthread_mutex_lock(&p->mtx);
            pl_remove(p->pl, ui.pl_sel);
            if (p->cur > ui.pl_sel) p->cur--;
            pthread_mutex_unlock(&p->mtx);
            ui.pl_sel = -1;
        }
        break;
    case PB_CLOSE:
        ui.pl_visible = 0;
        SDL_HideWindow(ui.wpl);
        break;
    case PB_LIB:
        pthread_mutex_lock(&p->mtx);
        lib_foreach_entry(pl_add_lib_cb, p->pl);
        pthread_mutex_unlock(&p->mtx);
        break;
    case B_OPEN: file_open(); break;
    case B_SEARCH:
        if (search_is_open()) search_close();
        else search_open(ui.wmain);
        break;
    }
}

static void handle_main_mouse(SDL_Event *e) {
    Player *p = ui.pl;
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT) {
        ui.menu = y < 22 ? 1 : 0;
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT && ui.menu) {
        Rect mi = { 2, 22, 110, 20 };
        ui.menu = 0;
        if (in_rect(&mi, x, y)) opt_open();
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(MAIN_W);
        if (in_rect(&xc, x, y)) { ui.pressed = B_XMAIN; return; }
        for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++)
            if (in_rect(MAIN_BTNS[i].r, x, y)) { ui.pressed = MAIN_BTNS[i].id; return; }
        if (in_rect(&R_SEEK, x, y)) {
            ui.drag = 1;
            pl_post_seek(p, (double)(x - R_SEEK.x - SEEK_KNOB_W / 2) / (double)(R_SEEK.w - SEEK_KNOB_W));
            return;
        }
        if (in_rect(&R_VOL, x, y)) {
            ui.drag = 2;
            p->vol = (x - R_VOL.x - 2) * 128 / (R_VOL.w - 16);
            if (p->vol < 0) p->vol = 0;
            if (p->vol > 128) p->vol = 128;
            return;
        }
        if (in_rect(&R_PAN, x, y)) {
            ui.drag = 10;
            p->pan = (x - R_PAN.x - 2) * 64 / (R_PAN.w - 16) - 32;
            if (p->pan < -32) p->pan = -32;
            if (p->pan > 32) p->pan = 32;
            return;
        }
        if (y < 22) { ui.drag = 3; drag_begin(ui.wmain); }
    } else if (e->type == SDL_MOUSEBUTTONUP && ui.drag == 0 && ui.pressed != B_NONE) {
        Rect xc = close_rect(MAIN_W);
        if (ui.pressed == B_XMAIN && in_rect(&xc, x, y)) {
            SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); /* quits every window */
        }
        for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++)
            if (MAIN_BTNS[i].id == ui.pressed && in_rect(MAIN_BTNS[i].r, x, y))
                action(ui.pressed);
        ui.pressed = B_NONE;
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.drag == 1) pl_post_seek(p, (double)(x - R_SEEK.x - SEEK_KNOB_W / 2) / (double)(R_SEEK.w - SEEK_KNOB_W));
        else if (ui.drag == 2) {
            p->vol = (x - R_VOL.x - 2) * 128 / (R_VOL.w - 16);
            if (p->vol < 0) p->vol = 0;
            if (p->vol > 128) p->vol = 128;
        } else if (ui.drag == 10) {
            p->pan = (x - R_PAN.x - 2) * 64 / (R_PAN.w - 16) - 32;
            if (p->pan < -32) p->pan = -32;
            if (p->pan > 32) p->pan = 32;
        } else if (ui.drag == 3) {
            drag_move(ui.wmain);
            int wx, wy;
            SDL_GetWindowPosition(ui.wmain, &wx, &wy);
            if (ui.pl_docked && ui.pl_visible)
                SDL_SetWindowPosition(ui.wpl, wx, wy + MAIN_H);
            for (int i = 0; i < 4; i++)
                if (ui.snap[i].w)
                    SDL_SetWindowPosition(ui.snap[i].w, wx + ui.snap[i].dx, wy + ui.snap[i].dy);
        }
    }
    if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.drag == 1 || ui.drag == 2 || ui.drag == 3 || ui.drag == 10) ui.drag = 0;
    }
}

static void handle_pl_mouse(SDL_Event *e) {
    Player *p = ui.pl;
    Playlist *pl = p->pl;
    int x = e->motion.x, y = e->motion.y;
    Rect ptext, pscroll;
    pl_layout(&ptext, &pscroll);
    int lines = ptext.h / 17;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(ui.pl_w);
        if (in_rect(&xc, x, y)) { ui.pressed = PB_X; return; }
        if (x >= ui.pl_w - 12 && y >= ui.pl_h - 12) {
            ui.drag = 8;
            SDL_GetGlobalMouseState(&ui.drag_gx, &ui.drag_gy);
            ui.drag_wx = ui.pl_w; ui.drag_wy = ui.pl_h;
            return;
        }
        for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++)
            if (in_rect(PL_BTNS[i].r, x, y)) { ui.pressed = PL_BTNS[i].id; return; }
        if (in_rect(&ptext, x, y)) {
            int idx = ui.pl_scroll + (y - ptext.y) / 17;
            if (idx < pl->count) {
                ui.pl_sel = idx;
                ui.pl_selall = 0;
                if (e->button.clicks == 2) pl_post(p, CMD_PLAY, idx);
            }
            return;
        }
        if (in_rect(&pscroll, x, y)) { ui.drag = 5; return; }
        if (y < 22) { ui.drag = 4; drag_begin(ui.wpl); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.pressed != B_NONE) {
            Rect xc = close_rect(ui.pl_w);
            if (ui.pressed == PB_X && in_rect(&xc, x, y)) action(PB_CLOSE);
            for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++)
                if (PL_BTNS[i].id == ui.pressed && in_rect(PL_BTNS[i].r, x, y))
                    action(ui.pressed);
            ui.pressed = B_NONE;
        }
        if (ui.drag == 4) {
            /* dropped while snapped? lock flush under main */
            if (ui.pl_docked) {
                int dx, dy;
                pl_dock_pos(&dx, &dy);
                SDL_SetWindowPosition(ui.wpl, dx, dy);
            }
        }
        if (ui.drag == 4 || ui.drag == 5 || ui.drag == 8) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.drag == 4) {
            drag_move_pl();
        } else if (ui.drag == 5 && pl->count > lines) {
            ui.pl_scroll = (y - pscroll.y) * (pl->count - lines) / pscroll.h;
            if (ui.pl_scroll < 0) ui.pl_scroll = 0;
        } else if (ui.drag == 8) {
            int gx, gy;
            SDL_GetGlobalMouseState(&gx, &gy);
            int w = ui.drag_wx + gx - ui.drag_gx, h = ui.drag_wy + gy - ui.drag_gy;
            if (w < PL_MIN_W) w = PL_MIN_W;
            /* horizontal size snaps to the main window's width within DOCK_SNAP px */
            if (abs(w - MAIN_W) <= DOCK_SNAP && w != MAIN_W) w = MAIN_W;
            if (h < PL_MIN_H) h = PL_MIN_H;
            if (w != ui.pl_w || h != ui.pl_h) {
                ui.pl_w = w; ui.pl_h = h;
                SDL_SetWindowSize(ui.wpl, w, h);
                ui.spl = SDL_GetWindowSurface(ui.wpl);
            }
        }
    } else if (e->type == SDL_MOUSEWHEEL) {
        ui.pl_scroll -= e->wheel.y;
        if (ui.pl_scroll < 0) ui.pl_scroll = 0;
        if (ui.pl_scroll > pl->count - lines)
            ui.pl_scroll = pl->count > lines ? pl->count - lines : 0;
    }
}

/* keys for the playlist window; returns 1 when handled so the playback
   shortcuts in handle_key don't also fire. pgup/pgdn scroll by one page
   minus one line, so the bottom visible line becomes the new top */
static int pl_key(SDL_Event *e) {
    Player *p = ui.pl;
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_a:
        if (!ctrl || p->pl->count == 0) return 0;
        ui.pl_selall = 1;
        return 1;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (!ui.pl_selall && ui.pl_sel < 0) return 0;
        action(PB_REM);
        return 1;
    case SDLK_PAGEUP:
    case SDLK_PAGEDOWN: {
        Rect ptext, pscroll;
        pl_layout(&ptext, &pscroll);
        int lines = ptext.h / 17;
        int step = lines > 1 ? lines - 1 : 1;
        if (e->key.keysym.sym == SDLK_PAGEDOWN) ui.pl_scroll += step;
        else ui.pl_scroll -= step;
        int maxs = p->pl->count - lines;
        if (maxs < 0) maxs = 0;
        if (ui.pl_scroll < 0) ui.pl_scroll = 0;
        if (ui.pl_scroll > maxs) ui.pl_scroll = maxs;
        return 1;
    }
    default:
        return 0;
    }
}

static void handle_eq_mouse(SDL_Event *e) {
    Player *p = ui.pl;
    int x = e->motion.x, y = e->motion.y;
    static const struct { int id; int x, w; } PRE[] = {
        {EQB_FLAT, 48, 34}, {EQB_ROCK, 86, 34}, {EQB_POP, 124, 30}, {EQB_JAZZ, 158, 34},
    };
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(EQ_W);
        if (in_rect(&xc, x, y)) { ui.pressed = EQB_X; return; }
        Rect on = {10, 28, 30, 12};
        if (in_rect(&on, x, y)) { ui.pressed = EQB_ON; return; }
        for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++) {
            Rect r = {PRE[i].x, 132, PRE[i].w, 12};
            if (in_rect(&r, x, y)) { ui.pressed = PRE[i].id; return; }
        }
        if (y >= EQ_SL_Y - 6 && y <= EQ_SL_Y + EQ_SL_H + 6) {
            if (x >= EQ_PRE_X - 2 && x < EQ_PRE_X + 14) { ui.drag = 6; ui.eq_band = 10; }
            else for (int i = 0; i < EQ_BANDS; i++)
                if (x >= EQ_BAND_X(i) - 2 && x < EQ_BAND_X(i) + 14) { ui.drag = 6; ui.eq_band = i; }
            if (ui.drag == 6) {
                int ky = y - 6;
                if (ky < EQ_SL_Y) ky = EQ_SL_Y;
                if (ky > EQ_SL_Y + EQ_SL_H - 12) ky = EQ_SL_Y + EQ_SL_H - 12;
                float db = knob_to_db(ky);
                if (ui.eq_band == 10) eq_set_preamp(&p->eq, db);
                else eq_set_band(&p->eq, ui.eq_band, db);
            }
            return;
        }
        if (y < 22) { ui.drag = 7; drag_begin(ui.weq); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = close_rect(EQ_W);
        if (ui.pressed == EQB_X && in_rect(&xc, x, y)) {
            ui.eq_visible = 0;
            SDL_HideWindow(ui.weq);
        } else if (ui.pressed == EQB_ON) p->eq.enabled = !p->eq.enabled;
        else for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++)
            if (ui.pressed == PRE[i].id) eq_preset(p, PRE[i].id);
        ui.pressed = B_NONE;
        if (ui.drag == 6 || ui.drag == 7) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 7) {
        drag_move_snap(ui.weq);
    }
}

static void handle_key(SDL_Event *e) {
    Player *p = ui.pl;
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_x: if (p->state == ST_STOP) pl_post(p, CMD_PLAY, p->cur >= 0 ? p->cur : 0); else if (p->state == ST_PAUSE) pl_post(p, CMD_TOGGLE_PAUSE, 0); break;
    case SDLK_c: pl_post(p, CMD_TOGGLE_PAUSE, 0); break;
    case SDLK_SPACE: pl_post(p, CMD_TOGGLE_PAUSE, 0); break;
    case SDLK_v: pl_post(p, CMD_STOP, 0); break;
    case SDLK_b: pl_post(p, CMD_PREV, 0); break;
    case SDLK_n: pl_post(p, CMD_NEXT, 0); break;
    case SDLK_s: p->shuffle = !p->shuffle; break;
    case SDLK_r: p->repeat = !p->repeat; break;
    case SDLK_RIGHT: pl_post_seek(p, (double)(p->pos_frames - p->track_start + p->rate * 5) /
                                      (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_LEFT: pl_post_seek(p, (double)(p->pos_frames - p->track_start - p->rate * 5) /
                                     (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_UP: p->vol += 8; if (p->vol > 128) p->vol = 128; break;
    case SDLK_DOWN: p->vol -= 8; if (p->vol < 0) p->vol = 0; break;
    case SDLK_l: if (ctrl) { pl_scan_tags(p->pl, &p->mtx); } break;
    case SDLK_e:
        ui.eq_visible = !ui.eq_visible;
        if (ui.eq_visible) SDL_ShowWindow(ui.weq);
        else SDL_HideWindow(ui.weq);
        break;
    case SDLK_o: file_open(); break;
    case SDLK_ESCAPE:
        if (ui.menu) { ui.menu = 0; break; }
        { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); } break;
    }
}

static Uint32 ev_win(const SDL_Event *e) {
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: return e->button.windowID;
    case SDL_MOUSEMOTION: return e->motion.windowID;
    case SDL_MOUSEWHEEL: return e->wheel.windowID;
    }
    return 0;
}

int ui_run(Player *p, int show_pl) {
    memset(&ui, 0, sizeof ui);
    ui.pl = p;
    ui.pressed = B_NONE;
    ui.pl_sel = -1;
    ui.pl_cur = -1;
    ui.pl_visible = show_pl;
    ui.pl_w = PL_W; ui.pl_h = PL_H;
    ui.pl_docked = 1;
    ui.eq_visible = ui_show_eq;
    ui.vis_on = ui_show_vis;
    search_init(p);

    SDL_SetHint(SDL_HINT_APP_NAME, "levari");
    /* deliver the button press that focuses a window, so one click on a
       background window both activates it and starts the title-bar drag */
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "levari: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_Rect disp;
    if (SDL_GetDisplayBounds(0, &disp) != 0) {
        disp.x = disp.y = 0;
        disp.w = 1280; disp.h = 800;
    }
    int mx = disp.x + (disp.w - MAIN_W) / 2;
    int my = disp.y + (disp.h - MAIN_H) / 2;
    int ex = mx, ey = my - EQ_H;
    if (ey < disp.y) {
        ex = mx + MAIN_W + 4; ey = my;
        if (ex + EQ_W > disp.x + disp.w) ex = disp.x + disp.w - EQ_W;
        if (ex < disp.x) ex = disp.x;
    }
    ui.wmain = SDL_CreateWindow("levari", mx, my,
                                MAIN_W, MAIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    ui.wpl = SDL_CreateWindow("levari - playlist", mx, my + MAIN_H,
                              PL_W, PL_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!ui.wmain || !ui.wpl) {
        fprintf(stderr, "levari: window: %s\n", SDL_GetError());
        if (ui.wmain) SDL_DestroyWindow(ui.wmain);
        if (ui.wpl) SDL_DestroyWindow(ui.wpl);
        SDL_Quit();
        return -1;
    }
    if (!show_pl) SDL_HideWindow(ui.wpl);
    ui.weq = SDL_CreateWindow("levari - equalizer", ex, ey,
                              EQ_W, EQ_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (ui.weq && !ui_show_eq) SDL_HideWindow(ui.weq);
    SDL_Surface *icon = skin_app_icon(32);
    if (icon) {
        SDL_SetWindowIcon(ui.wmain, icon);
        SDL_SetWindowIcon(ui.wpl, icon);
        if (ui.weq) SDL_SetWindowIcon(ui.weq, icon);
        SDL_FreeSurface(icon);
    }
    /* WMs often ignore/adjust the requested initial position; once the WM has
       had its say, re-place playlist/EQ relative to the main window's real
       position so the trio never ends up stacked or off-screen. */
    SDL_PumpEvents();
    int ax, ay;
    SDL_GetWindowPosition(ui.wmain, &ax, &ay);
    if (ax != mx || ay != my) {
        int py = ay + MAIN_H;
        if (py + PL_H > disp.y + disp.h) py = disp.y + disp.h - PL_H;
        if (py < disp.y) py = disp.y;
        SDL_SetWindowPosition(ui.wpl, ax, py);
        int aex = ax, aey = ay - EQ_H;
        if (aey < disp.y) {
            aex = ax + MAIN_W + 4; aey = ay;
            if (aex + EQ_W > disp.x + disp.w) aex = disp.x + disp.w - EQ_W;
            if (aex < disp.x) aex = disp.x;
        }
        SDL_SetWindowPosition(ui.weq, aex, aey);
    }
    /* EQ docks above main by default: register the snap so it follows
       the main window until dragged away */
    if (ui.weq) {
        int wx, wy, qx, qy;
        SDL_GetWindowPosition(ui.wmain, &wx, &wy);
        SDL_GetWindowPosition(ui.weq, &qx, &qy);
        ui.snap[0].w = ui.weq; ui.snap[0].dx = qx - wx; ui.snap[0].dy = qy - wy;
    }
    ui.smain = SDL_GetWindowSurface(ui.wmain);
    ui.spl = SDL_GetWindowSurface(ui.wpl);
    if (ui.weq) ui.seq = SDL_GetWindowSurface(ui.weq);
    if (ui_show_search) search_open(ui.wmain);

    Uint32 next = SDL_GetTicks();
    int running = 1;
    while (running) {
        if (ui_deadline_ms && SDL_GetTicks() >= ui_deadline_ms) break;
        SDL_Event e;
        int wait = (int)(next > SDL_GetTicks() ? next - SDL_GetTicks() : 0);
        if (SDL_WaitEventTimeout(&e, wait)) switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_WINDOWEVENT:
            if (e.window.windowID == SDL_GetWindowID(ui.wmain)) ui.smain = SDL_GetWindowSurface(ui.wmain);
            if (e.window.windowID == SDL_GetWindowID(ui.wpl)) ui.spl = SDL_GetWindowSurface(ui.wpl);
            if (ui.weq && e.window.windowID == SDL_GetWindowID(ui.weq)) ui.seq = SDL_GetWindowSurface(ui.weq);
            if (ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) ui.sfile = SDL_GetWindowSurface(ui.wfile);
            if (ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) ui.sopt = SDL_GetWindowSurface(ui.wopt);
            if (search_window() && e.window.windowID == search_window_id())
                search_surface_refresh();
            /* whichever of our windows gets focus, bring the whole set forward
               so activating the playlist never leaves the main window buried */
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                ui.focus_wid = e.window.windowID;
                focus_click_drag(e.window.windowID);
                if (search_is_open() && e.window.windowID == search_window_id()) SDL_StartTextInput();
                else if (ui.opt_visible && ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) SDL_StartTextInput();
                else SDL_StopTextInput();
                if (e.window.windowID == SDL_GetWindowID(ui.wmain) ||
                    e.window.windowID == SDL_GetWindowID(ui.wpl) ||
                    (ui.weq && e.window.windowID == SDL_GetWindowID(ui.weq)) ||
                    (ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) ||
                    (ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) ||
                    (search_is_open() && e.window.windowID == search_window_id()))
                    raise_all();
            }
            break;
        case SDL_KEYDOWN:
            if (search_is_open() && ui.focus_wid == search_window_id()) search_key(&e);
            else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) opt_key(&e);
            else if (ui.pl_visible && ui.wpl && ui.focus_wid == SDL_GetWindowID(ui.wpl)) {
                if (!pl_key(&e)) handle_key(&e);
            } else handle_key(&e);
            break;
        case SDL_TEXTINPUT:
            if (search_is_open() && ui.focus_wid == search_window_id()) search_text(e.text.text);
            else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) opt_text(e.text.text);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEWHEEL: {
            Uint32 wid = ev_win(&e);
            /* a swallowed button-release must not leave a drag latched on */
            if (e.type == SDL_MOUSEMOTION && ui.drag &&
                !(e.motion.state & SDL_BUTTON_LMASK)) ui.drag = 0;
            if (wid == SDL_GetWindowID(ui.wmain)) handle_main_mouse(&e);
            else if (wid == SDL_GetWindowID(ui.wpl)) handle_pl_mouse(&e);
            else if (ui.weq && wid == SDL_GetWindowID(ui.weq)) handle_eq_mouse(&e);
            else if (ui.wfile && wid == SDL_GetWindowID(ui.wfile)) handle_file_mouse(&e);
            else if (ui.wopt && wid == SDL_GetWindowID(ui.wopt)) handle_opt_mouse(&e);
            else if (search_is_open() && wid == search_window_id()) search_mouse(&e);
            break;
        }
        }
        if (SDL_AtomicGet(&ui.opt_scan_done)) {
            SDL_AtomicSet(&ui.opt_scan_done, 0);
            ui.opt_scanning = 0;
            snprintf(ui.opt_status, sizeof ui.opt_status,
                     "added %d (%d seen)", ui.opt_scan_added, ui.opt_scan_seen);
            search_lib_invalidate();
        }
        /* redraw only on the fixed 10 fps tick, not per event: otherwise a
           flood of mouse-motion events makes the title scroller race */
        Uint32 now = SDL_GetTicks();
        if (now >= next) {
            refresh_title();
            draw_main();
            SDL_UpdateWindowSurface(ui.wmain);
            if (ui.pl_visible) {
                draw_pl();
                SDL_UpdateWindowSurface(ui.wpl);
            }
            if (ui.eq_visible && ui.weq) {
                draw_eq();
                SDL_UpdateWindowSurface(ui.weq);
            }
            if (ui.file_visible && ui.sfile) {
                draw_file();
                SDL_UpdateWindowSurface(ui.wfile);
            }
            if (ui.opt_visible && ui.sopt) {
                draw_opt();
                SDL_UpdateWindowSurface(ui.wopt);
            }
            if (search_is_open() && search_window()) {
                search_draw();
                SDL_UpdateWindowSurface(search_window());
            }
            next = now + 100;
        }
    }
    /* save window dumps at exit so they reflect the final state */
    if (ui_dump_path) {
        char f[512];
        snprintf(f, sizeof f, "%s-main.bmp", ui_dump_path);
        if (ui.smain) SDL_SaveBMP(ui.smain, f);
        if (ui.pl_visible && ui.spl) {
            snprintf(f, sizeof f, "%s-pl.bmp", ui_dump_path);
            SDL_SaveBMP(ui.spl, f);
        }
        if (ui.eq_visible && ui.seq) {
            snprintf(f, sizeof f, "%s-eq.bmp", ui_dump_path);
            SDL_SaveBMP(ui.seq, f);
        }
        if (ui.file_visible && ui.sfile) {
            snprintf(f, sizeof f, "%s-file.bmp", ui_dump_path);
            SDL_SaveBMP(ui.sfile, f);
        }
        ui_dump_path = NULL;
    }
    search_shutdown();
    if (ui.wopt) SDL_DestroyWindow(ui.wopt);
    if (ui.wfile) SDL_DestroyWindow(ui.wfile);
    SDL_DestroyWindow(ui.weq);
    SDL_DestroyWindow(ui.wpl);
    SDL_DestroyWindow(ui.wmain);
    SDL_Quit();
    return 0;
}

void ui_open_playlist(void) {
    if (ui.wpl) {
        ui.pl_visible = 1;
        if (ui.pl_docked) {
            int dx, dy;
            pl_dock_pos(&dx, &dy);
            SDL_SetWindowPosition(ui.wpl, dx, dy);
        }
        SDL_ShowWindow(ui.wpl);
    }
}
