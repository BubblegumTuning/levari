#ifndef LEVARI_CONFIG_H
#define LEVARI_CONFIG_H

#include <stddef.h>
#include "../common.h"

#define CFG_MAX_LIBDIRS 8

typedef struct {
    char lib_dirs[CFG_MAX_LIBDIRS][LV_MAX_PATH]; /* library.dir, repeatable */
    int  lib_dir_n;
    char lib_index[LV_MAX_PATH];   /* library.index; "" = XDG default */
    char libav_dir[LV_MAX_PATH];   /* libav.dir; "" = system search */
    char audio_backend[16];        /* audio.backend: "" auto | pulse | alsa | null */
    char audio_device[128];        /* audio.device; "" = default */
    char socket_path[LV_MAX_PATH]; /* daemon.socket; "" = XDG default */
    int  ui_pl, ui_eq, ui_vis, ui_search; /* ui.playlist/eq/vis/search: -1 unset, 0 off, 1 on */
    int  shuffle_remember; /* player.shuffle_remember: -1 unset, 0 off, 1 on */
    int  shuffle;          /* player.shuffle: remembered state, -1 unset */
    int  fade_stop;        /* player.fade_stop: 0 off, 1 on; default on */
    int  allow_large;      /* decode.allow_large: 0 off (default), 1 on =
                              accept files over the 256 MB ingest cap */
    int  ui_always10;      /* ui.always_10fps: 0 off (default), 1 on = keep
                              repainting the main window at 10 fps whenever
                              a title is long enough to marquee, so it keeps
                              scrolling while paused/stopped/unfocused */
} Config;

extern Config cfg;

/* path of the config file (does not create anything) */
void cfg_path(char *out, size_t n);
/* load $XDG_CONFIG_HOME/levari/config; missing file is not an error */
void cfg_load(void);
/* write the current cfg back to the config file; 0 = ok */
int  cfg_save(void);

#endif
