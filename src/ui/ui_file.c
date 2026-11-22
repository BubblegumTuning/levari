/* src/ui/ui_file.c: built-in open/save file browser window — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

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
    int scanned = 0; /* batch 17: readdir progress for the era trace */
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
        lv98_tracef("flst:opendir-fail dir=[%s]", ui.file_dir);
        file_sync_field();
        file_build_view();
        return;
    }
    lv98_tracef("flst:opened");
    char **dirs = NULL, **files = NULL;
    int nd = 0, nf = 0, cd = 0, cf = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        /* batch 18: batch 17's every-512 progress was blind under the
           box's small C:\ — probe EVERY entry (re = readdir returned,
           st = this entry's stat returned) capped at 40: the last
           line names the exact entry/API that never came back */
        if (++scanned <= 40) lv98_tracef("flst:re %d [%s]", scanned, de->d_name);
        if (de->d_name[0] == '.') continue;
        char full[LV_MAX_PATH];
        lv_path_join(full, sizeof full, ui.file_dir, de->d_name);
        struct stat st;
        int src_ = stat(full, &st);
        if (scanned <= 40) lv98_tracef("flst:st %d rc=%d", scanned, src_);
        if (src_ != 0) continue;
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
        } else if (lv_is_audio(de->d_name) || ui.file_saving) {
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
        lv98_tracef("flst:nomem scan=%d nd=%d nf=%d", scanned, nd, nf);
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
    lv98_tracef("flst:done n=%d pseudo=%d scanned=%d view=%d",
                ui.file_n, ui.file_pseudo, scanned, ui.file_view_n);
}

void ui_file_close(void) {
    ui.file_visible = 0;
    ui.file_focus = 0;
    ui.file_saving = 0;
    if (ui.wfile) SDL_HideWindow(ui.wfile);
}

void ui_file_open(void) {
    /* batch 17: batch 16's log froze one line after title=[levari - open]
       (never a heartbeat again), so every hop of the open chain gets a
       trace line: the FIRST MISSING line names the kill hop. lv98_tracef
       expands to ((void)0) with discarded args outside the era build. */
    lv98_tracef("fop:enter wfile=%d dir=[%s]", ui.wfile ? 1 : 0, ui.file_dir);
    if (!ui.wfile) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        ui.wfile = SDL_CreateWindow("levari - open", mx + MAIN_W + 4, my,
                                    FILE_W, FILE_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        lv98_tracef("fop:created ok=%d", ui.wfile ? 1 : 0);
        if (!ui.wfile) return;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.wfile, icon);
            SDL_FreeSurface(icon);
        }
        lv98_tracef("fop:icon");
        ui.sfile = lv_win_surface(ui.wfile);
        lv98_tracef("fop:surf ok=%d", ui.sfile ? 1 : 0);
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
    lv98_tracef("fop:prelist dir=[%s] pseudo=%d", ui.file_dir, ui.file_pseudo);
    file_list();
    lv98_tracef("fop:postlist n=%d view=%d", ui.file_n, ui.file_view_n);
    ui.file_visible = 1;
    SDL_ShowWindow(ui.wfile);
    ui_raise_all();
    lv98_tracef("fop:shown");
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
    ui_file_close();
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
    ui_file_close();                     /* also clears file_saving */
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
    lv_path_join(full, sizeof full, ui.file_dir, name);
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
    ui_file_close();
}

/* Draw the text of an editable field inside its box: unfocused text
   scrolls left so the trailing suffix shows; focused text is anchored on
   the caret so it stays visible while editing mid-string. Draws the
   select-all highlight and the caret when focused && caret_on (the file
   field blinks its caret on the search cadence; options draw it solid).
   Shared by the options fields and the file path field. */
void ui_draw_field_text(SDL_Surface *s, const Rect f, const char *val,
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
        if (hl.w > 0) SDL_FillRect(s, &hl, ui_col(s, 40, 60, 120));
    }
    font_puts16(s, f.x + 3, f.y + 1, shown, ui_col(s, 220, 220, 220));
    if (focused && caret_on) {
        int cx = f.x + 3 + caret_w + 1;
        if (cx < f.x + f.w - 2) {
            SDL_Rect c = { cx, f.y + 3, 1, f.h - 6 };
            SDL_FillRect(s, &c, ui_col(s, 255, 255, 255));
        }
    }
}

void ui_draw_file(void) {
    SDL_Surface *s = ui.sfile;
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 17: first browser frame probed in and out: enter-without-done
       names the first draw as the kill site (the font/list paths were
       never exercised on real hardware; the sandbox has no display) */
    static int fdrw;
    if (fdrw == 0) {
        fdrw = 1;
        lv98_tracef("fdrw:enter n=%d view=%d scr=%d surf=%d",
                    ui.file_n, ui.file_view_n, ui.file_scroll, s ? 1 : 0);
    }
#endif
    if (!s) return;
    skin_window_bg(s, FILE_W, FILE_H);
    font_puts(s, 10, 6, ui.file_saving ? "SAVE PLAYLIST" : "OPEN FILE OR FOLDER",
              ui_col(s, 0, 224, 255));
    { Rect x = ui_close_rect(FILE_W); ui_draw_x(s, &x, ui.pressed == FB_X); }
    FILE_LIST_RECT;
    FILE_FLD_RECT;
    FILE_ADD_RECT;
    /* path field: the editable twin of file_dir. Unfocused it always
       shows the current folder (COMPUTER in the pseudo-root); focused it
       shows what the user typed and filters the list live. */
    SDL_Rect fb = { fld.x, fld.y, fld.w, fld.h };
    SDL_FillRect(s, &fb, ui_col(s, 0, 0, 0));
    SDL_Rect t = { fld.x, fld.y, fld.w, 1 }, b = { fld.x, fld.y + fld.h - 1, fld.w, 1 };
    SDL_Rect l = { fld.x, fld.y, 1, fld.h }, r = { fld.x + fld.w - 1, fld.y, 1, fld.h };
    int fwin_focused = ui.app_focused && ui.wfile &&
                       ui.focus_wid == SDL_GetWindowID(ui.wfile);
    Uint32 bc = (ui.file_focus && fwin_focused) ? ui_col(s, 0, 224, 255) : ui_col(s, 90, 90, 90);
    SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
    SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
    if (ui.file_err) {
        /* plain red, no bold: the field text returns after ~1 s */
        font_puts16(s, fld.x + 3, fld.y + 1, "path not valid", ui_col(s, 255, 60, 60));
    } else {
        int blink = (SDL_GetTicks() / 500) % 2 == 0;
        ui_draw_field_text(s, fld, ui.file_field, &ui.file_tf,
                        ui.file_focus && fwin_focused, blink);
    }
    SDL_Rect lb = {list.x, list.y, list.w, list.h};
    SDL_FillRect(s, &lb, ui_col(s, 0, 0, 0));
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
            lv_path_join(full, sizeof full, ui.file_dir, name);
            struct stat st;
            isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        char line[LV_MAX_PATH];
        /* pseudo-root entries are drive roots, already ending in "\" */
        snprintf(line, sizeof line, "%s%s", name,
                 (isdir && name[strlen(name) - 1] != '\\') ? "/" : "");
        int fit = font_fit(line, list.w - 6);
        if ((int)strlen(line) > fit) line[fit] = 0;
        Uint32 c = isdir ? ui_col(s, 255, 255, 255) : ui_col(s, 170, 170, 170);
        if (vi == ui.file_sel) c = ui_col(s, 0, 255, 0);
        font_puts16(s, list.x + 2, list.y + 1 + i * 17, line, c);
    }
    if (ui.file_view_n > lines) {
        int th = list.h * lines / ui.file_view_n;
        if (th < 8) th = 8;
        int ty = list.y + (list.h - th) * ui.file_scroll / (ui.file_view_n - lines);
        SDL_Rect r = {list.x + list.w - 7, ty, 5, th};
        SDL_FillRect(s, &r, ui_col(s, 110, 110, 110));
    }
    if (!ui.file_saving) {          /* ADD DIR is meaningless in SAVE mode */
        skin_button(s, &addb, ui.pressed == FB_ADDDIR, 0);
        font_puts(s, addb.x + (addb.w - font_w("ADD DIR")) / 2, addb.y + 3, "ADD DIR", ui_col(s, 220, 220, 220));
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
    font_puts16(s, 6, FILE_H - 17, dir, ui_col(s, 150, 150, 150));
#if defined(LV_WIN9X) && defined(_WIN32)
    if (fdrw == 1) {
        fdrw = 2;
        lv98_tracef("fdrw:done");
    }
#endif
}

void ui_handle_file_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    FILE_LIST_RECT;
    FILE_FLD_RECT;
    FILE_ADD_RECT;
    int lines = list.h / 17;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(FILE_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = FB_X; return; }
        if (ui_in_rect(&addb, x, y)) { if (!ui.file_saving) ui.pressed = FB_ADDDIR; return; }
        if (ui_in_rect(&fld, x, y)) {
            /* caret to end like the opt fields; the field already mirrors
               the current folder whenever it is unfocused */
            ui.file_focus = 1;
            ui.file_err = 0;
            ui.file_tf.caret = (int)strlen(ui.file_field);
            ui.file_tf.selall = 0;
            return;
        }
        if (ui_in_rect(&list, x, y)) {
            ui.file_focus = 0;
            int vi = ui.file_scroll + (y - list.y) / 17;
            if (vi < ui.file_view_n) {
                ui.file_sel = vi;
                file_choose(ui.file_names[ui.file_view[vi]]);
            }
            return;
        }
        if (y < 22) { ui.drag = 9; ui_drag_begin(ui.wfile); }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        Rect xc = ui_close_rect(FILE_W);
        if (ui.pressed == FB_X && ui_in_rect(&xc, x, y)) ui_file_close();
        else if (ui.pressed == FB_ADDDIR && ui_in_rect(&addb, x, y)) file_adddir();
        ui.pressed = B_NONE;
        if (ui.drag == 9) {
            ui_drag_bounce(ui.wfile, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 9) {
        ui_drag_move_snap(ui.wfile);
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
void ui_url_play_transient(Player *p, const char *url); /* fwd: defined with the URL window below */
#endif
static void file_field_enter(void) {
    if (ui.file_saving) { file_save_enter(); return; }
    if (!ui.file_field[0]) return;
#ifdef LV_URL_ENABLED
    if (lv_is_url(ui.file_field)) {
        ui_url_play_transient(ui.pl, ui.file_field);
        ui_file_close();
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
        if (lv_is_audio(base)) {
            Player *p = ui.pl;
            LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
            pthread_mutex_lock(&p->mtx);
            int added = pl_add(p->pl, ui.file_field);
            lib_fill_playlist_ix(p->pl, &ix);
            int idx = p->pl->count - 1;
            pthread_mutex_unlock(&p->mtx);
            lib_index_free(&ix);
            if (added == 0) pl_post(p, CMD_PLAY, idx); /* failed add: no wrong-track play */
            ui_file_close();
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
int ui_file_key(SDL_Event *e) {
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
    default:
        tf_handle_key(&ui.file_tf, e->key.keysym.sym, ctrl);
        break;
    }
    file_build_view();
    return 1;
}

void ui_file_text(const char *text) {
    if (!ui.file_focus) return;
    tf_insert(&ui.file_tf, text);
    file_build_view();
}
