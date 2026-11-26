/* unit test: the ADV-tab VIZ Hz cycle button (OPTB_VHZ) end to end —
   hand-built SDL_Events drive ui_handle_opt_mouse (down, up), the cycle
   walks 10 -> 24 -> 30 -> 60 -> 10, opt_save persists cfg.vis_hz, the
   config reader clamps anything outside the table back to the default,
   and ui_vis_frame_ms maps the table (plus a garbage default) to tick
   periods. Technique: #include %s. Build, link and run are CTest targets now
   (refactor item 3 — the find-glob recipes died with levari.dir's
   objects): cmake --build build && ctest --test-dir build -R '^testoptvhz$'
   Link = filtered levari_ui archive minus ui_opt.c.o + levari_engine
   (pulls levari_core) + the dec tier/ffab objects + vendored libs.
   tools/ctest_home.sh runs every case in a fresh scratch dir (the
   name contains 'scratch'): HOME/XDG_CONFIG_HOME/XDG_RUNTIME_DIR
   there, audio.backend=null, /tmp/scratch-p2 present, cwd inside,
   SDL_VIDEODRIVER=dummy.
*/
#include "../../src/ui/ui_opt.c"
#include <stdlib.h>

static Player fake_pl;
static int fails;
static void chk(const char *what, long got, long want) {
    if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
    else printf("PASS %s\n", what);
}

static void click(int x, int y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.motion.x = x; e.motion.y = y;
    ui_handle_opt_mouse(&e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.state = SDL_RELEASED;
    ui_handle_opt_mouse(&e);
}

/* rewrite the user config with one ui.vis_hz line and re-read it */
static void cfg_with_hz(const char *hzval) {
    const char *home = getenv("XDG_CONFIG_HOME");
    char path[LV_MAX_PATH], dir[LV_MAX_PATH];
    snprintf(dir, sizeof dir, "%s/levari", home);
    mkdir(dir, 0755);          /* EEXIST fine; HOME exists by contract */
    snprintf(path, sizeof path, "%s/levari/config", home);
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL cfg_with_hz: cannot write %s\n", path); fails++; return; }
    fprintf(f, "ui.vis_hz = %s\n", hzval);
    fclose(f);
    cfg_load();
}

/* rewrite the user config with one audio.replaygain line and re-read it */
static void cfg_with_rg(const char *val) {
    const char *home = getenv("XDG_CONFIG_HOME");
    char path[LV_MAX_PATH];
    snprintf(path, sizeof path, "%s/levari/config", home);
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL cfg_with_rg: cannot write %s\n", path); fails++; return; }
    fprintf(f, "audio.replaygain = %s\n", val);
    fclose(f);
    cfg_load();
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home) { printf("caller must set scratch HOME/XDG_CONFIG_HOME\n"); return 1; }

    /* --- config defaults and clamping --------------------------------- */
    /* idempotent on a reused scratch HOME: run 1's opt_save leaves
       ui.vis_hz = 60 behind — start from no config file */
    {
        char p[LV_MAX_PATH];
        snprintf(p, sizeof p, "%s/levari/config", getenv("XDG_CONFIG_HOME"));
        unlink(p);
    }
    cfg_load();
    chk("default vis_hz is 10", cfg.vis_hz, 10);
    cfg_with_hz("24");
    chk("config 24 accepted", cfg.vis_hz, 24);
    cfg_with_hz("99");
    chk("config 99 falls back to 10", cfg.vis_hz, 10);
    cfg_with_hz("0");
    chk("config 0 falls back to 10", cfg.vis_hz, 10);
    cfg_with_hz("60");
    chk("config 60 accepted", cfg.vis_hz, 60);

    /* --- the tick table (the scheduler's only hz consumer) ------------- */
    chk("10 Hz -> 100 ms", ui_vis_frame_ms(10), 100);
    chk("24 Hz -> 41 ms", ui_vis_frame_ms(24), 41);
    chk("30 Hz -> 33 ms", ui_vis_frame_ms(30), 33);
    chk("60 Hz -> 16 ms", ui_vis_frame_ms(60), 16);
    chk("garbage -> 100 ms (never a busy loop)", ui_vis_frame_ms(7), 100);

    /* --- the ADV cycle button via the real mouse chain ----------------- */
    cfg_with_hz("10");
    ui.pl = &fake_pl;
    ui.opt_visible = 1;
    ui.opt_tab = 1;
    ui.pressed = B_NONE;
    ui.opt_vhz = cfg.vis_hz;         /* what ui_opt_open does */
    chk("opens at 10", ui.opt_vhz, 10);

    Rect r = opt_vhz_rect();
    click(r.x + r.w / 2, r.y + r.h / 2);
    chk("cycle 1 -> 24", ui.opt_vhz, 24);
    click(r.x + r.w / 2, r.y + r.h / 2);
    chk("cycle 2 -> 30", ui.opt_vhz, 30);
    click(r.x + r.w / 2, r.y + r.h / 2);
    chk("cycle 3 -> 60", ui.opt_vhz, 60);
    click(r.x + r.w / 2, r.y + r.h / 2);
    chk("cycle wraps to 10", ui.opt_vhz, 10);

    /* land on 60, SAVE, re-read: the persisted value round-trips */
    click(r.x + r.w / 2, r.y + r.h / 2);
    click(r.x + r.w / 2, r.y + r.h / 2);
    click(r.x + r.w / 2, r.y + r.h / 2);
    opt_save();
    cfg_load();
    chk("opt_save persisted 60", cfg.vis_hz, 60);

    /* an off-table opt copy restarts at the default instead of jamming */
    ui.opt_vhz = 777;
    click(r.x + r.w / 2, r.y + r.h / 2);
    chk("garbage copy restarts at 10", ui.opt_vhz, 10);

    /* --- ReplayGain: config default/clamp + ADV cycle + SAVE store ---- */
    {
        char p[LV_MAX_PATH];
        snprintf(p, sizeof p, "%s/levari/config", getenv("XDG_CONFIG_HOME"));
        unlink(p);
    }
    cfg_load();
    chk("default replaygain is off", cfg.replaygain, 0);
    cfg_with_rg("track");
    chk("config track accepted", cfg.replaygain, 1);
    cfg_with_rg("ALBUM");
    chk("config ALBUM accepted (case-insensitive)", cfg.replaygain, 2);
    cfg_with_rg("loud");
    chk("config garbage stays off", cfg.replaygain, 0);

    cfg_with_rg("off");
    ui.opt_rg = cfg.replaygain;        /* what ui_opt_open does */
    chk("RG opens at off", ui.opt_rg, 0);
    Rect rr = opt_rg_rect();
    click(rr.x + rr.w / 2, rr.y + rr.h / 2);
    chk("RG cycle 1 -> track", ui.opt_rg, 1);
    click(rr.x + rr.w / 2, rr.y + rr.h / 2);
    chk("RG cycle 2 -> album", ui.opt_rg, 2);
    click(rr.x + rr.w / 2, rr.y + rr.h / 2);
    chk("RG cycle wraps to off", ui.opt_rg, 0);
    /* land on track, SAVE: cfg persists AND the player store lands live */
    click(rr.x + rr.w / 2, rr.y + rr.h / 2);
    opt_save();
    cfg_load();
    chk("opt_save persisted track", cfg.replaygain, 1);
    chk("opt_save stored rg_mode live on the player",
        atomic_load_explicit(&fake_pl.rg_mode, memory_order_relaxed), 1);

    printf(fails ? "FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
