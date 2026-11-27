/* src/ui/ui_opt.c: options window incl. scan/verify threads — split from ui.c;
   shared state + cross-window protos: ui_state.h */
#include "ui_state.h"

/* options window: main+playlist stack width plus room for the SCAN button,
   docked right */
#define OPT_SCAN_W 53
#define OPT_SCAN_GAP 4
#define OPT_CHK_W 46
/* the right-hand button column(s) fit two stacked 53px slots (SCAN/CLEAR
   row + VERIFY beside CLEAR) with the same trailing margin the old
   CHK-column formula left (was MAIN_W + SCAN_W + 2*GAP + CHK_W = 422) */
#define OPT_W (MAIN_W + 2 * (OPT_SCAN_W + OPT_SCAN_GAP))
/* +28: the ADV page's RG row (replaygain cycle) sits below SAVE/CLOSE */
#define OPT_H (MAIN_H + PL_H + 28)
#define OPT_FLD_X 100
#define OPT_FLD_W 207
#define OPT_FLD_H 18
/* ---------- options window ---------- */

static const struct { const char *lbl; int y; } OPT_ROWS[OPTF_N] = {
    { "LIBRARY DIRS",  30 }, { "LIB INDEX",     56 }, { "LIBAV DIR",   82 },
    { "AUDIO BACKEND", 108 }, { "AUDIO DEVICE", 134 }, { "DAEMON SOCKET", 160 },
};

static Rect opt_field_rect(int i) {
    Rect r = { OPT_FLD_X, OPT_ROWS[i].y, OPT_FLD_W, OPT_FLD_H };
    return r;
}
static Rect opt_save_rect(void)  { Rect r = { OPT_W - 142, 224, 60, 14 }; return r; }
static Rect opt_close_rect(void) { Rect r = { OPT_W - 72,  224, 60, 14 }; return r; }
static Rect opt_clear_rect(void) { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBIDX].y, OPT_SCAN_W, OPT_FLD_H }; return r; }
/* VERIFY sits RIGHT of CLEAR (same LIBIDX row, second button column) —
   under the LIBAV row it read as that field's button; VERIFY belongs to
   the index, visually with CLEAR. The column widened the window (OPT_W). */
static Rect opt_verify_rect(void) { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP + OPT_SCAN_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBIDX].y, OPT_SCAN_W, OPT_FLD_H }; return r; }
static Rect opt_tog_rect(int i)  { Rect r = { OPT_FLD_X + i * 52, 190, 48, 14 }; return r; }
/* always-on-top checkbox: second row under the STARTUP toggles, flush
   under UI PL. It applies live (shares ui.aot with the main-window AoT
   button); SAVE only persists it */
static Rect opt_aot_rect(void)   { Rect r = { OPT_FLD_X, 208, 48, 14 }; return r; }
/* mini-mode checkbox beside AOT: applies live via ui_set_mini (opens all
   three windows), SAVE only persists the startup default */
static Rect opt_mini_rect(void)  { Rect r = { OPT_FLD_X + 52, 208, 48, 14 }; return r; }
/* BASIC/ADVANCED/ABOUT tab buttons: small tabs in the title bar, right of
   the "OPTIONS" label (ends at x=52); visible on all three pages */
static Rect opt_tab_basic_rect(void) { Rect r = { 60, 4, 38, 14 }; return r; }
static Rect opt_tab_adv_rect(void)   { Rect r = { 102, 4, 26, 14 }; return r; }
static Rect opt_tab_about_rect(void) { Rect r = { 132, 4, 38, 14 }; return r; }
/* LARGE toggle (ADV page): beside its description text — the lines at
   y=52/66 end at x=178, so the button sits right of them, vertically
   centered on the two-line block */
static Rect opt_large_rect(void)     { Rect r = { 186, 56, 48, 14 }; return r; }
/* 10FPS toggle (ADV page): same column, below the LARGE block */
static Rect opt_fps_rect(void)       { Rect r = { 186, 114, 48, 14 }; return r; }
static Rect opt_enc_rect(void)       { Rect r = { 186, 150, 72, 14 }; return r; }
/* VIZ Hz cycle button (ADV page): visualizer band refresh rate */
static Rect opt_vhz_rect(void)       { Rect r = { 186, 214, 48, 14 }; return r; }
static Rect opt_rg_rect(void)        { Rect r = { 186, 248, 48, 14 }; return r; }
/* ui.vis_hz -> the viz tick period; only the four table values ever
   reach here (config clamps, the cycle button walks the same table),
   and the switch default makes a torn value 10 Hz, never a busy loop */
int ui_vis_frame_ms(int hz) {
    switch (hz) {
    case 60: return 16;
    case 30: return 33;
    case 24: return 41;
    default: return 100;
    }
}

/* ffmpeg detection status for the ADV page (streaming substrate): the
   probe dlopens-and-closes per call, so it runs ONCE at ui_opt_open into
   ui.opt_ff — same restart semantics as the libav.dir field beside it.
   Detected == dec_ffmpeg_probe()==1; anything else (2 = built but
   libav* missing at runtime, 0 = backend not compiled in) reads
   "unavailable" honestly. The version suffix is the release of the
   library the loader ACTUALLY resolved (probe_info) — the versioned
   soname candidates stop at 62, so a bare-name hit can be a newer
   major and the number is the only way to see which one is running. */
static const char *opt_ff_label(int ok, const char *ver, int *red) {
    static char buf[96];
    if (ok == 1) {
        *red = 0;
        if (ver && ver[0]) {
            snprintf(buf, sizeof buf, "ffmpeg: detected %s", ver);
            return buf;
        }
        return "ffmpeg: detected";
    }
    *red = 1;
    if (ok == 3) { /* found, but a different libav major than this
                      binary's headers — a mismatched layout would be
                      read as garbage fields, so ffmpeg formats stay off */
        if (ver && ver[0]) {
            snprintf(buf, sizeof buf, "ffmpeg: found %s - incompatible build", ver);
            return buf;
        }
        return "ffmpeg: found - incompatible build";
    }
    return "ffmpeg: not found - streaming unavailable";
}
/* Prefill a BLANK libav.dir field with the directory the probe resolved
   the libraries from (design ruling 2026-11-07: detected -> show where). Field
   only — cfg keeps its value until the usual save — and a filled field
   is the user's decision, never overwritten. A pinned dir can make
   loading no worse than an empty one: ff_dlopen falls back to the keg
   paths and the bare soname search when the configured dir misses. */
static void ff_prefill_libav(int probe_ok, const char *dir) {
    if ((probe_ok == 1 || probe_ok == 3) && dir && dir[0] && !ui.opt_field[OPTF_LIBAV][0])
        lv_strlcpy(ui.opt_field[OPTF_LIBAV], dir, LV_MAX_PATH);
}
static Rect opt_scan_rect(void)  { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBDIRS].y, OPT_SCAN_W, OPT_FLD_H }; return r; }
static Rect opt_chk_rect(void)   { Rect r = { OPT_FLD_X + OPT_FLD_W + OPT_SCAN_GAP + OPT_SCAN_W + OPT_SCAN_GAP, OPT_ROWS[OPTF_LIBDIRS].y, OPT_CHK_W, OPT_FLD_H }; return r; }

void ui_opt_open(void) {
    if (!ui.wopt) {
        int mx, my;
        SDL_GetWindowPosition(ui.wmain, &mx, &my);
        int ox = mx + (ui.mini ? MINI_W : MAIN_W);
        SDL_Rect d;
        if (SDL_GetDisplayBounds(0, &d) == 0 && ox + OPT_W > d.x + d.w) ox = mx - OPT_W;
        ui.wopt = SDL_CreateWindow("levari - options", ox, my,
                                   OPT_W, OPT_H, SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
        if (!ui.wopt) return;
        ui.snap[3].w = ui.wopt; ui.snap[3].dx = ox - mx; ui.snap[3].dy = 0;
        SDL_Surface *icon = skin_app_icon(32);
        if (icon) {
            SDL_SetWindowIcon(ui.wopt, icon);
            SDL_FreeSurface(icon);
        }
        ui.sopt = lv_win_surface(ui.wopt);
    }
    char ffdir[LV_MAX_PATH];
    ui.opt_field[OPTF_LIBDIRS][0] = 0;
    for (int i = 0; i < cfg.lib_dir_n; i++) {
        if (i) strncat(ui.opt_field[OPTF_LIBDIRS], ";", LV_MAX_PATH - strlen(ui.opt_field[OPTF_LIBDIRS]) - 1);
        strncat(ui.opt_field[OPTF_LIBDIRS], cfg.lib_dirs[i],
                LV_MAX_PATH - strlen(ui.opt_field[OPTF_LIBDIRS]) - 1);
    }
    lv_strlcpy(ui.opt_field[OPTF_LIBIDX],
               cfg.lib_index[0] ? cfg.lib_index : "~/.levari/index", LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_LIBAV],   cfg.libav_dir,     LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_BACKEND], cfg.audio_backend, LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_DEVICE],  cfg.audio_device,  LV_MAX_PATH);
    lv_strlcpy(ui.opt_field[OPTF_SOCK],    cfg.socket_path,   LV_MAX_PATH);
    ui.opt_pl  = cfg.ui_pl  > 0;
    ui.opt_eq  = cfg.ui_eq  > 0;
    ui.opt_vis = cfg.ui_vis > 0;
    ui.opt_srch = cfg.ui_search > 0;
    ui.opt_shuf = cfg.shuffle_remember > 0;
    ui.opt_fade = cfg.fade_stop > 0;
    ui.opt_large = cfg.allow_large > 0;
    ui.opt_f10 = cfg.ui_always10 > 0;
    ui.opt_vhz = (cfg.vis_hz == 24 || cfg.vis_hz == 30 || cfg.vis_hz == 60)
                 ? cfg.vis_hz : 10;
    ui.opt_rg = cfg.replaygain == 1 || cfg.replaygain == 2 ? cfg.replaygain : 0;
    ui.opt_enc = cfg.tag_legacy_enc + 1;   /* LV_ENC_AUTO(-1) -> cycle 0 */
    if (ui.opt_enc < 0 || ui.opt_enc > 4) ui.opt_enc = 0;
    ui.opt_ff = ui_ffprobe_info(ui.opt_ff_ver, sizeof ui.opt_ff_ver,
                                ffdir, sizeof ffdir);
    ff_prefill_libav(ui.opt_ff, ffdir);
    ui.opt_tab = 0;
    ui.opt_hover = -1;
    ui.opt_focus = -1;
    ui.opt_tf.buf = ui.opt_field[0]; /* bound for real on focus; keeps buf
                                        never NULL while the window draws */
    ui.opt_tf.cap = LV_MAX_PATH;
    ui.opt_tf.caret = 0;
    ui.opt_tf.selall = 0;
    ui.opt_status[0] = 0;
    ui.opt_tags = 1;
    ui.opt_visible = 1;
    SDL_ShowWindow(ui.wopt);
    ui_raise_all();
}

void ui_opt_close(void) {
    /* the encoding button applies live (tag_set_legacy_default on every click)
       but only SAVE persists it: on a close that was not a save, roll the
       live default back to the config value the window opened with, so the
       button (which re-reads cfg on next open) and tag.c never disagree.
       After a save cfg already equals the clicked value, so this is a no-op. */
    tag_set_legacy_default(cfg.tag_legacy_enc);
    ui.opt_visible = 0;
    ui.opt_focus = -1;
    ui.opt_tf.caret = 0;
    if (ui.wopt) SDL_HideWindow(ui.wopt);
    SDL_StopTextInput();
}

static void opt_save(void) {
    /* field writes under cfg lock: a concurrent daemon shuffle toggle
       must not interleave and let cfg_save persist a mixed snapshot */
    cfg_lock();
    cfg.lib_dir_n = 0;
    char copy[LV_MAX_PATH];
    lv_strlcpy(copy, ui.opt_field[OPTF_LIBDIRS], sizeof copy);
    char *tok = strtok(copy, ";");
    while (tok) {
        while (*tok == ' ') tok++;
        if (*tok && cfg.lib_dir_n < CFG_MAX_LIBDIRS)
            lv_strlcpy(cfg.lib_dirs[cfg.lib_dir_n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ";");
    }
    lv_strlcpy(cfg.lib_index,     ui.opt_field[OPTF_LIBIDX],  sizeof cfg.lib_index);
    lv_strlcpy(cfg.libav_dir,     ui.opt_field[OPTF_LIBAV],   sizeof cfg.libav_dir);
    lv_strlcpy(cfg.audio_backend, ui.opt_field[OPTF_BACKEND], sizeof cfg.audio_backend);
    lv_strlcpy(cfg.audio_device,  ui.opt_field[OPTF_DEVICE],  sizeof cfg.audio_device);
    lv_strlcpy(cfg.socket_path,   ui.opt_field[OPTF_SOCK],    sizeof cfg.socket_path);
    cfg.ui_pl = ui.opt_pl;
    cfg.ui_eq = ui.opt_eq;
    cfg.ui_vis = ui.opt_vis;
    cfg.ui_search = ui.opt_srch;
    cfg.shuffle_remember = ui.opt_shuf;
    if (ui.opt_shuf) cfg.shuffle = ui.pl->shuffle; /* capture the live state now */
    cfg.fade_stop = ui.opt_fade;
    ui.pl->fade_stop = ui.opt_fade; /* apply without waiting for a restart */
    cfg.ui_aot = ui.aot;            /* persists the live state (button or checkbox) */
    cfg.ui_mini = ui.mini;          /* ditto: the mini layout startup default */
    cfg.allow_large = ui.opt_large; /* player reads cfg at each load: no restart */
    cfg.ui_always10 = ui.opt_f10;   /* scheduler reads cfg live: no restart */
    cfg.vis_hz = ui.opt_vhz;        /* ditto: the vis tick period is live */
    cfg.replaygain = ui.opt_rg;     /* ditto: the store lands on the player
                                       immediately (atomic), the cache
                                       comparison applies it on the next
                                       chunk — no restart, no reload */
    atomic_store_explicit(&ui.pl->rg_mode, ui.opt_rg, memory_order_relaxed);
    cfg.tag_legacy_enc = ui.opt_enc - 1;  /* tag.c reads it live: decoding applies now,
                                             already-indexed rows keep old readings until re-scan */
    cfg_unlock();
    cfg_save();
}

typedef struct { char (*dirs)[LV_MAX_PATH]; int n; int tags; } ScanArg;

static int scan_thread(void *p) {
    ScanArg *a = p;
    int added = 0, seen = 0, skipped = 0;
    lib_scan_add(a->dirs, a->n, a->tags, &added, &seen, &skipped);
    ui.opt_scan_added = added;
    ui.opt_scan_seen = seen;
    ui.opt_scan_skipped = skipped;
    SDL_AtomicSet(&ui.opt_scan_done, 1);
    free(a->dirs);
    free(a);
    return 0;
}

/* VERIFY runs the same single-writer dance as scan: opt_scanning is the
   index-busy flag every index writer shares, opt_verifying only tells
   the completion block which report to print. lib_index_verify does its
   own atomic rewrite; nothing else may touch the index while it runs. */
static int verify_thread(void *unused) {
    (void)unused;
    char bank[LV_MAX_PATH];
    int kept = 0, purged = 0, unreach = 0;
    int rc = lib_index_verify(&kept, &purged, &unreach, bank, sizeof bank);
    ui.opt_vfy_rc = rc;
    ui.opt_vfy_kept = kept;
    ui.opt_vfy_purged = purged;
    ui.opt_vfy_unreach = unreach;
    lv_strlcpy(ui.opt_vfy_bank, bank, sizeof ui.opt_vfy_bank);
    SDL_AtomicSet(&ui.opt_scan_done, 1);
    return 0;
}

static void opt_start_verify(void) {
    if (ui.opt_scanning) return;      /* scan (or another verify) owns it */
    ui.opt_scanning = 1;
    ui.opt_verifying = 1;
    ui.opt_status[0] = 0;
    SDL_AtomicSet(&ui.opt_scan_done, 0);
    SDL_Thread *th = SDL_CreateThread(verify_thread, "lvverify", NULL);
    if (!th) {
        ui.opt_scanning = 0;
        ui.opt_verifying = 0;
        lv_strlcpy(ui.opt_status, "verify failed to start", sizeof ui.opt_status);
        return;
    }
    SDL_DetachThread(th);
}

static void opt_start_scan(void) {
    char dirs[CFG_MAX_LIBDIRS][LV_MAX_PATH];
    int n = 0;
    char copy[LV_MAX_PATH];
    lv_strlcpy(copy, ui.opt_field[OPTF_LIBDIRS], sizeof copy);
    char *tok = strtok(copy, ";");
    while (tok && n < CFG_MAX_LIBDIRS) {
        while (*tok == ' ') tok++;
        if (*tok) lv_strlcpy(dirs[n++], tok, LV_MAX_PATH);
        tok = strtok(NULL, ";");
    }
    if (!n) { lv_strlcpy(ui.opt_status, "no library dirs set", sizeof ui.opt_status); return; }
    ScanArg *a = malloc(sizeof *a);
    if (!a) { lv_strlcpy(ui.opt_status, "out of memory", sizeof ui.opt_status); return; }
    a->dirs = malloc((size_t)n * sizeof *a->dirs);
    if (!a->dirs) {
        free(a);
        lv_strlcpy(ui.opt_status, "out of memory", sizeof ui.opt_status);
        return;
    }
    a->n = n;
    a->tags = ui.opt_tags;
    for (int i = 0; i < n; i++) lv_strlcpy(a->dirs[i], dirs[i], LV_MAX_PATH);
    ui.opt_scanning = 1;
    ui.opt_status[0] = 0;
    SDL_AtomicSet(&ui.opt_scan_done, 0);
    SDL_Thread *th = SDL_CreateThread(scan_thread, "lvscan", a);
    if (!th) {
        free(a->dirs);
        free(a);
        ui.opt_scanning = 0;
        lv_strlcpy(ui.opt_status, "scan failed to start", sizeof ui.opt_status);
        return;
    }
    SDL_DetachThread(th);
}

static void opt_clear_index(void) {
    char idx[LV_MAX_PATH];
    lib_index_path(idx, sizeof idx);
    remove(idx);
    search_lib_invalidate();
    lv_strlcpy(ui.opt_status, "index cleared", sizeof ui.opt_status);
}

/* ABOUT page graphic: the levari logo (assets/levari.svg) as a 24x24
   pixel-art record, painted at 2x. Grid chars: '.' transparent, F rim +
   specular, G body, D groove, B label edge, K spindle hole — the colors
   are sampled from the SVG. Swap the rows for different art. */
static const char *ABOUT_ART[] = {
    ".........FFFFFF.........",
    ".......FFGGGGGGFF.......",
    ".....FFGFFFFFGGGGFF.....",
    "....FGFFGDDDDDDGGGGF....",
    "...FGFFDDGGGGGGDDGGGF...",
    "..FGGGDGGDDDDDDGGDGGGF..",
    "..FGGDGGDDGGGGDDGGDGGF..",
    ".FGGDGGDGGDDDDGGDGGDGGF.",
    ".FGGDGDGDDGGGGDDGDGDGGF.",
    "FGGDGDDGDGGBBGGDGDDGDGGF",
    "FGGDGDGDGGBGGBGGDGDGDGGF",
    "FGGDGDGDGBGKKGBGDGDGDGGF",
    "FGGDGDGDGBGKKGBGDGDGDGGF",
    "FGGDGDGDGGBGGBGGDGDGDGGF",
    "FGGDGDDGDGGBBGGDGDDGDGGF",
    ".FGGDGDGDDGGGGDDGDGDGGF.",
    ".FGGDGGDGGDDDDGGDGGDGGF.",
    "..FGGDGGDDGGGGDDGGDGGF..",
    "..FGGGDGGDDDDDDGGDGGGF..",
    "...FGGGDDGGGGGGDDGGGF...",
    "....FGGGGDDDDDDGGGGF....",
    ".....FFGGGGGGGGGGFF.....",
    ".......FFGGGGGGFF.......",
    ".........FFFFFF.........",
};
static Uint32 about_art_col(SDL_Surface *s, char ch) {
    switch (ch) {
    case 'F': return ui_col(s, 0xf2, 0xc9, 0x63);
    case 'G': return ui_col(s, 0xd9, 0xa4, 0x41);
    case 'D': return ui_col(s, 0xa8, 0x7f, 0x2f);
    case 'B': return ui_col(s, 0x7a, 0x5c, 0x20);
    default:  return ui_col(s, 0x2e, 0x24, 0x10); /* K + anything else */
    }
}

void ui_draw_opt(void) {
    SDL_Surface *s = ui.sopt;
    if (!s) return;
    skin_window_bg(s, OPT_W, OPT_H);
    font_puts(s, 10, 6, "OPTIONS", ui_col(s, 0, 224, 255));
    { Rect x = ui_close_rect(OPT_W);
      font_puts(s, x.x - 6 - font_w(LV_ARCH " v" LV_BUILD_ID), 6,
                LV_ARCH " v" LV_BUILD_ID, ui_col(s, 150, 150, 150));
      ui_draw_x(s, &x, ui.pressed == OPTB_X); }

    /* BASIC / ADV / ABOUT tabs: small buttons in the title bar, visible on
       all pages; the active tab is lit green */
    { Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tu = opt_tab_about_rect();
      skin_button(s, &tb, ui.pressed == OPTB_TABB, !ui.opt_tab);
      font_puts(s, tb.x + (tb.w - font_w("BASIC")) / 2, tb.y + 3, "BASIC",
                ui.opt_tab ? ui_col(s, 220, 220, 220) : ui_col(s, 0, 255, 0));
      skin_button(s, &ta, ui.pressed == OPTB_TABA, ui.opt_tab == 1);
      font_puts(s, ta.x + (ta.w - font_w("ADV")) / 2, ta.y + 3, "ADV",
                ui.opt_tab == 1 ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
      skin_button(s, &tu, ui.pressed == OPTB_TABU, ui.opt_tab == 2);
      font_puts(s, tu.x + (tu.w - font_w("ABOUT")) / 2, tu.y + 3, "ABOUT",
                ui.opt_tab == 2 ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220)); }

    if (!ui.opt_tab)
    for (int i = 0; i < OPTF_N; i++) {
        font_puts(s, 10, OPT_ROWS[i].y + 5, OPT_ROWS[i].lbl, ui_col(s, 150, 150, 150));
        Rect f = opt_field_rect(i);
        SDL_Rect fb = { f.x, f.y, f.w, f.h };
        SDL_FillRect(s, &fb, ui_col(s, 0, 0, 0));
        SDL_Rect t = { f.x, f.y, f.w, 1 }, b = { f.x, f.y + f.h - 1, f.w, 1 };
        SDL_Rect l = { f.x, f.y, 1, f.h }, r = { f.x + f.w - 1, f.y, 1, f.h };
        Uint32 bc = ui.opt_focus == i ? ui_col(s, 0, 224, 255) : ui_col(s, 90, 90, 90);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        char val[LV_MAX_PATH];
        lv_strlcpy(val, ui.opt_field[i], sizeof val);
        /* text longer than the box scrolls left. Unfocused: the trailing
           suffix shows. Focused: the window is anchored on the caret so it
           and its surroundings stay visible while editing mid-string. */
        TField tf = { val, sizeof val, ui.opt_tf.caret, ui.opt_tf.selall };
        ui_draw_field_text(s, f, val, &tf, ui.opt_focus == i, 1);
        ui.opt_tf.caret = tf.caret;   /* the helper clamps a stale caret */
    }

    if (!ui.opt_tab) {
    Rect sc = opt_scan_rect();
    skin_button(s, &sc, ui.pressed == OPTB_SCAN && !ui.opt_scanning, 0);
    const char *sl = ui.opt_scanning ? "BUSY" : "SCAN";
    font_puts(s, sc.x + (sc.w - font_w(sl)) / 2, sc.y + 3, sl,
              ui.opt_scanning ? ui_col(s, 150, 150, 150) : ui_col(s, 220, 220, 220));

    /* TAGS checkbox: when on, SCAN reads tags+duration; off = names only */
    Rect ck = opt_chk_rect();
    SDL_Rect box = { ck.x, ck.y + (ck.h - 12) / 2, 12, 12 };
    SDL_FillRect(s, &box, ui_col(s, 0, 0, 0));
    Uint32 bcol = ui.pressed == OPTB_CHK ? ui_col(s, 0, 224, 255) : ui_col(s, 140, 140, 140);
    SDL_Rect bt = { box.x, box.y, box.w, 1 }, bb = { box.x, box.y + box.h - 1, box.w, 1 };
    SDL_Rect bl = { box.x, box.y, 1, box.h }, br = { box.x + box.w - 1, box.y, 1, box.h };
    SDL_FillRect(s, &bt, bcol); SDL_FillRect(s, &bb, bcol);
    SDL_FillRect(s, &bl, bcol); SDL_FillRect(s, &br, bcol);
    if (ui.opt_tags) {
        Uint32 xc = ui_col(s, 0, 255, 0);
        for (int i = 0; i < 10; i++) {
            SDL_Rect p1 = { box.x + 1 + i, box.y + 1 + i, 1, 1 };
            SDL_Rect p2 = { box.x + 10 - i, box.y + 1 + i, 1, 1 };
            SDL_FillRect(s, &p1, xc); SDL_FillRect(s, &p2, xc);
        }
    }
    font_puts(s, box.x + 16, ck.y + 5, "TAGS",
              ui.opt_tags ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
    }

    if (!ui.opt_tab) {
    static const struct { int id; const char *l; } TOG[6] = {
        { OPTB_TPL, "UI PL" }, { OPTB_TEQ, "UI EQ" },
        { OPTB_TVIS, "UI VIS" }, { OPTB_TSRCH, "UI SRCH" },
        { OPTB_TSHUF, "SHUF" }, { OPTB_TFADE, "FADE" },
    };
    font_puts(s, 10, 193, "STARTUP", ui_col(s, 150, 150, 150));
    for (int i = 0; i < 6; i++) {
        Rect r = opt_tog_rect(i);
        int on = i == 0 ? ui.opt_pl : i == 1 ? ui.opt_eq :
                 i == 2 ? ui.opt_vis : i == 3 ? ui.opt_srch :
                 i == 4 ? ui.opt_shuf : ui.opt_fade;
        skin_button(s, &r, ui.pressed == TOG[i].id, on);
        font_puts(s, r.x + (r.w - font_w(TOG[i].l)) / 2, r.y + 3, TOG[i].l,
                  on ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
    }
    {   /* AOT: reads the LIVE state, not a staged copy — the main-window
           button can flip it while this window sits open */
        Rect ra = opt_aot_rect();
        skin_button(s, &ra, ui.pressed == OPTB_TAOT, ui.aot);
        font_puts(s, ra.x + (ra.w - font_w("AOT")) / 2, ra.y + 3, "AOT",
                  ui.aot ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
        Rect rm = opt_mini_rect();
        skin_button(s, &rm, ui.pressed == OPTB_TMINI, ui.mini);
        font_puts(s, rm.x + (rm.w - font_w("MINI")) / 2, rm.y + 3, "MINI",
                  ui.mini ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
    }
    } else if (ui.opt_tab == 1) {
        /* ADVANCED page: LARGE = allow ingest of files over the 256 MB
           decode cap (OOM guard for the slurp-based decoders) */
        font_puts(s, 10, 30, "ADVANCED", ui_col(s, 150, 150, 150));
        font_puts(s, 10, 52, "LARGE: allow ingest of files", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 66, "over 256 MB (memory risk)", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 88, "applies immediately, no restart", ui_col(s, 120, 120, 120));
        Rect r = opt_large_rect();
        skin_button(s, &r, ui.pressed == OPTB_TLARGE, ui.opt_large);
        font_puts(s, r.x + (r.w - font_w("LARGE")) / 2, r.y + 3, "LARGE",
                  ui.opt_large ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
        font_puts(s, 10, 110, "10FPS: keep repainting the", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 124, "main window at 10 fps", ui_col(s, 220, 220, 220));
        Rect r2 = opt_fps_rect();
        skin_button(s, &r2, ui.pressed == OPTB_TFPS, ui.opt_f10);
        font_puts(s, r2.x + (r2.w - font_w("10FPS")) / 2, r2.y + 3, "10FPS",
                  ui.opt_f10 ? ui_col(s, 0, 255, 0) : ui_col(s, 220, 220, 220));
        /* 5-state cycle button (NOT a checkbox): label shows the current
           override for legacy 8-bit tag fields */
        font_puts(s, 10, 146, "LEGACY: tag fields", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 160, "with no declared encoding", ui_col(s, 220, 220, 220));
        Rect r3 = opt_enc_rect();
        skin_button(s, &r3, ui.pressed == OPTB_TENC, 0);
        static const char *ENCL[5] = { "AUTO", "LATIN-1", "CP1251", "UTF-8", "SHIFT-JIS" };
        const char *el = ENCL[ui.opt_enc % 5];
        font_puts(s, r3.x + (r3.w - font_w(el)) / 2, r3.y + 3, el,
                  ui.opt_enc ? ui_col(s, 0, 224, 255) : ui_col(s, 220, 220, 220));
        /* ffmpeg substrate status (streaming + aac/m4a/wma): tells the
           user here whether installing/pointing libav.dir is needed at all */
        int ffred;
        const char *ffl = opt_ff_label(ui.opt_ff, ui.opt_ff_ver, &ffred);
        font_puts(s, 10, 186, ffl,
                  ffred ? ui_col(s, 255, 60, 60) : ui_col(s, 0, 255, 0));
        /* 4-state cycle button (NOT a checkbox): the visualizer band
           refresh rate; the label shows the current Hz. 10 is the floor
           default, so only a non-default value colors as an override. */
        font_puts(s, 10, 210, "VIZ: spectrum bars refresh", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 224, "cycle: 10 / 24 / 30 / 60 Hz", ui_col(s, 120, 120, 120));
        Rect r4 = opt_vhz_rect();
        skin_button(s, &r4, ui.pressed == OPTB_VHZ, 0);
        char vhzl[8];
        snprintf(vhzl, sizeof vhzl, "%dHZ", ui.opt_vhz);
        font_puts(s, r4.x + (r4.w - font_w(vhzl)) / 2, r4.y + 3, vhzl,
                  ui.opt_vhz != 10 ? ui_col(s, 0, 224, 255) : ui_col(s, 220, 220, 220));
        /* 3-state cycle button (NOT a checkbox): ReplayGain playback mode;
           off is the default, so only track/album color as an override.
           The row sits below SAVE/CLOSE (OPT_H carries the +28). */
        font_puts(s, 10, 248, "RG: replaygain playback", ui_col(s, 220, 220, 220));
        font_puts(s, 10, 262, "cycle: off / track / album", ui_col(s, 120, 120, 120));
        Rect r5 = opt_rg_rect();
        skin_button(s, &r5, ui.pressed == OPTB_RG, 0);
        static const char *RGL[3] = { "OFF", "TRACK", "ALBUM" };
        const char *rl = RGL[ui.opt_rg % 3];
        font_puts(s, r5.x + (r5.w - font_w(rl)) / 2, r5.y + 3, rl,
                  ui.opt_rg ? ui_col(s, 0, 224, 255) : ui_col(s, 220, 220, 220));
    } else {
        /* ABOUT page: static credits, no controls — centered title/icon/
           creator block over a left-aligned thanks list */
        font_puts_big(s, (OPT_W - font_w_big("Levari")) / 2, 28, "Levari", ui_col(s, 0, 224, 255));
        /* pixel-art record at 2x, centered under the title */
        int ax = (OPT_W - 48) / 2, ay = 48;
        for (int y = 0; y < (int)(sizeof ABOUT_ART / sizeof *ABOUT_ART); y++)
            for (int x = 0; ABOUT_ART[y][x]; x++) {
                if (ABOUT_ART[y][x] == '.') continue;
                SDL_Rect p = { ax + x * 2, ay + y * 2, 2, 2 };
                SDL_FillRect(s, &p, about_art_col(s, ABOUT_ART[y][x]));
            }
        {
            const char *lbl = "created by ", *nm = "Purkkaviritys";
            int cw = font_w(lbl) + 1 + font_w(nm);
            int cx = (OPT_W - cw) / 2;
            font_puts(s, cx, 106, lbl, ui_col(s, 150, 150, 150));
            font_puts(s, cx + font_w(lbl) + 1, 106, nm, ui_col(s, 0, 255, 0));
        }
        font_puts(s, 10, 126, "with thanks to", ui_col(s, 150, 150, 150));
        static const char *THANKS[] = {
            "SDL2",
            "libmodplug",
            "snes_spc (blargg)",
            "stb_vorbis (Sean Barrett)",
            "dr_wav, dr_flac (David Reid)",
            "minimp3 (lieff)",
            "cJSON (Dave Gamble)",
            "GNU Unifont",
            "ffmpeg",
        };
        for (int i = 0; i < (int)(sizeof THANKS / sizeof *THANKS); i++)
            font_puts(s, 10, 138 + i * 9, THANKS[i], ui_col(s, 220, 220, 220));
    }

    Rect sv = opt_save_rect(), cl = opt_close_rect();
    skin_button(s, &sv, ui.pressed == OPTB_SAVE, 0);
    font_puts(s, sv.x + (sv.w - font_w("SAVE")) / 2, sv.y + 3, "SAVE", ui_col(s, 220, 220, 220));
    skin_button(s, &cl, ui.pressed == OPTB_CLOSE, 0);
    font_puts(s, cl.x + (cl.w - font_w("CLOSE")) / 2, cl.y + 3, "CLOSE", ui_col(s, 220, 220, 220));

    if (!ui.opt_tab) {
    Rect cr = opt_clear_rect();
    skin_button(s, &cr, ui.pressed == OPTB_CLEAR && !ui.opt_scanning, 0);
    font_puts(s, cr.x + (cr.w - font_w("CLEAR")) / 2, cr.y + 3, "CLEAR", ui_col(s, 220, 220, 220));
    /* VERIFY: existence pass over the whole index (drops orphaned rows
       to <index>.dropped). Same busy gate as SCAN/CLEAR. */
    Rect vr = opt_verify_rect();
    skin_button(s, &vr, ui.pressed == OPTB_VERIFY && !ui.opt_scanning, 0);
    const char *vl = ui.opt_scanning ? "BUSY" : "VERIFY";
    font_puts(s, vr.x + (vr.w - font_w(vl)) / 2, vr.y + 3, vl,
              ui.opt_scanning ? ui_col(s, 150, 150, 150) : ui_col(s, 220, 220, 220));
    }

    if (ui.opt_status[0])
        font_puts(s, 10, 240, ui.opt_status, ui_col(s, 0, 224, 255));

    font_puts(s, 10, OPT_H - 12, "libav dir + socket apply on restart", ui_col(s, 120, 120, 120));

    /* hover tooltip: how library dirs are scanned + multi-dir syntax */
    if (ui.opt_hover == OPTF_LIBDIRS) {
        Rect f = opt_field_rect(OPTF_LIBDIRS);
        SDL_Rect tb = { 10, f.y + f.h + 4, 210, 30 };
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "dirs are scanned recursively", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "separate multiple dirs with ';'", ui_col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_VERIFY) {
        Rect rv = opt_verify_rect();
        SDL_Rect tb = { rv.x, rv.y + rv.h + 4, 250, 56 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x, tb.y + 1, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "checks every index row still exists", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "gone rows drop to index.dropped", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 29, "temporarily unreachable rows stay", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 41, "(share offline != file deleted)", ui_col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_SCAN) {
        Rect rs = opt_scan_rect();
        SDL_Rect tb = { rs.x, rs.y + rs.h + 4, 224, 42 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "scans the LIBRARY DIRS field", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "adds new rows, keeps existing", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 29, "TAGS box = read tags + length", ui_col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_CLEAR) {
        Rect rc = opt_clear_rect();
        SDL_Rect tb = { rc.x, rc.y + rc.h + 4, 224, 30 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "deletes the library index file", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "SCAN rebuilds it, tags re-read", ui_col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_TENC) {
        Rect r3 = opt_enc_rect();
        SDL_Rect tb = { r3.x, r3.y + r3.h + 4, 230, 42 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "encoding for fields with no marker", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "AUTO = decide from block evidence", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 29, "index needs CLEAR + SCAN to re-read", ui_col(s, 220, 220, 220));
    }
    if (ui.opt_hover == OPTB_VHZ) {
        Rect rv = opt_vhz_rect();
        /* above the button: below sits the RG row (the +28 ADV row) */
        SDL_Rect tb = { rv.x, rv.y - 34, 214, 30 };
        if (tb.x + tb.w > OPT_W) tb.x = OPT_W - tb.w;
        SDL_FillRect(s, &tb, ui_col(s, 40, 40, 40));
        SDL_Rect t = { tb.x, tb.y, tb.w, 1 }, b = { tb.x, tb.y + tb.h - 1, tb.w, 1 };
        SDL_Rect l = { tb.x, tb.y, 1, tb.h }, r = { tb.x + tb.w - 1, tb.y, 1, tb.h };
        Uint32 bc = ui_col(s, 140, 140, 140);
        SDL_FillRect(s, &t, bc); SDL_FillRect(s, &b, bc);
        SDL_FillRect(s, &l, bc); SDL_FillRect(s, &r, bc);
        font_puts(s, tb.x + 6, tb.y + 5, "visualizer refresh rate (Hz)", ui_col(s, 220, 220, 220));
        font_puts(s, tb.x + 6, tb.y + 17, "higher = smoother, more CPU", ui_col(s, 220, 220, 220));
    }
}

void ui_handle_opt_mouse(SDL_Event *e) {
    int x = e->motion.x, y = e->motion.y;
    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT) {
        Rect xc = ui_close_rect(OPT_W);
        if (ui_in_rect(&xc, x, y)) { ui.pressed = OPTB_X; return; }
        Rect sv = opt_save_rect(), cl = opt_close_rect();
        if (ui_in_rect(&sv, x, y)) { ui.pressed = OPTB_SAVE; return; }
        if (ui_in_rect(&cl, x, y)) { ui.pressed = OPTB_CLOSE; return; }
        Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tu = opt_tab_about_rect();
        if (ui_in_rect(&tb, x, y)) { ui.pressed = OPTB_TABB; return; }
        if (ui_in_rect(&ta, x, y)) { ui.pressed = OPTB_TABA; return; }
        if (ui_in_rect(&tu, x, y)) { ui.pressed = OPTB_TABU; return; }
        if (ui.opt_tab) {
            if (ui.opt_tab == 1) {
                Rect tg = opt_large_rect();
                if (ui_in_rect(&tg, x, y)) { ui.pressed = OPTB_TLARGE; return; }
                Rect tf = opt_fps_rect();
                if (ui_in_rect(&tf, x, y)) { ui.pressed = OPTB_TFPS; return; }
                Rect te = opt_enc_rect();
                if (ui_in_rect(&te, x, y)) { ui.pressed = OPTB_TENC; return; }
                Rect tv = opt_vhz_rect();
                if (ui_in_rect(&tv, x, y)) { ui.pressed = OPTB_VHZ; return; }
                Rect tr = opt_rg_rect();
                if (ui_in_rect(&tr, x, y)) { ui.pressed = OPTB_RG; return; }
            }
            if (y < 22) { ui.drag = 11; ui_drag_begin(ui.wopt); ui.opt_focus = -1; }
            return;
        }
        Rect sc = opt_scan_rect();
        if (ui_in_rect(&sc, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_SCAN; return; }
        Rect cr = opt_clear_rect();
        if (ui_in_rect(&cr, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_CLEAR; return; }
        Rect ck = opt_chk_rect();
        if (ui_in_rect(&ck, x, y)) { ui.pressed = OPTB_CHK; return; }
        Rect vr = opt_verify_rect();
        if (ui_in_rect(&vr, x, y)) { if (!ui.opt_scanning) ui.pressed = OPTB_VERIFY; return; }
        for (int i = 0; i < 6; i++) {
            Rect tg = opt_tog_rect(i);
            if (ui_in_rect(&tg, x, y)) { ui.pressed = OPTB_TPL + i; return; }
        }
        Rect ra = opt_aot_rect();
        if (ui_in_rect(&ra, x, y)) { ui.pressed = OPTB_TAOT; return; }
        Rect rm = opt_mini_rect();
        if (ui_in_rect(&rm, x, y)) { ui.pressed = OPTB_TMINI; return; }
        for (int i = 0; i < OPTF_N; i++) {
            Rect f = opt_field_rect(i);
            if (ui_in_rect(&f, x, y)) {
                ui.opt_focus = i;
                ui.opt_tf.buf = ui.opt_field[i];
                ui.opt_tf.caret = (int)strlen(ui.opt_tf.buf);
                ui.opt_tf.selall = 0;
                SDL_SetWindowInputFocus(ui.wopt);
                SDL_StartTextInput();
                return;
            }
        }
        if (y < 22) { ui.drag = 11; ui_drag_begin(ui.wopt); ui.opt_focus = -1; }
    } else if (e->type == SDL_MOUSEBUTTONUP) {
        if (ui.pressed != B_NONE) {
            Rect xc = ui_close_rect(OPT_W);
            Rect sv = opt_save_rect(), cl = opt_close_rect();
            Rect tb = opt_tab_basic_rect(), ta = opt_tab_adv_rect(), tg0 = opt_large_rect();
            Rect tf0 = opt_fps_rect(), te0 = opt_enc_rect(), tu = opt_tab_about_rect();
            Rect tv0 = opt_vhz_rect();
            if (ui.pressed == OPTB_X && ui_in_rect(&xc, x, y)) ui_opt_close();
            else if (ui.pressed == OPTB_TABB && ui_in_rect(&tb, x, y)) { ui.opt_tab = 0; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TABA && ui_in_rect(&ta, x, y)) { ui.opt_tab = 1; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TABU && ui_in_rect(&tu, x, y)) { ui.opt_tab = 2; ui.opt_focus = -1; }
            else if (ui.pressed == OPTB_TLARGE && ui_in_rect(&tg0, x, y)) ui.opt_large = !ui.opt_large;
            else if (ui.pressed == OPTB_TFPS && ui_in_rect(&tf0, x, y)) ui.opt_f10 = !ui.opt_f10;
            else if (ui.pressed == OPTB_TENC && ui_in_rect(&te0, x, y)) {
                ui.opt_enc = (ui.opt_enc + 1) % 5;
                tag_set_legacy_default(ui.opt_enc - 1); /* live effect; SAVE persists */
            }
            else if (ui.pressed == OPTB_VHZ && ui_in_rect(&tv0, x, y)) {
                /* 10 -> 24 -> 30 -> 60 -> 10 (the config table; a value
                   outside it — none is reachable — restarts at the default) */
                static const int VHZT[4] = { 10, 24, 30, 60 };
                int i = 0;
                while (i < 4 && VHZT[i] != ui.opt_vhz) i++;
                ui.opt_vhz = VHZT[i == 4 ? 0 : (i + 1) & 3];
                /* the scheduler reads cfg at tick time; effect lands on
                   SAVE with the rest (no hidden half-applied state) */
            }
            else if (ui.pressed == OPTB_RG) {
                Rect trg = opt_rg_rect();
                if (ui_in_rect(&trg, x, y)) ui.opt_rg = (ui.opt_rg + 1) % 3;  /* off -> track -> album */
            }
            else if (ui.pressed == OPTB_SAVE && ui_in_rect(&sv, x, y)) opt_save();
            else if (ui.pressed == OPTB_CLOSE && ui_in_rect(&cl, x, y)) ui_opt_close();
            else if (ui.pressed == OPTB_TMINI) {
                Rect rm = opt_mini_rect();
                if (ui_in_rect(&rm, x, y)) ui_set_mini(!ui.mini);
            }
            else if (ui.pressed == OPTB_TAOT) {
                /* own case, NOT folded into the OPTB_TPL.. range check
                   (that index maps to opt_tog_rect); applies immediately,
                   persists only through SAVE */
                Rect ra = opt_aot_rect();
                if (ui_in_rect(&ra, x, y)) {
                    ui.aot = !ui.aot;
                    ui_set_aot(ui.aot);
                }
            }
            else if (ui.pressed >= OPTB_TPL && ui.pressed <= OPTB_TFADE) {
                int i = ui.pressed - OPTB_TPL;
                Rect tg = opt_tog_rect(i);
                if (ui_in_rect(&tg, x, y)) {
                    if (i == 0) ui.opt_pl = !ui.opt_pl;
                    else if (i == 1) ui.opt_eq = !ui.opt_eq;
                    else if (i == 2) ui.opt_vis = !ui.opt_vis;
                    else if (i == 3) ui.opt_srch = !ui.opt_srch;
                    else if (i == 4) ui.opt_shuf = !ui.opt_shuf;
                    else ui.opt_fade = !ui.opt_fade;
                }
            }
            else if (ui.pressed == OPTB_SCAN) {
                Rect sc = opt_scan_rect();
                if (ui_in_rect(&sc, x, y)) opt_start_scan();
            } else if (ui.pressed == OPTB_CLEAR) {
                Rect cr = opt_clear_rect();
                if (ui_in_rect(&cr, x, y)) opt_clear_index();
            } else if (ui.pressed == OPTB_CHK) {
                Rect ck = opt_chk_rect();
                if (ui_in_rect(&ck, x, y)) ui.opt_tags = !ui.opt_tags;
            }
            else if (ui.pressed == OPTB_VERIFY) {
                Rect vr = opt_verify_rect();
                if (ui_in_rect(&vr, x, y)) opt_start_verify();
            }
            ui.pressed = B_NONE;
        }
        if (ui.drag == 11) {
            ui_drag_bounce(ui.wopt, ui.drag_wxlo, ui.drag_wxhi, ui.drag_wylo, ui.drag_wyhi);
            ui.drag = 0;
        }
    } else if (e->type == SDL_MOUSEMOTION && ui.drag == 11) {
        ui_drag_move_snap(ui.wopt);
    } else if (e->type == SDL_MOUSEMOTION) {
        if (ui.opt_tab == 1) {
            Rect te = opt_enc_rect(), tv = opt_vhz_rect();
            ui.opt_hover = ui_in_rect(&te, x, y) ? OPTB_TENC
                           : ui_in_rect(&tv, x, y) ? OPTB_VHZ : -1;
        } else if (!ui.opt_tab) {
            Rect f = opt_field_rect(OPTF_LIBDIRS), vr = opt_verify_rect();
            Rect sn = opt_scan_rect(), cr = opt_clear_rect();
            ui.opt_hover = ui_in_rect(&vr, x, y) ? OPTB_VERIFY
                           : ui_in_rect(&sn, x, y) ? OPTB_SCAN
                           : ui_in_rect(&cr, x, y) ? OPTB_CLEAR
                           : (x >= 8 && x < f.x + f.w && y >= f.y - 2 && y <= f.y + f.h + 2)
                           ? OPTF_LIBDIRS : -1;
        } else {
            ui.opt_hover = -1;
        }
    }
}

/* options field editing rides the shared TField ops (textedit.c) through
   the one shared switch (tf_handle_key); the ESC close and the per-field
   focus gate stay here, as do all OPTB_* side effects — only buffer
   editing moved */
void ui_opt_key(SDL_Event *e) {
    if (e->key.keysym.sym == SDLK_ESCAPE) { ui_opt_close(); return; }
    if (ui.opt_focus < 0) return;
    tf_handle_key(&ui.opt_tf, e->key.keysym.sym,
                  (e->key.keysym.mod & KMOD_CTRL) != 0);
}

void ui_opt_text(const char *text) {
    if (ui.opt_focus < 0) return;
    tf_insert(&ui.opt_tf, text);
}

/* when the options window's SHUF startup toggle is on, keep the shuffle
   state in the config file so it survives sessions */
void ui_shuffle_persist(void) {
    if (cfg.shuffle_remember > 0) cfg_set_shuffle(ui.pl->shuffle);
}
