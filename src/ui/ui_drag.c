/* src/ui/ui_drag.c: drag/snap/dock machine (windows follow, walls bounce) — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

void ui_pl_dock_pos(int *x, int *y) {
    SDL_GetWindowPosition(ui.wmain, x, y);
    *y += MAIN_H;
}
void ui_drag_begin(SDL_Window *w) {
    SDL_GetGlobalMouseState(&ui.drag_gx, &ui.drag_gy);
    /* refusal bookkeeping is drag-scoped: start from the window's true
       position with no known walls (see drag_step) */
    SDL_GetWindowPosition(w, &ui.drag_rx, &ui.drag_ry);
    ui.drag_wxlo = ui.drag_wxhi = ui.drag_wylo = ui.drag_wyhi = 0;
    /* carried/pushed windows are drag-scoped too (ui_follow_move in the
       drag-3 follow AND ui_reflow_above pushes during snap drags): a wall
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

void ui_reflow_above(SDL_Window *self, int cursor_y);

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
            ui_reflow_above(w, *y);
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
void ui_drag_move(SDL_Window *w) { drag_step(w, 0, &ui.drag_gx, &ui.drag_gy); }

/* file/eq/options drag: live-snap beside main, follows main until
   dragged away */
void ui_drag_move_snap(SDL_Window *w) { drag_step(w, 1, &ui.drag_gx, &ui.drag_gy); }

/* playlist drag: live-snap flush under the main window's bottom edge,
   x-aligned, within DOCK_SNAP px; docked state follows live like the
   side-snapped windows so it keeps following main until dragged away */
void ui_drag_move_pl(void) { drag_step(ui.wpl, 2, &ui.drag_gx, &ui.drag_gy); }

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
void ui_follow_move(SDL_Window *w, struct FollowState *fs, int x, int y) {
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
   Moves go through ui_follow_move so they carry the same refusal discipline
   as the drag-3 carry (walls recorded per drag, deeper requests clamped,
   identical requests skipped). Outside a drag (url open/close) the
   bookkeeping is re-baselined first: those are one-shot placement moves,
   a previous drag's wall must not clamp them. self (may be NULL) is
   skipped — the caller has just placed it. */
void ui_reflow_above(SDL_Window *self, int cursor_y) {
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
        ui_follow_move(ui.snap[i].w, &ui.follow_snap[i], mx, cursor_y);
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
   pins the carried window at its own clamp (ui_follow_move) while main keeps
   travelling until its clamp or its travel stops — main then sits inside
   or overlapping the pinned window. On main's release, pull main back to
   the dock position flush against every carried window it overshoots
   (per axis: a carried window at truth t with snap offset d must end up
   at main+d, so main returns to t-d on that axis). Only overshoot is
   corrected — a gap the user opened deliberately stays. This REPLACES
   the wall kick for main's release: the dock position is already clear of
   the wall, and kicking would tear the flush dock open again. */
int ui_drag_dock_restore(void) {
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
void ui_raise_all(void) {
#ifdef __APPLE__
    /* Cocoa already keeps every window of the active app above other apps.
       SDL_RaiseWindow here is makeKeyAndOrderFront + activation: calling it
       per window on every FOCUS_GAINED re-triggers FOCUS_GAINED and floods
       the main thread with WindowServer round-trips (focus storm, beachball). */
    return;
#else
    /* Xfwm (and some other X11 WMs) restack AND refocus on XRaiseWindow:
       the raise fires FOCUS_LOST/FOCUS_GAINED, which calls ui_raise_all again
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
void ui_focus_click_drag(Uint32 wid) {
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
           the button-up handler skips the ui_action while a drag is live */
        int lx = gx - wx, ly = gy - wy;
        if (lx >= ww - 18 && lx < ww - 6 && ly >= 4 && ly < 16) return;
        ui.drag = drag;
        ui_drag_begin(w);
    }
}
