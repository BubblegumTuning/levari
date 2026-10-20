#include "config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <stdio.h>

Config cfg;

void cfg_path(char *out, size_t n) {
    char base[LV_MAX_PATH];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) snprintf(base, sizeof base, "%s/levari", appdata);
    else lv_strlcpy(base, ".", sizeof base);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(base, sizeof base, "%s/levari", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(base, sizeof base, "%s/.config/levari", home ? home : ".");
    }
#endif
    snprintf(out, n, "%s/config", base);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static int onoff(const char *v) {
    if (!lv_strcasecmp(v, "on") || !lv_strcasecmp(v, "1") ||
        !lv_strcasecmp(v, "yes") || !lv_strcasecmp(v, "true")) return 1;
    if (!lv_strcasecmp(v, "off") || !lv_strcasecmp(v, "0") ||
        !lv_strcasecmp(v, "no") || !lv_strcasecmp(v, "false")) return 0;
    return -1;
}

static void set_key(const char *k, char *v) {
    if (!strcmp(k, "library.dir")) {
        if (*v && cfg.lib_dir_n < CFG_MAX_LIBDIRS)
            lv_strlcpy(cfg.lib_dirs[cfg.lib_dir_n++], v, LV_MAX_PATH);
    } else if (!strcmp(k, "library.index")) {
        lv_strlcpy(cfg.lib_index, v, sizeof cfg.lib_index);
    } else if (!strcmp(k, "libav.dir")) {
        lv_strlcpy(cfg.libav_dir, v, sizeof cfg.libav_dir);
    } else if (!strcmp(k, "audio.backend")) {
        lv_strlcpy(cfg.audio_backend, v, sizeof cfg.audio_backend);
    } else if (!strcmp(k, "audio.device")) {
        lv_strlcpy(cfg.audio_device, v, sizeof cfg.audio_device);
    } else if (!strcmp(k, "daemon.socket")) {
        lv_strlcpy(cfg.socket_path, v, sizeof cfg.socket_path);
    } else if (!strcmp(k, "ui.playlist")) {
        int o = onoff(v); if (o >= 0) cfg.ui_pl = o;
    } else if (!strcmp(k, "ui.eq")) {
        int o = onoff(v); if (o >= 0) cfg.ui_eq = o;
    } else if (!strcmp(k, "ui.vis")) {
        int o = onoff(v); if (o >= 0) cfg.ui_vis = o;
    } else if (!strcmp(k, "ui.search")) {
        int o = onoff(v); if (o >= 0) cfg.ui_search = o;
    }
}

void cfg_load(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.ui_pl = cfg.ui_eq = cfg.ui_vis = cfg.ui_search = -1;
    char path[LV_MAX_PATH];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            set_key(k, v);
        }
        fclose(f);
    }
    if (!cfg.lib_dir_n)
        lv_strlcpy(cfg.lib_dirs[cfg.lib_dir_n++], "~/Music", LV_MAX_PATH);
}

static const char *onoff_s(int v) { return v > 0 ? "on" : "off"; }

int cfg_save(void) {
    char path[LV_MAX_PATH];
    cfg_path(path, sizeof path);
    char dir[LV_MAX_PATH];
    lv_strlcpy(dir, path, sizeof dir);
    if (lv_dirname(dir)) mkdir(dir, 0755);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("# levari config\n", f);
    for (int i = 0; i < cfg.lib_dir_n; i++)
        fprintf(f, "library.dir = %s\n", cfg.lib_dirs[i]);
    fprintf(f, "library.index = %s\n", cfg.lib_index);
    fprintf(f, "libav.dir = %s\n", cfg.libav_dir);
    fprintf(f, "audio.backend = %s\n", cfg.audio_backend);
    fprintf(f, "audio.device = %s\n", cfg.audio_device);
    fprintf(f, "daemon.socket = %s\n", cfg.socket_path);
    if (cfg.ui_pl >= 0) fprintf(f, "ui.playlist = %s\n", onoff_s(cfg.ui_pl));
    if (cfg.ui_eq >= 0) fprintf(f, "ui.eq = %s\n", onoff_s(cfg.ui_eq));
    if (cfg.ui_vis >= 0) fprintf(f, "ui.vis = %s\n", onoff_s(cfg.ui_vis));
    if (cfg.ui_search >= 0) fprintf(f, "ui.search = %s\n", onoff_s(cfg.ui_search));
    fclose(f);
    return 0;
}
