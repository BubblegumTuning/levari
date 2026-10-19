#include "search.h"
#include "ui.h"
#include "../skin/skin.h"
#include "../playlist/playlist.h"
#include "../library/library.h"
#include "../common.h"
#include <stdlib.h>
#include <string.h>

enum { SB_NONE, SB_X, SB_SCOPE };

#define SRCH_W 340
#define SRCH_H (MAIN_H + PL_H) /* left column as tall as main+playlist: the trio forms a rectangle */
#define SRCH_MAXRES 256
#define SRCH_LINEH 17

typedef struct {
    char path[LV_MAX_PATH];
    char disp[512];
    int pl_idx; /* index in the playlist, -1 = library-only */
} Result;

typedef struct {
    char *path, *artist, *title, *album;
    char *nartist, *ntitle, *nalbum, *nfile; /* normalized for matching */
} LibEnt;

static Player *sp;
static SDL_Window *w;
static SDL_Surface *sf;
static int visible, pressed, drag, scroll, sel, nres;
static int scope; /* 0 = playlist, 1 = library */
static int selall; /* ctrl-a: whole query selected, next edit replaces it */
static char query[512];
static Result *res;
static int res_cap;
static LibEnt *libs;
static int lib_n, lib_cap, lib_loaded;
static int dgx, dgy, dwx, dwy;

static int in_rect(const Rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}
static Uint32 col(int r, int g, int b) { return SDL_MapRGB(sf->format, r, g, b); }

/* ---------- utf8 / normalization ---------- */

static int utf8_enc(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* fold ASCII case and fullwidth forms; CJK passes through untouched */
static void norm(const char *src, char *dst, size_t dn) {
    size_t o = 0;
    const char *q = src;
    while (*q && o + 4 < dn) {
        unsigned cp;
        int n = font_utf8_next(q, &cp);
        q += n;
        if (cp >= 'A' && cp <= 'Z') dst[o++] = (char)(cp + 32);
        else if (cp >= 0xFF01 && cp <= 0xFF5E) {
            char c = (char)(cp - 0xFEE0);
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            dst[o++] = c;
        } else if (cp == 0x3000) dst[o++] = ' ';
        else o += (size_t)utf8_enc(cp, dst + o);
    }
    dst[o] = 0;
}

/* split on spaces in-place, returns token count (max 8) */
static int tokenize(char *nq, const char **toks) {
    int n = 0;
    char *p = nq;
    while (*p && n < 8) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        toks[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return n;
}

static int word_start(char c) {
    return c == ' ' || c == '-' || c == '_' || c == '(' || c == '[';
}

/* token AND substring over title/artist/album/filename, weighted per field */
static int eval_fields(const char *F[4], const char *nq, const char **toks, int ntok) {
    static const int W[4] = { 100, 60, 40, 30 };
    int total = 0;
    for (int i = 0; i < ntok; i++) {
        int best = -1;
        for (int f = 0; f < 4; f++) {
            if (!F[f][0]) continue;
            const char *hit = strstr(F[f], toks[i]);
            if (!hit) continue;
            int sc = W[f];
            if (hit == F[f]) sc += 20;
            else if (word_start(hit[-1])) sc += 8;
            if (sc > best) best = sc;
        }
        if (best < 0) return -1;
        total += best;
    }
    for (int f = 0; f < 4; f++)
        if (F[f][0] && !strcmp(F[f], nq)) { total += 50; break; }
    return total;
}

/* ---------- library index cache ---------- */

static void lib_free(void) {
    for (int i = 0; i < lib_n; i++) {
        free(libs[i].path); free(libs[i].artist); free(libs[i].title);
        free(libs[i].album); free(libs[i].nartist); free(libs[i].ntitle);
        free(libs[i].nalbum); free(libs[i].nfile);
    }
    free(libs);
    libs = NULL; lib_n = lib_cap = 0;
}

static char *dupn(const char *s) { return strdup(s ? s : ""); }

static void lib_load(void) {
    lib_free();
    lib_loaded = 1;
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    FILE *f = fopen(idx, "r");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *f1 = line;
        char *f2 = strchr(f1, '\t'); if (!f2) continue; *f2++ = 0;
        char *f3 = strchr(f2, '\t'); if (!f3) continue; *f3++ = 0;
        char *f4 = strchr(f3, '\t'); if (!f4) continue; *f4++ = 0;
        char *f5 = strchr(f4, '\t'); if (f5) *f5 = 0;
        if (lib_n == lib_cap) {
            lib_cap = lib_cap ? lib_cap * 2 : 256;
            LibEnt *l2 = realloc(libs, (size_t)lib_cap * sizeof *libs);
            if (!l2) break;
            libs = l2;
        }
        LibEnt *e = &libs[lib_n++];
        e->path = dupn(f1); e->artist = dupn(f2); e->title = dupn(f3); e->album = dupn(f4);
        char fb[512];
        const char *b = strrchr(e->path, '/');
        b = b ? b + 1 : e->path;
        lv_strlcpy(fb, b, sizeof fb);
        char *dot = strrchr(fb, '.');
        if (dot && dot > fb) *dot = 0;
        size_t nn = strlen(fb) * 3 + 1;
        char *tmp = malloc(nn);
        norm(fb, tmp, nn);
        e->nfile = dupn(tmp);
        free(tmp);
        nn = strlen(e->title) * 3 + 1; tmp = malloc(nn); norm(e->title, tmp, nn); e->ntitle = dupn(tmp); free(tmp);
        nn = strlen(e->artist) * 3 + 1; tmp = malloc(nn); norm(e->artist, tmp, nn); e->nartist = dupn(tmp); free(tmp);
        nn = strlen(e->album) * 3 + 1; tmp = malloc(nn); norm(e->album, tmp, nn); e->nalbum = dupn(tmp); free(tmp);
    }
    fclose(f);
}

void search_lib_invalidate(void) { lib_loaded = 0; }

static int pl_find_path(const char *path) {
    int idx = -1;
    pthread_mutex_lock(&sp->mtx);
    for (int i = 0; i < sp->pl->count; i++)
        if (!strcmp(sp->pl->items[i].path, path)) { idx = i; break; }
    pthread_mutex_unlock(&sp->mtx);
    return idx;
}

/* ---------- search ---------- */

typedef struct { int score, seq; } Cand;

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    return x->seq - y->seq;
}

static void res_push(const char *path, const char *title, const char *artist, int pl_idx) {
    if (nres >= SRCH_MAXRES) return;
    if (nres == res_cap) {
        res_cap = res_cap ? res_cap * 2 : 64;
        Result *r2 = realloc(res, (size_t)res_cap * sizeof *res);
        if (!r2) return;
        res = r2;
    }
    Result *r = &res[nres++];
    lv_strlcpy(r->path, path, sizeof r->path);
    if (title && *title)
        snprintf(r->disp, sizeof r->disp, artist && *artist ? "%s - %s" : "%s",
                 title, artist && *artist ? artist : "");
    else {
        const char *b = strrchr(path, '/');
        lv_strlcpy(r->disp, b ? b + 1 : path, sizeof r->disp);
    }
    r->pl_idx = pl_idx;
}

static void run_search(void) {
    nres = 0; sel = 0; scroll = 0;
    char nq[512];
    norm(query, nq, sizeof nq);
    const char *toks[8];
    int ntok = tokenize(nq, toks);
    if (!ntok) return;

    if (scope == 0) {
        pthread_mutex_lock(&sp->mtx);
        int n = sp->pl->count;
        Cand *c = n > 0 ? malloc((size_t)n * sizeof *c) : NULL;
        if (n > 0 && !c) { pthread_mutex_unlock(&sp->mtx); return; }
        int cn = 0;
        for (int i = 0; i < n; i++) {
            Track *t = &sp->pl->items[i];
            char nt[768], na[768], nal[768], nf[768];
            norm(t->tags.title, nt, sizeof nt);
            norm(t->tags.artist, na, sizeof na);
            norm(t->tags.album, nal, sizeof nal);
            char fb[512];
            const char *b = strrchr(t->path, '/');
            b = b ? b + 1 : t->path;
            lv_strlcpy(fb, b, sizeof fb);
            char *dot = strrchr(fb, '.');
            if (dot && dot > fb) *dot = 0;
            norm(fb, nf, sizeof nf);
            const char *F[4] = { nt, na, nal, nf };
            int sc = eval_fields(F, nq, toks, ntok);
            if (sc >= 0) { c[cn].score = sc; c[cn].seq = i; cn++; }
        }
        qsort(c, (size_t)cn, sizeof *c, cand_cmp);
        for (int i = 0; i < cn && nres < SRCH_MAXRES; i++) {
            Track *t = &sp->pl->items[c[i].seq];
            res_push(t->path, t->tags.title, t->tags.artist, c[i].seq);
        }
        free(c);
        pthread_mutex_unlock(&sp->mtx);
    } else {
        if (!lib_loaded) lib_load();
        Cand *c = lib_n > 0 ? malloc((size_t)lib_n * sizeof *c) : NULL;
        if (lib_n > 0 && !c) return;
        int cn = 0;
        for (int i = 0; i < lib_n; i++) {
            const char *F[4] = { libs[i].ntitle, libs[i].nartist, libs[i].nalbum, libs[i].nfile };
            int sc = eval_fields(F, nq, toks, ntok);
            if (sc >= 0) { c[cn].score = sc; c[cn].seq = i; cn++; }
        }
        qsort(c, (size_t)cn, sizeof *c, cand_cmp);
        for (int i = 0; i < cn && nres < SRCH_MAXRES; i++) {
            LibEnt *e = &libs[c[i].seq];
            res_push(e->path, e->title, e->artist, pl_find_path(e->path));
        }
        free(c);
    }
}

static void do_jump(Result *r) {
    int idx = r->pl_idx;
    if (idx < 0) {
        pthread_mutex_lock(&sp->mtx);
        int at = sp->cur >= 0 && sp->cur < sp->pl->count ? sp->cur : sp->pl->count;
        pl_insert(sp->pl, at, r->path);
        idx = at;
        pthread_mutex_unlock(&sp->mtx);
    }
    if (sp->state != ST_STOP) {
        pl_post(sp, CMD_PLAY, idx);
    } else {
        pthread_mutex_lock(&sp->mtx);
        sp->cur = idx;
        pthread_mutex_unlock(&sp->mtx);
    }
}

/* ---------- window ---------- */

void search_init(Player *p) {
    sp = p;
    visible = pressed = drag = scroll = sel = nres = 0;
    scope = 0; selall = 0;
    query[0] = 0;
}

int search_is_open(void) { return visible; }

SDL_Window *search_window(void) { return w; }

Uint32 search_window_id(void) { return w ? SDL_GetWindowID(w) : 0; }

void search_open(SDL_Window *anchor) {
    if (!w) {
        int mx = 100, my = 100;
        if (anchor) SDL_GetWindowPosition(anchor, &mx, &my);
        int x = mx - SRCH_W, y = my;
        SDL_Rect disp;
        if (SDL_GetDisplayBounds(0, &disp) == 0) {
            if (x < disp.x) x = mx + MAIN_W;
            if (x + SRCH_W > disp.x + disp.w) x = disp.x + disp.w - SRCH_W;
            if (y + SRCH_H > disp.y + disp.h) y = disp.y + disp.h - SRCH_H;
            if (y < disp.y) y = disp.y;
        }
        w = SDL_CreateWindow("levari - search", x, y, SRCH_W, SRCH_H,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!w) return;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(w, icon);
            SDL_FreeSurface(icon);
        }
        sf = SDL_GetWindowSurface(w);
        if (anchor) {
            /* dock flush against the main window's left edge and follow it;
               re-read main's position in case the WM moved it after creation */
            SDL_GetWindowPosition(anchor, &mx, &my);
            x = mx - SRCH_W; y = my;
            if (SDL_GetDisplayBounds(0, &disp) == 0 && x < disp.x) x = mx + MAIN_W;
            ui_snap_next_to_main(w, &x, &y);
            SDL_SetWindowPosition(w, x, y);
        }
    }
    lib_loaded = 0; /* re-read the index on every open */
    selall = 0;
    visible = 1;
    SDL_ShowWindow(w);
    SDL_RaiseWindow(w);
    SDL_SetWindowInputFocus(w);
    SDL_StartTextInput();
    run_search();
}

void search_close(void) {
    visible = 0;
    if (w) SDL_HideWindow(w);
    SDL_StopTextInput();
}

void search_shutdown(void) {
    if (w) SDL_DestroyWindow(w);
    w = NULL; sf = NULL; visible = 0;
    lib_free();
    free(res);
    res = NULL; res_cap = 0;
}

void search_surface_refresh(void) {
    if (w) sf = SDL_GetWindowSurface(w);
}

static Rect srch_close_rect(void) {
    Rect r = { SRCH_W - 18, 4, 12, 12 };
    return r;
}
#define SRCH_EDIT_RECT   Rect edit = {6, 26, SRCH_W - 12 - 58, 18}
#define SRCH_SCOPE_RECT  Rect scopeb = {SRCH_W - 56, 26, 50, 18}
#define SRCH_LIST_RECT   Rect list = {6, 50, SRCH_W - 12, SRCH_H - 50 - 18}

static void draw_x(SDL_Surface *s, const Rect *r, int on) {
    skin_button(s, r, on, 0);
    font_puts(s, r->x + (r->w - font_w("X")) / 2, r->y + 3, "X", SDL_MapRGB(s->format, 220, 220, 220));
}

static void draw_edit(void) {
    SRCH_EDIT_RECT;
    SRCH_SCOPE_RECT;
    SDL_Rect b = { edit.x, edit.y, edit.w, edit.h };
    SDL_FillRect(sf, &b, col(0, 0, 0));
    SDL_Rect t1 = {edit.x, edit.y, edit.w, 1}, t2 = {edit.x, edit.y + edit.h - 1, edit.w, 1};
    SDL_Rect t3 = {edit.x, edit.y, 1, edit.h}, t4 = {edit.x + edit.w - 1, edit.y, 1, edit.h};
    Uint32 edge = col(90, 90, 90);
    SDL_FillRect(sf, &t1, edge); SDL_FillRect(sf, &t2, edge);
    SDL_FillRect(sf, &t3, edge); SDL_FillRect(sf, &t4, edge);

    int pad = 3, tw = font_w16(query);
    int off = tw > edit.w - 2 * pad - 6 ? tw - (edit.w - 2 * pad - 6) : 0;
    int x = edit.x + pad - off;
    if (selall && query[0]) {
        SDL_Rect hl = { x < edit.x + pad ? edit.x + pad : x, edit.y + 1,
                        (unsigned)tw, (unsigned)(edit.h - 2) };
        if (hl.x + (int)hl.w > edit.x + edit.w - pad)
            hl.w = edit.x + edit.w - pad - hl.x;
        if (hl.w > 0) SDL_FillRect(sf, &hl, col(40, 60, 120));
    }
    const char *q = query;
    while (*q) {
        if (x >= edit.x + edit.w - pad) break;
        unsigned cp;
        int n = font_utf8_next(q, &cp);
        if (x + font_cp_w16(cp) > edit.x + pad)
            font_putcp16_clip(sf, x, edit.y + 1, cp, col(255, 255, 255), edit.x + pad, edit.x + edit.w - pad);
        x += font_cp_w16(cp);
        q += n;
    }
    if ((SDL_GetTicks() / 500) % 2 == 0) {
        SDL_Rect c = { x < edit.x + edit.w - pad - 1 ? x : edit.x + edit.w - pad - 1,
                       edit.y + 2, 1, 14 };
        SDL_FillRect(sf, &c, col(0, 255, 0));
    }
    skin_button(sf, &scopeb, pressed == SB_SCOPE, 0);
    const char *lbl = scope ? "LIB" : "PL";
    font_puts(sf, scopeb.x + (scopeb.w - font_w(lbl)) / 2, scopeb.y + 3, lbl,
              scope ? col(0, 255, 0) : col(220, 220, 220));
}

void search_draw(void) {
    if (!sf) return;
    skin_window_bg(sf, SRCH_W, SRCH_H);
    font_puts(sf, 10, 6, "SEARCH", col(0, 224, 255));
    Rect xc = srch_close_rect();
    draw_x(sf, &xc, pressed == SB_X);
    draw_edit();

    SRCH_LIST_RECT;
    SDL_Rect lb = { list.x, list.y, list.w, list.h };
    SDL_FillRect(sf, &lb, col(0, 0, 0));
    int lines = list.h / SRCH_LINEH;
    if (scroll > nres - lines && nres >= lines) scroll = nres - lines;
    if (scroll < 0) scroll = 0;
    for (int i = 0; i < lines; i++) {
        int idx = scroll + i;
        if (idx >= nres) break;
        char line[512];
        lv_strlcpy(line, res[idx].disp, sizeof line);
        int fit = font_fit(line, list.w - 6);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = res[idx].pl_idx >= 0 ? col(170, 170, 170) : col(130, 130, 170);
        if (idx == sel) c = col(0, 255, 0);
        font_puts16(sf, list.x + 2, list.y + 1 + i * SRCH_LINEH, line, c);
    }
    if (nres > lines) {
        int th = list.h * lines / nres;
        if (th < 8) th = 8;
        int ty = list.y + (list.h - th) * scroll / (nres - lines);
        SDL_Rect r = { list.x + list.w - 7, ty, 5, th };
        SDL_FillRect(sf, &r, col(110, 110, 110));
    }
    char st[64];
    snprintf(st, sizeof st, "%d match%s%s", nres, nres == 1 ? "" : "es",
             scope ? " (library)" : " (playlist)");
    font_puts(sf, 6, SRCH_H - 14, st, col(150, 150, 150));
    if (!nres && query[0])
        font_puts16(sf, list.x + 2, list.y + 1, "(no matches)", col(150, 150, 150));
    else if (!query[0])
        font_puts16(sf, list.x + 2, list.y + 1, "(type to search)", col(110, 110, 110));
}

static void keep_scroll(void) {
    SRCH_LIST_RECT;
    int lines = list.h / SRCH_LINEH;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + lines) scroll = sel - lines + 1;
}

void search_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    SRCH_EDIT_RECT;
    SRCH_SCOPE_RECT;
    SRCH_LIST_RECT;
    int lines = list.h / SRCH_LINEH;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = srch_close_rect();
        if (in_rect(&xc, x, y)) { pressed = SB_X; return; }
        if (in_rect(&scopeb, x, y)) { pressed = SB_SCOPE; return; }
        if (in_rect(&edit, x, y)) {
            selall = 0;
            SDL_SetWindowInputFocus(w);
            SDL_StartTextInput();
            return;
        }
        if (in_rect(&list, x, y)) {
            int idx = scroll + (y - list.y) / SRCH_LINEH;
            if (idx < nres) { sel = idx; do_jump(&res[idx]); }
            return;
        }
        if (y < 22) {
            drag = 1;
            SDL_GetGlobalMouseState(&dgx, &dgy);
            SDL_GetWindowPosition(w, &dwx, &dwy);
        }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = srch_close_rect();
        if (pressed == SB_X && in_rect(&xc, x, y)) search_close();
        else if (pressed == SB_SCOPE && in_rect(&scopeb, x, y)) {
            scope = !scope;
            run_search();
        }
        pressed = SB_NONE;
        if (drag == 1) drag = 0;
    } else if (e->type == SDL_MOUSEMOTION && drag == 1) {
        if (!(e->motion.state & SDL_BUTTON_LMASK)) { drag = 0; return; }
        int gx, gy;
        SDL_GetGlobalMouseState(&gx, &gy);
        int x = dwx + gx - dgx, y = dwy + gy - dgy;
        ui_snap_next_to_main(w, &x, &y);
        SDL_SetWindowPosition(w, x, y);
    } else if (e->type == SDL_MOUSEWHEEL) {
        scroll -= e->wheel.y;
        if (scroll < 0) scroll = 0;
        if (scroll > nres - lines) scroll = nres > lines ? nres - lines : 0;
    }
}

static void del_last_cp(void) {
    size_t n = strlen(query);
    if (!n) return;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)query[i] & 0xC0) == 0x80) i--;
    query[i] = 0;
}

void search_key(SDL_Event *e) {
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_a:
        if (ctrl) selall = query[0] ? 1 : 0;
        break;
    case SDLK_c:
        if (ctrl) ui_clip_copy(query);
        break;
    case SDLK_x:
        if (ctrl && selall && query[0]) {
            ui_clip_copy(query);
            query[0] = 0; selall = 0; nres = 0; sel = scroll = 0;
        }
        break;
    case SDLK_v:
        if (ctrl) {
            if (selall) { query[0] = 0; selall = 0; }
            if (ui_clip_paste(query, sizeof query)) {
                if (query[0]) run_search();
                else { nres = 0; sel = scroll = 0; }
            }
        }
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (nres > 0) do_jump(&res[sel]);
        break;
    case SDLK_UP:
        if (sel > 0) { sel--; keep_scroll(); }
        break;
    case SDLK_DOWN:
        if (sel < nres - 1) { sel++; keep_scroll(); }
        break;
    case SDLK_BACKSPACE:
        if (selall) { query[0] = 0; selall = 0; nres = 0; sel = scroll = 0; }
        else if (query[0]) { del_last_cp(); run_search(); }
        break;
    case SDLK_DELETE:
        if (selall) { query[0] = 0; selall = 0; nres = 0; sel = scroll = 0; }
        break;
    case SDLK_ESCAPE:
        search_close();
        break;
    }
}

void search_text(const char *text) {
    if (selall) { query[0] = 0; selall = 0; }
    size_t len = strlen(query);
    const char *q = text;
    int changed = 0;
    while (*q && len + 4 < sizeof query) {
        unsigned cp;
        int n = font_utf8_next(q, &cp);
        if (cp >= 0x20 || cp == 0x09) {
            len += (size_t)utf8_enc(cp, query + len);
            query[len] = 0;
            changed = 1;
        }
        q += n;
    }
    if (changed) {
        if (query[0]) run_search();
        else { nres = 0; sel = scroll = 0; }
    }
}

/* the WM may swallow the button press that focuses the window, so when the
   search window gains focus check if the left button is still held over the
   title strip and start the drag from there */
void search_focus_drag(void) {
    if (!w || drag) return;
    int gx, gy, wx, wy, ww, wh;
    Uint32 mb = SDL_GetGlobalMouseState(&gx, &gy);
    if (!(mb & SDL_BUTTON_LMASK)) return;
    SDL_GetWindowPosition(w, &wx, &wy);
    SDL_GetWindowSize(w, &ww, &wh);
    if (gx >= wx && gx < wx + ww && gy >= wy && gy - wy < 22) {
        drag = 1;
        dgx = gx; dgy = gy; dwx = wx; dwy = wy;
    }
}
