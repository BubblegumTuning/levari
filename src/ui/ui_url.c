/* src/ui/ui_url.c: URL loader window (LV_URL_ENABLED) — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

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
void ui_url_play_transient(Player *p, const char *url) {
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
    ui_reflow_above(ui.wurl, my - uh);
}

void ui_url_close(void) {
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
        ui_reflow_above(NULL, my);
    }
}

void ui_urlwin_open(void) {
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
        ui.surl = lv_win_surface(ui.wurl);
    }
    /* The window must tell the USER that streaming cannot work without the
       libav* backend (probe: 1 loadable, 2 missing, 0 not compiled in — all
       non-1 states are inert): a terminal line is invisible to a GUI launch.
       Re-probed per open — the answer is cached inside ff_syms, so this is
       a flag read once ffmpeg has ever loaded. */
    ui.url_ff_code = ui_ffprobe_info(ui.url_ff_ver, sizeof ui.url_ff_ver, NULL, 0);
    ui.url_ff_ok = ui.url_ff_code == 1;
    if (!ui.url_ff_ok) {
        SDL_SetWindowSize(ui.wurl, URL_W, URL_H_NOFF);
        ui.surl = lv_win_surface(ui.wurl);
    } else if (ui.surl && ui.surl->h != URL_H) {
        SDL_SetWindowSize(ui.wurl, URL_W, URL_H);
        ui.surl = lv_win_surface(ui.wurl);
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
    ui_raise_all();
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
    ui_url_play_transient(ui.pl, ui.url_field);
    ui_url_close();
}

/* file-browser style: while the window is open EVERY key belongs to the
   field, so typing must never fire global playback shortcuts */
int ui_url_key(SDL_Event *e) {
    int ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
    switch (e->key.keysym.sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        url_enter();
        break;
    case SDLK_ESCAPE:
        ui_url_close();
        break;
    default:
        tf_handle_key(&ui.url_tf, e->key.keysym.sym, ctrl); /* tf_paste rides
            tf_sanitize_paste: CRLF/quote/control hygiene exactly like the
            file field — the paste hygiene rule of the streaming plan */
        break;
    }
    return 1;
}

void ui_url_text(const char *text) {
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

void ui_draw_url(void) {
    SDL_Surface *s = ui.surl;
    if (!s) return;
    skin_window_bg(s, URL_W, s->h);   /* grows with the no-ffmpeg notice */
    font_puts(s, 10, 6, "URL", ui_col(s, 0, 224, 255));
    { Rect x = ui_close_rect(URL_W); ui_draw_x(s, &x, ui.pressed == B_URLX); }
    URL_FLD_RECT;
    SDL_Rect fb = { fld.x, fld.y, fld.w, fld.h };
    SDL_FillRect(s, &fb, ui_col(s, 0, 0, 0));
    SDL_Rect t = { fld.x, fld.y, fld.w, 1 }, b = { fld.x, fld.y + fld.h - 1, fld.w, 1 };
    SDL_Rect l = { fld.x, fld.y, 1, fld.h }, r = { fld.x + fld.w - 1, fld.y, 1, fld.h };
    int fwin_focused = ui.app_focused && ui.wurl &&
                       ui.focus_wid == SDL_GetWindowID(ui.wurl);
    Uint32 bc = (ui.url_focus && fwin_focused) ? ui_col(s, 0, 224, 255) : ui_col(s, 90, 90, 90);
    SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
    SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
    if (ui.url_err) {
        font_puts16(s, fld.x + 3, fld.y + 1, "url not allowed", ui_col(s, 255, 60, 60));
    } else {
        int blink = (SDL_GetTicks() / 500) % 2 == 0;
        ui_draw_field_text(s, fld, ui.url_field, &ui.url_tf,
                        ui.url_focus && fwin_focused, blink);
    }
    if (!ui.url_ff_ok)
        font_puts(s, fld.x, URL_H + 3, url_ff_notice(ui.url_ff_code),
                  ui_col(s, 255, 60, 60));
}

void ui_handle_url_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    URL_FLD_RECT;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(URL_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = B_URLX; return; }
        if (ui_in_rect(&fld, x, y)) {
            ui.url_focus = 1;
            ui.url_err = 0;
            ui.url_tf.caret = (int)strlen(ui.url_field);
            ui.url_tf.selall = 0;
            return;
        }
        if (y < 22) { ui.drag = 13; ui_drag_begin(ui.wurl); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = ui_close_rect(URL_W);
        if (ui.pressed == B_URLX && ui_in_rect(&xc, x, y)) ui_url_close();
        ui.pressed = B_NONE;
        if (ui.drag == 13) {
            ui_drag_bounce(ui.wurl, ui.drag_wxlo, ui.drag_wxhi,
                           ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 13) {
        /* live side/above snap like EQ/options: the above branch inserts
           at the bottom of the above-stack (ui_snap_next_to_main) */
        ui_drag_move_snap(ui.wurl);
    }
}
#endif /* LV_URL_ENABLED */
