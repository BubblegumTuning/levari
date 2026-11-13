/* x8bit.c — 8-bit PseudoColor/GrayScale presentation path (X11).
   Header notes in x8bit.h. Two halves: a pure ARGB->palette-index
   quantizer (no X dependency: unit-testable headlessly, see
   test/x8bit/testx8bit.c) and the Xlib takeover machinery that feeds
   it, reached only when SDL_GetWindowSurface says no. */
#include "x8bit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- quantizer (pure, headless-testable) ---------------- */

/* 4x4 Bayer matrix (thresholds 0..15). Ordered dither is what keeps the
   skins' gradient ramps from banding on a 6-level-per-channel cube;
   the FFT-free cost is one 256-byte table lookup per channel. */
static const unsigned char X8_BAYER[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/* Q6[phase][v] -> 0..5 (the cube level for byte v at dither phase p).
   Levels are 51*k (k=0..5, so 255 is exact); +25 is the half-step
   rounding threshold. */
static unsigned char X8_Q6[16][256];
static int x8_q6_ready;

static void x8_q6_init(void)
{
    for (int p = 0; p < 16; p++) {
        for (int v = 0; v < 256; v++) {
            int q = v + ((int)X8_BAYER[p] - 8) * 51 / 16;
            if (q < 0) q = 0;
            else if (q > 255) q = 255;
            X8_Q6[p][v] = (unsigned char)((q + 25) / 51);
        }
    }
    x8_q6_ready = 1;
}

/* src is ARGB8888 (little-endian bytes: B,G,R,A), dst one index byte per
   pixel; both addressed by pitch. cube[] maps a cube index (r*36+g*6+b)
   to the byte actually written (identity in tests, the allocated palette
   cells in the live path). */
static void x8_quant(const unsigned char *src, int spitch, unsigned char *dst,
                     int dpitch, const unsigned char *cube, int rx, int ry,
                     int rw, int rh)
{
    if (!x8_q6_ready)
        x8_q6_init();
    for (int y = ry; y < ry + rh; y++) {
        const unsigned char *sp = src + (size_t)y * spitch + (size_t)rx * 4;
        unsigned char *dp = dst + (size_t)y * dpitch + rx;
        int py = (y & 3) << 2;
        for (int x = 0; x < rw; x++) {
            /* dither phase is window-absolute in BOTH axes: a partial
               (dirty-rect) pass must continue the pattern its neighbours
               show, or a rect edge reads as a seam on mid-tone ramps */
            const unsigned char *t = X8_Q6[py | ((rx + x) & 3)];
            *dp++ = cube[t[sp[2]] * 36 + t[sp[1]] * 6 + t[sp[0]]];
            sp += 4;
        }
    }
}

/* ------------------------- Xlib takeover half ------------------------ */

#if defined(LV_X8BIT) && defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
#define LV_X8_ACTIVE 1
#else
#define LV_X8_ACTIVE 0
#endif

#if LV_X8_ACTIVE
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <SDL_syswm.h>

/* dlopen'd subset of libX11 core (see x8bit.h for why not -lX11). */
static struct {
    Status (*GetWindowAttributes)(Display *, Window, XWindowAttributes *);
    GC (*CreateGC)(Display *, Drawable, unsigned long, XGCValues *);
    int (*FreeGC)(Display *, GC);
    Status (*AllocColor)(Display *, Colormap, XColor *);
    Colormap (*CreateColormap)(Display *, Window, Visual *, int);
    int (*StoreColors)(Display *, Colormap, XColor *, int);
    int (*SetWindowColormap)(Display *, Window, Colormap);
    XImage *(*CreateImage)(Display *, Visual *, unsigned, int, int, char *,
                           unsigned, unsigned, int, int);
    int (*DestroyImage)(XImage *);
    int (*PutImage)(Display *, Drawable, GC, XImage *, int, int, int, int,
                    unsigned, unsigned);
    int (*Flush)(Display *);
} x8x;
static int x8x_state; /* 0 untried, 1 loaded, -1 unavailable */

static int x8x_load(void)
{
    if (x8x_state)
        return x8x_state > 0;
    x8x_state = -1;
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return 0;
    int ok = 1;
#define X8_BIND(fn) do { *(void **)&x8x.fn = dlsym(h, "X" #fn); if (!x8x.fn) ok = 0; } while (0)
    X8_BIND(GetWindowAttributes);
    X8_BIND(CreateGC);
    X8_BIND(FreeGC);
    X8_BIND(AllocColor);
    X8_BIND(CreateColormap);
    X8_BIND(StoreColors);
    X8_BIND(SetWindowColormap);
    X8_BIND(CreateImage);
    X8_BIND(DestroyImage);
    X8_BIND(PutImage);
    X8_BIND(Flush);
#undef X8_BIND
    if (ok)
        x8x_state = 1;
    return ok;
}

struct x8win {
    SDL_Window *w;
    Display *dpy;
    Window win;
    GC gc;
    Visual *visual;
    SDL_Surface *surf;      /* the ARGB8888 surface the UI draws on */
    XImage *img;            /* depth-8 ZPixmap over the same extent */
    unsigned char cube[256];/* cube index -> colormap cell byte */
    int dead;               /* unsupported: no retries, no repeat reports */
};
#define X8_MAXW 8           /* main, pl, eq, file, opt, info, search, url */
static struct x8win X8W[X8_MAXW];

static struct x8win *x8_find(SDL_Window *w)
{
    for (int i = 0; i < X8_MAXW; i++)
        if (X8W[i].w == w)
            return &X8W[i];
    return NULL;
}

static void x8_free(struct x8win *e)
{
    if (e->img) x8x.DestroyImage(e->img);
    if (e->gc) x8x.FreeGC(e->dpy, e->gc);
    if (e->surf) SDL_FreeSurface(e->surf);
    memset(e, 0, sizeof *e);
}

static int x8_img(struct x8win *e)
{
    int w = e->surf->w, h = e->surf->h;
    int bpl = (w + 31) & ~31; /* pad to 32: XCreateImage's default unit */
    char *buf = malloc((size_t)bpl * (size_t)h);
    if (!buf)
        return 0;
    e->img = x8x.CreateImage(e->dpy, e->visual, 8, ZPixmap, 0, buf,
                             (unsigned)w, (unsigned)h, 32, bpl);
    if (!e->img) {
        free(buf);
        return 0;
    }
    return 1;
}

/* Build the 216-color 6x6x6 cube (Winamp-era palette shape; gradients
   survive on dither, not on palette size). Cells are reserved in the
   window's own colormap (read-only cells; the WM shares them). If any
   reservation fails, fall back to a private all-alloc colormap with the
   full cube stored — the colors then belong to the window alone. */
static int x8_palette(struct x8win *e, Colormap cmap)
{
    XColor c[216];
    for (int i = 0; i < 216; i++) {
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        c[i].flags = DoRed | DoGreen | DoBlue;
        c[i].red = (unsigned short)(r * 51 * 257);
        c[i].green = (unsigned short)(g * 51 * 257);
        c[i].blue = (unsigned short)(b * 51 * 257);
    }
    for (int i = 0; i < 216; i++) {
        if (!x8x.AllocColor(e->dpy, cmap, &c[i])) {
            /* reserve the whole colormap for this window and store
               (cells beyond 216 get gray ramp fill) */
            cmap = x8x.CreateColormap(e->dpy, e->win, e->visual, AllocAll);
            if (!cmap)
                return 0;
            XColor f[256];
            for (int k = 0; k < 216; k++) {
                int r = k / 36, g = (k / 6) % 6, b = k % 6;
                f[k].pixel = (unsigned long)k;
                f[k].flags = DoRed | DoGreen | DoBlue;
                f[k].red = (unsigned short)(r * 51 * 257);
                f[k].green = (unsigned short)(g * 51 * 257);
                f[k].blue = (unsigned short)(b * 51 * 257);
            }
            for (int k = 216; k < 256; k++) { /* gray ramp tail */
                int v = (k - 216) * 51;
                if (v > 255) v = 255;
                f[k].pixel = (unsigned long)k;
                f[k].flags = DoRed | DoGreen | DoBlue;
                f[k].red = f[k].green = f[k].blue = (unsigned short)(v * 257);
            }
            if (x8x.StoreColors(e->dpy, cmap, f, 256) == 0)
                return 0;
            if (x8x.SetWindowColormap(e->dpy, e->win, cmap) == 0)
                return 0;
            for (int k = 0; k < 256; k++)
                e->cube[k] = (unsigned char)k;
            return 1;
        }
        e->cube[i] = (unsigned char)c[i].pixel;
    }
    for (int k = 216; k < 256; k++)
        e->cube[k] = e->cube[215]; /* unused indices: white */
    return 1;
}

static int x8_claim(struct x8win *e)
{
    if (!x8x_load()) {
        fprintf(stderr, "levari: 8-bit display needs libX11.so.6 at runtime"
                        " — it was not found\n");
        return 0;
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(e->w, &wm) || wm.subsystem != SDL_SYSWM_X11) {
        fprintf(stderr, "levari: window surface unavailable (%s) and this is"
                        " not an X11 window — cannot take over\n", SDL_GetError());
        return 0;
    }
    Display *dpy = wm.info.x11.display;
    Window win = wm.info.x11.window;
    XWindowAttributes a;
    if (!x8x.GetWindowAttributes(dpy, win, &a))
        return 0;
    if (a.depth != 8 || (a.class != PseudoColor && a.class != GrayScale)) {
        fprintf(stderr, "levari: window surface unavailable (%s) — depth %d"
                        " visual class %d is not supported\n", SDL_GetError(),
                a.depth, a.class);
        return 0;
    }
    e->dpy = dpy;
    e->win = win;
    e->visual = a.visual;
    e->surf = SDL_CreateRGBSurfaceWithFormat(0, a.width, a.height, 32,
                                             SDL_PIXELFORMAT_ARGB8888);
    if (!e->surf)
        return 0;
    if (!x8_palette(e, a.colormap) || !x8_img(e)) {
        SDL_FreeSurface(e->surf);
        e->surf = NULL;
        return 0;
    }
    e->gc = x8x.CreateGC(dpy, win, 0, NULL);
    if (!e->gc) {
        x8x.DestroyImage(e->img);
        e->img = NULL;
        SDL_FreeSurface(e->surf);
        e->surf = NULL;
        return 0;
    }
    fprintf(stderr, "levari: %s 8-bit display — indexed presentation"
                    " engaged (%dx%d)\n", a.class == PseudoColor ?
                    "PseudoColor" : "GrayScale", a.width, a.height);
    return 1;
}

static void x8_present_win(struct x8win *e, const SDL_Rect *rgn, int n)
{
    int sw = e->surf->w, sh = e->surf->h;
    if (n <= 0) {
        x8_quant(e->surf->pixels, e->surf->pitch,
                 (unsigned char *)e->img->data, e->img->bytes_per_line,
                 e->cube, 0, 0, sw, sh);
        x8x.PutImage(e->dpy, e->win, e->gc, e->img, 0, 0, 0, 0,
                     (unsigned)sw, (unsigned)sh);
    } else {
        for (int i = 0; i < n; i++) {
            int x = rgn[i].x, y = rgn[i].y;
            int w = rgn[i].w, h = rgn[i].h;
            if (x < 0) { w += x; x = 0; }
            if (y < 0) { h += y; y = 0; }
            if (x >= sw || y >= sh || w <= 0 || h <= 0)
                continue;
            if (x + w > sw) w = sw - x;
            if (y + h > sh) h = sh - y;
            x8_quant(e->surf->pixels, e->surf->pitch,
                     (unsigned char *)e->img->data, e->img->bytes_per_line,
                     e->cube, x, y, w, h);
            x8x.PutImage(e->dpy, e->win, e->gc, e->img, x, y, x, y,
                         (unsigned)w, (unsigned)h);
        }
    }
    x8x.Flush(e->dpy); /* SDL's next WaitEvent does not guarantee a flush */
}
#endif /* LV_X8_ACTIVE */

/* ------------------------- public entry points ----------------------- */

SDL_Surface *lv_win_surface(SDL_Window *w)
{
    if (!w)
        return NULL;
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (s)
        return s;
#if LV_X8_ACTIVE
    struct x8win *e = x8_find(w);
    if (!e) {
        for (int i = 0; i < X8_MAXW; i++)
            if (!X8W[i].w) { e = &X8W[i]; e->w = w; break; }
    }
    if (!e) {
        fprintf(stderr, "levari: 8-bit takeover registry full\n");
        return NULL;
    }
    if (e->dead)
        return NULL;
    if (!e->surf && !x8_claim(e)) {
        e->dead = 1;
        return NULL;
    }
    int ww, wh;
    SDL_GetWindowSize(w, &ww, &wh);
    if (e->surf && (e->surf->w != ww || e->surf->h != wh)) {
        /* resized (playlist drag, url notice): rebuild surface and image */
        if (e->img) { x8x.DestroyImage(e->img); e->img = NULL; }
        SDL_FreeSurface(e->surf);
        e->surf = NULL;
        e->surf = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32,
                                                 SDL_PIXELFORMAT_ARGB8888);
        if (!e->surf || !x8_img(e)) {
            e->dead = 1;
            return NULL;
        }
    }
    return e->surf;
#else
    return NULL;
#endif
}

void lv_win_present(SDL_Window *w, const SDL_Rect *rgn, int n)
{
#if LV_X8_ACTIVE
    struct x8win *e = x8_find(w);
    if (e && e->surf && e->img) {
        x8_present_win(e, rgn, n);
        return;
    }
#endif
    if (w && n > 0 && rgn)
        SDL_UpdateWindowSurfaceRects(w, rgn, n);
    else if (w)
        SDL_UpdateWindowSurface(w);
}

void lv_win_release(SDL_Window *w)
{
#if LV_X8_ACTIVE
    struct x8win *e = x8_find(w);
    if (e)
        x8_free(e);
#else
    (void)w;
#endif
}
