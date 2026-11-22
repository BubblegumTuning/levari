/* src/ui/ui_info.c: song-info window — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

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
   ui_draw_field_text + TField + tf_copy — the house selection contract is
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

void ui_info_close(void) {
    ui.info_visible = 0;
    ui.info_focus = -1;
    if (ui.winfo) SDL_HideWindow(ui.winfo);
    ui_dirty |= D_INFO;
}

/* the focused box's whole value onto the clipboard (selection == buffer is
   the textedit contract; empty boxes copy nothing) */
int ui_info_copy_sel(void) {
    if (ui.info_focus < 0 || ui.info_focus >= INFOI_N) return 0;
    char *buf = (char *)info_row_text(ui.info_focus);   /* tf_copy only reads */
    if (!buf[0]) return 0;
    TField tf = { buf, 0, 0, 1 };
    return tf_copy(&tf);
}

void ui_info_open(void) {
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
        ui.sinfo = lv_win_surface(ui.winfo);
    }
    ui.info_focus = -1;               /* a re-open never inherits a stale focus */
    ui.info_visible = 1;
    SDL_ShowWindow(ui.winfo);
    ui_raise_all();
    ui_dirty |= D_INFO;
}

void ui_draw_info(void) {
    SDL_Surface *s = ui.sinfo;
    if (!s) return;
    skin_window_bg(s, INFO_W, INFO_H);
    font_puts(s, 10, 6, "SONG INFO", ui_col(s, 0, 224, 255));
    { Rect x = ui_close_rect(INFO_W); ui_draw_x(s, &x, ui.pressed == IB_X); }
    int win_focused = ui.app_focused && ui.winfo &&
                      ui.focus_wid == SDL_GetWindowID(ui.winfo);
    for (int i = 0; i < INFOI_N; i++) {
        Rect b = info_row_rect(i);
        int foc = win_focused && ui.info_focus == i;
        font_puts(s, 10, b.y + 5, INFO_LBL[i], ui_col(s, 160, 160, 160));
        /* the shared read-only field chrome: black box, gray border
           (cyan when focused, the file-field pairing), then the shared
           renderer for value/selection — caret pinned off (no editing) */
        SDL_Rect rb = { b.x, b.y, b.w, b.h };
        SDL_FillRect(s, &rb, ui_col(s, 0, 0, 0));
        Uint32 bc = foc ? ui_col(s, 0, 224, 255) : ui_col(s, 90, 90, 90);
        SDL_Rect t = { b.x, b.y, b.w, 1 }, bot = { b.x, b.y + b.h - 1, b.w, 1 };
        SDL_Rect l = { b.x, b.y, 1, b.h }, r = { b.x + b.w - 1, b.y, 1, b.h };
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &bot, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        const char *txt = info_row_text(i);
        TField tf = { 0, 0, info_row_tail(i) ? (int)strlen(txt) : 0, 1 };
        ui_draw_field_text(s, b, txt[0] ? txt : "-", &tf, foc, 0);
    }
}

void ui_handle_info_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(INFO_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = IB_X; return; }
        for (int i = 0; i < INFOI_N; i++) {
            Rect b = info_row_rect(i);
            if (ui_in_rect(&b, x, y)) { ui.info_focus = i; return; }
        }
        if (y < 22) { ui.drag = 14; ui_drag_begin(ui.winfo); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = ui_close_rect(INFO_W);
        if (ui.pressed == IB_X && ui_in_rect(&xc, x, y)) ui_info_close();
        ui.pressed = B_NONE;
        if (ui.drag == 14) {
            ui_drag_bounce(ui.winfo, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 14) {
        ui_drag_move_snap(ui.winfo);
    }
}
