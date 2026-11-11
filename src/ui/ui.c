#include "ui.h"
#include "search.h"
#include "textedit.h"
#include "../skin/skin.h"
#include "../skin/cjk_font.h"
#include "../viz/viz.h"
#include "../config/config.h"
#include "../tag/tag.h"
#include "../library/library.h"
#include "../decode/decode.h"   /* dec_ffmpeg_probe: URL-window notice */
#include "../mpris/mpris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>      /* powf: time-normalized viz fall-off */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>   /* GetLogicalDrives for the COMPUTER pseudo-root */
#endif

enum {
    B_PLAY, B_PAUSE, B_STOP, B_PREV, B_NEXT, B_OPEN,
    B_SHUF, B_REPEAT, B_EQ, B_VIS, B_LIST, B_ABA, B_ABB, B_XMAIN, B_SEARCH, B_URL, B_RPT1,
    PB_SHUF, PB_REPEAT, PB_ADD, PB_REM, PB_CLOSE, PB_LIB, PB_SCAN, PB_X, FB_ADDDIR, FB_X, B_URLX, IB_X,
    EQB_ON, EQB_FLAT, EQB_ROCK, EQB_POP, EQB_JAZZ, EQB_X, EQB_RESET,
    OPTB_SAVE, OPTB_CLOSE, OPTB_TPL, OPTB_TEQ, OPTB_TVIS, OPTB_TSRCH, OPTB_TSHUF,
    OPTB_TFADE, OPTB_TLARGE, OPTB_TFPS, OPTB_TENC, OPTB_VHZ,
    OPTB_SCAN, OPTB_CLEAR, OPTB_CHK, OPTB_X, OPTB_TABB, OPTB_TABA, OPTB_TABU,
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
/* slider group centered in EQ_W: preamp + 10 bands at 24 pitch, 12 wide —
   span 252 -> margins 31/32 */
#define EQ_PRE_X 31
#define EQ_BAND_X(i) (55 + (i) * 24)
/* bottom row: ON/OFF at the lower right with RESET tucked next to it —
   A centered RESET proved too easy to hit by accident. The four
   presets sit one button width (34 px) LEFT of true center:
   x 51/89/127/161 (span 51..195), leaving a 38 px breathing gap
   before RESET. ONE table drives draw AND hit-testing so they can never
   drift apart. */
#define EQ_PRE_Y 132
static const struct { int id; const char *l; int x, w; } EQ_PRE[] = {
    {EQB_FLAT, "FLAT", 51, 34}, {EQB_ROCK, "ROCK", 89, 34},
    {EQB_POP, "POP", 127, 30}, {EQB_JAZZ, "JAZZ", 161, 34},
    {EQB_RESET, "RESET", 233, 38},
};
static Rect eq_on_rect(void) { Rect r = {EQ_W - 40, EQ_PRE_Y, 30, 12}; return r; }

typedef struct { int id; const Rect *r; } Btn;

static const Btn MAIN_BTNS[] = {
    {B_SHUF, &R_SHUF}, {B_REPEAT, &R_REPEAT}, {B_EQ, &R_EQ}, {B_VIS, &R_VIS},
    {B_LIST, &R_LIST},
#ifdef LV_URL_ENABLED
    /* the URL button beside LST (streaming-plan UI entry): absence IS
       the message in LV_STREAMING=OFF builds — no registered id, no
       draw, no hit-test, no action */
    {B_URL, &R_URL},
#endif
    {B_PREV, &R_PREV}, {B_PLAY, &R_PLAY}, {B_PAUSE, &R_PAUSE},
    {B_STOP, &R_STOP}, {B_NEXT, &R_NEXT}, {B_OPEN, &R_OPEN},
    {B_ABA, &R_ABA}, {B_ABB, &R_ABB}, {B_SEARCH, &R_SEARCH}, {B_RPT1, &R_RPT1},
};
static const Btn PL_BTNS[] = {
    {PB_SHUF, &P_SHUF}, {PB_REPEAT, &P_REPEAT}, {PB_ADD, &P_ADD},
    {PB_REM, &P_REM}, {PB_CLOSE, &P_CLOSE}, {PB_LIB, &P_LIB}, {PB_SCAN, &P_SCAN},
};

/* ---- playlist right-click edit menu (D1: menu, not new PL_BTNS — chrome
   space is precious and Winamp-authentic playlists edit through the list
   menu). Two columns of flat items fit the default 116px window; 5x7
   ASCII labels. Right-click anywhere in the window opens it (the row list
   ignores right-click today, and the title-strip drag is left-only, so
   the menu cannot race the drag-4 latch; while open it EATS the next left
   click — outside: close only, inside: fire and close, the same one-click
   contract as the MAIN menu). The action switch lives next to
   handle_pl_mouse; CLEAR reuses PB_REM's proven select-all path. */
enum {
    PM_STITLE, PM_SARTIST, PM_SALBUM, PM_STRACK, PM_SFILE,
    PM_REVERSE, PM_SHUFFLE, PM_CROP, PM_CLEAR,
    PM_COPY, PM_CUT, PM_PASTE, PM_MUP, PM_MDOWN, PM_SAVEAS,
    PM_INFO, PM_N
};
static const struct { int act; const char *lbl; } PL_MENU[PM_N] = {
    { PM_STITLE,  "SORT TITLE"  }, { PM_SARTIST,  "SORT ARTIST" },
    { PM_SALBUM,  "SORT ALBUM"  }, { PM_STRACK,   "SORT TRACK"  },
    { PM_SFILE,   "SORT FILE"   }, { PM_REVERSE,  "REVERSE"     },
    { PM_SHUFFLE, "RANDOMIZE"   }, { PM_CROP,     "CROP"        },
    { PM_CLEAR,   "CLEAR"       },
    { PM_COPY,    "COPY"        }, { PM_CUT,      "CUT"         },
    { PM_PASTE,   "PASTE"       }, { PM_MUP,      "MOVE UP"     },
    { PM_MDOWN,   "MOVE DOWN"   }, { PM_SAVEAS,   "SAVE AS..."  },
    { PM_INFO,    "SONG INFO"   },
};
#define PL_MENU_ROWS 8                 /* first col 8, second the rest */
static Rect pl_menu_item_rect(int i) {
    Rect r = { 2 + (i / PL_MENU_ROWS) * 90, 2 + (i % PL_MENU_ROWS) * 13, 90, 13 };
    return r;
}

/* ui.snap[] / ui.follow_snap[] slots (see the UI struct): 0 eq, 1 file,
   2 search, 3 options, 4 url — the url slot exists in every build, it
   only ever fills while LV_URL_ENABLED compiles the URL window in */
#define SNAP_NSLOT 5
#define SNAP_URL 4

typedef struct {
    Player *pl;
    SDL_Window *wmain, *wpl, *weq, *wfile, *wopt, *winfo;
    SDL_Surface *smain, *spl, *seq, *sfile, *sopt, *sinfo;
    /* built-in open file/folder browser */
    char file_dir[LV_MAX_PATH];
    char **file_names;   /* entries of file_dir, dirs first, then ".." at 0 */
    int file_n, file_scroll, file_sel, file_visible;
    int *file_view;      /* indices into file_names while the field filters;
                            file_sel/file_scroll index THIS, not file_names */
    char *file_isdir;    /* parallel to file_names: dir flags captured while
                            listing, so the draw loop never stats per row
                            (NULL = not captured, fall back to stat) */
    int file_view_n;
    char file_field[LV_MAX_PATH]; /* editable twin of file_dir: path entry
                                     + live filter source (text after the
                                     last separator) */
    TField file_tf;      /* edit state; buf points at file_field */
    int file_focus;      /* 1 = the path field has the caret */
    int file_err;        /* "path not valid" showing over the field */
    Uint32 file_err_at;  /* SDL_GetTicks when the error started */
    int file_pseudo;     /* Windows pseudo-root: logical drives, no parent */
    int file_saving;     /* browser in SAVE mode (playlist Save-As): title
                            differs, Enter writes the file, no audio filter */
    /* options window */
    int opt_visible, opt_focus, opt_hover;
#ifdef LV_URL_ENABLED
    /* URL loader window (streaming-plan UI entry): ONE paste field,
       Enter = open + play (transient one-row list) + close, Esc = close.
       Prefill on OPEN comes from urlIndex and is inert — nothing GETs
       until Enter; the scheme gate re-validates whatever is in the field
       at Enter time, no matter its source (typed/pasted/prefilled). */
    SDL_Window *wurl;
    SDL_Surface *surl;
    int url_visible, url_focus, url_err;
    int url_ff_ok;               /* dec_ffmpeg_probe()==1, cached at open:
                                    0 = streaming cannot work (no libav) and
                                    the window says so below the field */
    int url_ff_code;             /* raw probe return (1/2/3): 3 reads a
                                    different red line — found, wrong ABI */
    char url_ff_ver[32];         /* release found at open (for that line) */
    Uint32 url_err_at;
    char url_field[LV_MAX_PATH];
    TField url_tf;               /* buf points at url_field */
#endif
    int opt_caret;               /* caret byte offset in the focused field (codepoint boundary) */
    int opt_selall;              /* ctrl-a: whole focused field selected, next edit replaces it */
    char opt_field[OPTF_N][LV_MAX_PATH];
    int opt_pl, opt_eq, opt_vis, opt_srch, opt_shuf, opt_fade, opt_large, opt_f10;
    int opt_vhz;                /* ADV cycle button copy: 10/24/30/60 (cfg.vis_hz) */
    int opt_enc;                 /* LEGACY button cycle: 0 auto, 1 latin1, 2 cp1251, 3 utf8, 4 shiftjis (cfg value = opt_enc - 1) */
    int opt_ff;                  /* ADV status line: ffmpeg (libav*) detected
                                    on the system — probed once at opt_open
                                    (the probe dlopens per call; libav.dir
                                    changes need a restart anyway) */
    char opt_ff_ver[32];         /* release of the ffmpeg actually found
                                    (probe_info), shown on the same line */
    int opt_tab;                 /* options window tab: 0 BASIC, 1 ADVANCED, 2 ABOUT */
    int opt_scanning;            /* a library scan thread is running */
    int opt_tags;                /* scan reads tags+duration, not just names */
    SDL_atomic_t opt_scan_done;  /* set by the scan thread when finished */
    int opt_scan_added, opt_scan_seen, opt_scan_skipped;
    char opt_status[96];         /* one-line scan/clear feedback */
    /* song-info window (playlist edit menu): ONE selected row's tags,
       freshly read from disk at invocation, plus its file location.
       Re-opening re-reads (the library index is never consulted).
       info_focus = the copy-focused value box, -1 = none (the values are
       read-only: click selects, Ctrl+C copies the whole value). */
    int info_visible;
    TagFull info_full;
    int info_focus;
    char info_path[LV_MAX_PATH];
    char info_dir[LV_MAX_PATH];
    char info_file[LV_MAX_PATH];
    int menu;            /* right-click titlebar dropdown: 0 closed 1 open */
    int pl_menu;         /* playlist right-click edit menu: 0 closed 1 open */
    int pressed;
    int drag;          /* 0 none 1 seek 2 vol 3 win-main 4 win-pl 5 plscroll 6 eq-slider 7 win-eq 8 pl-resize 9 win-file 10 pan 11 win-opt 12 pl-row 13 win-url 14 win-info */
    int eq_band;       /* 0..9 band, 10 = preamp, while dragging */
    int pl_scroll, pl_sel;
    int pl_cur;            /* last cur index the playlist was scrolled for */
    int pl_selall;           /* ctrl-a: whole playlist selected, del/rem removes it */
    int pl_drop;             /* drag-12: row index the block lands AT (insertion
                                gap, 0..count; -1 = drag armed but not yet moved
                                past the arm threshold, so nothing to apply) */
    int pl_press_y;          /* drag-12: press y, the arm-threshold origin */
    int pl_dbl;              /* drag-12: row the deferred double-click plays
                                on release, -1 = none (deferred so a row drag
                                eats the play the old down-time play fired) */
    int pl_collapse;         /* drag-12: row a plain press inside a multi-row
                              selection collapses to on release (-1 = none) */
    int *pl_multi;         /* sorted row indices of the multi-selection — the
                              whole truth once a modifier click has run (plain
                              click keeps it the single row); pl_sel is the
                              shift-range anchor. Indices go stale when the
                              playlist changes elsewhere; every consumer clamps
                              (pl_sel alone always did). */
    int pl_multi_n, pl_multi_cap;
    int pl_visible, eq_visible;
    int pl_w, pl_h;        /* current playlist window size */
    int pl_docked;         /* playlist attached under main window */
    int vis_on;
    char title[512];
    int scroll_off;
    int marquee;            /* 1 when draw_main took the scrolling-title path */
    char msg[64];           /* transient red line over the title strip (rescan
                               feedback; users dislike silence); empty
                               = off, kept short on purpose so it never scrolls */
    Uint32 msg_at;          /* SDL_GetTicks when the message appeared */
    int drag_gx, drag_gy;   /* global mouse pos anchor, re-anchored each drag step */
    int drag_wx, drag_wy;   /* window size at resize start (drag 8) */
    int drag_rx, drag_ry;   /* last position REQUESTED this drag (refusal check) */
    int drag_wxlo, drag_wxhi, drag_wylo, drag_wyhi;  /* walls hit this drag */
    /* per-window WM-refusal bookkeeping for the drag-3 follow moves (see
       follow_move): last REQUESTED pos + walls hit this drag; valid=0 until
       the window first takes part in the current drag */
    struct FollowState { int rx, ry, xlo, xhi, ylo, yhi, valid; }
        follow_pl, follow_snap[SNAP_NSLOT];
    Uint32 focus_wid;       /* window id of the focused one of ours */
    int app_focused;        /* 1 while any of our windows holds WM focus */
    /* windows snapped beside the main one: follow it while it moves.
       slots: SNAP_NSLOT (0 eq, 1 file, 2 search, 3 options, 4 url);
       w = NULL when not snapped */
    struct { SDL_Window *w; int dx, dy; } snap[SNAP_NSLOT];
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
        /* same printable filter as the search query: DEL, C1 and CESU-8
           surrogates must not reach path/query fields, the daemon or D-Bus */
        if (cp >= 0x20 && cp != 0x7F &&
            !(cp >= 0x80 && cp <= 0x9F) &&
            !(cp >= 0xD800 && cp <= 0xDFFF)) {
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

/* ---- playlist multi-selection -------------------------------------------
   pl_multi is a sorted array of selected row indices — the whole truth of
   the selection (pl_sel is the shift-range anchor, pl_selall the ctrl-a
   shortcut). Indices can go stale when the playlist changes elsewhere
   (daemon adds, drops); every consumer clamps on use, as pl_sel always did. */
static void pl_sel_reset(void) { ui.pl_multi_n = 0; }

static int pl_sel_find(int idx) { /* >=0 hit, else -(insertion point + 1) */
    int lo = 0, hi = ui.pl_multi_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ui.pl_multi[mid] == idx) return mid;
        if (ui.pl_multi[mid] < idx) lo = mid + 1; else hi = mid - 1;
    }
    return -(lo + 1);
}

static int pl_sel_has(int idx) { return pl_sel_find(idx) >= 0; }

static int pl_multi_grow(int need) {
    if (need <= ui.pl_multi_cap) return 0;
    int cap = ui.pl_multi_cap ? ui.pl_multi_cap * 2 : 16;
    while (cap < need) cap *= 2;
    int *m = realloc(ui.pl_multi, (size_t)cap * sizeof *m);
    if (!m) return -1;                  /* OOM: selection stays as-is, never crash */
    ui.pl_multi = m; ui.pl_multi_cap = cap;
    return 0;
}

static void pl_sel_add(int idx) {
    int at = pl_sel_find(idx);
    if (at >= 0) return;
    if (pl_multi_grow(ui.pl_multi_n + 1) != 0) return;
    at = -at - 1;
    memmove(&ui.pl_multi[at + 1], &ui.pl_multi[at],
            (size_t)(ui.pl_multi_n - at) * sizeof *ui.pl_multi);
    ui.pl_multi[at] = idx;
    ui.pl_multi_n++;
}

static void pl_sel_del(int idx) {
    int at = pl_sel_find(idx);
    if (at < 0) return;
    memmove(&ui.pl_multi[at], &ui.pl_multi[at + 1],
            (size_t)(ui.pl_multi_n - at - 1) * sizeof *ui.pl_multi);
    ui.pl_multi_n--;
}

/* ctrl-a collapses "everything" into a flag; the first modifier click
   expands it back into rows before touching the selection */
static void pl_sel_materialize(int count) {
    if (pl_multi_grow(count) != 0) return;
    for (int i = 0; i < count; i++) ui.pl_multi[i] = i;
    ui.pl_multi_n = count;
}

static void pl_dock_pos(int *x, int *y) {
    SDL_GetWindowPosition(ui.wmain, x, y);
    *y += MAIN_H;
}

static void drag_begin(SDL_Window *w) {
    SDL_GetGlobalMouseState(&ui.drag_gx, &ui.drag_gy);
    /* refusal bookkeeping is drag-scoped: start from the window's true
       position with no known walls (see drag_step) */
    SDL_GetWindowPosition(w, &ui.drag_rx, &ui.drag_ry);
    ui.drag_wxlo = ui.drag_wxhi = ui.drag_wylo = ui.drag_wyhi = 0;
    /* carried/pushed windows are drag-scoped too (follow_move in the
       drag-3 follow AND reflow_above pushes during snap drags): a wall
       from a previous drag must never leak into this one */
    ui.follow_pl.valid = 0;
    for (int i = 0; i < SNAP_NSLOT; i++) ui.follow_snap[i].valid = 0;
}

static int snap_idx(SDL_Window *w) {
    if (w == ui.weq) return 0;
    if (w == ui.wfile) return 1;
    if (w == search_window()) return 2;
    if (w == ui.wopt) return 3;
#ifdef LV_URL_ENABLED
    if (ui.wurl && w == ui.wurl) return SNAP_URL;
#endif
    return -1;
}

static void reflow_above(SDL_Window *self, int cursor_y);

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
        /* the edge above main is a STACK (2026-11-07 layout):
           the URL window always inserts flush above main (bottom of the
           stack, pushing everything already snapped above one level up);
           every other window stacks flush above the current top. The
           URL window is URL_W=320 vs MAIN_W=315: like the old flush-above
           rule it left-aligns to main, the 5 px ride over its right edge. */
        *x = mx;
        snapped = 1;
#ifdef LV_URL_ENABLED
        if (w == ui.wurl) {
            *y = my - wh;
            reflow_above(w, *y);
        } else
#endif
        {
            int top = my;
            for (int j = 0; j < SNAP_NSLOT; j++) {
                if (!ui.snap[j].w || ui.snap[j].w == w || ui.snap[j].dy >= 0) continue;
                int jx, jy;
                SDL_GetWindowPosition(ui.snap[j].w, &jx, &jy);
                if (jy < top) top = jy;
            }
            *y = top - wh;
        }
    }
    int i = snap_idx(w);
    if (i >= 0) {
        if (snapped) { ui.snap[i].w = w; ui.snap[i].dx = *x - mx; ui.snap[i].dy = *y - my; }
        else ui.snap[i].w = NULL;
    }
}

/* One incremental move step with WM-refusal bookkeeping. The rules below
   are pinned by test/pldock/testpldock.c:
   - Incremental: apply the mouse delta since the previous step to the
     window's CURRENT position, so a WM clamp at a screen edge leaves no
     phantom overshoot (2d69fac).
   - The X11 SDL polls up to 100 ms per REFUSED position request
     (X11_SetWindowPosition waits for the WM to honour the move): one
     refused call per motion event while a window is pinned against a
     wall starves the event loop and prompts the desktop's "app not
     responding" popup. So a refusal is remembered as a wall for the
     rest of the drag, deeper requests are clamped away from it, and a
     request identical to the last one is skipped entirely.
   - A refusal is CONSUMED: the anchor jumps to the mouse, so travel
     into a wall is spent, not stored — coordinates must stop updating
     when the window is not moving, or the pull-back must retrace the
     whole push (the pre-2d69fac dead zone).
   - Otherwise the anchor advances by the delta ACTUALLY APPLIED
     post-snap: travel eaten by the dock/snap stays parked at the anchor
     and accumulates, so a slow pull can still cross DOCK_SNAP and break
     free (afc93b5; an externally-corrected position is treated as fully
     applied, keeping the clamp property).
   snap: 0 none, 1 beside/above main, 2 docked under main (playlist).
   ax/ay are the caller-owned drag anchors. */
static void drag_step(SDL_Window *w, int snap, int *ax, int *ay) {
    int gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    int x, y;
    SDL_GetWindowPosition(w, &x, &y);
    int ox = x, oy = y;
    if (ui.drag_rx != x) {
        *ax = gx;
        if (ui.drag_rx < x) ui.drag_wxlo = x; else ui.drag_wxhi = x;
    }
    if (ui.drag_ry != y) {
        *ay = gy;
        if (ui.drag_ry < y) ui.drag_wylo = y; else ui.drag_wyhi = y;
    }
    x += gx - *ax; y += gy - *ay;
    if (snap == 2) {
        int mx, my, mw, mh;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        SDL_GetWindowSize(ui.wmain, &mw, &mh);
        int snapped = 0;
        if (abs(y - (my + mh)) <= DOCK_SNAP) { y = my + mh; snapped = 1; }
        if (snapped && abs(x - mx) <= DOCK_SNAP) x = mx;
        ui.pl_docked = snapped;
    } else if (snap == 1) {
        ui_snap_next_to_main(w, &x, &y);
    }
    int clx = 0, cly = 0;
    if (ui.drag_wxlo && x < ui.drag_wxlo) { x = ui.drag_wxlo; clx = 1; }
    if (ui.drag_wxhi && x > ui.drag_wxhi) { x = ui.drag_wxhi; clx = 1; }
    if (ui.drag_wylo && y < ui.drag_wylo) { y = ui.drag_wylo; cly = 1; }
    if (ui.drag_wyhi && y > ui.drag_wyhi) { y = ui.drag_wyhi; cly = 1; }
    if (clx) *ax = gx;
    if (cly) *ay = gy;
    if (x != ui.drag_rx || y != ui.drag_ry) SDL_SetWindowPosition(w, x, y);
    ui.drag_rx = x; ui.drag_ry = y;
    *ax += x - ox; *ay += y - oy;
}

/* main window drag: no snap */
static void drag_move(SDL_Window *w) { drag_step(w, 0, &ui.drag_gx, &ui.drag_gy); }

/* file/eq/options drag: live-snap beside main, follows main until
   dragged away */
static void drag_move_snap(SDL_Window *w) { drag_step(w, 1, &ui.drag_gx, &ui.drag_gy); }

/* playlist drag: live-snap flush under the main window's bottom edge,
   x-aligned, within DOCK_SNAP px; docked state follows live like the
   side-snapped windows so it keeps following main until dragged away */
static void drag_move_pl(void) { drag_step(ui.wpl, 2, &ui.drag_gx, &ui.drag_gy); }

/* Refusal-aware follow move for the windows the main drag (3) carries:
   same discipline as drag_step — a refusal becomes a wall for the rest of
   the drag, deeper targets clamp away from it, and a request identical to
   the last is skipped — but the target is fully determined by main's live
   position, so there is no anchor to manage. Without this, a docked or
   snapped carry-over window pinned at a screen edge receives a fresh
   REFUSED position request per motion event, and each refused request
   costs SDL's X11 backend up to 100 ms of polling (see drag_step): the
   event loop starves and the desktop pops "app not responding". A window
   first seen in this drag baselines on its TRUE position so a mid-drag
   dock or snap cannot fabricate a refusal. */
static void follow_move(SDL_Window *w, struct FollowState *fs, int x, int y) {
    int tx, ty;
    SDL_GetWindowPosition(w, &tx, &ty);
    if (!fs->valid) {
        fs->rx = tx; fs->ry = ty;
        fs->xlo = fs->xhi = fs->ylo = fs->yhi = 0;
        fs->valid = 1;
    } else {
        if (fs->rx != tx) { if (fs->rx < tx) fs->xlo = tx; else fs->xhi = tx; }
        if (fs->ry != ty) { if (fs->ry < ty) fs->ylo = ty; else fs->yhi = ty; }
    }
    if (fs->xlo && x < fs->xlo) x = fs->xlo;
    if (fs->xhi && x > fs->xhi) x = fs->xhi;
    if (fs->ylo && y < fs->ylo) y = fs->ylo;
    if (fs->yhi && y > fs->yhi) y = fs->yhi;
    if (x != fs->rx || y != fs->ry) SDL_SetWindowPosition(w, x, y);
    fs->rx = x; fs->ry = y;
}

/* The edge above main is a stack (see ui_snap_next_to_main). Re-pack the
   windows snapped there — slot registered with dy < 0 — in their current
   distance order from main (snap dy, most-negative = highest first),
   stacking upward from cursor_y; the stack is left-aligned to main.
   Moves go through follow_move so they carry the same refusal discipline
   as the drag-3 carry (walls recorded per drag, deeper requests clamped,
   identical requests skipped). Outside a drag (url open/close) the
   bookkeeping is re-baselined first: those are one-shot placement moves,
   a previous drag's wall must not clamp them. self (may be NULL) is
   skipped — the caller has just placed it. */
static void reflow_above(SDL_Window *self, int cursor_y) {
    int mx, my;
    SDL_GetWindowPosition(ui.wmain, &mx, &my);
    int left[SNAP_NSLOT], n = 0;
    for (int i = 0; i < SNAP_NSLOT; i++)
        if (ui.snap[i].w && ui.snap[i].w != self && ui.snap[i].dy < 0) left[n++] = i;
    /* nearest-to-main first = largest (least negative) dy, selected */
    while (n > 0) {
        int best = 0;
        for (int m = 1; m < n; m++)
            if (ui.snap[left[m]].dy > ui.snap[left[best]].dy) best = m;
        int i = left[best];
        left[best] = left[--n];
        int sw, sh;
        SDL_GetWindowSize(ui.snap[i].w, &sw, &sh);
        cursor_y -= sh;
        if (!ui.drag) ui.follow_snap[i].valid = 0;
        follow_move(ui.snap[i].w, &ui.follow_snap[i], mx, cursor_y);
        ui.snap[i].dy = cursor_y - my;
    }
}

/* End of a window drag that met a WM wall: kick the window 1 px off that
   wall, opposite the direction it was pushed. A window left pinned
   exactly at the clamp boundary feels sticky on the next grab; one pixel
   clear, the first motion of a new drag moves freely. The wall values are
   the last ACCEPTED (truth) positions recorded by drag_step/search.c, so
   the bounce lands inside the WM's clamp and is honoured. No wall -> no
   move. */
void ui_drag_bounce(SDL_Window *w, int xlo, int xhi, int ylo, int yhi) {
    int x, y, move = 0;
    SDL_GetWindowPosition(w, &x, &y);
    if (xlo && x <= xlo) { x = xlo + 1; move = 1; }
    else if (xhi && x >= xhi) { x = xhi - 1; move = 1; }
    if (ylo && y <= ylo) { y = ylo + 1; move = 1; }
    else if (yhi && y >= yhi) { y = yhi - 1; move = 1; }
    if (move) SDL_SetWindowPosition(w, x, y);
}

/* The wall-facing window need not be the dragged one: with a docked or
   snapped window between main and the wall, pushing MAIN into the wall
   pins the carried window at its own clamp (follow_move) while main keeps
   travelling until its clamp or its travel stops — main then sits inside
   or overlapping the pinned window. On main's release, pull main back to
   the dock position flush against every carried window it overshoots
   (per axis: a carried window at truth t with snap offset d must end up
   at main+d, so main returns to t-d on that axis). Only overshoot is
   corrected — a gap the user opened deliberately stays. This REPLACES
   the wall kick for main's release: the dock position is already clear of
   the wall, and kicking would tear the flush dock open again. */
static int drag_dock_restore(void) {
    int mx, my;
    SDL_GetWindowPosition(ui.wmain, &mx, &my);
    struct { SDL_Window *w; int dx, dy; } c[SNAP_NSLOT + 1];
    int n = 0;
    if (ui.pl_docked && ui.pl_visible) {
        c[n].w = ui.wpl; c[n].dx = 0; c[n].dy = MAIN_H; n++;
    }
    for (int i = 0; i < SNAP_NSLOT; i++)
        if (ui.snap[i].w) { c[n].w = ui.snap[i].w; c[n].dx = ui.snap[i].dx; c[n].dy = ui.snap[i].dy; n++; }
    int moved = 0;
    for (int i = 0; i < n; i++) {
        int tx, ty;
        SDL_GetWindowPosition(c[i].w, &tx, &ty);
        if (c[i].dx < 0 && mx < tx - c[i].dx) { mx = tx - c[i].dx; moved = 1; }
        else if (c[i].dx > 0 && mx > tx - c[i].dx) { mx = tx - c[i].dx; moved = 1; }
        if (c[i].dy < 0 && my < ty - c[i].dy) { my = ty - c[i].dy; moved = 1; }
        else if (c[i].dy > 0 && my > ty - c[i].dy) { my = ty - c[i].dy; moved = 1; }
    }
    if (moved) SDL_SetWindowPosition(ui.wmain, mx, my);
    return moved;
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

/* the playlist footer total: sum in long long (durations arrive from
   untrusted files — EXTINF/LengthN/index rows are raw atoi — and two
   INT_MAX-sized rows wrapped the old int accumulator, so the footer
   read 0:00), treat non-positive rows as unknown, and saturate the
   readout at 9999:59:59+ so it stays bounded no matter the input. */
#define PL_TOTAL_CAP (9999LL * 3600 + 59 * 60 + 59)   /* 35,999,999 s */
static void fmt_pl_total(char *buf, size_t n, const Playlist *pl) {
    long long total = 0;
    for (int i = 0; i < pl->count; i++) {
        if (pl->items[i].duration <= 0) continue;
        total += pl->items[i].duration;
        if (total > PL_TOTAL_CAP) break;
    }
    if (total > PL_TOTAL_CAP) lv_strlcpy(buf, "9999:59:59+", n);
    else fmt_time(buf, n, total);
}

#define D_MAIN 1u
#define D_PL   2u
#define D_EQ   4u
#define D_FILE 8u
#define D_OPT  16u
#define D_SRCH 32u
#define D_URL  64u
#define D_INFO 128u
#define D_ALL  (D_MAIN | D_PL | D_EQ | D_FILE | D_OPT | D_SRCH | D_URL | D_INFO)
/* ---- main-window draw regions (cpu_floor_hunting item 1) ------------------
   D_MAIN (and the scheduler's window bits above) stays the conservative
   full-window bit that every event site marks. The producer bits here let
   the two steady-state repaint drivers mark ONLY what moves: a marquee or
   viz tick repaints the title strip, the 1 s position poll repaints the
   time readout + info line + seek knob. draw_main(regs) restores the
   background inside exactly the marked rects — every skin bg primitive is
   an SDL_FillRect, so confining clip_rect confines the whole replay (item
   2 replaces it with a blit from a cached static surface) — then repaints
   every widget INTERSECTING a marked rect. A widget skipped for a tick
   must not have changed content since the last paint that covered it:
   the audited truth is that time/strip/seek move only with the marked
   poll/anim producers, and everything else in the window changes only on
   events (which mark D_MAIN) or player-thread state caught by the poll
   snapshot, which marks full. */
#define D_M_TITLE 256u   /* title strip: marquee text / rescan message */
#define D_M_VIZ   512u   /* viz bars — the same strip, different producer */
#define D_M_TIME  1024u  /* time readout + info line + seek knob (poll) */
#define D_M_BG    2048u  /* whole window (expose, press, theme-level) */
#define D_M_ANY   (D_M_TITLE | D_M_VIZ | D_M_TIME | D_M_BG)
#define D_M_FULL  (D_MAIN | D_M_BG)

static SDL_Rect rgn_rects[4];  /* this pass's marked rects; rgn_n == 0 = full */
static int rgn_n;

static void rgn_add(const Rect *r) {
    if (rgn_n >= (int)(sizeof rgn_rects / sizeof rgn_rects[0])) return;
    rgn_rects[rgn_n].x = r->x; rgn_rects[rgn_n].y = r->y;
    rgn_rects[rgn_n].w = r->w; rgn_rects[rgn_n].h = r->h;
    rgn_n++;
}

/* does a widget rect fall inside this pass's regions? */
static int rgn_hit(const Rect *r) {
    if (!rgn_n) return 1;   /* full-window pass */
    for (int i = 0; i < rgn_n; i++)
        if (r->x < rgn_rects[i].x + rgn_rects[i].w &&
            rgn_rects[i].x < r->x + r->w &&
            r->y < rgn_rects[i].y + rgn_rects[i].h &&
            rgn_rects[i].y < r->y + r->h) return 1;
    return 0;
}

/* ui_dirty -> the region mask draw_main wants; any full bit forces all */
static unsigned main_draw_regions(unsigned m) {
    m &= D_MAIN | D_M_ANY;
    return (m & D_M_FULL) ? D_M_FULL : m;
}

#ifdef LV_PROFILE
/* Standing draw-path instrument for the low-floor CPU claims: per-producer
   tick counts, dirty-rect pixel totals and per-stage microseconds, printed
   to stderr at exit. Off unless -DLV_PROFILE=ON — the flag is the standing
   instrument for all floor claims, never shipped on. */
static struct {
    unsigned full, title, viz, ticks_time;
    unsigned long long px;
    unsigned long long bg_us, txt_us, wid_us, blit_us;
} lvprof;
static unsigned long long lv_us(void) {
    return SDL_GetPerformanceCounter() * 1000000ull
           / SDL_GetPerformanceFrequency();
}
static void lvprof_tick(unsigned regs) {
    if (regs & D_M_FULL) {
        lvprof.full++;
        lvprof.px += (unsigned long long)MAIN_W * MAIN_H;
    } else {
        if (regs & D_M_TITLE) lvprof.title++;
        if (regs & D_M_VIZ) lvprof.viz++;
        if (regs & D_M_TIME) lvprof.ticks_time++;
        for (int i = 0; i < rgn_n; i++)
            lvprof.px += (unsigned long long)rgn_rects[i].w * rgn_rects[i].h;
    }
}
#define LV_T0 unsigned long long _lv_t = lv_us()
#define LV_ACC(f) lvprof.f += lv_us() - _lv_t
#else
#define LV_T0 ((void)0)
#define LV_ACC(f) ((void)0)
#endif

static void draw_main(unsigned regs) {
    SDL_Surface *s = ui.smain;
    Player *p = ui.pl;

    /* this pass's rects (TITLE and VIZ share the strip — add it once) */
    rgn_n = 0;
    if (!(regs & D_M_FULL)) {
        if (regs & (D_M_TITLE | D_M_VIZ)) rgn_add(&R_TSCROLL);
        if (regs & D_M_TIME) {
            rgn_add(&R_TIME);
            rgn_add(&R_INFO);
            rgn_add(&R_SEEK);
        }
    }
#ifdef LV_PROFILE
    lvprof_tick(regs);
#endif

    {   /* background: replay confined to the marked rects */
        LV_T0;
        if (rgn_n) {
            for (int i = 0; i < rgn_n; i++) {
                SDL_SetClipRect(s, &rgn_rects[i]);
                skin_draw_main_bg(s);
            }
            SDL_SetClipRect(s, NULL);
        } else {
            skin_draw_main_bg(s);
        }
        LV_ACC(bg_us);
    }

    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    long long pos = p->rate ? (p->pos_frames - base) / p->rate : 0;

    {   /* text sections: skipped wholesale when their rects are clean */
        LV_T0;
        Rect bar = { 0, 0, MAIN_W, 22 };
        if (rgn_hit(&bar)) {
            font_puts(s, 10, 6, "LEVARI", col(s, 0, 224, 255));
            Rect x = close_rect(MAIN_W);
            draw_x(s, &x, ui.pressed == B_XMAIN);
        }

        /* row 1: play time in a box, 2x font, scrolling title beside it */
        if (rgn_hit(&R_TIME)) {
            char tb[16];
            fmt_time(tb, sizeof tb, p->state == ST_STOP ? 0 : pos);
            font_puts_big(s, R_TIME.x + 6, R_TIME.y + 2, tb, col(s, 0, 255, 0));
        }

        if (rgn_hit(&R_TSCROLL)) {
            int tx = R_TSCROLL.x + 2, tw = R_TSCROLL.w - 4;
            ui.marquee = 0;
            if (ui.msg[0]) {
                /* transient rescan feedback outranks even the viz bars: the
                   strip carries the message, plain red, for its 10 s */
                font_puts16(s, tx, R_TSCROLL.y + 1, ui.msg, col(s, 255, 60, 60));
            } else if (ui.vis_on) {
                static float bars[VIZ_BARS], smooth[VIZ_BARS];
                static Uint32 viz_last;
                int16_t snap[4096];
                int n;
                unsigned seq;
                int playing = p->state == ST_PLAY;
                pthread_mutex_lock(&p->viz_mtx);
                n = p->viz_n;
                if (n > 4096) n = 4096;
                if (n < 0) n = 0;
                seq = p->viz_seq;
                if (n > 0) memcpy(snap, p->viz_buf, (size_t)n * sizeof snap[0]);
                pthread_mutex_unlock(&p->viz_mtx);
                /* A FRESH engine chunk since the last frame? The player
                   feeds one chunk per decode pass (~21 Hz: CHUNK 2048 at
                   real-time pace), so above that UI rate a frame sees the
                   same seq. Re-FFTing it would redraw identical bars;
                   feeding silence (the old behavior) made them sawtooth.
                   No fresh chunk -> zero bars through the same 0.85
                   fall-off: pure decay until the next chunk (paused and
                   stopped ride this same branch, as before). */
                if (playing && seq != viz_last) {
                    viz_last = seq;
                    viz_spectrum(snap, n, p->rate, bars);
                } else {
                    memset(bars, 0, sizeof bars);
                }
                /* the 0.85 fall-off is PER 100 ms (the historical frame
                   rate); at 24/30/60 Hz a bare 0.85 decays 2.4x..6x too
                   fast. Normalize by real elapsed time: factor =
                   0.85^(dt/100), exactly 0.85 at the 10 Hz default. */
                static Uint32 viz_t;
                Uint32 tnow = SDL_GetTicks();
                float dt = viz_t ? (float)(tnow - viz_t) : 100.f;
                viz_t = tnow;
                if (dt > 500.f) dt = 500.f;   /* post-pause frame = decayed out */
                float k = powf(0.85f, dt / 100.f);
                for (int b = 0; b < VIZ_BARS; b++) {
                    smooth[b] = bars[b] > smooth[b] ? bars[b] : smooth[b] * k;
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
                ui.marquee = 1;
                char padded[600];
                snprintf(padded, sizeof padded, "%s   ", ui.title);
                int len = font_w16(padded);
                /* pause between cycles is a fixed 20 px (10 frames = 1 s at
                   2 px per 10 Hz tick); a tw-sized pause left the strip
                   blank ~9 s */
                int off = ui.scroll_off % (len + 20);
                /* draw codepoint by codepoint, hard-clipped to the title
                   strip so glyphs can never bleed into the time box */
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
                /* advance only while the marquee is live (playing, or the
                   10FPS override): paused/stopped repaints (hover, dirty
                   flags) must not creep the title along a few px per mouse
                   movement — and only while the strip is actually being
                   repainted (region passes that skip the strip must not
                   creep it invisibly) */
                if (p->state == ST_PLAY || cfg.ui_always10 > 0)
                    ui.scroll_off += 2;
            }
        }

        /* row 2: khz / kbps readout; bitrate estimated from file size over
           the decoded length (whole file, so CUE virtual tracks stay
           correct) */
        if (p->state != ST_STOP && p->rate && rgn_hit(&R_INFO)) {
        char rb[48];
        long long kbps = 0;
        long long tf = p->total_frames;
        if (tf > 0) {
            char path[LV_MAX_PATH];
            path[0] = 0;
            pthread_mutex_lock(&p->mtx);
            int ci = p->cur;
            if (ci >= 0 && ci < p->pl->count)
                lv_strlcpy(path, p->pl->items[ci].path, sizeof path);
            pthread_mutex_unlock(&p->mtx);
            /* stat once per track, not once per frame: on network
               shares a per-frame stat is a per-frame round-trip */
            static char sz_path[LV_MAX_PATH];
            static long long sz_size;
            if (path[0] && strcmp(path, sz_path) != 0) {
                struct stat st;
                sz_size = (stat(path, &st) == 0) ? st.st_size : 0;
                lv_strlcpy(sz_path, path, sizeof sz_path);
            }
            if (sz_size > 0)
                kbps = (long long)((double)sz_size * 8 * p->rate /
                                   (1000 * (double)tf) + 0.5);
        }
        if (kbps > 0) {
            /* long long through the cast (a sparse or crafted file can
               push size*rate/frames past INT_MAX, where the old (int)
               cast was implementation-defined), and the readout
               saturates at 999kbps+ */
            char kb[16];
            if (kbps >= 1000) lv_strlcpy(kb, "999kbps+", sizeof kb);
            else snprintf(kb, sizeof kb, "%lldkbps", kbps);
            snprintf(rb, sizeof rb, "%dkhz %s %s",
                     (int)(p->rate / 1000), kb, p->ch == 2 ? "st" : "mo");
        } else
            snprintf(rb, sizeof rb, "%dkhz %s",
                     (int)(p->rate / 1000), p->ch == 2 ? "st" : "mo");
        font_puts(s, R_INFO.x + 2, R_INFO.y + 1, rb, col(s, 180, 180, 180));
        }
        LV_ACC(txt_us);
    }

    {   /* widgets: gated per rect like the text */
        LV_T0;
    for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++) {
        const Btn *b = &MAIN_BTNS[i];
        if (!rgn_hit(b->r)) continue;
        int active = 0;
        if (b->id == B_SHUF) active = p->shuffle;
        if (b->id == B_REPEAT) active = p->repeat == 1;
        if (b->id == B_RPT1) active = p->repeat == 2;
        if (b->id == B_EQ) active = eq_enabled(&p->eq);
        if (b->id == B_VIS) active = ui.vis_on;
        skin_button(s, b->r, ui.pressed == b->id, active);
        Uint32 c = col(s, 220, 220, 220);
        if (b->id == B_SHUF) font_puts(s, b->r->x + (b->r->w - font_w("SHUFFLE")) / 2, b->r->y + 3, "SHUFFLE", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_REPEAT) font_puts(s, b->r->x + (b->r->w - font_w("REPEAT")) / 2, b->r->y + 3, "REPEAT", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_EQ) font_puts(s, b->r->x + 6, b->r->y + 3, "EQ", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_VIS) font_puts(s, b->r->x + 5, b->r->y + 3, "VIS", active ? col(s, 0, 255, 0) : c);
        if (b->id == B_LIST) font_puts(s, b->r->x + (b->r->w - font_w("LST")) / 2, b->r->y + 3, "LST", c);
#ifdef LV_URL_ENABLED
        if (b->id == B_URL) font_puts(s, b->r->x + (b->r->w - font_w("URL")) / 2, b->r->y + 3, "URL",
                                      ui.url_visible ? col(s, 0, 255, 0) : c);
#endif
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
    {
        Rect vrow = { 8, 62, MAIN_W - 16, 13 };
        if (rgn_hit(&vrow)) {
            font_puts(s, 8, 65, "VOL", col(s, 150, 150, 150));
            font_puts(s, 158, 65, "BIAS", col(s, 150, 150, 150));
            int vx = R_VOL.x + 2 + p->vol * (R_VOL.w - 16) / 128;
            skin_slider(s, &R_VOL, vx, 12);
            int px = R_PAN.x + 2 + (p->pan + 32) * (R_PAN.w - 16) / 64;
            skin_slider(s, &R_PAN, px, 12);
        }
    }
    /* row 5: timeline slider, its own row below the display; its knob is
       double the width of the volume/bias knobs */
    if (rgn_hit(&R_SEEK)) {
        double frac = endf > base ? (double)(p->pos_frames - base) / (double)(endf - base) : 0;
        int kx = R_SEEK.x + (int)(frac * (R_SEEK.w - SEEK_KNOB_W));
        skin_slider(s, &R_SEEK, kx, SEEK_KNOB_W);
    }

    if (ui.menu) {
        Rect mi = { 2, 22, 110, 20 };
        if (rgn_hit(&mi)) {
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
        LV_ACC(wid_us);
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
        case PB_SCAN: lbl = "SCAN"; break;
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
        char pretty[LV_MAX_PATH];
        const char *name;
        if (t->tags.title[0]) name = t->tags.title;
        else { lv_pretty_name(t->path, pretty, sizeof pretty); name = pretty; }
        if (t->duration) snprintf(line, sizeof line, "%02d. %s [%d:%02d]", idx + 1, name, t->duration / 60, t->duration % 60);
        else snprintf(line, sizeof line, "%02d. %s", idx + 1, name);
        /* truncate to fit, without splitting UTF-8 sequences */
        int fit = font_fit(line, ptext.w - 4);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = col(s, 170, 170, 170);
        if (idx == p->cur) c = col(s, 0, 255, 0);
        else if (ui.pl_selall || pl_sel_has(idx)) c = col(s, 255, 255, 255);
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

    /* row-drag (12): drop-index highlight — a green hairline at the gap
       the block will land at, clamped to the visible rows */
    if (ui.drag == 12 && ui.pl_drop >= 0) {
        int dy = ptext.y + (ui.pl_drop - ui.pl_scroll) * 17;
        if (dy < ptext.y) dy = ptext.y;
        if (dy > ptext.y + lines * 17 - 1) dy = ptext.y + lines * 17 - 1;
        SDL_Rect r = {ptext.x, dy, ptext.w, 1};
        SDL_FillRect(s, &r, col(s, 0, 255, 0));
    }

    /* totals, pinned to the bottom edge (fmt_pl_total saturates at
       9999:59:59+ so a crafted playlist cannot wrap the sum) */
    char tb[32];
    fmt_pl_total(tb, sizeof tb, pl);
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

    if (ui.pl_menu) {
        /* dark sheet over everything, same shape as the MAIN dropdown:
           two columns of flat labels, the hit rects are pl_menu_item_rect */
        int cols = (PM_N + PL_MENU_ROWS - 1) / PL_MENU_ROWS;
        SDL_Rect mb = { 2, 2, cols * 90, PL_MENU_ROWS * 13 };
        SDL_FillRect(s, &mb, col(s, 40, 40, 40));
        SDL_Rect t = { mb.x, mb.y, mb.w, 1 }, b = { mb.x, mb.y + mb.h - 1, mb.w, 1 };
        SDL_Rect l = { mb.x, mb.y, 1, mb.h }, r = { mb.x + mb.w - 1, mb.y, 1, mb.h };
        Uint32 bc = col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        for (int i = 0; i < PM_N; i++) {
            Rect ir = pl_menu_item_rect(i);
            font_puts(s, ir.x + 8, ir.y + 3, PL_MENU[i].lbl, col(s, 220, 220, 220));
        }
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

    int eq_on = eq_enabled(&p->eq);
    Rect on = eq_on_rect();
    skin_button(s, &on, ui.pressed == EQB_ON, eq_on);
    const char *onl = eq_on ? "ON" : "OFF";
    font_puts(s, on.x + (on.w - font_w(onl)) / 2, on.y + 3, onl,
              eq_on ? col(s, 0, 255, 0) : col(s, 220, 220, 220));

    for (size_t i = 0; i < sizeof EQ_PRE / sizeof EQ_PRE[0]; i++) {
        Rect r = {EQ_PRE[i].x, EQ_PRE_Y, EQ_PRE[i].w, 12};
        skin_button(s, &r, ui.pressed == EQ_PRE[i].id, 0);
        font_puts(s, r.x + (r.w - font_w(EQ_PRE[i].l)) / 2, EQ_PRE_Y + 3,
                  EQ_PRE[i].l, col(s, 220, 220, 220));
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
            char pretty[LV_MAX_PATH];
            lv_pretty_name(t->path, pretty, sizeof pretty);
            snprintf(buf, sizeof buf, "  %s", pretty);
        }
    }
    pthread_mutex_unlock(&p->mtx);
    /* the "artist - N - title" snprintf above can cut a multi-byte tag char
       at the 511-byte mark */
    lv_utf8_trim(buf);
    if (strcmp(buf, ui.title) != 0) {
        lv_strlcpy(ui.title, buf, sizeof ui.title);
        ui.scroll_off = 0;
        ui.msg[0] = 0;   /* a real title change preempts the transient red
                            line, unless something else replaces it */
    }
}

/* transient red feedback over the title strip, 10 s (main-loop expiry):
   kept short by every caller so it never needs the marquee path */
static void ui_msg(const char *text) {
    lv_strlcpy(ui.msg, text, sizeof ui.msg);
    ui.msg_at = SDL_GetTicks();
}

/* ---------- built-in open file/folder browser ---------- */

#ifdef _WIN32
/* Windows paths use both separators; "C:" alone is drive-relative, not root */
static int fs_is_root(const char *d) {
    size_t n = strlen(d);
    return (n == 1 && d[0] == '/') ||
           (n == 3 && d[1] == ':' && (d[2] == '\\' || d[2] == '/')) ||
           (n == 2 && d[1] == ':');
}
#else
static int fs_is_root(const char *d) { return !strcmp(d, "/"); }
#endif

#define FILE_W 300
#define FILE_H (MAIN_H + PL_H) /* spans the main+playlist stack height, like OPT_H */
#define FILE_LIST_RECT Rect list = {6, 42, FILE_W - 12, FILE_H - 42 - 20}
#define FILE_FLD_RECT  Rect fld = {6, 24, FILE_W - 12, 16}
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

/* the field mirrors the current folder whenever it has no caret (or the
   pseudo-root is showing, where no path applies) */
static void file_sync_field(void) {
#ifdef _WIN32
    if (ui.file_pseudo) {
        lv_strlcpy(ui.file_field, "COMPUTER", sizeof ui.file_field);
        ui.file_tf.caret = (int)strlen(ui.file_field);
        ui.file_tf.selall = 0;
        return;
    }
#endif
    lv_strlcpy(ui.file_field, ui.file_dir, sizeof ui.file_field);
    ui.file_tf.caret = (int)strlen(ui.file_field);
    ui.file_tf.selall = 0;
}

/* Rebuild the list view over file_names: while the path field has the
   caret and differs from the current dir, the text after the last
   separator filters entry names as a case-insensitive substring (the
   song-search feel). ".." is always kept, at view index 0. Pure string
   pass over the cached names — no stat. file_sel/file_scroll index the
   VIEW, so both reset here. */
static void file_build_view(void) {
    int n = ui.file_n;
    int *v = n > 0 ? malloc((size_t)n * sizeof *v) : NULL;
    if (n > 0 && !v) {
        free(ui.file_view);
        ui.file_view = NULL;
        ui.file_view_n = 0;
        return;
    }
    free(ui.file_view);
    ui.file_view = v;
    ui.file_view_n = 0;
    if (!n) return;
    const char *term = NULL;
    if (ui.file_focus && !ui.file_pseudo &&
        strcmp(ui.file_field, ui.file_dir) != 0) {
        const char *sep = lv_strrsep(ui.file_field);
        term = sep ? sep + 1 : ui.file_field;
        if (!*term) term = NULL;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *name = ui.file_names[i];
        if (!strcmp(name, "..") || !term || lv_stristr(name, term))
            v[k++] = i;
    }
    ui.file_view_n = k;
    ui.file_scroll = 0;
    ui.file_sel = -1;
}

static void file_list(void) {
    for (int i = 0; i < ui.file_n; i++) free(ui.file_names[i]);
    free(ui.file_names);
    free(ui.file_isdir);
    ui.file_names = NULL; ui.file_isdir = NULL; ui.file_n = 0;
#ifdef _WIN32
    if (ui.file_pseudo) {
        /* pseudo-root above the drive roots: one entry per logical drive,
           no ".." (nothing above COMPUTER). GetLogicalDrives only —
           GetVolumeInformationW blocks on empty/sleepy drives. */
        ui.file_n = drives_from_mask(GetLogicalDrives(), &ui.file_names);
        if (ui.file_n > 0) {
            ui.file_isdir = malloc((size_t)ui.file_n);
            if (ui.file_isdir) memset(ui.file_isdir, 1, (size_t)ui.file_n);
        }
    } else
#endif
    {
    DIR *d = opendir(ui.file_dir);
    if (!d) {
        file_sync_field();
        file_build_view();
        return;
    }
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
        if (!dup) continue; /* a NULL entry would crash fcmp's strcmp */
        if (S_ISDIR(st.st_mode)) {
            if (nd == cd) {
                cd = cd ? cd * 2 : 32;
                char **d2 = realloc(dirs, (size_t)cd * sizeof *dirs);
                if (!d2) { free(dup); continue; }
                dirs = d2;
            }
            dirs[nd++] = dup;
        } else if (audio_name(de->d_name) || ui.file_saving) {
            /* SAVE mode lists every file (no audio filter): the user must
               see the .m3u they aim to overwrite */
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
#ifdef _WIN32
    /* drive roots KEEP "..": it hops to the COMPUTER pseudo-root */
    int root = 0;
#else
    int root = fs_is_root(ui.file_dir);
#endif
    ui.file_n = nd + nf + (root ? 0 : 1);
    ui.file_names = malloc((size_t)(ui.file_n + 1) * sizeof *ui.file_names);
    if (!ui.file_names) {
        for (int i = 0; i < nd; i++) free(dirs[i]);
        for (int i = 0; i < nf; i++) free(files[i]);
        free(dirs); free(files);
        ui.file_n = 0;
        file_sync_field();
        file_build_view();
        return;
    }
    int k = 0;
    /* capture dir flags while listing: the draw loop used to stat every
       visible row on every repaint (brutal on network shares) */
    ui.file_isdir = ui.file_n > 0 ? malloc((size_t)ui.file_n) : NULL;
    if (!root) {
        char *up = strdup("..");
        if (up) {
            ui.file_names[k++] = up;
            if (ui.file_isdir) ui.file_isdir[k - 1] = 1;
        }
    }
    for (int i = 0; i < nd; i++) {
        ui.file_names[k++] = dirs[i];
        if (ui.file_isdir) ui.file_isdir[k - 1] = 1;
    }
    for (int i = 0; i < nf; i++) {
        ui.file_names[k++] = files[i];
        if (ui.file_isdir) ui.file_isdir[k - 1] = 0;
    }
    free(dirs); free(files);
    }
    ui.file_scroll = 0; ui.file_sel = -1;
    ui.file_focus = 0;
    file_sync_field();
    file_build_view();
}

static void raise_all(void) {
#ifdef __APPLE__
    /* Cocoa already keeps every window of the active app above other apps.
       SDL_RaiseWindow here is makeKeyAndOrderFront + activation: calling it
       per window on every FOCUS_GAINED re-triggers FOCUS_GAINED and floods
       the main thread with WindowServer round-trips (focus storm, beachball). */
    return;
#else
    /* Xfwm (and some other X11 WMs) restack AND refocus on XRaiseWindow:
       the raise fires FOCUS_LOST/FOCUS_GAINED, which calls raise_all again
       (app_focused was cleared by the FOCUS_LOST in between) — a self
       feeding focus storm that slams _NET_ACTIVE_WINDOW between our own
       windows many times a second. A short cooldown breaks the loop while
       keeping click-to-front for real activations from other apps. */
    static Uint32 last_raise_ms;
    Uint32 now = SDL_GetTicks();
    if (last_raise_ms && !SDL_TICKS_PASSED(now, last_raise_ms + 500)) return;
    last_raise_ms = now;
    SDL_RaiseWindow(ui.wmain);
    if (ui.pl_visible && ui.wpl) SDL_RaiseWindow(ui.wpl);
    if (ui.eq_visible && ui.weq) SDL_RaiseWindow(ui.weq);
    if (ui.file_visible && ui.wfile) SDL_RaiseWindow(ui.wfile);
    if (ui.opt_visible && ui.wopt) SDL_RaiseWindow(ui.wopt);
    if (ui.info_visible && ui.winfo) SDL_RaiseWindow(ui.winfo);
#ifdef LV_URL_ENABLED
    if (ui.url_visible && ui.wurl) SDL_RaiseWindow(ui.wurl);
#endif
    if (search_is_open()) SDL_RaiseWindow(search_window());
#endif
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
    else if (ui.winfo && wid == SDL_GetWindowID(ui.winfo)) { w = ui.winfo; drag = 14; }
#ifdef LV_URL_ENABLED
    else if (ui.wurl && wid == SDL_GetWindowID(ui.wurl)) { w = ui.wurl; drag = 13; }
#endif
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
        /* the close button lives inside the title strip: a press there is a
           click, not a drag. Latching a drag here left the X stuck down —
           the button-up handler skips the action while a drag is live */
        int lx = gx - wx, ly = gy - wy;
        if (lx >= ww - 18 && lx < ww - 6 && ly >= 4 && ly < 16) return;
        ui.drag = drag;
        drag_begin(w);
    }
}

static void file_close(void) {
    ui.file_visible = 0;
    ui.file_focus = 0;
    ui.file_saving = 0;
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
        char *slash = lv_strrsep(path);
        if (slash) { *slash = 0; lv_strlcpy(ui.file_dir, path, sizeof ui.file_dir); }
        else if (!getcwd(ui.file_dir, sizeof ui.file_dir))
            lv_strlcpy(ui.file_dir, "/", sizeof ui.file_dir);
    }
    ui.file_tf.buf = ui.file_field;
    ui.file_tf.cap = sizeof ui.file_field;
    file_list();
    ui.file_visible = 1;
    SDL_ShowWindow(ui.wfile);
    raise_all();
}

static void file_adddir(void) {
    Player *p = ui.pl;
    LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
    pthread_mutex_lock(&p->mtx);
    int old = p->pl->count;
    pl_add(p->pl, ui.file_dir);
    lib_fill_playlist_ix(p->pl, &ix);
    int added = p->pl->count - old;
    pthread_mutex_unlock(&p->mtx);
    lib_index_free(&ix);
    if (added > 0) pl_post(p, CMD_PLAY, old);
    file_close();
}

/* ---- SAVE mode (playlist Save-As, decision D4: .m3u EXTINF is the only
   format we write; overwrite is silent, an empty list refuses via the
   title-strip ui_msg — the browser only ever opens from a non-empty list,
   the recheck guards the race where REM emptied it meanwhile) ---- */
static void file_save_to(const char *path) {
    Player *p = ui.pl;
    /* the save reads every row: one micro-hold, an m3u dump is a fast
       sequential write with no tag I/O */
    pthread_mutex_lock(&p->mtx);
    int rc = p->pl->count == 0 ? -1 : pl_save_m3u(p->pl, path);
    pthread_mutex_unlock(&p->mtx);
    if (rc) {
        ui.file_err = 1;              /* same ~1 s red feedback as open mode */
        ui.file_err_at = SDL_GetTicks();
        return;
    }
    file_close();                     /* also clears file_saving */
}

static void file_save_enter(void) {
    if (!ui.file_field[0]) return;
    struct stat st;
    if (stat(ui.file_field, &st) == 0 && S_ISDIR(st.st_mode)) {
        lv_strlcpy(ui.file_dir, ui.file_field, sizeof ui.file_dir);
        ui.file_pseudo = 0;
        file_list();
        return;
    }
    /* a bare name lands in the folder being shown; no extension -> .m3u */
    char full[LV_MAX_PATH];
    if (lv_is_abs(ui.file_field)) lv_strlcpy(full, ui.file_field, sizeof full);
    else lv_path_join(full, sizeof full, ui.file_dir, ui.file_field);
    const char *base = lv_strrsep(full);
    base = base ? base + 1 : full;
    if (!strrchr(base, '.')) {
        size_t fl = strlen(full);
        if (fl + 5 < sizeof full) snprintf(full + fl, sizeof full - fl, ".m3u");
    }
    file_save_to(full);
}

static void file_choose(const char *name) {
#ifdef _WIN32
    if (ui.file_pseudo) {
        /* pseudo-root entries ARE full drive roots: descend and leave
           the pseudo state (no "%s/%s" join) */
        lv_strlcpy(ui.file_dir, name, sizeof ui.file_dir);
        ui.file_pseudo = 0;
        file_list();
        return;
    }
#endif
    if (!strcmp(name, "..")) {
#ifdef _WIN32
        /* ".." from any drive root hops to the COMPUTER pseudo-root */
        if (fs_is_root(ui.file_dir)) {
            ui.file_pseudo = 1;
            file_list();
            return;
        }
#endif
        char *slash = lv_strrsep(ui.file_dir);
        if (slash && slash != ui.file_dir) *slash = 0;
        else if (slash) ui.file_dir[1] = 0; /* stay at "/" */
#ifdef _WIN32
        /* "C:" is drive-relative, not the root: pin to "C:\" */
        if (ui.file_dir[1] == ':' && !ui.file_dir[2]) {
            ui.file_dir[2] = '\\';
            ui.file_dir[3] = 0;
        }
#endif
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
    if (ui.file_saving) {          /* SAVE mode: clicking a file overwrites it */
        file_save_to(full);
        return;
    }
    LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
    pthread_mutex_lock(&p->mtx);
    int added = pl_add(p->pl, full);
    lib_fill_playlist_ix(p->pl, &ix);
    int idx = p->pl->count - 1;
    pthread_mutex_unlock(&p->mtx);
    lib_index_free(&ix);
    /* a failed add (OOM) would make count-1 the PREVIOUS track: never
       post a play for a row that did not land */
    if (added == 0) pl_post(p, CMD_PLAY, idx);
    file_close();
}

/* Draw the text of an editable field inside its box: unfocused text
   scrolls left so the trailing suffix shows; focused text is anchored on
   the caret so it stays visible while editing mid-string. Draws the
   select-all highlight and the caret when focused && caret_on (the file
   field blinks its caret on the search cadence; options draw it solid).
   Shared by the options fields and the file path field. */
static void draw_field_text(SDL_Surface *s, const Rect f, const char *val,
                            TField *tf, int focused, int caret_on) {
    char disp[LV_MAX_PATH];
    const char *shown;
    int caret_w = 0;
    if (focused) {
        int len = (int)strlen(val);
        if (tf->caret < 0 || tf->caret > len) tf->caret = len;
        char pre[LV_MAX_PATH];
        lv_strlcpy(pre, val, (size_t)tf->caret + 1);
        const char *st = font_fit_tail(pre, f.w - 8);
        int off = (int)(st - pre);
        caret_w = font_w16(st);
        int room = (f.w - 8) - caret_w;
        int extra = room > 0 ? font_fit(val + tf->caret, room) : 0;
        int shown_len = (tf->caret - off) + extra;
        memcpy(disp, val + off, (size_t)shown_len);
        disp[shown_len] = 0;
        shown = disp;
    } else {
        shown = font_fit_tail(val, f.w - 8);
    }
    if (focused && tf->selall && shown[0]) {
        SDL_Rect hl = { f.x + 3, f.y + 1, (unsigned)font_w16(shown), (unsigned)(f.h - 2) };
        if (hl.x + (int)hl.w > f.x + f.w - 3)
            hl.w = f.x + f.w - 3 - hl.x;
        if (hl.w > 0) SDL_FillRect(s, &hl, col(s, 40, 60, 120));
    }
    font_puts16(s, f.x + 3, f.y + 1, shown, col(s, 220, 220, 220));
    if (focused && caret_on) {
        int cx = f.x + 3 + caret_w + 1;
        if (cx < f.x + f.w - 2) {
            SDL_Rect c = { cx, f.y + 3, 1, f.h - 6 };
            SDL_FillRect(s, &c, col(s, 255, 255, 255));
        }
    }
}

static void draw_file(void) {
    SDL_Surface *s = ui.sfile;
    if (!s) return;
    skin_window_bg(s, FILE_W, FILE_H);
    font_puts(s, 10, 6, ui.file_saving ? "SAVE PLAYLIST" : "OPEN FILE OR FOLDER",
              col(s, 0, 224, 255));
    { Rect x = close_rect(FILE_W); draw_x(s, &x, ui.pressed == FB_X); }
    FILE_LIST_RECT;
    FILE_FLD_RECT;
    FILE_ADD_RECT;
    /* path field: the editable twin of file_dir. Unfocused it always
       shows the current folder (COMPUTER in the pseudo-root); focused it
       shows what the user typed and filters the list live. */
    SDL_Rect fb = { fld.x, fld.y, fld.w, fld.h };
    SDL_FillRect(s, &fb, col(s, 0, 0, 0));
    SDL_Rect t = { fld.x, fld.y, fld.w, 1 }, b = { fld.x, fld.y + fld.h - 1, fld.w, 1 };
    SDL_Rect l = { fld.x, fld.y, 1, fld.h }, r = { fld.x + fld.w - 1, fld.y, 1, fld.h };
    int fwin_focused = ui.app_focused && ui.wfile &&
                       ui.focus_wid == SDL_GetWindowID(ui.wfile);
    Uint32 bc = (ui.file_focus && fwin_focused) ? col(s, 0, 224, 255) : col(s, 90, 90, 90);
    SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
    SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
    if (ui.file_err) {
        /* plain red, no bold: the field text returns after ~1 s */
        font_puts16(s, fld.x + 3, fld.y + 1, "path not valid", col(s, 255, 60, 60));
    } else {
        int blink = (SDL_GetTicks() / 500) % 2 == 0;
        draw_field_text(s, fld, ui.file_field, &ui.file_tf,
                        ui.file_focus && fwin_focused, blink);
    }
    SDL_Rect lb = {list.x, list.y, list.w, list.h};
    SDL_FillRect(s, &lb, col(s, 0, 0, 0));
    int lines = list.h / 17;
    if (ui.file_scroll > ui.file_view_n - lines && ui.file_view_n >= lines)
        ui.file_scroll = ui.file_view_n - lines;
    if (ui.file_scroll < 0) ui.file_scroll = 0;
    for (int i = 0; i < lines; i++) {
        int vi = ui.file_scroll + i;
        if (vi >= ui.file_view_n) break;
        char *name = ui.file_names[ui.file_view[vi]];
        int isdir = ui.file_pseudo || !strcmp(name, "..");
        if (!isdir && ui.file_isdir)
            isdir = ui.file_isdir[ui.file_view[vi]];
        if (!isdir && !ui.file_isdir) {
            /* flags not captured (OOM while listing): last-resort stat */
            char full[LV_MAX_PATH];
            snprintf(full, sizeof full, "%s/%s", ui.file_dir, name);
            struct stat st;
            isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        char line[LV_MAX_PATH];
        /* pseudo-root entries are drive roots, already ending in "\" */
        snprintf(line, sizeof line, "%s%s", name,
                 (isdir && name[strlen(name) - 1] != '\\') ? "/" : "");
        int fit = font_fit(line, list.w - 6);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = isdir ? col(s, 255, 255, 255) : col(s, 170, 170, 170);
        if (vi == ui.file_sel) c = col(s, 0, 255, 0);
        font_puts16(s, list.x + 2, list.y + 1 + i * 17, line, c);
    }
    if (ui.file_view_n > lines) {
        int th = list.h * lines / ui.file_view_n;
        if (th < 8) th = 8;
        int ty = list.y + (list.h - th) * ui.file_scroll / (ui.file_view_n - lines);
        SDL_Rect r = {list.x + list.w - 7, ty, 5, th};
        SDL_FillRect(s, &r, col(s, 110, 110, 110));
    }
    if (!ui.file_saving) {          /* ADD DIR is meaningless in SAVE mode */
        skin_button(s, &addb, ui.pressed == FB_ADDDIR, 0);
        font_puts(s, addb.x + (addb.w - font_w("ADD DIR")) / 2, addb.y + 3, "ADD DIR", col(s, 220, 220, 220));
    }
    char dir[LV_MAX_PATH];
#ifdef _WIN32
    if (ui.file_pseudo) lv_strlcpy(dir, "COMPUTER", sizeof dir);
    else
#endif
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
    FILE_FLD_RECT;
    FILE_ADD_RECT;
    int lines = list.h / 17;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(FILE_W);
        if (in_rect(&xc, x, y)) { ui.pressed = FB_X; return; }
        if (in_rect(&addb, x, y)) { if (!ui.file_saving) ui.pressed = FB_ADDDIR; return; }
        if (in_rect(&fld, x, y)) {
            /* caret to end like the opt fields; the field already mirrors
               the current folder whenever it is unfocused */
            ui.file_focus = 1;
            ui.file_err = 0;
            ui.file_tf.caret = (int)strlen(ui.file_field);
            ui.file_tf.selall = 0;
            return;
        }
        if (in_rect(&list, x, y)) {
            ui.file_focus = 0;
            int vi = ui.file_scroll + (y - list.y) / 17;
            if (vi < ui.file_view_n) {
                ui.file_sel = vi;
                file_choose(ui.file_names[ui.file_view[vi]]);
            }
            return;
        }
        if (y < 22) { ui.drag = 9; drag_begin(ui.wfile); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = close_rect(FILE_W);
        if (ui.pressed == FB_X && in_rect(&xc, x, y)) file_close();
        else if (ui.pressed == FB_ADDDIR && in_rect(&addb, x, y)) file_adddir();
        ui.pressed = B_NONE;
        if (ui.drag == 9) {
            ui_drag_bounce(ui.wfile, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 9) {
        drag_move_snap(ui.wfile);
    } else if (e->type == SDL_MOUSEWHEEL) {
        ui.file_scroll -= e->wheel.y;
        if (ui.file_scroll < 0) ui.file_scroll = 0;
        if (ui.file_scroll > ui.file_view_n - lines)
            ui.file_scroll = ui.file_view_n > lines ? ui.file_view_n - lines : 0;
    }
}

/* Enter in the path field: valid dir -> navigate; valid audio file ->
   load + play + close (same tail as file_choose); a URL -> the URL
   window's transient-play path (D3: a typed URL must not become a
   persistent row); anything else -> red "path not valid" for ~1 s with
   the typed text UNTOUCHED (the browser stays put so the entry can be
   corrected). */
#ifdef LV_URL_ENABLED
static void url_play_transient(Player *p, const char *url); /* fwd: defined with the URL window below */
#endif
static void file_field_enter(void) {
    if (ui.file_saving) { file_save_enter(); return; }
    if (!ui.file_field[0]) return;
#ifdef LV_URL_ENABLED
    if (lv_is_url(ui.file_field)) {
        url_play_transient(ui.pl, ui.file_field);
        file_close();
        return;
    }
#endif
    struct stat st;
    if (stat(ui.file_field, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            lv_strlcpy(ui.file_dir, ui.file_field, sizeof ui.file_dir);
            ui.file_pseudo = 0;
            file_list();
            return;
        }
        const char *base = lv_strrsep(ui.file_field);
        base = base ? base + 1 : ui.file_field;
        if (audio_name(base)) {
            Player *p = ui.pl;
            LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
            pthread_mutex_lock(&p->mtx);
            int added = pl_add(p->pl, ui.file_field);
            lib_fill_playlist_ix(p->pl, &ix);
            int idx = p->pl->count - 1;
            pthread_mutex_unlock(&p->mtx);
            lib_index_free(&ix);
            if (added == 0) pl_post(p, CMD_PLAY, idx); /* failed add: no wrong-track play */
            file_close();
            return;
        }
    }
    ui.file_err = 1;
    ui.file_err_at = SDL_GetTicks();
}

/* keys for the file window while the path field has the caret. Returns 1
   when consumed: while the caret is in the field EVERY key belongs to the
   field, so typing 'c' or 'o' must not fire the global playback/browser
   shortcuts in handle_key. Unfocused (caret not in the field) the keys
   fall through to the global shortcuts as before. */
static int file_key(SDL_Event *e) {
    if (!ui.file_focus) return 0;
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        file_field_enter();
        break;
    case SDLK_ESCAPE:
        /* restore the field to the current folder, clear the filter and
           any error; the window itself closes via X / FB_X only */
        ui.file_focus = 0;
        ui.file_err = 0;
        file_sync_field();
        file_build_view();
        break;
    case SDLK_LEFT:      tf_left(&ui.file_tf);  break;
    case SDLK_RIGHT:     tf_right(&ui.file_tf); break;
    case SDLK_HOME:      tf_home(&ui.file_tf);  break;
    case SDLK_END:       tf_end(&ui.file_tf);   break;
    case SDLK_BACKSPACE: tf_backspace(&ui.file_tf); break;
    case SDLK_DELETE:    tf_del(&ui.file_tf);   break;
    case SDLK_a: if (ctrl) tf_selectall(&ui.file_tf); break;
    case SDLK_c: if (ctrl) tf_copy(&ui.file_tf); break;
    case SDLK_x: if (ctrl) tf_cut(&ui.file_tf); break;
    case SDLK_v: if (ctrl) tf_paste(&ui.file_tf); break;
    default: break;
    }
    file_build_view();
    return 1;
}

static void file_text(const char *text) {
    if (!ui.file_focus) return;
    tf_insert(&ui.file_tf, text);
    file_build_view();
}

#ifdef LV_URL_ENABLED
/* ---------- URL loader window (streaming-plan UI entry) ---------- */
#define URL_W 320
#define URL_H 48
#define URL_H_NOFF 64 /* grown variant: one red notice line below the field */
#define URL_FLD_RECT Rect fld = {6, 26, URL_W - 12, 16}

/* Transient one-row playlist (plan: play = transient, the row NEVER
   persists): built off-lock, pointers swapped under the microlock —
   the daemon play-handler shape, so big-open discipline and the
   every-frame UI draw lock stay intact. Shared by the URL window's
   Enter and a URL typed into the file-browser path field. */
static void url_play_transient(Player *p, const char *url) {
    Playlist np;
    pl_init(&np);
    pl_add_file(&np, url);
    if (np.count == 0) { pl_clear(&np); return; } /* OOM: play nothing */
    pthread_mutex_lock(&p->mtx);
    pl_clear(p->pl);
    p->pl->items = np.items;
    p->pl->count = np.count;
    p->pl->cap = np.cap;
    pthread_mutex_unlock(&p->mtx);
    pl_post(p, CMD_PLAY, 0);
}

/* Park the URL window flush above main — the BOTTOM of the above-stack
   (layout: URL between main and EQ) — and push every window
   already snapped above one level up. The height is read from the live
   window (48 normal, 64 with the no-ffmpeg notice), never URL_H.
   x left-aligns to main like the flush-above snap rule (URL_W=320 rides
   5 px over main's right edge, accepted geometry). */
static void url_dock_above_main(void) {
    int mx, my, uw, uh;
    SDL_GetWindowPosition(ui.wmain, &mx, &my);
    SDL_GetWindowSize(ui.wurl, &uw, &uh);
    SDL_SetWindowPosition(ui.wurl, mx, my - uh);
    ui.snap[SNAP_URL].w = ui.wurl;
    ui.snap[SNAP_URL].dx = 0;
    ui.snap[SNAP_URL].dy = -uh;
    reflow_above(ui.wurl, my - uh);
}

static void url_close(void) {
    ui.url_visible = 0;
    ui.url_focus = 0;
    /* leaving the above-stack merges everything snapped above it down
       (closing URL while docked to EQ merges EQ to main) */
    int was_above = ui.snap[SNAP_URL].w == ui.wurl && ui.snap[SNAP_URL].dy < 0;
    ui.snap[SNAP_URL].w = NULL;
    if (ui.wurl) SDL_HideWindow(ui.wurl);
    if (was_above) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        reflow_above(NULL, my);
    }
}

static void urlwin_open(void) {
    if (!ui.wurl) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        ui.wurl = SDL_CreateWindow("levari - url", mx + MAIN_W + 4, my + 40,
                                   URL_W, URL_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!ui.wurl) return;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.wurl, icon);
            SDL_FreeSurface(icon);
        }
        ui.surl = SDL_GetWindowSurface(ui.wurl);
    }
    /* The window must tell the USER that streaming cannot work without the
       libav* backend (probe: 1 loadable, 2 missing, 0 not compiled in — all
       non-1 states are inert): a terminal line is invisible to a GUI launch.
       Re-probed per open — the answer is cached inside ff_syms, so this is
       a flag read once ffmpeg has ever loaded. */
    ui.url_ff_code = dec_ffmpeg_probe_info(ui.url_ff_ver, sizeof ui.url_ff_ver, NULL, 0);
    ui.url_ff_ok = ui.url_ff_code == 1;
    if (!ui.url_ff_ok) {
        SDL_SetWindowSize(ui.wurl, URL_W, URL_H_NOFF);
        ui.surl = SDL_GetWindowSurface(ui.wurl);
    } else if (ui.surl && ui.surl->h != URL_H) {
        SDL_SetWindowSize(ui.wurl, URL_W, URL_H);
        ui.surl = SDL_GetWindowSurface(ui.wurl);
    }
    /* LAZY prefill on open (D5): text only, inert — NOTHING probes or
       GETs here; whatever urlIndex holds (including tampered bytes) is
       re-validated by the Enter gate at play time */
    char banked[LV_MAX_PATH];
    if (lib_url_index_load(banked, sizeof banked) == 0)
        lv_strlcpy(ui.url_field, banked, sizeof ui.url_field);
    else
        ui.url_field[0] = 0;
    ui.url_tf.buf = ui.url_field;
    ui.url_tf.cap = sizeof ui.url_field;
    ui.url_tf.caret = (int)strlen(ui.url_field);
    ui.url_tf.selall = 0;
    ui.url_focus = 1;
    ui.url_err = 0;
    ui.url_visible = 1;
    /* every open docks the window above main (2026-11-07 layout):
       flush above main, below anything already snapped there, which is
       pushed up. Dock AFTER the 48/64 resize above — the live size decides
       the geometry. */
    url_dock_above_main();
    SDL_ShowWindow(ui.wurl);
    raise_all();
}

/* Enter = open + play, then close. The scheme gate + allowlist re-check
   EVERYTHING regardless of source (typed/pasted/prefilled-from-tampered-
   file): rejection is the file_err pattern — red field text ~1 s, the
   entry survives so it can be corrected, the window stays open. */
static void url_enter(void) {
    if (!ui.url_field[0]) return;
    if (!lv_is_url(ui.url_field) ||
        !lv_url_scheme_allowed(ui.url_field, LV_URL_SCHEMES)) {
        ui.url_err = 1;
        ui.url_err_at = SDL_GetTicks();
        return;
    }
    if (!ui.url_ff_ok) return; /* streaming cannot work at all: the red
                                  notice in the window is the explanation,
                                  and swapping in a doomed row would only
                                  confuse (no shape error — that is a
                                  different message) */
    url_play_transient(ui.pl, ui.url_field);
    url_close();
}

/* file-browser style: while the window is open EVERY key belongs to the
   field, so typing must never fire global playback shortcuts */
static int url_key(SDL_Event *e) {
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        url_enter();
        break;
    case SDLK_ESCAPE:
        url_close();
        break;
    case SDLK_LEFT:      tf_left(&ui.url_tf);  break;
    case SDLK_RIGHT:     tf_right(&ui.url_tf); break;
    case SDLK_HOME:      tf_home(&ui.url_tf);  break;
    case SDLK_END:       tf_end(&ui.url_tf);   break;
    case SDLK_BACKSPACE: tf_backspace(&ui.url_tf); break;
    case SDLK_DELETE:    tf_del(&ui.url_tf);   break;
    case SDLK_a: if (ctrl) tf_selectall(&ui.url_tf); break;
    case SDLK_c: if (ctrl) tf_copy(&ui.url_tf); break;
    case SDLK_x: if (ctrl) tf_cut(&ui.url_tf); break;
    case SDLK_v: if (ctrl) tf_paste(&ui.url_tf); break; /* tf_sanitize_paste:
        CRLF/quote/control hygiene exactly like the file field — the paste
        hygiene rule of the streaming plan */
    default: break;
    }
    return 1;
}

static void url_text(const char *text) {
    if (ui.url_focus) tf_insert(&ui.url_tf, text);
}

/* Red notice under the field, by probe state: absent (2) and ABI-mismatch
   (3) get different sentences — a machine with a real ffmpeg that merely
   predates this build must not be told "not found" (Ubuntu
   6.1.1 vs a 7.x-built binary, 2026-11-07). Enter is inert for both. */
static const char *url_ff_notice(int code) {
    return code == 3 ? "ffmpeg found but incompatible - see options"
                     : "ffmpeg not found - install it or set libav.dir";
}

static void draw_url(void) {
    SDL_Surface *s = ui.surl;
    if (!s) return;
    skin_window_bg(s, URL_W, s->h);   /* grows with the no-ffmpeg notice */
    font_puts(s, 10, 6, "URL", col(s, 0, 224, 255));
    { Rect x = close_rect(URL_W); draw_x(s, &x, ui.pressed == B_URLX); }
    URL_FLD_RECT;
    SDL_Rect fb = { fld.x, fld.y, fld.w, fld.h };
    SDL_FillRect(s, &fb, col(s, 0, 0, 0));
    SDL_Rect t = { fld.x, fld.y, fld.w, 1 }, b = { fld.x, fld.y + fld.h - 1, fld.w, 1 };
    SDL_Rect l = { fld.x, fld.y, 1, fld.h }, r = { fld.x + fld.w - 1, fld.y, 1, fld.h };
    int fwin_focused = ui.app_focused && ui.wurl &&
                       ui.focus_wid == SDL_GetWindowID(ui.wurl);
    Uint32 bc = (ui.url_focus && fwin_focused) ? col(s, 0, 224, 255) : col(s, 90, 90, 90);
    SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
    SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
    if (ui.url_err) {
        font_puts16(s, fld.x + 3, fld.y + 1, "url not allowed", col(s, 255, 60, 60));
    } else {
        int blink = (SDL_GetTicks() / 500) % 2 == 0;
        draw_field_text(s, fld, ui.url_field, &ui.url_tf,
                        ui.url_focus && fwin_focused, blink);
    }
    if (!ui.url_ff_ok)
        font_puts(s, fld.x, URL_H + 3, url_ff_notice(ui.url_ff_code),
                  col(s, 255, 60, 60));
}

static void handle_url_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    URL_FLD_RECT;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(URL_W);
        if (in_rect(&xc, x, y)) { ui.pressed = B_URLX; return; }
        if (in_rect(&fld, x, y)) {
            ui.url_focus = 1;
            ui.url_err = 0;
            ui.url_tf.caret = (int)strlen(ui.url_field);
            ui.url_tf.selall = 0;
            return;
        }
        if (y < 22) { ui.drag = 13; drag_begin(ui.wurl); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = close_rect(URL_W);
        if (ui.pressed == B_URLX && in_rect(&xc, x, y)) url_close();
        ui.pressed = B_NONE;
        if (ui.drag == 13) {
            ui_drag_bounce(ui.wurl, ui.drag_wxlo, ui.drag_wxhi,
                           ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 13) {
        /* live side/above snap like EQ/options: the above branch inserts
           at the bottom of the above-stack (ui_snap_next_to_main) */
        drag_move_snap(ui.wurl);
    }
}
#endif /* LV_URL_ENABLED */

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
/* BASIC/ADVANCED/ABOUT tab buttons: small tabs in the title bar, right of
   the "OPTIONS" label (ends at x=52); visible on all three pages */
static Rect opt_tab_basic_rect(void) { Rect r = { 60, 4, 38, 14 }; return r; }
static Rect opt_tab_adv_rect(void)   { Rect r = { 102, 4, 26, 14 }; return r; }
static Rect opt_tab_about_rect(void) { Rect r = { 132, 4, 38, 14 }; return r; }
/* LARGE toggle (ADV page): beside its description text — the lines at
   y=52/66 end at x=178, so the button sits right of them, vertically
   centered on the two-line block */
static Rect opt_large_rect(void)     { Rect r = { 186, 56, 48, 14 }; return r; }
/* 10FPS toggle (ADV page): same column, below the LARGE block */
static Rect opt_fps_rect(void)       { Rect r = { 186, 114, 48, 14 }; return r; }
static Rect opt_enc_rect(void)       { Rect r = { 186, 150, 72, 14 }; return r; }
/* VIZ Hz cycle button (ADV page): visualizer band refresh rate */
static Rect opt_vhz_rect(void)       { Rect r = { 186, 214, 48, 14 }; return r; }
/* ui.vis_hz -> the viz tick period; only the four table values ever
   reach here (config clamps, the cycle button walks the same table),
   and the switch default makes a torn value 10 Hz, never a busy loop */
static int vis_frame_ms(int hz) {
    switch (hz) {
    case 60: return 16;
    case 30: return 33;
    case 24: return 41;
    default: return 100;
    }
}

/* ffmpeg detection status for the ADV page (streaming substrate): the
   probe dlopens-and-closes per call, so it runs ONCE at opt_open into
   ui.opt_ff — same restart semantics as the libav.dir field beside it.
   Detected == dec_ffmpeg_probe()==1; anything else (2 = built but
   libav* missing at runtime, 0 = backend not compiled in) reads
   "unavailable" honestly. The version suffix is the release of the
   library the loader ACTUALLY resolved (probe_info) — the versioned
   soname candidates stop at 62, so a bare-name hit can be a newer
   major and the number is the only way to see which one is running. */
static const char *opt_ff_label(int ok, const char *ver, int *red) {
    static char buf[96];
    if (ok == 1) {
        *red = 0;
        if (ver && ver[0]) {
            snprintf(buf, sizeof buf, "ffmpeg: detected %s", ver);
            return buf;
        }
        return "ffmpeg: detected";
    }
    *red = 1;
    if (ok == 3) { /* found, but a different libav major than this
                      binary's headers — a mismatched layout would be
                      read as garbage fields, so ffmpeg formats stay off */
        if (ver && ver[0]) {
            snprintf(buf, sizeof buf, "ffmpeg: found %s - incompatible build", ver);
            return buf;
        }
        return "ffmpeg: found - incompatible build";
    }
    return "ffmpeg: not found - streaming unavailable";
}
/* Prefill a BLANK libav.dir field with the directory the probe resolved
   the libraries from (master 2026-11-07: detected -> show where). Field
   only — cfg keeps its value until the usual save — and a filled field
   is the user's decision, never overwritten. A pinned dir can make
   loading no worse than an empty one: ff_dlopen falls back to the keg
   paths and the bare soname search when the configured dir misses. */
static void ff_prefill_libav(int probe_ok, const char *dir) {
    if ((probe_ok == 1 || probe_ok == 3) && dir && dir[0] && !ui.opt_field[OPTF_LIBAV][0])
        lv_strlcpy(ui.opt_field[OPTF_LIBAV], dir, LV_MAX_PATH);
}
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
    char ffdir[LV_MAX_PATH];
    ui.opt_field[OPTF_LIBDIRS][0] = 0;
    for (int i = 0; i < cfg.lib_dir_n; i++) {
        if (i) strncat(ui.opt_field[OPTF_LIBDIRS], ";", LV_MAX_PATH - strlen(ui.opt_field[OPTF_LIBDIRS]) - 1);
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
    ui.opt_shuf = cfg.shuffle_remember > 0;
    ui.opt_fade = cfg.fade_stop > 0;
    ui.opt_large = cfg.allow_large > 0;
    ui.opt_f10 = cfg.ui_always10 > 0;
    ui.opt_vhz = (cfg.vis_hz == 24 || cfg.vis_hz == 30 || cfg.vis_hz == 60)
                 ? cfg.vis_hz : 10;
    ui.opt_enc = cfg.tag_legacy_enc + 1;   /* LV_ENC_AUTO(-1) -> cycle 0 */
    if (ui.opt_enc < 0 || ui.opt_enc > 4) ui.opt_enc = 0;
    ui.opt_ff = dec_ffmpeg_probe_info(ui.opt_ff_ver, sizeof ui.opt_ff_ver,
                                      ffdir, sizeof ffdir);
    ff_prefill_libav(ui.opt_ff, ffdir);
    ui.opt_tab = 0;
    ui.opt_hover = -1;
    ui.opt_focus = -1;
    ui.opt_caret = 0;
    ui.opt_selall = 0;
    ui.opt_status[0] = 0;
    ui.opt_tags = 1;
    ui.opt_visible = 1;
    SDL_ShowWindow(ui.wopt);
    raise_all();
}

static void opt_close(void) {
    /* the encoding button applies live (tag_set_legacy_default on every click)
       but only SAVE persists it: on a close that was not a save, roll the
       live default back to the config value the window opened with, so the
       button (which re-reads cfg on next open) and tag.c never disagree.
       After a save cfg already equals the clicked value, so this is a no-op. */
    tag_set_legacy_default(cfg.tag_legacy_enc);
    ui.opt_visible = 0;
    ui.opt_focus = -1;
    ui.opt_caret = 0;
    if (ui.wopt) SDL_HideWindow(ui.wopt);
    SDL_StopTextInput();
}

static void opt_save(void) {
    /* field writes under cfg lock: a concurrent daemon shuffle toggle
       must not interleave and let cfg_save persist a mixed snapshot */
    cfg_lock();
    cfg.lib_dir_n = 0;
    char copy[LV_MAX_PATH];
    lv_strlcpy(copy, ui.opt_field[OPTF_LIBDIRS], sizeof copy);
    char *tok = strtok(copy, ";");
    while (tok) {
        while (*tok == ' ') tok++;
        if (*tok && cfg.lib_dir_n < CFG_MAX_LIBDIRS)
            lv_strlcpy(cfg.lib_dirs[cfg.lib_dir_n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ";");
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
    cfg.shuffle_remember = ui.opt_shuf;
    if (ui.opt_shuf) cfg.shuffle = ui.pl->shuffle; /* capture the live state now */
    cfg.fade_stop = ui.opt_fade;
    ui.pl->fade_stop = ui.opt_fade; /* apply without waiting for a restart */
    cfg.allow_large = ui.opt_large; /* decode.c reads cfg live: no restart */
    cfg.ui_always10 = ui.opt_f10;   /* scheduler reads cfg live: no restart */
    cfg.vis_hz = ui.opt_vhz;        /* ditto: the vis tick period is live */
    cfg.tag_legacy_enc = ui.opt_enc - 1;  /* tag.c reads it live: decoding applies now,
                                             already-indexed rows keep old readings until re-scan */
    cfg_unlock();
    cfg_save();
}

typedef struct { char (*dirs)[LV_MAX_PATH]; int n; int tags; } ScanArg;

static int scan_thread(void *p) {
    ScanArg *a = p;
    int added = 0, seen = 0, skipped = 0;
    lib_scan_add(a->dirs, a->n, a->tags, &added, &seen, &skipped);
    ui.opt_scan_added = added;
    ui.opt_scan_seen = seen;
    ui.opt_scan_skipped = skipped;
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
    char *tok = strtok(copy, ";");
    while (tok && n < CFG_MAX_LIBDIRS) {
        while (*tok == ' ') tok++;
        if (*tok) lv_strlcpy(dirs[n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ";");
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

/* ABOUT page graphic: the levari logo (assets/levari.svg) as a 24x24
   pixel-art record, painted at 2x. Grid chars: '.' transparent, F rim +
   specular, G body, D groove, B label edge, K spindle hole — the colors
   are sampled from the SVG. Swap the rows for different art. */
static const char *ABOUT_ART[] = {
    ".........FFFFFF.........",
    ".......FFGGGGGGFF.......",
    ".....FFGFFFFFGGGGFF.....",
    "....FGFFGDDDDDDGGGGF....",
    "...FGFFDDGGGGGGDDGGGF...",
    "..FGGGDGGDDDDDDGGDGGGF..",
    "..FGGDGGDDGGGGDDGGDGGF..",
    ".FGGDGGDGGDDDDGGDGGDGGF.",
    ".FGGDGDGDDGGGGDDGDGDGGF.",
    "FGGDGDDGDGGBBGGDGDDGDGGF",
    "FGGDGDGDGGBGGBGGDGDGDGGF",
    "FGGDGDGDGBGKKGBGDGDGDGGF",
    "FGGDGDGDGBGKKGBGDGDGDGGF",
    "FGGDGDGDGGBGGBGGDGDGDGGF",
    "FGGDGDDGDGGBBGGDGDDGDGGF",
    ".FGGDGDGDDGGGGDDGDGDGGF.",
    ".FGGDGGDGGDDDDGGDGGDGGF.",
    "..FGGDGGDDGGGGDDGGDGGF..",
    "..FGGGDGGDDDDDDGGDGGGF..",
    "...FGGGDDGGGGGGDDGGGF...",
    "....FGGGGDDDDDDGGGGF....",
    ".....FFGGGGGGGGGGFF.....",
    ".......FFGGGGGGFF.......",
    ".........FFFFFF.........",
};
static Uint32 about_art_col(SDL_Surface *s, char ch) {
    switch (ch) {
    case 'F': return col(s, 0xf2, 0xc9, 0x63);
    case 'G': return col(s, 0xd9, 0xa4, 0x41);
    case 'D': return col(s, 0xa8, 0x7f, 0x2f);
    case 'B': return col(s, 0x7a, 0x5c, 0x20);
    default:  return col(s, 0x2e, 0x24, 0x10); /* K + anything else */
    }
}

static void draw_opt(void) {
    SDL_Surface *s = ui.sopt;
    if (!s) return;
    skin_window_bg(s, OPT_W, OPT_H);
    font_puts(s, 10, 6, "OPTIONS", col(s, 0, 224, 255));
    { Rect x = close_rect(OPT_W);
      font_puts(s, x.x - 6 - font_w(LV_ARCH " v" LV_VERSION), 6,
                LV_ARCH " v" LV_VERSION, col(s, 150, 150, 150));
      draw_x(s, &x, ui.pressed == OPTB_X); }

    /* BASIC / ADV / ABOUT tabs: small buttons in the title bar, visible on
       all pages; the active tab is lit green */
    { Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tu = opt_tab_about_rect();
      skin_button(s, &tb, ui.pressed == OPTB_TABB, !ui.opt_tab);
      font_puts(s, tb.x + (tb.w - font_w("BASIC")) / 2, tb.y + 3, "BASIC",
                ui.opt_tab ? col(s, 220, 220, 220) : col(s, 0, 255, 0));
      skin_button(s, &ta, ui.pressed == OPTB_TABA, ui.opt_tab == 1);
      font_puts(s, ta.x + (ta.w - font_w("ADV")) / 2, ta.y + 3, "ADV",
                ui.opt_tab == 1 ? col(s, 0, 255, 0) : col(s, 220, 220, 220));
      skin_button(s, &tu, ui.pressed == OPTB_TABU, ui.opt_tab == 2);
      font_puts(s, tu.x + (tu.w - font_w("ABOUT")) / 2, tu.y + 3, "ABOUT",
                ui.opt_tab == 2 ? col(s, 0, 255, 0) : col(s, 220, 220, 220)); }

    if (!ui.opt_tab)
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
        /* text longer than the box scrolls left. Unfocused: the trailing
           suffix shows. Focused: the window is anchored on the caret so it
           and its surroundings stay visible while editing mid-string. */
        TField tf = { val, sizeof val, ui.opt_caret, ui.opt_selall };
        draw_field_text(s, f, val, &tf, ui.opt_focus == i, 1);
        ui.opt_caret = tf.caret;   /* the helper clamps a stale caret */
    }

    if (!ui.opt_tab) {
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
    }

    if (!ui.opt_tab) {
    static const struct { int id; const char *l; } TOG[6] = {
        { OPTB_TPL, "UI PL" }, { OPTB_TEQ, "UI EQ" },
        { OPTB_TVIS, "UI VIS" }, { OPTB_TSRCH, "UI SRCH" },
        { OPTB_TSHUF, "SHUF" }, { OPTB_TFADE, "FADE" },
    };
    font_puts(s, 10, 193, "STARTUP", col(s, 150, 150, 150));
    for (int i = 0; i < 6; i++) {
        Rect r = opt_tog_rect(i);
        int on = i == 0 ? ui.opt_pl : i == 1 ? ui.opt_eq :
                 i == 2 ? ui.opt_vis : i == 3 ? ui.opt_srch :
                 i == 4 ? ui.opt_shuf : ui.opt_fade;
        skin_button(s, &r, ui.pressed == TOG[i].id, on);
        font_puts(s, r.x + (r.w - font_w(TOG[i].l)) / 2, r.y + 3, TOG[i].l,
                  on ? col(s, 0, 255, 0) : col(s, 220, 220, 220));
    }
    } else if (ui.opt_tab == 1) {
        /* ADVANCED page: LARGE = allow ingest of files over the 256 MB
           decode cap (OOM guard for the slurp-based decoders) */
        font_puts(s, 10, 30, "ADVANCED", col(s, 150, 150, 150));
        font_puts(s, 10, 52, "LARGE: allow ingest of files", col(s, 220, 220, 220));
        font_puts(s, 10, 66, "over 256 MB (memory risk)", col(s, 220, 220, 220));
        font_puts(s, 10, 88, "applies immediately, no restart", col(s, 120, 120, 120));
        Rect r = opt_large_rect();
        skin_button(s, &r, ui.pressed == OPTB_TLARGE, ui.opt_large);
        font_puts(s, r.x + (r.w - font_w("LARGE")) / 2, r.y + 3, "LARGE",
                  ui.opt_large ? col(s, 0, 255, 0) : col(s, 220, 220, 220));
        font_puts(s, 10, 110, "10FPS: keep repainting the", col(s, 220, 220, 220));
        font_puts(s, 10, 124, "main window at 10 fps", col(s, 220, 220, 220));
        Rect r2 = opt_fps_rect();
        skin_button(s, &r2, ui.pressed == OPTB_TFPS, ui.opt_f10);
        font_puts(s, r2.x + (r2.w - font_w("10FPS")) / 2, r2.y + 3, "10FPS",
                  ui.opt_f10 ? col(s, 0, 255, 0) : col(s, 220, 220, 220));
        /* 5-state cycle button (NOT a checkbox): label shows the current
           override for legacy 8-bit tag fields */
        font_puts(s, 10, 146, "LEGACY: tag fields", col(s, 220, 220, 220));
        font_puts(s, 10, 160, "with no declared encoding", col(s, 220, 220, 220));
        Rect r3 = opt_enc_rect();
        skin_button(s, &r3, ui.pressed == OPTB_TENC, 0);
        static const char *ENCL[5] = { "AUTO", "LATIN-1", "CP1251", "UTF-8", "SHIFT-JIS" };
        const char *el = ENCL[ui.opt_enc % 5];
        font_puts(s, r3.x + (r3.w - font_w(el)) / 2, r3.y + 3, el,
                  ui.opt_enc ? col(s, 0, 224, 255) : col(s, 220, 220, 220));
        /* ffmpeg substrate status (streaming + aac/m4a/wma): tells the
           user here whether installing/pointing libav.dir is needed at all */
        int ffred;
        const char *ffl = opt_ff_label(ui.opt_ff, ui.opt_ff_ver, &ffred);
        font_puts(s, 10, 186, ffl,
                  ffred ? col(s, 255, 60, 60) : col(s, 0, 255, 0));
        /* 4-state cycle button (NOT a checkbox): the visualizer band
           refresh rate; the label shows the current Hz. 10 is the floor
           default, so only a non-default value colors as an override. */
        font_puts(s, 10, 210, "VIZ: spectrum bars refresh", col(s, 220, 220, 220));
        font_puts(s, 10, 224, "cycle: 10 / 24 / 30 / 60 Hz", col(s, 120, 120, 120));
        Rect r4 = opt_vhz_rect();
        skin_button(s, &r4, ui.pressed == OPTB_VHZ, 0);
        char vhzl[8];
        snprintf(vhzl, sizeof vhzl, "%dHZ", ui.opt_vhz);
        font_puts(s, r4.x + (r4.w - font_w(vhzl)) / 2, r4.y + 3, vhzl,
                  ui.opt_vhz != 10 ? col(s, 0, 224, 255) : col(s, 220, 220, 220));
    } else {
        /* ABOUT page: static credits, no controls — centered title/icon/
           creator block over a left-aligned thanks list */
        font_puts_big(s, (OPT_W - font_w_big("Levari")) / 2, 28, "Levari", col(s, 0, 224, 255));
        /* pixel-art record at 2x, centered under the title */
        int ax = (OPT_W - 48) / 2, ay = 48;
        for (int y = 0; y < (int)(sizeof ABOUT_ART / sizeof *ABOUT_ART); y++)
            for (int x = 0; ABOUT_ART[y][x]; x++) {
                if (ABOUT_ART[y][x] == '.') continue;
                SDL_Rect p = { ax + x * 2, ay + y * 2, 2, 2 };
                SDL_FillRect(s, &p, about_art_col(s, ABOUT_ART[y][x]));
            }
        {
            const char *lbl = "created by ", *nm = "Purkkaviritys";
            int cw = font_w(lbl) + 1 + font_w(nm);
            int cx = (OPT_W - cw) / 2;
            font_puts(s, cx, 106, lbl, col(s, 150, 150, 150));
            font_puts(s, cx + font_w(lbl) + 1, 106, nm, col(s, 0, 255, 0));
        }
        font_puts(s, 10, 126, "with thanks to", col(s, 150, 150, 150));
        static const char *THANKS[] = {
            "SDL2",
            "libmodplug",
            "snes_spc (blargg)",
            "stb_vorbis (Sean Barrett)",
            "dr_wav, dr_flac (David Reid)",
            "minimp3 (lieff)",
            "cJSON (Dave Gamble)",
            "GNU Unifont",
            "ffmpeg",
        };
        for (int i = 0; i < (int)(sizeof THANKS / sizeof *THANKS); i++)
            font_puts(s, 10, 138 + i * 9, THANKS[i], col(s, 220, 220, 220));
    }

    Rect sv = opt_save_rect(), cl = opt_close_rect();
    skin_button(s, &sv, ui.pressed == OPTB_SAVE, 0);
    font_puts(s, sv.x + (sv.w - font_w("SAVE")) / 2, sv.y + 3, "SAVE", col(s, 220, 220, 220));
    skin_button(s, &cl, ui.pressed == OPTB_CLOSE, 0);
    font_puts(s, cl.x + (cl.w - font_w("CLOSE")) / 2, cl.y + 3, "CLOSE", col(s, 220, 220, 220));

    if (!ui.opt_tab) {
    Rect cr = opt_clear_rect();
    skin_button(s, &cr, ui.pressed == OPTB_CLEAR && !ui.opt_scanning, 0);
    font_puts(s, cr.x + (cr.w - font_w("CLEAR")) / 2, cr.y + 3, "CLEAR", col(s, 220, 220, 220));
    }

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
        font_puts(s, tb.x + 6, tb.y + 17, "separate multiple dirs with ';'", col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_TENC) {
        Rect r3 = opt_enc_rect();
        SDL_Rect tb = { r3.x, r3.y + r3.h + 4, 230, 42 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "encoding for fields with no marker", col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "AUTO = decide from block evidence", col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 29, "index needs CLEAR + SCAN to re-read", col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_VHZ) {
        Rect rv = opt_vhz_rect();
        SDL_Rect tb = { rv.x, rv.y + rv.h + 4, 214, 30 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "visualizer refresh rate (Hz)", col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "higher = smoother, more CPU", col(s, 220, 220, 220));
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
        Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tu = opt_tab_about_rect();
        if (in_rect(&tb, x, y)) { ui.pressed = OPTB_TABB; return; }
        if (in_rect(&ta, x, y)) { ui.pressed = OPTB_TABA; return; }
        if (in_rect(&tu, x, y)) { ui.pressed = OPTB_TABU; return; }
        if (ui.opt_tab) {
            if (ui.opt_tab == 1) {
                Rect tg = opt_large_rect();
                if (in_rect(&tg, x, y)) { ui.pressed = OPTB_TLARGE; return; }
                Rect tf = opt_fps_rect();
                if (in_rect(&tf, x, y)) { ui.pressed = OPTB_TFPS; return; }
                Rect te = opt_enc_rect();
                if (in_rect(&te, x, y)) { ui.pressed = OPTB_TENC; return; }
                Rect tv = opt_vhz_rect();
                if (in_rect(&tv, x, y)) { ui.pressed = OPTB_VHZ; return; }
            }
            if (y < 22) { ui.drag = 11; drag_begin(ui.wopt); ui.opt_focus = -1; }
            return;
        }
        Rect sc = opt_scan_rect();
        if (in_rect(&sc, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_SCAN; return; }
        Rect cr = opt_clear_rect();
        if (in_rect(&cr, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_CLEAR; return; }
        Rect ck = opt_chk_rect();
        if (in_rect(&ck, x, y)) { ui.pressed = OPTB_CHK; return; }
        for (int i = 0; i < 6; i++) {
            Rect tg = opt_tog_rect(i);
            if (in_rect(&tg, x, y)) { ui.pressed = OPTB_TPL + i; return; }
        }
        for (int i = 0; i < OPTF_N; i++) {
            Rect f = opt_field_rect(i);
            if (in_rect(&f, x, y)) {
                ui.opt_focus = i;
                ui.opt_caret = (int)strlen(ui.opt_field[i]);
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
            Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tg0 = opt_large_rect();
            Rect tf0 = opt_fps_rect(), te0 = opt_enc_rect(), tu = opt_tab_about_rect();
            Rect tv0 = opt_vhz_rect();
            if (ui.pressed == OPTB_X && in_rect(&xc, x, y)) opt_close();
            else if (ui.pressed == OPTB_TABB && in_rect(&tb, x, y)) { ui.opt_tab = 0; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TABA && in_rect(&ta, x, y)) { ui.opt_tab = 1; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TABU && in_rect(&tu, x, y)) { ui.opt_tab = 2; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TLARGE && in_rect(&tg0, x, y)) ui.opt_large = !ui.opt_large;
            else if (ui.pressed == OPTB_TFPS && in_rect(&tf0, x, y)) ui.opt_f10 = !ui.opt_f10;
            else if (ui.pressed == OPTB_TENC && in_rect(&te0, x, y)) {
                ui.opt_enc = (ui.opt_enc + 1) % 5;
                tag_set_legacy_default(ui.opt_enc - 1); /* live effect; SAVE persists */
            }
            else if (ui.pressed == OPTB_VHZ && in_rect(&tv0, x, y)) {
                /* 10 -> 24 -> 30 -> 60 -> 10 (the config table; a value
                   outside it — none is reachable — restarts at the default) */
                static const int VHZT[4] = { 10, 24, 30, 60 };
                int i = 0;
                while (i < 4 && VHZT[i] != ui.opt_vhz) i++;
                ui.opt_vhz = VHZT[i == 4 ? 0 : (i + 1) & 3];
                /* the scheduler reads cfg at tick time; effect lands on
                   SAVE with the rest (no hidden half-applied state) */
            }
            else if (ui.pressed == OPTB_SAVE && in_rect(&sv, x, y)) opt_save();
            else if (ui.pressed == OPTB_CLOSE && in_rect(&cl, x, y)) opt_close();
            else if (ui.pressed >= OPTB_TPL && ui.pressed <= OPTB_TFADE) {
                int i = ui.pressed - OPTB_TPL;
                Rect tg = opt_tog_rect(i);
                if (in_rect(&tg, x, y)) {
                    if (i == 0) ui.opt_pl = !ui.opt_pl;
                    else if (i == 1) ui.opt_eq = !ui.opt_eq;
                    else if (i == 2) ui.opt_vis = !ui.opt_vis;
                    else if (i == 3) ui.opt_srch = !ui.opt_srch;
                    else if (i == 4) ui.opt_shuf = !ui.opt_shuf;
                    else ui.opt_fade = !ui.opt_fade;
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
        if (ui.drag == 11) {
            ui_drag_bounce(ui.wopt, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 11) {
        drag_move_snap(ui.wopt);
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.opt_tab == 1) {
            Rect te = opt_enc_rect(), tv = opt_vhz_rect();
            ui.opt_hover = in_rect(&te, x, y) ? OPTB_TENC
                           : in_rect(&tv, x, y) ? OPTB_VHZ : -1;
        } else if (!ui.opt_tab) {
            Rect f = opt_field_rect(OPTF_LIBDIRS);
            ui.opt_hover = (x >= 8 && x < f.x + f.w && y >= f.y - 2 && y <= f.y + f.h + 2)
                           ? OPTF_LIBDIRS : -1;
        } else {
            ui.opt_hover = -1;
        }
    }
}

/* options field editing rides the shared TField ops (textedit.c); the
   ESC close and the per-field focus gate stay here, as do all OPTB_*
   side effects — only buffer editing moved */
static void opt_key(SDL_Event *e) {
    if (e->key.keysym.sym == SDLK_ESCAPE) { opt_close(); return; }
    if (ui.opt_focus < 0) return;
    TField tf = { ui.opt_field[ui.opt_focus], LV_MAX_PATH,
                  ui.opt_caret, ui.opt_selall };
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_LEFT:      tf_left(&tf);  break;
    case SDLK_RIGHT:     tf_right(&tf); break;
    case SDLK_HOME:      tf_home(&tf);  break;
    case SDLK_END:       tf_end(&tf);   break;
    case SDLK_a: if (ctrl) tf_selectall(&tf); else return; break;
    case SDLK_c: if (ctrl) tf_copy(&tf); else return; break;
    case SDLK_x: if (ctrl) tf_cut(&tf); else return; break;
    case SDLK_v: if (ctrl) tf_paste(&tf); else return; break;
    case SDLK_BACKSPACE: tf_backspace(&tf); break;
    case SDLK_DELETE:    tf_del(&tf);   break;
    default: return;
    }
    ui.opt_caret = tf.caret;
    ui.opt_selall = tf.selall;
}

static void opt_text(const char *text) {
    if (ui.opt_focus < 0) return;
    TField tf = { ui.opt_field[ui.opt_focus], LV_MAX_PATH,
                  ui.opt_caret, ui.opt_selall };
    tf_insert(&tf, text);
    ui.opt_caret = tf.caret;
    ui.opt_selall = tf.selall;
}

/* when the options window's SHUF startup toggle is on, keep the shuffle
   state in the config file so it survives sessions */
static void shuffle_persist(void) {
    if (cfg.shuffle_remember > 0) cfg_set_shuffle(ui.pl->shuffle);
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
    case B_SHUF: p->shuffle = !p->shuffle; shuffle_persist(); break;
    case B_REPEAT: p->repeat = p->repeat == 1 ? 0 : 1; break;
    case B_RPT1: p->repeat = p->repeat == 2 ? 0 : 2; break;
#ifdef LV_URL_ENABLED
    case B_URL:
        if (ui.url_visible) url_close();
        else urlwin_open();
        break;
#endif
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
    case B_VIS: ui.vis_on = !ui.vis_on; p->viz_feed = ui.vis_on; break;
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
    case PB_SHUF: p->shuffle = !p->shuffle; shuffle_persist(); break;
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
            pl_sel_reset();
        } else if (ui.pl_multi_n > 0) {
            /* multi-removal: walk the sorted set high-to-low so removals
               never shift a still-pending index; p->cur drops by the number
               of removed rows below it (CMD_STOP keeps p->cur, exactly as
               the single-row path relied on: deleting the current row lets
               cur fall onto its successor) */
            if (p->cur >= 0 && pl_sel_has(p->cur)) pl_post(p, CMD_STOP, 0);
            int below = 0;
            for (int i = 0; i < ui.pl_multi_n && p->cur >= 0; i++)
                if (ui.pl_multi[i] < p->cur) below++; else break;
            pthread_mutex_lock(&p->mtx);
            for (int i = ui.pl_multi_n - 1; i >= 0; i--)
                pl_remove(p->pl, ui.pl_multi[i]);  /* stale-high clamped inside */
            if (p->cur >= 0) p->cur -= below;
            pthread_mutex_unlock(&p->mtx);
            ui.pl_sel = -1;
            pl_sel_reset();
        }
        break;
    case PB_CLOSE:
        ui.pl_visible = 0;
        SDL_HideWindow(ui.wpl);
        break;
    case PB_LIB: {
        LibIndex ix = lib_index_load(); /* whole-index read off the player lock */
        pthread_mutex_lock(&p->mtx);
        for (int i = 0; i < ix.n; i++) {
            const LibRow *r = &ix.rows[i];
            /* index rows are always files: no stat. a failed add (OOM)
               must NOT fall through: items[count-1] would be the PREVIOUS
               track and the row's tags would overwrite its fields */
            if (pl_add_file(p->pl, r->path) != 0) continue;
            Track *t = &p->pl->items[p->pl->count - 1];
            lv_strlcpy(t->tags.artist, r->artist, sizeof t->tags.artist);
            lv_strlcpy(t->tags.title, r->title, sizeof t->tags.title);
            lv_strlcpy(t->tags.album, r->album, sizeof t->tags.album);
            t->duration = r->dur;
            t->tags.track = r->track;
            t->notag = r->notag;
        }
        pthread_mutex_unlock(&p->mtx);
        lib_index_free(&ix);
        break;
    }
    case PB_SCAN: {
        /* Re-probe every SELECTED row (tags + duration) and update each
           index row in place — the antidote to the "stale by design" rule.
           Multi-selection is bounded by what the user clicked; the ctrl-a
           select-all state stays a deliberate no-op (decision D: a whole-
           playlist probe on the UI thread can stall on network shares).
           CUE virtual rows skip (their tags live in the parent .cue); never
           during a background scan (the scan thread appends to the very
           index being rewritten). Decision F: a failure leaves that row's
           index entry untouched, reports in the "cannot decode" house style
           to stdout, and shows red on the title strip for 10 s (preemptable
           by a real title change); one summary message covers the batch. */
        ui.msg[0] = 0;                      /* a fresh attempt clears the old error */
        if (ui.pl_selall || ui.pl_multi_n == 0) break;
        if (ui.opt_scanning) { ui_msg("  scan running, wait"); break; }
        int fails = 0, refused = 0;
        for (int k = 0; k < ui.pl_multi_n; k++) {
            int sel = ui.pl_multi[k];
            char path[LV_MAX_PATH];
            path[0] = 0;
            pthread_mutex_lock(&p->mtx);
            if (sel >= 0 && sel < p->pl->count && p->pl->items[sel].end_sec == 0)
                lv_strlcpy(path, p->pl->items[sel].path, sizeof path);
            pthread_mutex_unlock(&p->mtx);
            if (!path[0]) continue;         /* stale index / virtual row */
            if (lv_is_url(path)) {          /* the index must NEVER see a
                URL row (plan guard; hygiene even in LV_STREAMING=OFF
                builds — transient rows are playable but rescannable-
                index material they are not) */
                printf("levari: scan skips URL row: %s\n", path);
                continue;
            }
            Tags nt;
            int ndur = 0, nnotag = 0;
            int rrc = lib_index_rescan(path, &nt, &ndur, &nnotag);
            if (rrc == -3) {          /* pre-v2 index, un-migratable: the
                batch refuses, the file is untouched — never silently
                half-reset the cache (only a full scan rewrites) */
                fprintf(stderr, "levari: rescan refused, index needs a full"
                                " scan: %s\n", path);
                refused = 1;
                continue;
            }
            if (rrc != 0) {
                fprintf(stderr, "levari: cannot rescan %s\n", path);
                fails = 1;
                continue;
            }
            /* apply the fresh values to the Track — micro-lock hold, path-guarded
               write-back exactly like pl_scan_tags (the row may have moved) */
            pthread_mutex_lock(&p->mtx);
            if (sel < p->pl->count && !strcmp(p->pl->items[sel].path, path)) {
                p->pl->items[sel].tags = nt;
                if (ndur) p->pl->items[sel].duration = ndur;
                p->pl->items[sel].notag = nnotag;
            }
            pthread_mutex_unlock(&p->mtx);
        }
        if (refused) ui_msg("  index: run full scan");
        else if (fails) ui_msg("  cannot rescan");
        break;
    }
    case B_OPEN: case PB_ADD: file_open(); break;
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
        /* a button press cancels any latched drag (focus_click_drag can
           latch one before the click-through press arrives): without this
           the up handler's drag==0 gate swallows the click and leaves the
           button painted down */
        Rect xc = close_rect(MAIN_W);
        if (in_rect(&xc, x, y)) { ui.pressed = B_XMAIN; ui.drag = 0; return; }
        for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++)
            if (in_rect(MAIN_BTNS[i].r, x, y)) { ui.pressed = MAIN_BTNS[i].id; ui.drag = 0; return; }
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
        if (y < 22) {
            ui.drag = 3; drag_begin(ui.wmain);
            /* follow bookkeeping is drag-scoped too: drag_begin
               re-baselines every carried window */
        }
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
                follow_move(ui.wpl, &ui.follow_pl, wx, wy + MAIN_H);
            for (int i = 0; i < SNAP_NSLOT; i++)
                if (ui.snap[i].w)
                    follow_move(ui.snap[i].w, &ui.follow_snap[i], wx + ui.snap[i].dx, wy + ui.snap[i].dy);
        }
    }
    if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.drag == 3) {
            /* wall-facing carried window -> main jumps back flush to the
               dock instead of the wall kick (drag_dock_restore) */
            if (!drag_dock_restore())
                ui_drag_bounce(ui.wmain, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
        }
        if (ui.drag == 1 || ui.drag == 2 || ui.drag == 3 || ui.drag == 10) ui.drag = 0;
    }
}

/* OS file drop (SDL_DROPBEGIN / SDL_DROPFILE* / SDL_DROPCOMPLETE): files
   dragged from the desktop onto the main or playlist window are buffered
   and applied as ONE batch on DROPCOMPLETE — a single off-lock index load
   plus one lock hold, the file-browser add pattern. Files append in
   arrival order; folders recurse through pl_add's dir probe; playlist
   files route through their loader by pl_ext_kind (relative entries in a
   dropped .m3u/.pls resolve against the list's own dir). The brackets are
   emitted by every backend that does drops, but a DROPFILE arriving with
   no open session opens one implicitly so a backend omitting the brackets
   still lands its files. */
static char **drop_paths;
static int drop_n, drop_cap;
static int drop_open;

static void drop_add(const char *path) {
    if (drop_n == drop_cap) {
        int cap = drop_cap ? drop_cap * 2 : 16;
        char **n2 = realloc(drop_paths, (size_t)cap * sizeof *drop_paths);
        if (!n2) return;
        drop_paths = n2; drop_cap = cap;
    }
    char *dup = strdup(path);
    if (!dup) return;
    drop_paths[drop_n++] = dup;
}

static void drop_flush(void) {
    Player *p = ui.pl;
    int n = drop_n;
    drop_n = 0;
    if (!n) return;
    LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
    pthread_mutex_lock(&p->mtx);
    int old = p->pl->count;
    for (int i = 0; i < n; i++) {
        PlKind kind = pl_ext_kind(drop_paths[i]);
        if (kind == PL_M3U) pl_load_m3u(p->pl, drop_paths[i]);
        else if (kind == PL_PLS) pl_load_pls(p->pl, drop_paths[i]);
        else if (kind == PL_CUE) pl_load_cue(p->pl, drop_paths[i]);
        else pl_add(p->pl, drop_paths[i]);
    }
    lib_fill_playlist_ix(p->pl, &ix);
    int added = p->pl->count - old;
    pthread_mutex_unlock(&p->mtx);
    lib_index_free(&ix);
    for (int i = 0; i < n; i++) free(drop_paths[i]);
    /* play the first NEW row (file_adddir's tail); a batch of nothing new
       must not stop or restart the running track */
    if (added > 0) pl_post(p, CMD_PLAY, old);
}

/* ---- playlist edit actions (menu items and shortcuts converge here) ---- */

/* scratch: the materialized, group-expanded selection for the edit
   actions (grown on demand; never NULL-freed to shrink, house style) */
static int *pl_edit_sel;
static int pl_edit_sel_cap;

/* the selection as the edit actions see it: ctrl-a collapses to rows,
   stale indices clamp, then rows grow to whole CUE groups. Returns the
   expanded row count (0 = nothing selected), -1 on OOM. Needs p->mtx. */
static int pl_sel_material(Playlist *pl) {
    int n = ui.pl_selall ? pl->count : ui.pl_multi_n;
    if (n <= 0) return 0;
    if (pl_edit_sel_cap < pl->count * 2) {
        int *g = realloc(pl_edit_sel, (size_t)pl->count * 2 * sizeof *g);
        if (!g) return -1;
        pl_edit_sel = g;
        pl_edit_sel_cap = pl->count * 2;
    }
    if (ui.pl_selall) {
        for (int i = 0; i < pl->count; i++) pl_edit_sel[i] = i;
        n = pl->count;
    } else {
        int k = 0;
        for (int i = 0; i < ui.pl_multi_n; i++)
            if (ui.pl_multi[i] >= 0 && ui.pl_multi[i] < pl->count)
                pl_edit_sel[k++] = ui.pl_multi[i];
        n = k;
        if (!n) return 0;
    }
    int en = pl_sel_expand(pl, pl_edit_sel, n, pl_edit_sel + pl->count);
    if (en < 0) return -1;
    memcpy(pl_edit_sel, pl_edit_sel + pl->count, (size_t)en * sizeof *pl_edit_sel);
    return en;
}

/* membership against the LIVE selection (pre-expansion): conservative,
   same lookup the PB_REM stop-check uses — a row kept only because its
   group grew around the selection may see an over-eager stop, which is
   always safe (CMD_STOP never touches p->cur) */
static int pl_sel_live_has(int idx) {
    if (idx < 0) return 0;
    if (ui.pl_selall) return 1;
    return pl_sel_has(idx);
}

static void pl_edit_clear_sel(void) {
    ui.pl_sel = -1;
    ui.pl_selall = 0;
    pl_sel_reset();
}

/* row-drag (12) release: move the live group-expanded selection to the
   gap. pl_drop remaps pl_edit_sel onto the landed rows, and the set
   rides along (the PM_MUP/PM_MDOWN pattern) so repeated drags compose. */
static void pl_drop_apply(void) {
    Player *p = ui.pl;
    pthread_mutex_lock(&p->mtx);
    int n = pl_sel_material(p->pl);
    if (n > 0) {
        int cur = p->cur;
        pl_drop(p->pl, pl_edit_sel, n, ui.pl_drop, &cur);
        p->cur = cur;
        if (pl_multi_grow(n) == 0) {
            memcpy(ui.pl_multi, pl_edit_sel, (size_t)n * sizeof *ui.pl_multi);
            ui.pl_multi_n = n;
            ui.pl_selall = 0;
            ui.pl_sel = ui.pl_multi[n - 1];
        }
    }
    pthread_mutex_unlock(&p->mtx);
}

static void pl_saveas_open(void) {
    Player *p = ui.pl;
    pthread_mutex_lock(&p->mtx);
    int n = p->pl->count;
    pthread_mutex_unlock(&p->mtx);
    if (!n) { ui_msg("  playlist empty"); return; }   /* D7: empty refuses */
    ui.file_saving = 1;
    file_open();
}

static void info_open(void);

static void pl_menu_action(int act) {
    Player *p = ui.pl;
    switch (act) {
    case PM_INFO:
        info_open();
        break;
    case PM_SAVEAS:
        pl_saveas_open();
        break;
    case PM_CLEAR:
        /* select-all is already the proven destructive path in PB_REM
           (stop + pl_clear + cur=-1 + sel reset): reuse it verbatim */
        ui.pl_selall = 1;
        action(PB_REM);
        break;
    case PM_STITLE: case PM_SARTIST: case PM_SALBUM: case PM_STRACK: case PM_SFILE:
    case PM_REVERSE: case PM_SHUFFLE: {
        static const int KEY[5] = { PL_SORT_TITLE, PL_SORT_ARTIST, PL_SORT_ALBUM,
                                    PL_SORT_TRACK, PL_SORT_FILE };
        pthread_mutex_lock(&p->mtx);
        int cur = p->cur;
        if (act == PM_REVERSE) {
            srand((unsigned)SDL_GetTicks());   /* re-randomize every press (D6) */
            pl_reverse(p->pl, &cur);
        } else if (act == PM_SHUFFLE) {
            srand((unsigned)SDL_GetTicks());
            pl_shuffle(p->pl, &cur);
        } else
            pl_sort(p->pl, KEY[act - PM_STITLE], &cur);
        p->cur = cur;                          /* permutation remapped: same MUSIC */
        pthread_mutex_unlock(&p->mtx);
        pl_edit_clear_sel();                   /* row indices lost their meaning */
        break;
    }
    case PM_COPY:
        pthread_mutex_lock(&p->mtx);
        {
            int n = pl_sel_material(p->pl);
            if (n > 0 && pl_clip_copy(p->pl, pl_edit_sel, n) != 0)
                ui_msg("  out of memory");     /* refused whole, old clip intact */
        }
        pthread_mutex_unlock(&p->mtx);
        break;
    case PM_CUT:
        if (pl_sel_live_has(p->cur)) pl_post(p, CMD_STOP, 0);  /* multi-REM pattern */
        pthread_mutex_lock(&p->mtx);
        {
            int n = pl_sel_material(p->pl);
            int cur = p->cur;
            if (n > 0) {
                int before = pl_clip_count();
                pl_clip_cut(p->pl, pl_edit_sel, n, &cur);
                if (pl_clip_count() == before && n > 0) ui_msg("  out of memory");
                p->cur = cur;
            }
        }
        pthread_mutex_unlock(&p->mtx);
        pl_edit_clear_sel();
        break;
    case PM_PASTE: {
        pthread_mutex_lock(&p->mtx);
        /* paste below the single selected row when there is one, else append */
        int at = (!ui.pl_selall && ui.pl_multi_n == 1) ? ui.pl_multi[0] + 1
                                                       : p->pl->count;
        int cur = p->cur;
        int rc = pl_clip_paste(p->pl, at, &cur);
        p->cur = cur;
        pthread_mutex_unlock(&p->mtx);
        if (rc) ui_msg("  out of memory");
        pl_edit_clear_sel();
        break;
    }
    case PM_CROP:
        /* an empty selection is a total no-op — it must not stop the track */
        if ((ui.pl_selall || ui.pl_multi_n > 0) &&
            p->cur >= 0 && !pl_sel_live_has(p->cur))
            pl_post(p, CMD_STOP, 0);
        pthread_mutex_lock(&p->mtx);
        {
            int n = pl_sel_material(p->pl);
            int cur = p->cur;
            if (n > 0) {
                pl_crop(p->pl, pl_edit_sel, n, &cur);
                p->cur = cur;
            }
        }
        pthread_mutex_unlock(&p->mtx);
        pl_edit_clear_sel();
        break;
    case PM_MUP: case PM_MDOWN:
        pthread_mutex_lock(&p->mtx);
        {
            int n = pl_sel_material(p->pl);
            if (n > 0) {
                int cur = p->cur;
                pl_move(p->pl, pl_edit_sel, n, act == PM_MDOWN ? 1 : -1, &cur);
                p->cur = cur;
                /* the selection rides along with the moved rows */
                if (pl_multi_grow(n) == 0) {
                    memcpy(ui.pl_multi, pl_edit_sel, (size_t)n * sizeof *ui.pl_multi);
                    ui.pl_multi_n = n;
                    ui.pl_selall = 0;
                    ui.pl_sel = ui.pl_multi[n - 1];
                }
            }
        }
        pthread_mutex_unlock(&p->mtx);
        break;
    }
}

static void handle_pl_mouse(SDL_Event *e) {
    Player *p = ui.pl;
    Playlist *pl = p->pl;
    int x = e->motion.x, y = e->motion.y;
    Rect ptext, pscroll;
    pl_layout(&ptext, &pscroll);
    int lines = ptext.h / 17;
    if (e->type == SDL_MOUSEBUTTONDOWN) {
        /* any press ends the previous press cycle: a lost release (the
           event loop clears a latched drag on a button-less motion) must
           never leave a deferred collapse/play riding on the next UP */
        ui.pl_dbl = -1;
        ui.pl_collapse = -1;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_RIGHT) {
        ui.pl_menu = 1;
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT && ui.pl_menu) {
        /* the menu eats one click: outside closes, inside fires and closes */
        ui.pl_menu = 0;
        for (int i = 0; i < PM_N; i++) {
            Rect r = pl_menu_item_rect(i);
            if (in_rect(&r, x, y)) { pl_menu_action(PL_MENU[i].act); break; }
        }
        return;
    }
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
                /* SDL2 mouse-button events carry no modifier mask — the
                   physical keyboard state is the only truth at click time */
                const Uint8 *ks = SDL_GetKeyboardState(NULL);
                int ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
                int shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
                if (ctrl) {
                    if (ui.pl_selall) {
                        pl_sel_materialize(pl->count);
                        ui.pl_selall = 0;
                    }
                    if (pl_sel_has(idx)) pl_sel_del(idx);
                    else pl_sel_add(idx);
                    ui.pl_sel = idx;   /* anchor follows the ctrl click */
                } else if (shift && ui.pl_sel >= 0) {
                    ui.pl_selall = 0;
                    pl_sel_reset();
                    int a = ui.pl_sel, b = idx;   /* anchor keeps its place,
                                                     further shift-clicks extend */
                    if (a > b) { int t = a; a = b; b = t; }
                    if (b >= pl->count) b = pl->count - 1;  /* stale anchor past end */
                    if (a <= b && pl_multi_grow(b - a + 1) == 0) {
                        for (int i = a; i <= b; i++) ui.pl_multi[i - a] = i;
                        ui.pl_multi_n = b - a + 1;
                    } else {
                        pl_sel_add(idx);
                    }
                } else {
                    /* plain press. A press inside a multi-row selection
                       DEFERS the collapse-to-one to release: a drag that
                       starts inside the set carries the whole set (pl_drop
                       group-expands), while a released click collapses to
                       exactly the single row it always selected. Outside
                       the set (or over ctrl-a) the collapse stays
                       immediate. The row drag arms on every plain press;
                       the drop only exists once motion past the threshold
                       sets a gap, and the double-click play defers to
                       release so a drag between the clicks eats it. */
                    int keep = ui.pl_multi_n > 1 && !ui.pl_selall &&
                               pl_sel_has(idx);
                    ui.pl_collapse = keep ? idx : -1;
                    if (!keep) {
                        ui.pl_sel = idx;
                        ui.pl_selall = 0;
                        pl_sel_reset();
                        pl_sel_add(idx);
                    }
                    ui.drag = 12;
                    ui.pl_drop = -1;
                    ui.pl_press_y = y;
                    /* a modifier double-click still never plays (toggle
                       semantics), and this branch only runs without them */
                    ui.pl_dbl = (e->button.clicks == 2) ? idx : -1;
                }
            }
            return;
        }
        if (in_rect(&pscroll, x, y)) { ui.drag = 5; return; }
        if (y < 22) { ui.drag = 4; drag_begin(ui.wpl); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        /* row-drag (12) first: applying the drop, and only then the
           deferred click completions. A press that became a drag (a gap
           was set past the arm threshold) neither collapses an
           in-selection set nor plays the deferred double-click — the
           plan's "double-click must not fire after a drag". */
        int rowdrop = 0;
        if (ui.drag == 12) {
            rowdrop = ui.pl_drop >= 0;
            if (rowdrop) pl_drop_apply();
            ui.drag = 0;
        }
        if (ui.pl_collapse >= 0) {
            int idx = ui.pl_collapse;
            ui.pl_collapse = -1;
            if (!rowdrop && idx < pl->count) {
                ui.pl_sel = idx;
                ui.pl_selall = 0;
                pl_sel_reset();
                pl_sel_add(idx);
            }
        }
        if (ui.pl_dbl >= 0) {
            int idx = ui.pl_dbl;
            ui.pl_dbl = -1;
            if (!rowdrop && idx < pl->count) pl_post(p, CMD_PLAY, idx);
        }
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
            } else
                ui_drag_bounce(ui.wpl, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
        }
        if (ui.drag == 4 || ui.drag == 5 || ui.drag == 8) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.drag == 4) {
            drag_move_pl();
        } else if (ui.drag == 12) {
            /* armed below an arm threshold of 4px so click jitter never
               turns into a drop (the deferred double-click must survive
               a two-pixel wobble). Half-row gap bias: the cursor in the
               top half of a row drops above it, the bottom half below. */
            if (ui.pl_drop >= 0 || abs(y - ui.pl_press_y) >= 4) {
                /* edge auto-scroll: riding the list bounds keeps long
                   drags reachable from one press (motion-event driven) */
                if (y < ptext.y + 8 && ui.pl_scroll > 0)
                    ui.pl_scroll--;
                else if (pl->count > lines && y >= ptext.y + lines * 17 - 8 &&
                         ui.pl_scroll < pl->count - lines)
                    ui.pl_scroll++;
                int gap = ui.pl_scroll + (y - ptext.y + 8) / 17;
                if (gap < 0) gap = 0;
                if (gap > pl->count) gap = pl->count;
                ui.pl_drop = gap;
            }
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

/* plain Up/Down walks the playlist cursor by one row (volume moved to
   +/-): the selection collapses to the single landed row, the origin is
   the selection's edge in the travel direction, falling back to the
   anchor, then to the playing track. The view follows with the same
   just-enough-scroll rule draw_pl applies to the playing track. */
static void pl_cursor(int dir) {
    Player *p = ui.pl;
    if (p->pl->count == 0) return;
    int base = ui.pl_multi_n > 0
                   ? (dir > 0 ? ui.pl_multi[ui.pl_multi_n - 1] : ui.pl_multi[0])
                   : (ui.pl_sel >= 0 ? ui.pl_sel : p->cur);
    int idx = base + dir;              /* base -1 (nothing anywhere):
                                          Down lands on 0, Up clamps to 0 */
    if (idx < 0) idx = 0;
    if (idx > p->pl->count - 1) idx = p->pl->count - 1;
    ui.pl_sel = idx;
    ui.pl_selall = 0;
    pl_sel_reset();
    pl_sel_add(idx);
    Rect ptext, pscroll;
    pl_layout(&ptext, &pscroll);
    int lines = ptext.h / 17;
    if (idx < ui.pl_scroll) ui.pl_scroll = idx;
    else if (idx >= ui.pl_scroll + lines) ui.pl_scroll = idx - lines + 1;
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
        pl_sel_reset();
        return 1;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (!ui.pl_selall && ui.pl_sel < 0 && ui.pl_multi_n == 0) return 0;
        action(PB_REM);
        return 1;
    case SDLK_ESCAPE:
        /* while the edit menu is open, Esc closes it instead of quitting */
        if (ui.pl_menu) { ui.pl_menu = 0; return 1; }
        return 0;
    case SDLK_c:
        if (ctrl && (ui.pl_selall || ui.pl_multi_n)) { pl_menu_action(PM_COPY); return 1; }
        return 0;
    case SDLK_x:
        if (ctrl && (ui.pl_selall || ui.pl_multi_n)) { pl_menu_action(PM_CUT); return 1; }
        return 0;
    case SDLK_v:
        if (ctrl && pl_clip_count() > 0) { pl_menu_action(PM_PASTE); return 1; }
        return 0;
    case SDLK_k:
        if (ctrl) { pl_menu_action(PM_SAVEAS); return 1; }
        return 0;
    case SDLK_UP:
    case SDLK_DOWN:
        /* Alt+Up/Down move the (group-expanded) selection; the plain
           arrows walk the playlist cursor (they used to be volume,
           +/- took over — handle_key carries the main-window path) */
        if ((e->key.keysym.mod & KMOD_ALT) && (ui.pl_selall || ui.pl_multi_n)) {
            pl_menu_action(e->key.keysym.sym == SDLK_DOWN ? PM_MDOWN : PM_MUP);
            return 1;
        }
        if (!(e->key.keysym.mod & (KMOD_ALT | KMOD_CTRL)) && p->pl->count > 0) {
            pl_cursor(e->key.keysym.sym == SDLK_DOWN ? 1 : -1);
            return 1;
        }
        return 0;
    case SDLK_HOME:
    case SDLK_END: {
        /* jump the selection AND the view to the first/last row: with an
           anchor set, the range keys compose (End then shift-click, or a
           later Shift+Home); empty list returns 0 so the global hotkeys
           stay reachable */
        if (p->pl->count == 0) return 0;
        Rect ptext, pscroll;
        pl_layout(&ptext, &pscroll);
        int lines = ptext.h / 17;
        int last = p->pl->count - 1;
        int at_end = e->key.keysym.sym == SDLK_END;
        int idx = at_end ? last : 0;
        ui.pl_sel = idx;              /* new anchor, single selection */
        ui.pl_selall = 0;
        pl_sel_reset();
        pl_sel_add(idx);
        ui.pl_scroll = at_end && p->pl->count > lines ? p->pl->count - lines : 0;
        return 1;
    }
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
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(EQ_W);
        if (in_rect(&xc, x, y)) { ui.pressed = EQB_X; return; }
        Rect on = eq_on_rect();
        if (in_rect(&on, x, y)) { ui.pressed = EQB_ON; return; }
        for (size_t i = 0; i < sizeof EQ_PRE / sizeof EQ_PRE[0]; i++) {
            Rect r = {EQ_PRE[i].x, EQ_PRE_Y, EQ_PRE[i].w, 12};
            if (in_rect(&r, x, y)) { ui.pressed = EQ_PRE[i].id; return; }
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
        } else if (ui.pressed == EQB_ON) eq_toggle(&p->eq);
        else for (size_t i = 0; i < sizeof EQ_PRE / sizeof EQ_PRE[0]; i++)
            if (ui.pressed == EQ_PRE[i].id) eq_preset(p, EQ_PRE[i].id);
        ui.pressed = B_NONE;
        if (ui.drag == 7)
            ui_drag_bounce(ui.weq, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
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
    case SDLK_s: p->shuffle = !p->shuffle; shuffle_persist(); break;
    case SDLK_r: p->repeat = !p->repeat; break;
    case SDLK_RIGHT: pl_post_seek(p, (double)(p->pos_frames - p->track_start + p->rate * 5) /
                                      (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_LEFT: pl_post_seek(p, (double)(p->pos_frames - p->track_start - p->rate * 5) /
                                     (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_UP:
    case SDLK_DOWN:
        /* arrows walk the playlist cursor from any window (pl_key
           already claimed the playlist window's own); Alt stays with the
           row-move path, Ctrl passes through */
        if (e->key.keysym.mod & (KMOD_ALT | KMOD_CTRL)) break;
        pl_cursor(e->key.keysym.sym == SDLK_DOWN ? 1 : -1);
        break;
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
    case SDLK_EQUALS:   /* '+' shares the key with '=' on common layouts
                           — volume up accepts both, and the numpad */
        p->vol += 8; if (p->vol > 128) p->vol = 128; break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        p->vol -= 8; if (p->vol < 0) p->vol = 0; break;
    case SDLK_l: if (ctrl) { pl_scan_tags(p->pl, &p->mtx); } break;
    case SDLK_f:
#ifdef __APPLE__
        if (ctrl || (e->key.keysym.mod & KMOD_GUI)) search_open(ui.wmain);
#else
        if (ctrl) search_open(ui.wmain);
#endif
        break;
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

/* ---- idle redraw scheduler -------------------------------------------------
   The old loop repainted every visible window at a fixed 10 fps no matter
   what: paused, unfocused, nothing changed. That steady 10 Hz wake-up is the
   entire steady-state CPU cost and keeps the CPU out of deep sleep states on
   laptops. Now events mark their window dirty, a poll tick (1 s, or 10 Hz
   only while an animation is live) catches player-thread changes, and only
   dirty windows are repainted. Unfocused windows are not repainted at all
    (FOCUS_GAINED marks the whole set dirty again), with four exceptions: the
    main window while the visualizer is on and playing (the vis band lives in
    the main window and a user watching it wants it live), the main window
    while ui.always_10fps is on and a title is long enough to marquee (the
    options > advanced 10FPS toggle keeps it scrolling even paused and
    unfocused), the main window at 1 Hz while playing (the time readout and
    position slider keep pace instead of jumping on refocus), and a one-shot
    repaint after EXPOSED/SHOWN, because on an uncomposited X11 desktop the
    WM hands us a garbage window when someone uncovers it. */
/* the window bits D_MAIN..D_ALL live at the top of draw_main, beside the
   main-window sub-region producer bits that split D_MAIN's repaints */
static unsigned ui_dirty;

/* ---- song-info window (edit menu PM_INFO) ---------------------------------
   ONE row's tags, read fresh from disk at invocation — deliberately NOT
   the library index: the user sees what the file carries now (tags edited
   after the last scan, files never scanned; the extended dump is
   tag_read_full, the ID3v2/Vorbis-comment set on top of tag_probe's four
   basics). A
   virtual CUE row keeps the sheet-parsed tags instead: its on-disk file
   is the .cue sheet and re-reading that would show nothing of the track.
   The row is the single selected row, else the shift anchor, else the
   playing row; none of those -> refused with the ui_msg line.
   Values render in click-to-select copy boxes (the shared field chrome:
   draw_field_text + TField + tf_copy — the house selection contract is
   whole-buffer, so there is nothing to edit: click, Ctrl+C, paste). The
   location splits LOCATION(dir)+FILE: LV_MAX_PATH is 4096 and the values
   are one-liners, so the split keeps the file NAME visible. */
#define INFO_W 360
#define INFO_H 276
#define INFO_TOP 28
#define INFO_PITCH 22
#define INFO_BOX_X 86
enum { INFOI_TITLE, INFOI_ARTIST, INFOI_AARTIST, INFOI_COMPOSER, INFOI_ALBUM,
       INFOI_TRACK, INFOI_YEAR, INFOI_GENRE, INFOI_COMMENT,
       INFOI_DIR, INFOI_FILE, INFOI_N };
static const char *const INFO_LBL[INFOI_N] = {
    "TITLE", "ARTIST", "ALBUM ARTIST", "COMPOSER", "ALBUM",
    "TRACK", "YEAR", "GENRE", "COMMENT", "LOCATION", "FILE"
};
static Rect info_row_rect(int i) {
    Rect r = { INFO_BOX_X, INFO_TOP + i * INFO_PITCH, INFO_W - INFO_BOX_X - 10, 18 };
    return r;
}
/* LOCATION/FILE show their tail (paths end in what matters); the tag
   fields show their head */
static int info_row_tail(int i) { return i >= INFOI_DIR; }
static const char *info_row_text(int i) {
    switch (i) {
    case INFOI_TITLE:    return ui.info_full.title;
    case INFOI_ARTIST:   return ui.info_full.artist;
    case INFOI_AARTIST:  return ui.info_full.album_artist;
    case INFOI_COMPOSER: return ui.info_full.composer;
    case INFOI_ALBUM:    return ui.info_full.album;
    case INFOI_TRACK:    return ui.info_full.track;
    case INFOI_YEAR:     return ui.info_full.year;
    case INFOI_GENRE:    return ui.info_full.genre;
    case INFOI_COMMENT:  return ui.info_full.comment;
    case INFOI_DIR:      return ui.info_dir;
    case INFOI_FILE:     return ui.info_file;
    default:             return "";
    }
}

static void info_close(void) {
    ui.info_visible = 0;
    ui.info_focus = -1;
    if (ui.winfo) SDL_HideWindow(ui.winfo);
    ui_dirty |= D_INFO;
}

/* the focused box's whole value onto the clipboard (selection == buffer is
   the textedit contract; empty boxes copy nothing) */
static int info_copy_sel(void) {
    if (ui.info_focus < 0 || ui.info_focus >= INFOI_N) return 0;
    char *buf = (char *)info_row_text(ui.info_focus);   /* tf_copy only reads */
    if (!buf[0]) return 0;
    TField tf = { buf, 0, 0, 1 };
    return tf_copy(&tf);
}

static void info_open(void) {
    Player *p = ui.pl;
    char path[LV_MAX_PATH];
    Tags stored;
    int cue = 0, have = 0;
    pthread_mutex_lock(&p->mtx);
    {
        int row = (!ui.pl_selall && ui.pl_multi_n == 1) ? ui.pl_multi[0] : ui.pl_sel;
        if (row < 0 || row >= p->pl->count) row = p->cur;
        if (row >= 0 && row < p->pl->count) {
            Track *t = &p->pl->items[row];
            lv_strlcpy(path, t->path, sizeof path);
            stored = t->tags;
            cue = pl_ext_kind(path) == PL_CUE;
            have = 1;
        }
    }
    pthread_mutex_unlock(&p->mtx);
    if (!have) { ui_msg("  no song selected"); return; }
    tag_full_init(&ui.info_full);
    if (cue) {                        /* CUE rows keep the sheet's parsed tags */
        lv_strlcpy(ui.info_full.title, stored.title, sizeof ui.info_full.title);
        lv_strlcpy(ui.info_full.artist, stored.artist, sizeof ui.info_full.artist);
        lv_strlcpy(ui.info_full.album, stored.album, sizeof ui.info_full.album);
        if (stored.track)
            snprintf(ui.info_full.track, sizeof ui.info_full.track, "%d", stored.track);
    } else {
        tag_read_full(path, &ui.info_full);   /* fresh disk read, off the lock */
    }
    lv_strlcpy(ui.info_path, path, sizeof ui.info_path);
    char *slash = lv_strrsep(ui.info_path);
    if (slash) {
        size_t dlen = (size_t)(slash - ui.info_path);
        if (dlen >= sizeof ui.info_dir) dlen = sizeof ui.info_dir - 1;
        memcpy(ui.info_dir, ui.info_path, dlen);
        ui.info_dir[dlen] = 0;
        lv_strlcpy(ui.info_file, slash + 1, sizeof ui.info_file);
    } else {
        ui.info_dir[0] = 0;
        lv_strlcpy(ui.info_file, ui.info_path, sizeof ui.info_file);
    }
    if (!ui.winfo) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        ui.winfo = SDL_CreateWindow("levari - song info", mx + MAIN_W + 4, my + 40,
                                    INFO_W, INFO_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!ui.winfo) return;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.winfo, icon);
            SDL_FreeSurface(icon);
        }
        ui.sinfo = SDL_GetWindowSurface(ui.winfo);
    }
    ui.info_focus = -1;               /* a re-open never inherits a stale focus */
    ui.info_visible = 1;
    SDL_ShowWindow(ui.winfo);
    raise_all();
    ui_dirty |= D_INFO;
}

static void draw_info(void) {
    SDL_Surface *s = ui.sinfo;
    if (!s) return;
    skin_window_bg(s, INFO_W, INFO_H);
    font_puts(s, 10, 6, "SONG INFO", col(s, 0, 224, 255));
    { Rect x = close_rect(INFO_W); draw_x(s, &x, ui.pressed == IB_X); }
    int win_focused = ui.app_focused && ui.winfo &&
                      ui.focus_wid == SDL_GetWindowID(ui.winfo);
    for (int i = 0; i < INFOI_N; i++) {
        Rect b = info_row_rect(i);
        int foc = win_focused && ui.info_focus == i;
        font_puts(s, 10, b.y + 5, INFO_LBL[i], col(s, 160, 160, 160));
        /* the shared read-only field chrome: black box, gray border
           (cyan when focused, the file-field pairing), then the shared
           renderer for value/selection — caret pinned off (no editing) */
        SDL_Rect rb = { b.x, b.y, b.w, b.h };
        SDL_FillRect(s, &rb, col(s, 0, 0, 0));
        Uint32 bc = foc ? col(s, 0, 224, 255) : col(s, 90, 90, 90);
        SDL_Rect t = { b.x, b.y, b.w, 1 }, bot = { b.x, b.y + b.h - 1, b.w, 1 };
        SDL_Rect l = { b.x, b.y, 1, b.h }, r = { b.x + b.w - 1, b.y, 1, b.h };
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &bot, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        const char *txt = info_row_text(i);
        TField tf = { 0, 0, info_row_tail(i) ? (int)strlen(txt) : 0, 1 };
        draw_field_text(s, b, txt[0] ? txt : "-", &tf, foc, 0);
    }
}

static void handle_info_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = close_rect(INFO_W);
        if (in_rect(&xc, x, y)) { ui.pressed = IB_X; return; }
        for (int i = 0; i < INFOI_N; i++) {
            Rect b = info_row_rect(i);
            if (in_rect(&b, x, y)) { ui.info_focus = i; return; }
        }
        if (y < 22) { ui.drag = 14; drag_begin(ui.winfo); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = close_rect(INFO_W);
        if (ui.pressed == IB_X && in_rect(&xc, x, y)) info_close();
        ui.pressed = B_NONE;
        if (ui.drag == 14) {
            ui_drag_bounce(ui.winfo, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 14) {
        drag_move_snap(ui.winfo);
    }
}

static void win_present(SDL_Window *w) {
    if (w && !(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED))
        SDL_UpdateWindowSurface(w);
}

/* item 4 (cpu_floor_hunting): the main window's damage is exactly the
   regions the last draw_main pass painted — present just those so X11
   receives rect damage instead of whole-window damage (an uncomposited
   Xvnc re-encodes every damaged pixel: the second, OUTSIDE-levari draw
   cost). rgn_rects/rgn_n are draw_main's file-scope statics, so this
   must be called immediately after draw_main, as the scheduler does.
   Other windows keep the whole-window call: their draw paths have no
   region scheme (yet). */
static void win_present_main(SDL_Window *w) {
    if (!w || (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED)) return;
    if (rgn_n) SDL_UpdateWindowSurfaceRects(w, rgn_rects, rgn_n);
    else SDL_UpdateWindowSurface(w);
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
    p->viz_feed = ui_show_vis;  /* engine feeds viz_buf only while the band shows */
    search_init(p);

    SDL_SetHint(SDL_HINT_APP_NAME, "levari");
    /* deliver the button press that focuses a window, so one click on a
       background window both activates it and starts the title-bar drag */
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "levari: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    /* levari draws exclusively on software window surfaces, so opt out of
       SDL's texture-framebuffer path: on X11 without GL it slots in a
       SOFTWARE renderer (verified on the P4: SDL_video.c
       SDL_UpdateWindowTexture) and upgrades EVERY present — rect or not —
       to a full-surface texture upload + full RenderCopy + full-window
       PutImage (LD_PRELOAD spy on XShmPutImage: one 315x150 request per
       tick regardless of damage, before and after item 4 alike). Direct
       driver framebuffer ops are the only ones that honor our rects.
       The opt-out is X11-only: it was measured and justified on X11
       alone, and on Cocoa it is actively harmful (verified 0.3.6 on
       mac112: with the hint SDL_GetWindowSurface returns NULL on the
       NSView-backed window, ui.smain is NULL and the first draw
       NULL-derefs in skin_draw_main_bg; SDL_FRAMEBUFFER_ACCELERATION=1
       as env proves the hint is the cause and the 0.3.5 binary passes
       the same run unchanged). Windows is excluded too — never tested
       there, and 0.3.5 shipped fine without the hint. */
#if !defined(_WIN32) && !defined(__APPLE__)
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
#endif
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
    ui_dirty = D_ALL;
    int wake = 1;   /* one-shot unfocused repaint after EXPOSED/SHOWN; 1 = paint once at startup */
    struct pollsnap { int state, cur, count; long long pos_s;
                      int vol, pan, shuffle, repeat, ab_a, ab_b, eq_on;
                      int pl_vis, eq_vis, file_vis, opt_vis, url_vis, search_open; };
    struct pollsnap snap_prev =
        { -99, -99, -99, -9999, -99, -99, -99, -99, -99, -99, -99,
          -1, -1, -1, -1, -1, -1 };
    int running = 1;
    while (running) {
        if (ui_deadline_ms && SDL_GetTicks() >= ui_deadline_ms) break;
        if (mpris_take_raise()) raise_all(); /* MPRIS Raise request */
        SDL_Event e;
        int wait = next ? (int)(next > SDL_GetTicks() ? next - SDL_GetTicks() : 0)
                        : -1;   /* no tick scheduled: sleep until an event */
        /* a set deadline must also bound the UNTIMED wait: with wait=-1
           the loop sleeps forever whenever nothing scheduled a redraw
           (stopped/undecodable playlist), and the break above is never
           reached — selftest runs outlived their deadline by minutes */
        if (ui_deadline_ms && (wait < 0 || wait > 1000)) wait = 1000;
        if (SDL_WaitEventTimeout(&e, wait)) switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_WINDOWEVENT:
            if (e.window.windowID == SDL_GetWindowID(ui.wmain)) ui.smain = SDL_GetWindowSurface(ui.wmain);
            if (e.window.windowID == SDL_GetWindowID(ui.wpl)) ui.spl = SDL_GetWindowSurface(ui.wpl);
            if (ui.weq && e.window.windowID == SDL_GetWindowID(ui.weq)) ui.seq = SDL_GetWindowSurface(ui.weq);
            if (ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) ui.sfile = SDL_GetWindowSurface(ui.wfile);
            if (ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) ui.sopt = SDL_GetWindowSurface(ui.wopt);
            if (ui.winfo && e.window.windowID == SDL_GetWindowID(ui.winfo)) ui.sinfo = SDL_GetWindowSurface(ui.winfo);
#ifdef LV_URL_ENABLED
            if (ui.wurl && e.window.windowID == SDL_GetWindowID(ui.wurl)) ui.surl = SDL_GetWindowSurface(ui.wurl);
#endif
            if (search_window() && e.window.windowID == search_window_id())
                search_surface_refresh();
            /* whichever of our windows gets focus, bring the whole set forward
               so activating the playlist never leaves the main window buried */
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                /* focus left us for another app (or a window of ours closed):
                   the next FOCUS_GAINED must re-raise the whole set */
                ui.app_focused = 0;
                /* one-shot repaint so the search caret disappears even when
                   the whole app loses focus (unfocused windows otherwise
                   aren't repainted and would keep the last blink frame) */
                wake = 1;
            }
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                int was_focused = ui.app_focused;
                ui.app_focused = 1;
                ui.focus_wid = e.window.windowID;
                focus_click_drag(e.window.windowID);
                if (search_is_open() && e.window.windowID == search_window_id()) SDL_StartTextInput();
                else if (ui.opt_visible && ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) SDL_StartTextInput();
                else if (ui.file_visible && ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) SDL_StartTextInput();
#ifdef LV_URL_ENABLED
                else if (ui.url_visible && ui.wurl && e.window.windowID == SDL_GetWindowID(ui.wurl)) SDL_StartTextInput();
#endif
                else SDL_StopTextInput();
                /* the set surfaces as a unit: when focus arrives from another
                   app, raise every visible window, not just the one the WM
                   clicked. On Cocoa raise_all() is a no-op; on X11 raising
                   can itself refocus (Xfwm), so raise_all() rate-limits
                   itself to keep that from becoming a storm */
                if (!was_focused) raise_all();
            }
            ui_dirty |= D_ALL;
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SHOWN) wake = 1;
            break;
        case SDL_KEYDOWN:
            /* media keys bypass the focus routing: they belong to the
               player, not to whichever window has the caret */
            switch (e.key.keysym.sym) {
            case SDLK_AUDIONEXT: pl_post(p, CMD_NEXT, 0); break;
            case SDLK_AUDIOPREV: pl_post(p, CMD_PREV, 0); break;
            case SDLK_AUDIOPLAY: pl_post(p, CMD_TOGGLE_PAUSE, 0); break;
            case SDLK_AUDIOSTOP: pl_post(p, CMD_STOP, 0); break;
            default:
                if (ui.info_visible && ui.winfo && ui.focus_wid == SDL_GetWindowID(ui.winfo)) {
                    /* Esc closes the info window (pl_menu precedent). The
                       value boxes are read-only: Ctrl+C (or Ctrl+Insert)
                       copies the focused box wholesale — the textedit
                       selection contract is whole-buffer, so Ctrl+A is
                       already the state. Every other key keeps the global
                       hotkey behaviour, same as when the EQ window holds
                       focus — the window has no fields, so it must not
                       eat text */
                    const Uint8 *iks = SDL_GetKeyboardState(NULL);
                    int ictrl = iks[SDL_SCANCODE_LCTRL] || iks[SDL_SCANCODE_RCTRL];
                    SDL_Keycode isym = e.key.keysym.sym;
                    if (isym == SDLK_ESCAPE) { info_close(); ui_dirty |= D_INFO; }
                    else if (ictrl && (isym == SDLK_c || isym == SDLK_INSERT)) { info_copy_sel(); }
                    else if (ictrl && isym == SDLK_a) { ui_dirty |= D_INFO; }
                    else handle_key(&e);
                }
                else
#ifdef LV_URL_ENABLED
                if (ui.url_visible && ui.wurl && ui.focus_wid == SDL_GetWindowID(ui.wurl)) {
                    /* the URL field eats EVERY key while its window holds
                       focus (file-browser style): global playback hotkeys
                       must not fire mid-paste */
                    url_key(&e);
                    ui_dirty |= D_URL;
                    next = SDL_GetTicks(); /* paint the edit now */
                }
                else
#endif
                if (search_is_open() && ui.focus_wid == search_window_id()) search_key(&e);
                else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) opt_key(&e);
                else if (ui.file_visible && ui.wfile && ui.focus_wid == SDL_GetWindowID(ui.wfile)) {
                    /* the file field eats its keys BEFORE the global
                       shortcuts: typing 'c'/'o' must not pause or reopen */
                    if (file_key(&e)) {
                        ui_dirty |= D_FILE;
                        next = SDL_GetTicks(); /* paint the edit now */
                    } else handle_key(&e);
                }
                else if (ui.pl_visible && ui.wpl && ui.focus_wid == SDL_GetWindowID(ui.wpl)) {
                    if (!pl_key(&e)) handle_key(&e);
                } else handle_key(&e);
            }
            ui_dirty |= D_MAIN | D_PL | D_SRCH | D_OPT;
            break;
        case SDL_TEXTINPUT:
            if (search_is_open() && ui.focus_wid == search_window_id()) search_text(e.text.text);
            else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) opt_text(e.text.text);
#ifdef LV_URL_ENABLED
            else if (ui.url_visible && ui.wurl && ui.focus_wid == SDL_GetWindowID(ui.wurl) &&
                     ui.url_focus) {
                url_text(e.text.text);
                ui_dirty |= D_URL;
                next = SDL_GetTicks();
            }
#endif
            else if (ui.file_visible && ui.wfile && ui.focus_wid == SDL_GetWindowID(ui.wfile) &&
                     ui.file_focus) {
                file_text(e.text.text);
                ui_dirty |= D_FILE;
                next = SDL_GetTicks();
            }
            ui_dirty |= D_SRCH | D_OPT;
            break;
        case SDL_DROPBEGIN:
            if (drop_n) drop_flush(); /* previous session never completed */
            drop_open = 1;
            break;
        case SDL_DROPFILE: {
            /* route by target window: drops on the search/options/browser
               windows are ignored. e.drop.file is SDL-owned: copy into the
               batch, then SDL_free the original */
            if (e.drop.windowID == SDL_GetWindowID(ui.wmain) ||
                e.drop.windowID == SDL_GetWindowID(ui.wpl)) {
                drop_open = 1;
                if (e.drop.file) drop_add(e.drop.file);
            }
            if (e.drop.file) SDL_free(e.drop.file);
            break;
        }
        case SDL_DROPCOMPLETE:
            if (drop_open) drop_flush();
            drop_open = 0;
            ui_dirty |= D_PL | D_MAIN;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEWHEEL: {
            Uint32 wid = ev_win(&e);
            /* a swallowed button-release must not leave a drag latched on */
            if (e.type == SDL_MOUSEMOTION && ui.drag &&
                !(e.motion.state & SDL_BUTTON_LMASK)) ui.drag = 0;
            if (wid == SDL_GetWindowID(ui.wmain)) { handle_main_mouse(&e); ui_dirty |= D_MAIN; }
            else if (wid == SDL_GetWindowID(ui.wpl)) { handle_pl_mouse(&e); ui_dirty |= D_PL; }
            else if (ui.weq && wid == SDL_GetWindowID(ui.weq)) { handle_eq_mouse(&e); ui_dirty |= D_EQ; }
            else if (ui.wfile && wid == SDL_GetWindowID(ui.wfile)) { handle_file_mouse(&e); ui_dirty |= D_FILE; }
            else if (ui.wopt && wid == SDL_GetWindowID(ui.wopt)) { handle_opt_mouse(&e); ui_dirty |= D_OPT; }
            else if (ui.winfo && wid == SDL_GetWindowID(ui.winfo)) { handle_info_mouse(&e); ui_dirty |= D_INFO; }
#ifdef LV_URL_ENABLED
            else if (ui.wurl && wid == SDL_GetWindowID(ui.wurl)) { handle_url_mouse(&e); ui_dirty |= D_URL; }
#endif
            else if (search_is_open() && wid == search_window_id()) { search_mouse(&e); ui_dirty |= D_SRCH; }
            /* a latched press must paint its pressed frame NOW: the 100 ms
               coalesce below would swallow the animation of a fast click
               (release arrives before any repaint, pressed already NONE) */
            if (e.type == SDL_MOUSEBUTTONDOWN && ui.pressed != B_NONE)
                next = SDL_GetTicks();
            break;
        }
        }
        if (SDL_AtomicGet(&ui.opt_scan_done)) {
            SDL_AtomicSet(&ui.opt_scan_done, 0);
            ui.opt_scanning = 0;
            if (ui.opt_scan_skipped)
                snprintf(ui.opt_status, sizeof ui.opt_status,
                         "added %d (%d seen, %d path(s) too long)",
                         ui.opt_scan_added, ui.opt_scan_seen, ui.opt_scan_skipped);
            else
                snprintf(ui.opt_status, sizeof ui.opt_status,
                         "added %d (%d seen)", ui.opt_scan_added, ui.opt_scan_seen);
            search_lib_invalidate();
            ui_dirty |= D_OPT;
        }
        /* idle redraw scheduler: repaint only dirty windows, and only on the
           poll tick. 10 Hz while an animation is live (scrolling title,
           visualizer, drag, scan), else 1 Hz for the position text. An
           unfocused app sleeps on events, except a live visualizer (the vis
           band lives in the main window and a watcher wants it moving), a
           1 Hz main-window repaint while playing (no time/position jump on
           refocus), or a one-shot repaint after the WM exposes/shows a
           window. Visibility transitions mark the window dirty so a freshly
           shown window paints once without touching every toggle site. */
        Uint32 now = SDL_GetTicks();
        /* interaction pending while focused: guarantee a prompt (<=100 ms)
           repaint so hover/seek feedback stays responsive even when no
           animation is live; rapid events coalesce into the same tick. */
        if (ui.app_focused && ui_dirty && next && next > now + 100)
            next = now + 100;
        if (now >= next) {
            int playing = p->state == ST_PLAY;
            int so = search_is_open();
#ifdef LV_URL_ENABLED
            int uvo = ui.url_visible;
#else
            int uvo = 0;
#endif
            struct pollsnap sn = {
                p->state, p->cur, p->pl->count,
                p->rate ? p->pos_frames / p->rate : 0,
                p->vol, p->pan, p->shuffle, p->repeat, p->ab_a, p->ab_b,
                eq_enabled(&p->eq),
                ui.pl_visible, ui.eq_visible, ui.file_visible,
                ui.opt_visible, uvo, so };
            if (sn.state != snap_prev.state || sn.cur != snap_prev.cur ||
                sn.count != snap_prev.count)
                ui_dirty |= D_MAIN | D_PL;
            if (sn.pos_s != snap_prev.pos_s) ui_dirty |= D_M_TIME;
            /* theme-level producers: button tints and the vol/pan knobs sit
               OUTSIDE the poll regions, so a change of these must force the
               full pass the old 1 Hz D_MAIN poll healed implicitly (a
               shuffle/repeat/AB/EQ toggle or a daemon vol step reaching
               the UI only through this poll is exactly that class) */
            if (sn.vol != snap_prev.vol || sn.pan != snap_prev.pan ||
                sn.shuffle != snap_prev.shuffle ||
                sn.repeat != snap_prev.repeat ||
                sn.ab_a != snap_prev.ab_a || sn.ab_b != snap_prev.ab_b ||
                sn.eq_on != snap_prev.eq_on)
                ui_dirty |= D_MAIN;
            if (sn.pl_vis != snap_prev.pl_vis) ui_dirty |= D_PL;
            if (sn.eq_vis != snap_prev.eq_vis) ui_dirty |= D_EQ;
            if (sn.file_vis != snap_prev.file_vis) ui_dirty |= D_FILE;
            if (sn.opt_vis != snap_prev.opt_vis) ui_dirty |= D_OPT;
            if (sn.url_vis != snap_prev.url_vis) ui_dirty |= D_URL;
            if (sn.search_open != snap_prev.search_open) ui_dirty |= D_SRCH;
            snap_prev = sn;
            if (ui.opt_scanning) ui_dirty |= D_OPT;

            /* the search caret blinks only while the search window has focus */
            int srch_focused = so && ui.app_focused &&
                               ui.focus_wid == search_window_id();
            search_set_focus(srch_focused);

            /* the file path field blinks on the same cadence when its
               window holds the caret */
            int file_focused = ui.file_visible && ui.app_focused &&
                               ui.file_focus && ui.wfile &&
                               ui.focus_wid == SDL_GetWindowID(ui.wfile);
            /* "path not valid" shows ~1 s, then the field text returns */
            if (ui.file_err && now - ui.file_err_at >= 1000) {
                ui.file_err = 0;
                ui_dirty |= D_FILE;
            }
#ifdef LV_URL_ENABLED
            /* the URL field caret blinks on the same 500 ms cadence when
               its window holds the caret; "url not allowed" shows ~1 s
               (file_err precedent — expiry repaint marks it dirty) */
            int url_focused = ui.url_visible && ui.app_focused &&
                              ui.url_focus && ui.wurl &&
                              ui.focus_wid == SDL_GetWindowID(ui.wurl);
            if (ui.url_err && now - ui.url_err_at >= 1000) {
                ui.url_err = 0;
                ui_dirty |= D_URL;
            }
#else
            int url_focused = 0;
#endif
            /* the title-strip rescan message lasts 10 s; D_MAIN below makes
               the expiry repaint immediate (file_err precedent) */
            if (ui.msg[0] && now - ui.msg_at >= 10000) {
                ui.msg[0] = 0;
                ui_dirty |= D_MAIN;
            }

            int vis_live = ui.vis_on && playing;
            int f10_live = cfg.ui_always10 > 0 && ui.marquee;
            /* the plain marquee animates at 10 Hz only while we hold focus
               (the documented unfocused contract above): unfocused it keeps
               crawling, but one step per SECOND — the old 1 Hz poll repaint
               rate, riding the same second-change that refreshes the time
               readout (scroll_off advances only inside drawn strip passes).
               The always_10FPS toggle and a live visualizer override focus
               (the vis band repaints the strip anyway, so the marquee keeps
               10 Hz pace with it). */
            int strip_live = ui.marquee && (playing || f10_live) &&
                             (ui.app_focused || f10_live || vis_live);
            int strip_slow = ui.marquee && playing && !ui.app_focused &&
                             !f10_live && !vis_live;
            int anim = strip_live || vis_live ||
                       ui.drag || ui.opt_scanning || ui.msg[0];
            /* anim producers mark ONLY the region they move (item 1): the
               marquee/viz creep never forces a full-window repaint again.
               Drag (press chrome anywhere) and the scan spinner are
               theme-level — those mark the full bit D_M_BG. */
            if (anim) {
                if (vis_live) ui_dirty |= D_M_VIZ;
                else if (strip_live) ui_dirty |= D_M_TITLE;
                if (ui.msg[0] && ui.app_focused) ui_dirty |= D_M_TITLE;
                if (ui.drag || ui.opt_scanning) ui_dirty |= D_M_BG;
            }
            /* the 1 Hz unfocused crawl: D_M_TIME is set exactly on the
               poll's second-change (pos_s flip), so this marks the strip
               once per second and nothing schedules the 10 Hz tick here */
            if (strip_slow && (ui_dirty & D_M_TIME)) ui_dirty |= D_M_TITLE;
            unsigned mregs = main_draw_regions(ui_dirty);

            if (ui.app_focused) {
                if (mregs) {
                    refresh_title();
                    draw_main(mregs);
                    LV_T0;
                    win_present_main(ui.wmain);
                    LV_ACC(blit_us);
                }
                if ((ui_dirty & D_PL) && ui.pl_visible) { draw_pl(); win_present(ui.wpl); }
                if ((ui_dirty & D_EQ) && ui.eq_visible && ui.weq) { draw_eq(); win_present(ui.weq); }
                if (((ui_dirty & D_FILE) || file_focused) && ui.file_visible && ui.sfile) { draw_file(); win_present(ui.wfile); }
                if ((ui_dirty & D_OPT) && ui.opt_visible && ui.sopt) { draw_opt(); win_present(ui.wopt); }
                if ((ui_dirty & D_INFO) && ui.info_visible && ui.sinfo) { draw_info(); win_present(ui.winfo); }
#ifdef LV_URL_ENABLED
                if (((ui_dirty & D_URL) || url_focused) && ui.url_visible && ui.surl) { draw_url(); win_present(ui.wurl); }
#endif
                if (((ui_dirty & D_SRCH) || srch_focused) && so && search_window()) { search_draw(); win_present(search_window()); }
                ui_dirty = 0;
            } else if (vis_live || f10_live || playing || (wake && ui_dirty)) {
                if (mregs) {
                    refresh_title();
                    draw_main(mregs);
                    LV_T0;
                    win_present_main(ui.wmain);
                    LV_ACC(blit_us);
                }
                if ((ui_dirty & D_PL) && ui.pl_visible) { draw_pl(); win_present(ui.wpl); }
                if ((ui_dirty & D_EQ) && ui.eq_visible && ui.weq) { draw_eq(); win_present(ui.weq); }
                if ((ui_dirty & D_FILE) && ui.file_visible && ui.sfile) { draw_file(); win_present(ui.wfile); }
                if ((ui_dirty & D_OPT) && ui.opt_visible && ui.sopt) { draw_opt(); win_present(ui.wopt); }
                if ((ui_dirty & D_INFO) && ui.info_visible && ui.sinfo) { draw_info(); win_present(ui.winfo); }
#ifdef LV_URL_ENABLED
                if ((ui_dirty & D_URL) && ui.url_visible && ui.surl) { draw_url(); win_present(ui.wurl); }
#endif
                if ((ui_dirty & D_SRCH) && so && search_window()) { search_draw(); win_present(search_window()); }
                ui_dirty = 0;
                wake = 0;
            }

            /* vis_live is part of anim, so a live visualizer always ticks at
               10 Hz whether focused or not. A focused search field redraws on
               each 500 ms caret boundary (2 Hz, just enough to toggle it).
               Focused idle polls at 1 Hz for player-thread state changes;
               unfocused but playing polls at 1 Hz so the time readout and
               position slider keep pace instead of jumping on refocus;
               unfocused idle sleeps on events. */
            if (anim) next = now + (vis_live ? vis_frame_ms(cfg.vis_hz) : 100);
            else if (srch_focused || file_focused || url_focused) next = now + (unsigned)(500 - (int)(now % 500));
            else if (ui.app_focused) next = now + 1000;
            else if (playing) next = now + 1000;
            else next = 0;   /* unfocused, nothing playing: sleep on events */
        }
    }
    /* save window dumps at exit so they reflect the final state */
#ifdef LV_PROFILE
    fprintf(stderr, "lvprof: full=%u title=%u viz=%u time=%u dirty_px=%llu "
            "us:bg=%llu text=%llu widgets=%llu blit=%llu\n",
            lvprof.full, lvprof.title, lvprof.viz, lvprof.ticks_time,
            lvprof.px, lvprof.bg_us, lvprof.txt_us, lvprof.wid_us,
            lvprof.blit_us);
#endif
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
#ifdef LV_URL_ENABLED
        if (ui.url_visible && ui.surl) {
            snprintf(f, sizeof f, "%s-url.bmp", ui_dump_path);
            SDL_SaveBMP(ui.surl, f);
        }
#endif
        if (ui.info_visible && ui.sinfo) {
            snprintf(f, sizeof f, "%s-info.bmp", ui_dump_path);
            SDL_SaveBMP(ui.sinfo, f);
        }
        ui_dump_path = NULL;
    }
    search_shutdown();
    if (ui.wopt) SDL_DestroyWindow(ui.wopt);
    if (ui.wfile) SDL_DestroyWindow(ui.wfile);
    if (ui.winfo) SDL_DestroyWindow(ui.winfo);
#ifdef LV_URL_ENABLED
    if (ui.wurl) SDL_DestroyWindow(ui.wurl);
#endif
    SDL_DestroyWindow(ui.weq);
    SDL_DestroyWindow(ui.wpl);
    SDL_DestroyWindow(ui.wmain);
    SDL_Quit();
    return 0;
}
