#include "ui/ui.h"
#include "player/player.h"
#include "playlist/playlist.h"
#include "decode/decode.h"
#include "daemon/daemon.h"
#include "library/library.h"
#include "config/config.h"
#include "common.h"
#include <stdio.h>

static int is_cli_cmd(const char *s) {
    static const char *CMDS[] = {"play","pause","resume","toggle","stop","next","prev",
                                 "seek","vol","volup","voldown","mute","pan","speed","status","pl","add","shuffle","repeat","eq"};
    for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++)
        if (!strcmp(s, CMDS[i])) return 1;
    return 0;
}

static int probe(const char *path) {
    Decoder *d = dec_open(path);
    if (!d) { printf("%-24s FAIL open\n", path); return 1; }
    Tags t; tag_init(&t); tag_read(path, &t);
    int16_t buf[2048 * 2];
    int got = dec_read(d, buf, 2048);
    long long tot = dec_total_frames(d);
    int seekok = tot ? (dec_seek(d, tot / 2) == 0 && dec_read(d, buf, 2048) > 0) : 1;
    printf("%-24s %dHz %dch %lld frames  read=%d seek=%s  [%s / %s]\n",
           path, dec_rate(d), dec_channels(d), tot, got,
           seekok ? "ok" : "FAIL", t.artist, t.title);
    dec_close(d);
    return 0;
}

static Player player;
static Playlist playlist;

static int is_m3u(const char *p) {
    const char *e = strrchr(p, '.');
    return e && !lv_strcasecmp(e, ".m3u");
}
static int is_cue(const char *p) {
    const char *e = strrchr(p, '.');
    return e && !lv_strcasecmp(e, ".cue");
}

int main(int argc, char **argv) {
    cfg_load();
    if (argc > 1 && !strcmp(argv[1], "library"))
        return lib_cmd(argc - 1, argv + 1);
    if (argc > 1 && is_cli_cmd(argv[1]))
        return cli_run(argc, argv);

    if (dec_ffmpeg_status() == 2)
        fprintf(stderr, "levari: libav* shared libraries not found; aac/m4a/wma disabled\n");

    int show_pl = 1;
    if (cfg.ui_pl >= 0) show_pl = cfg.ui_pl;
    if (cfg.ui_eq >= 0) ui_show_eq = cfg.ui_eq;
    if (cfg.ui_vis >= 0) ui_show_vis = cfg.ui_vis;
    if (cfg.ui_search >= 0) ui_show_search = cfg.ui_search;
    const char *save_to = NULL;
    int selftest = 0;
    pl_init(&playlist);
    pl_player_init(&player, &playlist);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nopl")) show_pl = 0;
        else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) ui_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--eq")) ui_show_eq = 1;
        else if (!strcmp(argv[i], "--vis")) ui_show_vis = 1;
        else if (!strcmp(argv[i], "--probe")) {
            for (int j = i + 1; j < argc; j++) probe(argv[j]);
            return 0;
        }
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_to = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: levari [--nopl] [--save out.m3u] [files...]\n");
            return 0;
        }
        else if (is_m3u(argv[i])) pl_load_m3u(&playlist, argv[i]);
        else if (is_cue(argv[i])) pl_load_cue(&playlist, argv[i]);
        else pl_add(&playlist, argv[i]);
    }

    pl_player_start(&player);
    srv_start(&player);
    if (playlist.count > 0) pl_post(&player, CMD_PLAY, 0);
    if (selftest > 0) ui_deadline_ms = (unsigned)(selftest * 1000);
    ui_run(&player, show_pl);
    srv_stop();
    printf("selftest: cur=%d state=%d pos=%lld/%lld rate=%d\n",
           player.cur, player.state, player.pos_frames, player.total_frames, player.rate);
    pl_player_stop(&player);
    if (save_to) pl_save_m3u(&playlist, save_to);
    pl_clear(&playlist);
    return 0;
}
