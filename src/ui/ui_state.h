#ifndef LEVARI_UI_STATE_H
#define LEVARI_UI_STATE_H
/* Shared UI state for the ui.c split: the UI struct lives here; every
   ui_*.c TU includes this header. ui.c owns the definition of ui and
   ui_dirty; everything else in a section TU stays static unless this
   header declares it (cross-window helpers). */

#include "ui.h"
#include "search.h"
#include "textedit.h"
#include "x8bit.h"
#include "../skin/skin.h"
#include "../skin/cjk_font.h"
#include "../viz/viz.h"
#include "../config/config.h"
#include "../compat/win98_diag.h"
#include "../tag/tag.h"
#include "../library/library.h"
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
    B_SHUF, B_REPEAT, B_EQ, B_VIS, B_LIST, B_ABA, B_ABB, B_XMAIN, B_SEARCH, B_URL, B_RPT1, B_AOT,
    B_MSTOP, B_MPREV, B_MPLAY, B_MNEXT,
    PB_SHUF, PB_REPEAT, PB_ADD, PB_REM, PB_CLOSE, PB_LIB, PB_SCAN, PB_X, PB_SSCOPE, FB_ADDDIR, FB_X, B_URLX, IB_X,
    EQB_ON, EQB_FLAT, EQB_ROCK, EQB_POP, EQB_JAZZ, EQB_X, EQB_RESET,
    OPTB_SAVE, OPTB_CLOSE, OPTB_TPL, OPTB_TEQ, OPTB_TVIS, OPTB_TSRCH, OPTB_TSHUF,
    OPTB_TFADE, OPTB_TAOT, OPTB_TMINI, OPTB_TLARGE, OPTB_TFPS, OPTB_TENC, OPTB_VHZ, OPTB_RG,
    OPTB_SCAN, OPTB_CLEAR, OPTB_CHK, OPTB_VERIFY, OPTB_X, OPTB_TABB, OPTB_TABA, OPTB_TABU,
    B_NONE
};
enum { OPTF_LIBDIRS, OPTF_LIBIDX, OPTF_LIBAV, OPTF_BACKEND, OPTF_DEVICE, OPTF_SOCK, OPTF_N };
/* equalizer: same width as main, docked above it by default */
#define EQ_W MAIN_W
#define EQ_H 156
/* ui.snap[] / ui.follow_snap[] slots (see the UI struct): 0 eq, 1 file,
   2 search, 3 options, 4 url — the url slot exists in every build, it
   only ever fills while LV_URL_ENABLED compiles the URL window in */
#define SNAP_NSLOT 5
#define SNAP_URL 4

/* mini layout geometry (shared: ui.c draws the bar, search.c docks under it) */
#define MINI_W 320
#define MINI_H 22

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
    int url_ff_ok;               /* ui_ffprobe_info()==1, cached at open:
                                    0 = streaming cannot work (no libav) and
                                    the window says so below the field */
    int url_ff_code;             /* raw probe return (1/2/3): 3 reads a
                                    different red line — found, wrong ABI */
    char url_ff_ver[32];         /* release found at open (for that line) */
    Uint32 url_err_at;
    char url_field[LV_MAX_PATH];
    TField url_tf;               /* buf points at url_field */
#endif
    TField opt_tf;               /* options editor state; buf bound to
                                    opt_field[opt_focus] on every focus
                                    change (caret re-anchors to the new
                                    field's end on focus) */
    char opt_field[OPTF_N][LV_MAX_PATH];
    int opt_pl, opt_eq, opt_vis, opt_srch, opt_shuf, opt_fade, opt_large, opt_f10;
    int opt_vhz;                /* ADV cycle button copy: 10/24/30/60 (cfg.vis_hz) */
    int opt_rg;                 /* ADV cycle button copy: 0 off / 1 track / 2 album (cfg.replaygain) */
    int opt_enc;                 /* LEGACY button cycle: 0 auto, 1 latin1, 2 cp1251, 3 utf8, 4 shiftjis (cfg value = opt_enc - 1) */
    int opt_ff;                  /* ADV status line: ffmpeg (libav*) detected
                                    on the system — probed once at ui_opt_open
                                    (the probe dlopens per call; libav.dir
                                    changes need a restart anyway) */
    char opt_ff_ver[32];         /* release of the ffmpeg actually found
                                    (probe_info), shown on the same line */
    int opt_tab;                 /* options window tab: 0 BASIC, 1 ADVANCED, 2 ABOUT */
    int opt_scanning;            /* a library scan thread is running */
    int opt_verifying;           /* ... and VERIFY owns the index (shared
                                    opt_scanning gate, its own BUSY label) */
    int opt_vfy_rc, opt_vfy_kept, opt_vfy_purged, opt_vfy_unreach;
    char opt_vfy_bank[LV_MAX_PATH]; /* where the dropped rows were banked */
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
    int aot;               /* always-on-top live state: shared truth of the
                              main-window AoT button and the options
                              checkbox; startup seeds from cfg.ui_aot,
                              only SAVE persists an override */
    int mini;              /* mini layout live state (same persist shape as
                              aot: menu item + options checkbox toggle it
                              live, only SAVE persists) */
    Uint32 mini_dbl;       /* last scroller click tick (double-click -> viz) */
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
       ui_follow_move): last REQUESTED pos + walls hit this drag; valid=0 until
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
extern UI ui;
#define PL_MIN_W 275   /* keep the whole button row visible */
#define PL_MIN_H 60
#define DOCK_SNAP 12   /* px of slack when snapping the playlist back under main */
#define D_MAIN 1u
#define D_PL   2u
#define D_EQ   4u
#define D_FILE 8u
#define D_OPT  16u
#define D_SRCH 32u
#define D_URL  64u
#define D_INFO 128u
#define D_ALL  (D_MAIN | D_PL | D_EQ | D_FILE | D_OPT | D_SRCH | D_URL | D_INFO)
typedef struct { int id; const Rect *r; } Btn;

extern unsigned ui_dirty;               /* window bits D_MAIN..D_ALL */

/* cross-window helpers (defined by the TU noted) */
int ui_in_rect(const Rect *r, int x, int y);
Rect ui_close_rect(int win_w);
void ui_draw_x(SDL_Surface *s, const Rect *r, int pressed);
Uint32 ui_col(SDL_Surface *s, int r, int g, int b);
void ui_action(int id);
void ui_set_mini(int on);  /* live mini-mode switch (menu item + options
                              checkbox; only opt_save persists cfg.ui_mini) */
void ui_msg(const char *text);
int ui_ffprobe_info(char *ver, size_t vn, char *dir, size_t dn);
void ui_pl_dock_pos(int *x, int *y);
void ui_drag_begin(SDL_Window *w);
void ui_drag_move(SDL_Window *w);
void ui_drag_move_snap(SDL_Window *w);
void ui_drag_move_pl(void);
void ui_follow_move(SDL_Window *w, struct FollowState *fs, int x, int y);
void ui_reflow_above(SDL_Window *self, int cursor_y);
int ui_drag_dock_restore(void);
void ui_raise_all(void);
void ui_focus_click_drag(Uint32 wid);
void ui_draw_pl(void);
void ui_handle_pl_mouse(SDL_Event *e);
void ui_pl_cursor(int dir);
int ui_pl_key(SDL_Event *e);
void ui_pl_sel_reset(void);
int ui_pl_sel_has(int idx);
int ui_pl_multi_grow(int need);
/* merged mini search (ui_pl.c): the playlist band hosts the query field;
   ui.c routes TEXTINPUT/FOCUS to these while the field holds the caret */
int ui_pl_search_focused(void);
void ui_pl_text(const char *text);
void ui_pl_search_reset(void);   /* mini enter/exit: query always starts clean */
void ui_pl_search_focus(void);   /* Ctrl+F in mini */
void ui_draw_eq(void);
void ui_handle_eq_mouse(SDL_Event *e);
void ui_file_open(void);
void ui_file_close(void);
int ui_file_key(SDL_Event *e);
void ui_file_text(const char *text);
void ui_draw_file(void);
void ui_handle_file_mouse(SDL_Event *e);
void ui_draw_field_text(SDL_Surface *s, const Rect f, const char *val, TField *tf, int focused, int caret_on);
void ui_urlwin_open(void);
void ui_url_close(void);
int ui_url_key(SDL_Event *e);
void ui_url_text(const char *text);
void ui_draw_url(void);
void ui_handle_url_mouse(SDL_Event *e);
void ui_url_play_transient(Player *p, const char *url);
void ui_opt_open(void);
void ui_opt_close(void);
void ui_opt_key(SDL_Event *e);
void ui_opt_text(const char *text);
void ui_draw_opt(void);
void ui_handle_opt_mouse(SDL_Event *e);
void ui_shuffle_persist(void);
int ui_vis_frame_ms(int hz);
void ui_info_open(void);
void ui_info_close(void);
int ui_info_copy_sel(void);
void ui_draw_info(void);
void ui_handle_info_mouse(SDL_Event *e);

#endif
