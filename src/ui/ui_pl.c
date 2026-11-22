/* src/ui/ui_pl.c: playlist window: draw, mouse, keys, selection, edit menu — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

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
   contract as the MAIN menu). The ui_action switch lives next to
   ui_handle_pl_mouse; CLEAR reuses PB_REM's proven select-all path. */
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
#define PL_MENU_ROWS 8                 /* first ui_col 8, second the rest */
static Rect pl_menu_item_rect(int i) {
    Rect r = { 2 + (i / PL_MENU_ROWS) * 90, 2 + (i % PL_MENU_ROWS) * 13, 90, 13 };
    return r;
}
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
void ui_pl_sel_reset(void) { ui.pl_multi_n = 0; }

static int pl_sel_find(int idx) { /* >=0 hit, else -(insertion point + 1) */
    int lo = 0, hi = ui.pl_multi_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ui.pl_multi[mid] == idx) return mid;
        if (ui.pl_multi[mid] < idx) lo = mid + 1; else hi = mid - 1;
    }
    return -(lo + 1);
}

int ui_pl_sel_has(int idx) { return pl_sel_find(idx) >= 0; }

int ui_pl_multi_grow(int need) {
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
    if (ui_pl_multi_grow(ui.pl_multi_n + 1) != 0) return;
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
    if (ui_pl_multi_grow(count) != 0) return;
    for (int i = 0; i < count; i++) ui.pl_multi[i] = i;
    ui.pl_multi_n = count;
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
    else lv_fmt_time(buf, n, total);
}
void ui_draw_pl(void) {
    SDL_Surface *s = ui.spl;
    Player *p = ui.pl;
    Playlist *pl = p->pl;
    pthread_mutex_lock(&p->mtx);
    Rect ptext, pscroll;
    pl_layout(&ptext, &pscroll);
    skin_draw_pl_bg(s, ui.pl_w, ui.pl_h, &ptext, &pscroll);
    { Rect x = ui_close_rect(ui.pl_w); ui_draw_x(s, &x, ui.pressed == PB_X); }

    for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++) {
        const Btn *b = &PL_BTNS[i];
        int active = 0;
        if (b->id == PB_SHUF) active = p->shuffle;
        if (b->id == PB_REPEAT) active = p->repeat == 1;
        skin_button(s, b->r, ui.pressed == b->id, active);
        Uint32 c = active ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220);
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
        Uint32 c = ui_col(s, 170, 170, 170);
        if (idx == p->cur) c = ui_col(s, 0, 255, 0);
        else if (ui.pl_selall || ui_pl_sel_has(idx)) c = ui_col(s, 255, 255, 255);
        font_puts16(s, ptext.x + 2, ptext.y + 1 + i * 17, line, c);
    }

    /* scrollbar thumb */
    if (pl->count > lines) {
        int th = pscroll.h * lines / pl->count;
        if (th < 8) th = 8;
        int ty = pscroll.y + (pscroll.h - th) * ui.pl_scroll / (pl->count - lines);
        SDL_Rect r = {pscroll.x + 1, ty, pscroll.w - 2, th};
        SDL_FillRect(s, &r, ui_col(s, 110, 110, 110));
    }

    /* row-drag (12): drop-index highlight — a green hairline at the gap
       the block will land at, clamped to the visible rows */
    if (ui.drag == 12 && ui.pl_drop >= 0) {
        int dy = ptext.y + (ui.pl_drop - ui.pl_scroll) * 17;
        if (dy < ptext.y) dy = ptext.y;
        if (dy > ptext.y + lines * 17 - 1) dy = ptext.y + lines * 17 - 1;
        SDL_Rect r = {ptext.x, dy, ptext.w, 1};
        SDL_FillRect(s, &r, ui_col(s, 0, 255, 0));
    }

    /* totals, pinned to the bottom edge (fmt_pl_total saturates at
       9999:59:59+ so a crafted playlist cannot wrap the sum) */
    char tb[32];
    fmt_pl_total(tb, sizeof tb, pl);
    font_puts(s, 14, ui.pl_h - 10, tb, ui_col(s, 0, 255, 0));
    snprintf(tb, sizeof tb, "%d of %d", pl->count, pl->count);
    font_puts(s, 200, ui.pl_h - 10, tb, ui_col(s, 170, 170, 170));

    /* winamp-style resize grip in the bottom-right corner */
    Uint32 ghi = ui_col(s, 110, 110, 110), glo = ui_col(s, 20, 20, 20);
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
        SDL_FillRect(s, &mb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { mb.x, mb.y, mb.w, 1 }, b = { mb.x, mb.y + mb.h - 1, mb.w, 1 };
        SDL_Rect l = { mb.x, mb.y, 1, mb.h }, r = { mb.x + mb.w - 1, mb.y, 1, mb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        for (int i = 0; i < PM_N; i++) {
            Rect ir = pl_menu_item_rect(i);
            font_puts(s, ir.x + 8, ir.y + 3, PL_MENU[i].lbl, ui_col(s, 220, 220, 220));
        }
    }
    pthread_mutex_unlock(&p->mtx);
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
    return ui_pl_sel_has(idx);
}

static void pl_edit_clear_sel(void) {
    ui.pl_sel = -1;
    ui.pl_selall = 0;
    ui_pl_sel_reset();
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
        if (ui_pl_multi_grow(n) == 0) {
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
    ui_file_open();
}

void ui_info_open(void);

static void pl_menu_action(int act) {
    Player *p = ui.pl;
    switch (act) {
    case PM_INFO:
        ui_info_open();
        break;
    case PM_SAVEAS:
        pl_saveas_open();
        break;
    case PM_CLEAR:
        /* select-all is already the proven destructive path in PB_REM
           (stop + pl_clear + cur=-1 + sel reset): reuse it verbatim */
        ui.pl_selall = 1;
        ui_action(PB_REM);
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
                if (ui_pl_multi_grow(n) == 0) {
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

void ui_handle_pl_mouse(SDL_Event *e) {
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
            if (ui_in_rect(&r, x, y)) { pl_menu_action(PL_MENU[i].act); break; }
        }
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(ui.pl_w);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = PB_X; return; }
        if (x >= ui.pl_w - 12 && y >= ui.pl_h - 12) {
            ui.drag = 8;
            SDL_GetGlobalMouseState(&ui.drag_gx, &ui.drag_gy);
            ui.drag_wx = ui.pl_w; ui.drag_wy = ui.pl_h;
            return;
        }
        for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++)
            if (ui_in_rect(PL_BTNS[i].r, x, y)) { ui.pressed = PL_BTNS[i].id; return; }
        if (ui_in_rect(&ptext, x, y)) {
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
                    if (ui_pl_sel_has(idx)) pl_sel_del(idx);
                    else pl_sel_add(idx);
                    ui.pl_sel = idx;   /* anchor follows the ctrl click */
                } else if (shift && ui.pl_sel >= 0) {
                    ui.pl_selall = 0;
                    ui_pl_sel_reset();
                    int a = ui.pl_sel, b = idx;   /* anchor keeps its place,
                                                     further shift-clicks extend */
                    if (a > b) { int t = a; a = b; b = t; }
                    if (b >= pl->count) b = pl->count - 1;  /* stale anchor past end */
                    if (a <= b && ui_pl_multi_grow(b - a + 1) == 0) {
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
                               ui_pl_sel_has(idx);
                    ui.pl_collapse = keep ? idx : -1;
                    if (!keep) {
                        ui.pl_sel = idx;
                        ui.pl_selall = 0;
                        ui_pl_sel_reset();
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
        if (ui_in_rect(&pscroll, x, y)) { ui.drag = 5; return; }
        if (y < 22) { ui.drag = 4; ui_drag_begin(ui.wpl); }
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
                ui_pl_sel_reset();
                pl_sel_add(idx);
            }
        }
        if (ui.pl_dbl >= 0) {
            int idx = ui.pl_dbl;
            ui.pl_dbl = -1;
            if (!rowdrop && idx < pl->count) pl_post(p, CMD_PLAY, idx);
        }
        if (ui.pressed != B_NONE) {
            Rect xc = ui_close_rect(ui.pl_w);
            if (ui.pressed == PB_X && ui_in_rect(&xc, x, y)) ui_action(PB_CLOSE);
            for (size_t i = 0; i < sizeof PL_BTNS / sizeof PL_BTNS[0]; i++)
                if (PL_BTNS[i].id == ui.pressed && ui_in_rect(PL_BTNS[i].r, x, y))
                    ui_action(ui.pressed);
            ui.pressed = B_NONE;
        }
        if (ui.drag == 4) {
            /* dropped while snapped? lock flush under main */
            if (ui.pl_docked) {
                int dx, dy;
                ui_pl_dock_pos(&dx, &dy);
                SDL_SetWindowPosition(ui.wpl, dx, dy);
            } else
                ui_drag_bounce(ui.wpl, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
        }
        if (ui.drag == 4 || ui.drag == 5 || ui.drag == 8) ui.drag = 0;
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.drag == 4) {
            ui_drag_move_pl();
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
                ui.spl = lv_win_surface(ui.wpl);
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
   just-enough-scroll rule ui_draw_pl applies to the playing track. */
void ui_pl_cursor(int dir) {
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
    ui_pl_sel_reset();
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
int ui_pl_key(SDL_Event *e) {
    Player *p = ui.pl;
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_a:
        if (!ctrl || p->pl->count == 0) return 0;
        ui.pl_selall = 1;
        ui_pl_sel_reset();
        return 1;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (!ui.pl_selall && ui.pl_sel < 0 && ui.pl_multi_n == 0) return 0;
        ui_action(PB_REM);
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
            ui_pl_cursor(e->key.keysym.sym == SDLK_DOWN ? 1 : -1);
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
        ui_pl_sel_reset();
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
