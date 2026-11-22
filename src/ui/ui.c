#include "ui_state.h"
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

/* decode/mpris live behind function pointers wired by main (layering:
   ui.h includes neither decode.h nor mpris.h). Unset ffprobe hook reads
   as "0 = not compiled in" — the inert streaming states. */
static int (*ui_ffprobe)(char *ver_out, size_t ver_n, char *dir_out, size_t dir_n);
static int (*ui_raise_hook)(void);
void ui_set_ffprobe(int (*fn)(char *, size_t, char *, size_t)) { ui_ffprobe = fn; }
void ui_set_raise_hook(int (*fn)(void)) { ui_raise_hook = fn; }
int ui_ffprobe_info(char *ver, size_t vn, char *dir, size_t dn) {
    return ui_ffprobe ? ui_ffprobe(ver, vn, dir, dn) : 0;
}





static const Btn MAIN_BTNS[] = {
    {B_SHUF, &R_SHUF}, {B_REPEAT, &R_REPEAT}, {B_EQ, &R_EQ}, {B_VIS, &R_VIS},
    {B_LIST, &R_LIST},
#ifdef LV_URL_ENABLED
    /* the URL button beside LST (streaming-plan UI entry): absence IS
       the message in LV_STREAMING=OFF builds — no registered id, no
       draw, no hit-test, no ui_action */
    {B_URL, &R_URL},
#endif
    {B_PREV, &R_PREV}, {B_PLAY, &R_PLAY}, {B_PAUSE, &R_PAUSE},
    {B_STOP, &R_STOP}, {B_NEXT, &R_NEXT}, {B_OPEN, &R_OPEN},
    {B_ABA, &R_ABA}, {B_ABB, &R_ABB}, {B_SEARCH, &R_SEARCH}, {B_RPT1, &R_RPT1},
};



UI ui;
unsigned ui_deadline_ms;
const char *ui_dump_path;
int ui_show_eq;
int ui_show_vis;
int ui_show_search;

int ui_in_rect(const Rect *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* top-right corner close button, present on every window */
Rect ui_close_rect(int win_w) {
    Rect r = { win_w - 18, 4, 12, 12 };
    return r;
}
void ui_draw_x(SDL_Surface *s, const Rect *r, int pressed) {
    skin_button(s, r, pressed, 0);
    font_puts(s, r->x + (r->w - font_w("X")) / 2, r->y + 3, "X", SDL_MapRGB(s->format, 220, 220, 220));
}

#define SEEK_KNOB_W 24 /* seek knob: double the 12px vol/bias knobs */



Uint32 ui_col(SDL_Surface *s, int r, int g, int b) { return SDL_MapRGB(s->format, r, g, b); }

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

/* time readout: lv_fmt_time (common.h) — the tree's single M:SS/H:MM:SS
   format, shared with the daemon's status line */


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
            font_puts(s, 10, 6, "LEVARI", ui_col(s, 0, 224, 255));
            Rect x = ui_close_rect(MAIN_W);
            ui_draw_x(s, &x, ui.pressed == B_XMAIN);
        }

        /* row 1: play time in a box, 2x font, scrolling title beside it */
        if (rgn_hit(&R_TIME)) {
            char tb[16];
            lv_fmt_time(tb, sizeof tb, p->state == ST_STOP ? 0 : pos);
            font_puts_big(s, R_TIME.x + 6, R_TIME.y + 2, tb, ui_col(s, 0, 255, 0));
        }

        if (rgn_hit(&R_TSCROLL)) {
            int tx = R_TSCROLL.x + 2, tw = R_TSCROLL.w - 4;
            ui.marquee = 0;
            if (ui.msg[0]) {
                /* transient rescan feedback outranks even the viz bars: the
                   strip carries the message, plain red, for its 10 s */
                font_puts16(s, tx, R_TSCROLL.y + 1, ui.msg, ui_col(s, 255, 60, 60));
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
                        SDL_FillRect(s, &r, ui_col(s, 0, 200, 60));
                    }
                }
            } else if (font_w16(ui.title) <= tw) {
                font_puts16(s, tx, R_TSCROLL.y + 1, ui.title, ui_col(s, 0, 224, 255));
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
                        font_putcp16_clip(s, x, R_TSCROLL.y + 1, cp, ui_col(s, 0, 224, 255), tx, tx + tw);
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
        font_puts(s, R_INFO.x + 2, R_INFO.y + 1, rb, ui_col(s, 180, 180, 180));
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
        Uint32 c = ui_col(s, 220, 220, 220);
        if (b->id == B_SHUF) font_puts(s, b->r->x + (b->r->w - font_w("SHUFFLE")) / 2, b->r->y + 3, "SHUFFLE", active ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_REPEAT) font_puts(s, b->r->x + (b->r->w - font_w("REPEAT")) / 2, b->r->y + 3, "REPEAT", active ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_EQ) font_puts(s, b->r->x + 6, b->r->y + 3, "EQ", active ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_VIS) font_puts(s, b->r->x + 5, b->r->y + 3, "VIS", active ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_LIST) font_puts(s, b->r->x + (b->r->w - font_w("LST")) / 2, b->r->y + 3, "LST", c);
#ifdef LV_URL_ENABLED
        if (b->id == B_URL) font_puts(s, b->r->x + (b->r->w - font_w("URL")) / 2, b->r->y + 3, "URL",
                                      ui.url_visible ? ui_col(s, 0, 255, 0) : c);
#endif
        if (b->id == B_SEARCH) font_puts(s, b->r->x + (b->r->w - font_w("SRCH")) / 2, b->r->y + 3,
                                         "SRCH", search_is_open() ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_RPT1) font_puts(s, b->r->x + (b->r->w - font_w("RPT1")) / 2, b->r->y + 3,
                                      "RPT1", active ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_ABA) font_puts(s, b->r->x + 3, b->r->y + 3, "A",
                                      p->ab_a >= 0 ? ui_col(s, 0, 255, 0) : c);
        if (b->id == B_ABB) font_puts(s, b->r->x + 3, b->r->y + 3, "B",
                                      p->ab_b >= 0 ? ui_col(s, 0, 255, 0) : c);
        draw_icon(s, b->id, b->r, c);
    }

    /* row 3: labeled volume + balance sliders share the row */
    {
        Rect vrow = { 8, 62, MAIN_W - 16, 13 };
        if (rgn_hit(&vrow)) {
            font_puts(s, 8, 65, "VOL", ui_col(s, 150, 150, 150));
            font_puts(s, 158, 65, "BIAS", ui_col(s, 150, 150, 150));
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
            SDL_FillRect(s, &mb, ui_col(s, 40, 40, 40));
            SDL_Rect t = { mi.x, mi.y, mi.w, 1 }, b = { mi.x, mi.y + mi.h - 1, mi.w, 1 };
            SDL_Rect l = { mi.x, mi.y, 1, mi.h }, r = { mi.x + mi.w - 1, mi.y, 1, mi.h };
            Uint32 bc = ui_col(s, 140, 140, 140);
            SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
            SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
            font_puts(s, mi.x + 8, mi.y + 6, "OPTIONS...", ui_col(s, 220, 220, 220));
        }
    }
        LV_ACC(wid_us);
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
void ui_msg(const char *text) {
    lv_strlcpy(ui.msg, text, sizeof ui.msg);
    ui.msg_at = SDL_GetTicks();
}


void ui_action(int id) {
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
    case B_SHUF: p->shuffle = !p->shuffle; ui_shuffle_persist(); break;
    case B_REPEAT: p->repeat = p->repeat == 1 ? 0 : 1; break;
    case B_RPT1: p->repeat = p->repeat == 2 ? 0 : 2; break;
#ifdef LV_URL_ENABLED
    case B_URL:
        if (ui.url_visible) ui_url_close();
        else ui_urlwin_open();
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
                ui_pl_dock_pos(&dx, &dy);
                SDL_SetWindowPosition(ui.wpl, dx, dy);
            }
            SDL_ShowWindow(ui.wpl);
        } else SDL_HideWindow(ui.wpl);
        break;
    case PB_SHUF: p->shuffle = !p->shuffle; ui_shuffle_persist(); break;
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
            ui_pl_sel_reset();
        } else if (ui.pl_multi_n > 0) {
            /* multi-removal: walk the sorted set high-to-low so removals
               never shift a still-pending index; p->cur drops by the number
               of removed rows below it (CMD_STOP keeps p->cur, exactly as
               the single-row path relied on: deleting the current row lets
               cur fall onto its successor) */
            if (p->cur >= 0 && ui_pl_sel_has(p->cur)) pl_post(p, CMD_STOP, 0);
            int below = 0;
            for (int i = 0; i < ui.pl_multi_n && p->cur >= 0; i++)
                if (ui.pl_multi[i] < p->cur) below++; else break;
            pthread_mutex_lock(&p->mtx);
            for (int i = ui.pl_multi_n - 1; i >= 0; i--)
                pl_remove(p->pl, ui.pl_multi[i]);  /* stale-high clamped inside */
            if (p->cur >= 0) p->cur -= below;
            pthread_mutex_unlock(&p->mtx);
            ui.pl_sel = -1;
            ui_pl_sel_reset();
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
    case B_OPEN: case PB_ADD: ui_file_open(); break;
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
        if (ui_in_rect(&mi, x, y)) ui_opt_open();
        return;
    }
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        /* a button press cancels any latched drag (ui_focus_click_drag can
           latch one before the click-through press arrives): without this
           the up handler's drag==0 gate swallows the click and leaves the
           button painted down */
        Rect xc = ui_close_rect(MAIN_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = B_XMAIN; ui.drag = 0; return; }
        for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++)
            if (ui_in_rect(MAIN_BTNS[i].r, x, y)) { ui.pressed = MAIN_BTNS[i].id; ui.drag = 0; return; }
        if (ui_in_rect(&R_SEEK, x, y)) {
            ui.drag = 1;
            pl_post_seek(p, (double)(x - R_SEEK.x - SEEK_KNOB_W / 2) / (double)(R_SEEK.w - SEEK_KNOB_W));
            return;
        }
        if (ui_in_rect(&R_VOL, x, y)) {
            ui.drag = 2;
            p->vol = (x - R_VOL.x - 2) * 128 / (R_VOL.w - 16);
            if (p->vol < 0) p->vol = 0;
            if (p->vol > 128) p->vol = 128;
            return;
        }
        if (ui_in_rect(&R_PAN, x, y)) {
            ui.drag = 10;
            p->pan = (x - R_PAN.x - 2) * 64 / (R_PAN.w - 16) - 32;
            if (p->pan < -32) p->pan = -32;
            if (p->pan > 32) p->pan = 32;
            return;
        }
        if (y < 22) {
            ui.drag = 3; ui_drag_begin(ui.wmain);
            /* follow bookkeeping is drag-scoped too: ui_drag_begin
               re-baselines every carried window */
        }
    } else if (e->type == SDL_MOUSEBUTTONUP && ui.drag == 0 && ui.pressed != B_NONE) {
        Rect xc = ui_close_rect(MAIN_W);
        if (ui.pressed == B_XMAIN && ui_in_rect(&xc, x, y)) {
            SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); /* quits every window */
        }
        for (size_t i = 0; i < sizeof MAIN_BTNS / sizeof MAIN_BTNS[0]; i++)
            if (MAIN_BTNS[i].id == ui.pressed && ui_in_rect(MAIN_BTNS[i].r, x, y))
                ui_action(ui.pressed);
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
            ui_drag_move(ui.wmain);
            int wx, wy;
            SDL_GetWindowPosition(ui.wmain, &wx, &wy);
            if (ui.pl_docked && ui.pl_visible)
                ui_follow_move(ui.wpl, &ui.follow_pl, wx, wy + MAIN_H);
            for (int i = 0; i < SNAP_NSLOT; i++)
                if (ui.snap[i].w)
                    ui_follow_move(ui.snap[i].w, &ui.follow_snap[i], wx + ui.snap[i].dx, wy + ui.snap[i].dy);
        }
    }
    if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.drag == 3) {
            /* wall-facing carried window -> main jumps back flush to the
               dock instead of the wall kick (ui_drag_dock_restore) */
            if (!ui_drag_dock_restore())
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
    case SDLK_s: p->shuffle = !p->shuffle; ui_shuffle_persist(); break;
    case SDLK_r: p->repeat = !p->repeat; break;
    case SDLK_RIGHT: pl_post_seek(p, (double)(p->pos_frames - p->track_start + p->rate * 5) /
                                      (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_LEFT: pl_post_seek(p, (double)(p->pos_frames - p->track_start - p->rate * 5) /
                                     (double)((p->track_end > 0 ? p->track_end : p->total_frames) - p->track_start)); break;
    case SDLK_UP:
    case SDLK_DOWN:
        /* arrows walk the playlist cursor from any window (ui_pl_key
           already claimed the playlist window's own); Alt stays with the
           row-move path, Ctrl passes through */
        if (e->key.keysym.mod & (KMOD_ALT | KMOD_CTRL)) break;
        ui_pl_cursor(e->key.keysym.sym == SDLK_DOWN ? 1 : -1);
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
    case SDLK_o: ui_file_open(); break;
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
   laptops. Now events mark their window dirty, a poll tick (1 s, or only
   while an animation is live — a 10 Hz beat, and the visualizer's own
   cfg.vis_hz rate while the band runs) catches player-thread changes, and
   only dirty windows are repainted. Unfocused windows are not repainted at all
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
unsigned ui_dirty;

static void win_present(SDL_Window *w) {
    if (w && !(SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED))
        lv_win_present(w, NULL, 0);
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
    lv_win_present(w, rgn_n ? rgn_rects : NULL, rgn_n);
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
        lv98_tracef("ui:sdl_init:fail [%s]", SDL_GetError());
#if defined(LV_WIN9X) && defined(_WIN32)
        /* GUI subsystem: stderr goes nowhere; and on 9x the *W* dialog
           APIs are dead stubs — MessageBoxA unconditionally, else a
           video-init failure is a silent process exit (hardware
           2026-11-17: died instantly, no window, no message). */
        {
            char msg[512];
            SDL_snprintf(msg, sizeof msg,
                         "SDL_Init failed: %s\n\nOn Windows 98 a window "
                         "usually needs unicows.dll (MS Layer for "
                         "Unicode) next to levari.exe. If it is already "
                         "there, see WIN98-NOTES.txt.", SDL_GetError());
            MessageBoxA(NULL, msg, "levari", MB_OK | MB_ICONERROR);
        }
#endif
        return -1;
    }
    lv98_tracef("ui:sdl_init:ok");
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
       macOS: with the hint SDL_GetWindowSurface returns NULL on the
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
    lv98_tracef("ui:windows wmain=%p wpl=%p", (void *)ui.wmain,
                (void *)ui.wpl);
    if (!ui.wmain || !ui.wpl) {
        fprintf(stderr, "levari: window: %s\n", SDL_GetError());
        lv98_tracef("ui:window:fail [%s]", SDL_GetError());
#if defined(LV_WIN9X) && defined(_WIN32)
        /* same silent-death class as the SDL_Init path (hardware
           2026-11-17): the GUI subsystem owns no stderr */
        {
            char msg[512];
            SDL_snprintf(msg, sizeof msg, "Window creation failed: %s",
                         SDL_GetError());
            MessageBoxA(NULL, msg, "levari", MB_OK | MB_ICONERROR);
        }
#endif
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
    ui.smain = lv_win_surface(ui.wmain);
    ui.spl = lv_win_surface(ui.wpl);
    if (ui.weq) ui.seq = lv_win_surface(ui.weq);
    /* Tier-0 guard: an unsupported depth/visual must name itself, not
       SIGSEGV on the first draw (lv_win_surface already told the terminal
       why when a takeover attempt failed). */
    if (!ui.smain || !ui.spl) {
        fprintf(stderr, "levari: no drawable surface on this display — "
                        "the screen depth/visual is unsupported\n");
        return -1;
    }
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 14 telemetry: the dark/white-paint question is a surface-
       format-vs-bpp question — name the formats the moment they exist.
       Batch 15 correction: ->format is the SDL_PixelFormat* — print
       the FORMAT ENUM and bpp, not the pointer. */
    lv98_tracef("surf fmt=%u/%ubpp,%u/%ubpp,%u/%ubpp sz=%dx%d",
                (unsigned)ui.smain->format->format,
                (unsigned)ui.smain->format->BitsPerPixel,
                ui.spl ? (unsigned)ui.spl->format->format : 0u,
                ui.spl ? (unsigned)ui.spl->format->BitsPerPixel : 0u,
                ui.seq ? (unsigned)ui.seq->format->format : 0u,
                ui.seq ? (unsigned)ui.seq->format->BitsPerPixel : 0u,
                ui.smain->w, ui.smain->h);
#endif
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
    lv98_tracef("ui:loop:enter");
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 13 telemetry: heartbeat distinguishes a dead loop from a
       saturated one from a live-but-blind one (hardware 2026-11-17:
       painted once, went white, hover-hourglass = pump starvation). */
    unsigned hb_ev = 0;
    int hb_last = -1;
    Uint32 hb_due = SDL_GetTicks() + 2000;
#endif
    while (running) {
        if (ui_deadline_ms && SDL_GetTicks() >= ui_deadline_ms) break;
        if (ui_raise_hook && ui_raise_hook()) ui_raise_all(); /* MPRIS Raise request */
        SDL_Event e;
        int wait = next ? (int)(next > SDL_GetTicks() ? next - SDL_GetTicks() : 0)
                        : -1;   /* no tick scheduled: sleep until an event */
        /* a set deadline must also bound the UNTIMED wait: with wait=-1
           the loop sleeps forever whenever nothing scheduled a redraw
           (stopped/undecodable playlist), and the break above is never
           reached — selftest runs outlived their deadline by minutes */
        if (ui_deadline_ms && (wait < 0 || wait > 1000)) wait = 1000;
        int got = SDL_WaitEventTimeout(&e, wait);
#if defined(LV_WIN9X) && defined(_WIN32)
        if (got) { hb_ev++; hb_last = e.type; }
        if ((long)(SDL_GetTicks() - hb_due) >= 0) {
            lv98_tracef("hb tick=%u ev=%u lastevt=%d dirty=%u wait=%d",
                        SDL_GetTicks(), hb_ev, hb_last, (unsigned)ui_dirty,
                        wait);
            hb_due = SDL_GetTicks() + 2000;
        }
        {   /* batch 14: first-arrival census per SDL event TYPE (the
               lastevt number alone hides whether motion/key ever
               arrive; 16 distinct types bound the log). Only when an
               event was actually received - e is uninitialized on a
               timed-out wait. */
            static int evt_seen[16];
            static int evt_n;
            if (got) {
                int i;
                for (i = 0; i < evt_n; i++)
                    if (evt_seen[i] == (int)e.type)
                        break;
                if (i == evt_n && evt_n < (int)(sizeof evt_seen)) {
                    evt_seen[evt_n++] = e.type;
                    /* batch 15: +win — windowID sits at offset 8 in
                       all of window/motion/button/key (button's view
                       reads the others correctly). */
                    lv98_tracef("evt first type=%d win=%u", e.type,
                                e.button.windowID);
                }
            }
        }
#endif
        if (got) switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_WINDOWEVENT:
            if (e.window.windowID == SDL_GetWindowID(ui.wmain)) ui.smain = lv_win_surface(ui.wmain);
            if (e.window.windowID == SDL_GetWindowID(ui.wpl)) ui.spl = lv_win_surface(ui.wpl);
            if (ui.weq && e.window.windowID == SDL_GetWindowID(ui.weq)) ui.seq = lv_win_surface(ui.weq);
            if (ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) ui.sfile = lv_win_surface(ui.wfile);
            if (ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) ui.sopt = lv_win_surface(ui.wopt);
            if (ui.winfo && e.window.windowID == SDL_GetWindowID(ui.winfo)) ui.sinfo = lv_win_surface(ui.winfo);
#ifdef LV_URL_ENABLED
            if (ui.wurl && e.window.windowID == SDL_GetWindowID(ui.wurl)) ui.surl = lv_win_surface(ui.wurl);
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
                ui_focus_click_drag(e.window.windowID);
                if (search_is_open() && e.window.windowID == search_window_id()) SDL_StartTextInput();
                else if (ui.opt_visible && ui.wopt && e.window.windowID == SDL_GetWindowID(ui.wopt)) SDL_StartTextInput();
                else if (ui.file_visible && ui.wfile && e.window.windowID == SDL_GetWindowID(ui.wfile)) SDL_StartTextInput();
#ifdef LV_URL_ENABLED
                else if (ui.url_visible && ui.wurl && e.window.windowID == SDL_GetWindowID(ui.wurl)) SDL_StartTextInput();
#endif
                else SDL_StopTextInput();
                /* the set surfaces as a unit: when focus arrives from another
                   app, raise every visible window, not just the one the WM
                   clicked. On Cocoa ui_raise_all() is a no-op; on X11 raising
                   can itself refocus (Xfwm), so ui_raise_all() rate-limits
                   itself to keep that from becoming a storm */
                if (!was_focused) ui_raise_all();
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
                    if (isym == SDLK_ESCAPE) { ui_info_close(); ui_dirty |= D_INFO; }
                    else if (ictrl && (isym == SDLK_c || isym == SDLK_INSERT)) { ui_info_copy_sel(); }
                    else if (ictrl && isym == SDLK_a) { ui_dirty |= D_INFO; }
                    else handle_key(&e);
                }
                else
#ifdef LV_URL_ENABLED
                if (ui.url_visible && ui.wurl && ui.focus_wid == SDL_GetWindowID(ui.wurl)) {
                    /* the URL field eats EVERY key while its window holds
                       focus (file-browser style): global playback hotkeys
                       must not fire mid-paste */
                    ui_url_key(&e);
                    ui_dirty |= D_URL;
                    next = SDL_GetTicks(); /* paint the edit now */
                }
                else
#endif
                if (search_is_open() && ui.focus_wid == search_window_id()) search_key(&e);
                else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) ui_opt_key(&e);
                else if (ui.file_visible && ui.wfile && ui.focus_wid == SDL_GetWindowID(ui.wfile)) {
                    /* the file field eats its keys BEFORE the global
                       shortcuts: typing 'c'/'o' must not pause or reopen */
                    if (ui_file_key(&e)) {
                        ui_dirty |= D_FILE;
                        next = SDL_GetTicks(); /* paint the edit now */
                    } else handle_key(&e);
                }
                else if (ui.pl_visible && ui.wpl && ui.focus_wid == SDL_GetWindowID(ui.wpl)) {
                    if (!ui_pl_key(&e)) handle_key(&e);
                } else handle_key(&e);
            }
            ui_dirty |= D_MAIN | D_PL | D_SRCH | D_OPT;
            break;
        case SDL_TEXTINPUT:
            if (search_is_open() && ui.focus_wid == search_window_id()) search_text(e.text.text);
            else if (ui.opt_visible && ui.wopt && ui.focus_wid == SDL_GetWindowID(ui.wopt)) ui_opt_text(e.text.text);
#ifdef LV_URL_ENABLED
            else if (ui.url_visible && ui.wurl && ui.focus_wid == SDL_GetWindowID(ui.wurl) &&
                     ui.url_focus) {
                ui_url_text(e.text.text);
                ui_dirty |= D_URL;
                next = SDL_GetTicks();
            }
#endif
            else if (ui.file_visible && ui.wfile && ui.focus_wid == SDL_GetWindowID(ui.wfile) &&
                     ui.file_focus) {
                ui_file_text(e.text.text);
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
#if defined(LV_WIN9X) && defined(_WIN32)
            {   /* batch 15: echo at dispatch entry, before any hit
                   logic — if this prints but the UI does not react,
                   the fault is in levari's routing/hit-test, not the
                   event chain. 24 cap. */
                static int click_n;
                if (click_n < 24) {
                    click_n++;
                    lv98_tracef("click b=%d x=%d y=%d win=%u",
                                (int)e.button.button, e.button.x,
                                e.button.y, e.button.windowID);
                }
            }
#endif
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEWHEEL: {
            Uint32 wid = ev_win(&e);
            /* a swallowed button-release must not leave a drag latched on */
            if (e.type == SDL_MOUSEMOTION && ui.drag &&
                !(e.motion.state & SDL_BUTTON_LMASK)) ui.drag = 0;
            if (wid == SDL_GetWindowID(ui.wmain)) { handle_main_mouse(&e); ui_dirty |= D_MAIN; }
            else if (wid == SDL_GetWindowID(ui.wpl)) { ui_handle_pl_mouse(&e); ui_dirty |= D_PL; }
            else if (ui.weq && wid == SDL_GetWindowID(ui.weq)) { ui_handle_eq_mouse(&e); ui_dirty |= D_EQ; }
            else if (ui.wfile && wid == SDL_GetWindowID(ui.wfile)) { ui_handle_file_mouse(&e); ui_dirty |= D_FILE; }
            else if (ui.wopt && wid == SDL_GetWindowID(ui.wopt)) { ui_handle_opt_mouse(&e); ui_dirty |= D_OPT; }
            else if (ui.winfo && wid == SDL_GetWindowID(ui.winfo)) { ui_handle_info_mouse(&e); ui_dirty |= D_INFO; }
#ifdef LV_URL_ENABLED
            else if (ui.wurl && wid == SDL_GetWindowID(ui.wurl)) { ui_handle_url_mouse(&e); ui_dirty |= D_URL; }
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
            if (ui.opt_verifying) {
                ui.opt_verifying = 0;
                if (ui.opt_vfy_rc == -3)
                    lv_strlcpy(ui.opt_status, "index: run a full scan",
                               sizeof ui.opt_status);
                else if (ui.opt_vfy_rc != 0)
                    lv_strlcpy(ui.opt_status, "verify failed (index untouched)",
                               sizeof ui.opt_status);
                else if (ui.opt_vfy_purged)
                    snprintf(ui.opt_status, sizeof ui.opt_status,
                             "dropped %d (%d kept, %d unreachable)",
                             ui.opt_vfy_purged, ui.opt_vfy_kept,
                             ui.opt_vfy_unreach);
                else if (ui.opt_vfy_unreach)
                    snprintf(ui.opt_status, sizeof ui.opt_status,
                             "all %d rows kept (%d unreachable)",
                             ui.opt_vfy_kept, ui.opt_vfy_unreach);
                else
                    lv_strlcpy(ui.opt_status, "all rows verified",
                               sizeof ui.opt_status);
            }
            else if (ui.opt_scan_skipped)
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
           drag, scan) — except a live visualizer, which drives the tick at
           its own cfg.vis_hz rate (10/24/30/60 Hz) — else 1 Hz for the
           position text. An unfocused app sleeps on events, except a live
           visualizer (the vis
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
               pace with it — at the band's cfg.vis_hz rate). */
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
                if ((ui_dirty & D_PL) && ui.pl_visible) { ui_draw_pl(); win_present(ui.wpl); }
                if ((ui_dirty & D_EQ) && ui.eq_visible && ui.weq) { ui_draw_eq(); win_present(ui.weq); }
                if (((ui_dirty & D_FILE) || file_focused) && ui.file_visible && ui.sfile) { ui_draw_file(); win_present(ui.wfile); }
                if ((ui_dirty & D_OPT) && ui.opt_visible && ui.sopt) { ui_draw_opt(); win_present(ui.wopt); }
                if ((ui_dirty & D_INFO) && ui.info_visible && ui.sinfo) { ui_draw_info(); win_present(ui.winfo); }
#ifdef LV_URL_ENABLED
                if (((ui_dirty & D_URL) || url_focused) && ui.url_visible && ui.surl) { ui_draw_url(); win_present(ui.wurl); }
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
                if ((ui_dirty & D_PL) && ui.pl_visible) { ui_draw_pl(); win_present(ui.wpl); }
                if ((ui_dirty & D_EQ) && ui.eq_visible && ui.weq) { ui_draw_eq(); win_present(ui.weq); }
                if ((ui_dirty & D_FILE) && ui.file_visible && ui.sfile) { ui_draw_file(); win_present(ui.wfile); }
                if ((ui_dirty & D_OPT) && ui.opt_visible && ui.sopt) { ui_draw_opt(); win_present(ui.wopt); }
                if ((ui_dirty & D_INFO) && ui.info_visible && ui.sinfo) { ui_draw_info(); win_present(ui.winfo); }
#ifdef LV_URL_ENABLED
                if ((ui_dirty & D_URL) && ui.url_visible && ui.surl) { ui_draw_url(); win_present(ui.wurl); }
#endif
                if ((ui_dirty & D_SRCH) && so && search_window()) { search_draw(); win_present(search_window()); }
                ui_dirty = 0;
                wake = 0;
            }

            /* vis_live is part of anim, so a live visualizer drives the anim
               tick at its own cfg.vis_hz rate (10/24/30/60 Hz, default 10 —
               the ui_vis_frame_ms switch) whether focused or not; every other
               live animation keeps the 100 ms beat. A focused search field
               redraws on each 500 ms caret boundary (2 Hz, just enough to
               toggle it).
               Focused idle polls at 1 Hz for player-thread state changes;
               unfocused but playing polls at 1 Hz so the time readout and
               position slider keep pace instead of jumping on refocus;
               unfocused idle sleeps on events. */
            if (anim) next = now + (vis_live ? ui_vis_frame_ms(cfg.vis_hz) : 100);
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
    if (ui.wopt) { lv_win_release(ui.wopt); SDL_DestroyWindow(ui.wopt); }
    if (ui.wfile) { lv_win_release(ui.wfile); SDL_DestroyWindow(ui.wfile); }
    if (ui.winfo) { lv_win_release(ui.winfo); SDL_DestroyWindow(ui.winfo); }
#ifdef LV_URL_ENABLED
    if (ui.wurl) { lv_win_release(ui.wurl); SDL_DestroyWindow(ui.wurl); }
#endif
    if (ui.weq) lv_win_release(ui.weq);
    if (ui.wpl) lv_win_release(ui.wpl);
    if (ui.wmain) lv_win_release(ui.wmain);
    SDL_DestroyWindow(ui.weq);
    SDL_DestroyWindow(ui.wpl);
    SDL_DestroyWindow(ui.wmain);
    SDL_Quit();
    return 0;
}
