#include "daemon.h"
#include "../playlist/playlist.h"
#include "../library/library.h"
#include "../config/config.h"
#include "../common.h"
#ifdef _WIN32
#include "../compat/win32.h"
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <errno.h>

#define VOL_STEP 8

static pthread_t srv_th;
static Player *srv_p;
static volatile int srv_quit;

#ifdef _WIN32
static HANDLE cur_pipe;
#else
static int listen_fd = -1;
#endif

static void sock_path(char *out, size_t n) {
    if (cfg.socket_path[0]) { lv_expand_home(cfg.socket_path, out, n); return; }
#ifdef _WIN32
    char user[256];
    lv_user_tag(user, sizeof user);
    snprintf(out, n, "\\\\.\\pipe\\levari-%s", user);
#else
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) snprintf(out, n, "%s/levari-%d.sock", xdg, (int)getuid());
    else snprintf(out, n, "/tmp/levari-%d.sock", (int)getuid());
#endif
}

static void fmt_t(char *b, size_t n, long long sec) {
    if (sec < 0) sec = 0;
    snprintf(b, n, "%lld:%02lld", sec / 60, sec % 60);
}

static void status_line(Player *p, char *out, size_t n) {
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    long long pos = p->rate ? (p->pos_frames - base) / p->rate : 0;
    long long tot = p->rate ? (endf - base) / p->rate : 0;
    char ps[16], ts[16];
    fmt_t(ps, sizeof ps, pos);
    fmt_t(ts, sizeof ts, tot);
    const char *st = p->state == ST_PLAY ? "playing" : p->state == ST_PAUSE ? "paused" : "stopped";
    char title[256];
    pl_current_title(p, title, sizeof title);
    snprintf(out, n, "%s %d/%d pos=%s/%s shuffle=%d repeat=%d vol=%d title=%s",
             st, p->cur + 1, p->pl->count, ps, ts,
             p->shuffle, p->repeat, p->vol, title);
}

static void handle_cmd(Player *p, char *line, char *resp, size_t rn) {
    char cmd[64] = "";
    sscanf(line, "%63s", cmd);
    resp[0] = 0;

    if (!strcmp(cmd, "status")) {
        status_line(p, resp, rn);
    } else if (!strcmp(cmd, "play")) {
        char *rest = line + 4;
        while (*rest == ' ') rest++;
        if (*rest) {
            /* build into a temp playlist WITHOUT holding p->mtx: a
               multi-MB m3u on a network share can take a while, and the
               UI draw loop blocks on that mutex every frame -- holding
               it through the load makes the desktop declare the window
               unresponsive. only the swap is locked. */
            Playlist np;
            pl_init(&np);
            char *tok = strtok(rest, " ");
            while (tok) {
                const char *dot = strrchr(tok, '.');
                if (dot && !lv_strcasecmp(dot, ".m3u")) pl_load_m3u(&np, tok);
                else pl_add(&np, tok);
                tok = strtok(NULL, " ");
            }
            /* index first: library queries arrive as bare paths, and the
               index already holds tags + duration for scanned files */
            lib_fill_playlist(&np);
            pthread_mutex_lock(&p->mtx);
            pl_clear(p->pl);
            p->pl->items = np.items;
            p->pl->count = np.count;
            p->pl->cap = np.cap;
            pthread_mutex_unlock(&p->mtx);
            pl_post(p, CMD_PLAY, 0);
        } else {
            if (p->state == ST_PAUSE) pl_post(p, CMD_TOGGLE_PAUSE, 0);
            else pl_post(p, CMD_PLAY, p->cur >= 0 ? p->cur : 0);
        }
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "toggle") || !strcmp(cmd, "pause") || !strcmp(cmd, "resume")) {
        pl_post(p, CMD_TOGGLE_PAUSE, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "stop")) {
        pl_post(p, CMD_STOP, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "next")) {
        pl_post(p, CMD_NEXT, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "prev")) {
        pl_post(p, CMD_PREV, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "seek")) {
        long long base = p->track_start;
        long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
        double cur = p->rate ? (double)(p->pos_frames - base) / p->rate : 0;
        char *v = line + 4;
        while (*v == ' ') v++;
        double target = cur;
        if (*v == '+' || *v == '-') target = cur + atof(v);
        else target = atof(v);
        if (endf > base)
            pl_post_seek(p, (target * p->rate) / (double)(endf - base));
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "vol")) {
        int v = atoi(line + 3);
        if (v < 0) v = 0;
        if (v > 128) v = 128;
        p->vol = v;
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "volup") || !strcmp(cmd, "voldown")) {
        int v = p->vol + (!strcmp(cmd, "volup") ? VOL_STEP : -VOL_STEP);
        if (v < 0) v = 0;
        if (v > 128) v = 128;
        p->vol = v;
        snprintf(resp, rn, "vol %d", v);
    } else if (!strcmp(cmd, "mute")) {
        if (p->vol > 0) { p->vol_prev = p->vol; p->vol = 0; }
        else p->vol = p->vol_prev > 0 ? p->vol_prev : 100;
        snprintf(resp, rn, "vol %d", p->vol);
    } else if (!strcmp(cmd, "pan")) {
        int v = atoi(line + 3);
        if (v < -32) v = -32;
        if (v > 32) v = 32;
        p->pan = v;
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "shuffle") || !strcmp(cmd, "repeat")) {
        int on = strstr(line, "on") != NULL;
        int off = strstr(line, "off") != NULL;
        int one = !strcmp(cmd, "repeat") && strstr(line, "one") != NULL;
        volatile int *flag = !strcmp(cmd, "shuffle") ? &p->shuffle : &p->repeat;
        if (one) *flag = 2;
        else if (on) *flag = 1; else if (off) *flag = 0; else *flag = !*flag;
        if (flag == &p->shuffle && cfg.shuffle_remember > 0) {
            cfg.shuffle = *flag;
            cfg_save();
        }
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "eq")) {
        int on = strstr(line, "on") != NULL;
        int off = strstr(line, "off") != NULL;
        if (on) eq_set_enabled(&p->eq, 1);
        else if (off) eq_set_enabled(&p->eq, 0);
        else eq_toggle(&p->eq);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "speed")) {
        double v = atof(line + 5);
        if (v < 0.5) v = 0.5;
        if (v > 2.0) v = 2.0;
        pl_post(p, CMD_SPEED, (int)(v * 100 + 0.5));
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "pl")) {
        pthread_mutex_lock(&p->mtx);
        int off = 0;
        for (int i = 0; i < p->pl->count && off < (int)rn - 64; i++) {
            Track *t = &p->pl->items[i];
            char pretty[LV_MAX_PATH];
            const char *name;
            if (t->tags.title[0]) name = t->tags.title;
            else { lv_pretty_name(t->path, pretty, sizeof pretty); name = pretty; }
            off += snprintf(resp + off, rn - off, "%d\t%s\t%s\n", i + 1, name, t->path);
        }
        pthread_mutex_unlock(&p->mtx);
        if (!off) lv_strlcpy(resp, "(empty)", rn);
    } else if (!strcmp(cmd, "add")) {
        char *rest = line + 3;
        while (*rest == ' ' || *rest == '\t') rest++;
        char *end = rest + strlen(rest);
        while (end > rest && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            *--end = 0;
        if (*rest) {
            pthread_mutex_lock(&p->mtx);
            pl_add(p->pl, rest);
            lib_fill_playlist(p->pl);
            pthread_mutex_unlock(&p->mtx);
        }
        lv_strlcpy(resp, "OK", rn);
    } else {
        snprintf(resp, rn, "ERR unknown command '%s'", cmd);
    }
}

#ifdef _WIN32

static void *srv_thread(void *arg) {
    (void)arg;
    char path[512];
    sock_path(path, sizeof path);
    /* owner-only pipe: with a NULL security descriptor the pipe inherits the
       token's default DACL, which lets other local users open the single
       instance (read-only, but enough to lock the real CLI/MCP out). The
       owner-SID ACE blocks them at open while the same user's CLI/MCP keep
       access even from another logon session. */
    PSECURITY_DESCRIPTOR psd = NULL;
    SECURITY_ATTRIBUTES sa, *sap = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;OW)", SDDL_REVISION_1, &psd, NULL)) {
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;
        sap = &sa;
    }
    while (!srv_quit) {
        HANDLE h = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    1, 0, 4096, 1000, sap);
        if (h == INVALID_HANDLE_VALUE) { if (psd) LocalFree(psd); return NULL; }
        cur_pipe = h;
        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h); cur_pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        if (!srv_quit) {
            char line[2048];
            DWORD n = 0;
            if (ReadFile(h, line, sizeof line - 1, &n, NULL) && n > 0) {
                line[n] = 0;
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                char resp[4096];
                handle_cmd(srv_p, line, resp, sizeof resp);
                size_t rl = strlen(resp);
                resp[rl++] = '\n';
                DWORD w = 0;
                WriteFile(h, resp, (DWORD)rl, &w, NULL);
                FlushFileBuffers(h);
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
        cur_pipe = INVALID_HANDLE_VALUE;
    }
    if (psd) LocalFree(psd);
    return NULL;
}

int srv_start(Player *p) {
    srv_p = p;
    srv_quit = 0;
    cur_pipe = INVALID_HANDLE_VALUE;
    pthread_create(&srv_th, NULL, srv_thread, NULL);
    return 0;
}

void srv_stop(void) {
    srv_quit = 1;
    HANDLE cp = cur_pipe;
    if (cp != INVALID_HANDLE_VALUE) CancelIoEx(cp, NULL);
    pthread_join(srv_th, NULL);
}

#else /* POSIX */

static void *srv_thread(void *arg) {
    (void)arg;
    while (!srv_quit) {
        struct pollfd pf = {listen_fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0) continue;
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0) continue;
        char line[2048];
        ssize_t n = read(c, line, sizeof line - 1);
        if (n > 0) {
            line[n] = 0;
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char resp[4096];
            handle_cmd(srv_p, line, resp, sizeof resp);
            size_t rl = strlen(resp);
            resp[rl++] = '\n';
            ssize_t off = 0;
            while (off < (ssize_t)rl) {
                ssize_t w = write(c, resp + off, rl - off);
                if (w <= 0) break;
                off += w;
            }
        }
        close(c);
    }
    return NULL;
}

int srv_start(Player *p) {
    srv_p = p;
    srv_quit = 0;
    char path[512];
    sock_path(path, sizeof path);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    /* if a live listener owns the path, do not steal it: unlink+bind
       would leave the first instance playing but unreachable by CLI/MCP */
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, (struct sockaddr *)&sa, sizeof sa) == 0) {
            close(probe);
            fprintf(stderr, "levari: another instance is already running; daemon disabled\n");
            return -1;
        }
        close(probe);
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    unlink(path);
    /* bind with a private umask so the socket is never group/other
       connectable, even briefly before the chmod */
    mode_t old_mask = umask(0077);
    int bind_rc = bind(listen_fd, (struct sockaddr *)&sa, sizeof sa);
    umask(old_mask);
    if (bind_rc < 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    chmod(path, 0600);
    if (listen(listen_fd, 4) < 0) { close(listen_fd); listen_fd = -1; return -1; }
    pthread_create(&srv_th, NULL, srv_thread, NULL);
    return 0;
}

void srv_stop(void) {
    if (listen_fd < 0) return;
    srv_quit = 1;
    pthread_join(srv_th, NULL);
    close(listen_fd);
    listen_fd = -1;
    char path[512];
    sock_path(path, sizeof path);
    unlink(path);
}

#endif
