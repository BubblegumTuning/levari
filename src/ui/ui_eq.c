/* src/ui/ui_eq.c: equalizer window — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

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

void ui_draw_eq(void) {
    SDL_Surface *s = ui.seq;
    Player *p = ui.pl;
    skin_window_bg(s, EQ_W, EQ_H);
    font_puts(s, 10, 6, "EQUALIZER", ui_col(s, 0, 224, 255));
    { Rect x = ui_close_rect(EQ_W); ui_draw_x(s, &x, ui.pressed == EQB_X); }

    int eq_on = eq_enabled(&p->eq);
    Rect on = eq_on_rect();
    skin_button(s, &on, ui.pressed == EQB_ON, eq_on);
    const char *onl = eq_on ? "ON" : "OFF";
    font_puts(s, on.x + (on.w - font_w(onl)) / 2, on.y + 3, onl,
              eq_on ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));

    for (size_t i = 0; i < sizeof EQ_PRE / sizeof EQ_PRE[0]; i++) {
        Rect r = {EQ_PRE[i].x, EQ_PRE_Y, EQ_PRE[i].w, 12};
        skin_button(s, &r, ui.pressed == EQ_PRE[i].id, 0);
        font_puts(s, r.x + (r.w - font_w(EQ_PRE[i].l)) / 2, EQ_PRE_Y + 3,
                  EQ_PRE[i].l, ui_col(s, 220, 220, 220));
    }

    /* preamp */
    font_puts(s, EQ_PRE_X + 1, 38, "PRE", ui_col(s, 150, 150, 150));
    skin_vslider(s, EQ_PRE_X, EQ_SL_Y, EQ_SL_H, db_to_knob(p->eq.preamp_db));
    char db[8];
    snprintf(db, sizeof db, "%+d", (int)p->eq.preamp_db);
    font_puts(s, EQ_PRE_X - 2, 124, db, ui_col(s, 0, 255, 0));

    static const char *LBL[10] = {"60","170","310","600","1k","3k","6k","12","14","16"};
    for (int i = 0; i < EQ_BANDS; i++) {
        int x = EQ_BAND_X(i);
        skin_vslider(s, x, EQ_SL_Y, EQ_SL_H, db_to_knob(p->eq.band_db[i]));
        font_puts(s, x + (12 - font_w(LBL[i])) / 2, 124, LBL[i], ui_col(s, 150, 150, 150));
    }
}

void ui_handle_eq_mouse(SDL_Event *e) {
    Player *p = ui.pl;
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(EQ_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = EQB_X; return; }
        Rect on = eq_on_rect();
        if (ui_in_rect(&on, x, y)) { ui.pressed = EQB_ON; return; }
        for (size_t i = 0; i < sizeof EQ_PRE / sizeof EQ_PRE[0]; i++) {
            Rect r = {EQ_PRE[i].x, EQ_PRE_Y, EQ_PRE[i].w, 12};
            if (ui_in_rect(&r, x, y)) { ui.pressed = EQ_PRE[i].id; return; }
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
        if (y < 22) { ui.drag = 7; ui_drag_begin(ui.weq); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = ui_close_rect(EQ_W);
        if (ui.pressed == EQB_X && ui_in_rect(&xc, x, y)) {
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
        ui_drag_move_snap(ui.weq);
    }
}
