/* struct ucred (SO_PEERCRED) is a GNU extension in glibc headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "daemon.h"
#include "../playlist/playlist.h"
#include "../library/library.h"
#include "../config/config.h"
#include "../common.h"
#include "peercred.h"
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
#ifndef _WIN32
#include <time.h>
#endif

#define VOL_STEP 8

static pthread_t srv_th;
static Player *srv_p;
static volatile int srv_quit;

#ifdef _WIN32
static HANDLE cur_pipe;
/* srv_stop reads cur_pipe while the server thread assigns and closes it:
   unsynchronized, CancelIoEx could target a handle value already closed
   and recycled for another object. the thread clears cur_pipe under this
   lock *before* CloseHandle, so a handle srv_stop sees is still open. */
static pthread_mutex_t pipe_mtx = PTHREAD_MUTEX_INITIALIZER;
#else
static int listen_fd = -1;
static char srv_path[512];   /* the path actually bound (random in /tmp fallback) */
static char srv_ptr_pub[512]; /* pointer file this instance published ('' if none) */
#endif

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
    /* count/cur read under p->mtx: the daemon's own play handler swaps
       the playlist under that lock; pl_current_title takes it internally,
       so grab the scalars first and release before calling it */
    pthread_mutex_lock(&p->mtx);
    int cur = p->cur, count = p->pl->count;
    pthread_mutex_unlock(&p->mtx);
    pl_current_title(p, title, sizeof title);
    lv_utf8_trim(title);
    snprintf(out, n, "%s %d/%d pos=%s/%s shuffle=%d repeat=%d vol=%d title=%s",
             st, cur + 1, count, ps, ts,
             p->shuffle, p->repeat, p->vol, title);
    lv_utf8_trim(out);
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
            /* split on spaces but honor double quotes: strtok(" ") made
               every path containing a space into several bogus paths.
               inside quotes \" and \\ decode to " and \, so paths that
               contain both spaces and quotes survive transport */
            for (char *s = rest; *s; ) {
                char *tok = s;
                if (*tok == '"') {
                    tok++;
                    char *r = tok, *w = tok;
                    while (*r && *r != '"') {
                        if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                            *w++ = r[1];
                            r += 2;
                        } else {
                            *w++ = *r++;
                        }
                    }
                    /* the closing quote must be tested BEFORE the NUL goes
                       in: with no escapes w lands on it and would erase
                       the only marker of where the token ended */
                    int closed = *r == '"';
                    *w = 0;
                    s = closed ? r + 1 : r;
                } else {
                    char *sp = strchr(tok, ' ');
                    if (sp) { *sp = 0; s = sp + 1; }
                    else s = tok + strlen(tok);
                }
                while (*s == ' ') s++;
                if (!*tok) continue;
                const char *dot = strrchr(tok, '.');
                if (dot && !lv_strcasecmp(dot, ".m3u")) pl_load_m3u(&np, tok);
                else pl_add(&np, tok);
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
        char *v = line + 3;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: vol <0-128>"); }
        else {
            int x = atoi(v);
            if (x < 0) x = 0;
            if (x > 128) x = 128;
            p->vol = x;
            lv_strlcpy(resp, "OK", rn);
        }
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
        char *v = line + 3;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: pan <-32..32>"); }
        else {
            int x = atoi(v);
            if (x < -32) x = -32;
            if (x > 32) x = 32;
            p->pan = x;
            lv_strlcpy(resp, "OK", rn);
        }
    } else if (!strcmp(cmd, "shuffle") || !strcmp(cmd, "repeat")) {
        /* the on/off/one token is positional: strstr over the whole line
           matched "on" inside paths and arguments */
        char tok[16] = "";
        sscanf(line + strlen(cmd), "%15s", tok);
        int on = !strcmp(tok, "on");
        int off = !strcmp(tok, "off");
        int one = !strcmp(cmd, "repeat") && !strcmp(tok, "one");
        _Atomic int *flag = !strcmp(cmd, "shuffle") ? &p->shuffle : &p->repeat;
        if (one) *flag = 2;
        else if (on) *flag = 1; else if (off) *flag = 0; else *flag = !*flag;
        if (flag == &p->shuffle && cfg.shuffle_remember > 0) {
            cfg_set_shuffle(*flag);
        }
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "eq")) {
        char tok[16] = "";
        sscanf(line + strlen(cmd), "%15s", tok);
        int on = !strcmp(tok, "on");
        int off = !strcmp(tok, "off");
        if (on) eq_set_enabled(&p->eq, 1);
        else if (off) eq_set_enabled(&p->eq, 0);
        else eq_toggle(&p->eq);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "speed")) {
        char *v = line + 5;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: speed <0.5-2.0>"); }
        else {
            double x = atof(v);
            if (x < 0.5) x = 0.5;
            if (x > 2.0) x = 2.0;
            pl_post(p, CMD_SPEED, (int)(x * 100 + 0.5));
            lv_strlcpy(resp, "OK", rn);
        }
    } else if (!strcmp(cmd, "pl")) {
        pthread_mutex_lock(&p->mtx);
        int off = 0;
        for (int i = 0; i < p->pl->count && off < (int)rn - 64; i++) {
            Track *t = &p->pl->items[i];
            char pretty[LV_MAX_PATH];
            const char *name;
            if (t->tags.title[0]) name = t->tags.title;
            else { lv_pretty_name(t->path, pretty, sizeof pretty); name = pretty; }
            /* snprintf returns the would-be length: off could grow past
               the buffer while the loop bound only re-checks next turn,
               making the next resp + off arithmetic fragile */
            int w = snprintf(resp + off, rn - off, "%d\t%s\t%s\n", i + 1, name, t->path);
            if (w < 0 || (size_t)w >= rn - off) break;
            off += w;
        }
        pthread_mutex_unlock(&p->mtx);
        if (!off) lv_strlcpy(resp, "(empty)", rn);
        else lv_utf8_trim(resp);
    } else if (!strcmp(cmd, "add")) {
        char *rest = line + 3;
        while (*rest == ' ' || *rest == '\t') rest++;
        char *end = rest + strlen(rest);
        while (end > rest && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            *--end = 0;
        /* the CLI quotes args containing spaces or quotes (same convention
           as the play tokenizer above); add keeps the rest of the line as
           ONE path, so strip exactly one surrounding quote pair and undo
           the \" \\ escapes it may contain */
        size_t rl = (size_t)(end - rest);
        if (rl >= 2 && rest[0] == '"' && end[-1] == '"') {
            end[-1] = 0;
            rest++;
            char *r = rest, *w = rest;
            while (*r) {
                if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    *w++ = r[1];
                    r += 2;
                } else {
                    *w++ = *r++;
                }
            }
            *w = 0;
        }
        if (*rest) {
            LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
            pthread_mutex_lock(&p->mtx);
            pl_add(p->pl, rest);
            lib_fill_playlist_ix(p->pl, &ix);
            pthread_mutex_unlock(&p->mtx);
            lib_index_free(&ix);
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
    lv_sock_path(path, sizeof path);
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
        /* FILE_FLAG_OVERLAPPED: the only supported way to bound a pipe
           read. SetCommTimeouts is documented for communications devices
           (not named pipes) and its return value was not even checked —
           the old "bounded read" comment claimed more than the code did. */
        HANDLE h = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    1, 0, 4096, 1000, sap);
        if (h == INVALID_HANDLE_VALUE) { if (psd) LocalFree(psd); return NULL; }
        pthread_mutex_lock(&pipe_mtx);
        cur_pipe = h;
        pthread_mutex_unlock(&pipe_mtx);
        OVERLAPPED conn = {0};
        conn.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        BOOL cok = ConnectNamedPipe(h, &conn);
        DWORD ce = GetLastError();
        if (!cok && ce == ERROR_IO_PENDING) {
            if (WaitForSingleObject(conn.hEvent, INFINITE) == WAIT_OBJECT_0) {
                DWORD ign = 0;
                /* the event only says "a completion arrived"; GetOverlappedResult
                   reaps it and reports the true connect status/error. Without it
                   the overlapped operation is left unreaped. */
                cok = GetOverlappedResult(h, &conn, &ign, FALSE);
                if (!cok) ce = GetLastError();
            }
        }
        if (!cok && ce != ERROR_PIPE_CONNECTED) {
            pthread_mutex_lock(&pipe_mtx);
            cur_pipe = INVALID_HANDLE_VALUE;
            pthread_mutex_unlock(&pipe_mtx);
            if (conn.hEvent) CloseHandle(conn.hEvent);
            CloseHandle(h);
            continue;
        }
        /* read one newline-framed line under a deadline over the WHOLE
           line: the per-read 1 s wait only bounds silence — a client
           dribbling bytes just under 1 s apart kept this serial loop
           busy byte-by-byte. 5 s total: an idle, dribbling or stalled
           client is dropped and the loop re-accepts. */
        if (!srv_quit) {
            OVERLAPPED rd = {0};
            rd.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            char line[8192];
            ULONGLONG t0 = GetTickCount64();
            DWORD n = 0, off = 0;
            while (off < sizeof line - 1) {
                ULONGLONG spent = GetTickCount64() - t0;
                DWORD left = spent >= 5000 ? 0 : (DWORD)(5000 - spent);
                if (left == 0) break; /* whole-line budget spent: drop it */
                ResetEvent(rd.hEvent);
                if (!ReadFile(h, line + off, (DWORD)(sizeof line - 1 - off), &n, &rd)) {
                    if (GetLastError() != ERROR_IO_PENDING) break;
                    if (WaitForSingleObject(rd.hEvent, left) != WAIT_OBJECT_0) {
                        CancelIoEx(h, &rd);
                        WaitForSingleObject(rd.hEvent, INFINITE);
                        break; /* budget/silence expired: drop the client */
                    }
                    if (!GetOverlappedResult(h, &rd, &n, FALSE)) break;
                }
                if (n == 0) break;
                off += n;
                line[off] = 0;
                if (strchr(line, '\n')) break;
            }
            if (rd.hEvent) CloseHandle(rd.hEvent);
            if (off > 0) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                char resp[4096];
                handle_cmd(srv_p, line, resp, sizeof resp);
                size_t rl = strlen(resp);
                resp[rl++] = '\n';
                /* the pipe is FILE_FLAG_OVERLAPPED: a synchronous WriteFile
                   (lpOverlapped == NULL) is invalid on an async handle. Drive
                   the write with its own OVERLAPPED + event, bounded by the
                   same 5 s budget the read uses. */
                OVERLAPPED wr = {0};
                wr.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
                DWORD w = 0;
                if (!WriteFile(h, resp, (DWORD)rl, &w, &wr) &&
                    GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(wr.hEvent, 5000) == WAIT_OBJECT_0)
                        GetOverlappedResult(h, &wr, &w, FALSE);
                    else
                        CancelIoEx(h, &wr);
                }
                if (wr.hEvent) CloseHandle(wr.hEvent);
                FlushFileBuffers(h);
            }
        }
        if (conn.hEvent) CloseHandle(conn.hEvent);
        DisconnectNamedPipe(h);
        pthread_mutex_lock(&pipe_mtx);
        cur_pipe = INVALID_HANDLE_VALUE;
        pthread_mutex_unlock(&pipe_mtx);
        CloseHandle(h);
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
    /* cancel under the lock: the thread cannot close the handle it just
       handed us while we hold it, so CancelIoEx never sees a closed or
       recycled value */
    pthread_mutex_lock(&pipe_mtx);
    HANDLE cp = cur_pipe;
    if (cp != INVALID_HANDLE_VALUE) CancelIoEx(cp, NULL);
    pthread_mutex_unlock(&pipe_mtx);
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
        /* with XDG_RUNTIME_DIR unset the socket path falls back to
           world-writable /tmp: refuse a peer owned by another uid so a
           pre-bound impostor cannot harvest our status/title responses */
        if (!peer_is_self(c)) { close(c); continue; }
        /* bound the read: a client that connects and sends nothing must
           not park the server thread forever (that deadlocks CLI/MCP),
           and the client sends up to 8 KB in one line — one read() into
           2 KB used to truncate long commands mid-path. The bound is a
           deadline over the WHOLE line read: SO_RCVTIMEO bounds a single
           read() only, so a client dribbling a byte every few seconds
           kept this serial accept loop from serving anyone else for
           minutes. */
        struct timespec t0, now;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        char line[8192];
        ssize_t off = 0;
        while (off < (ssize_t)sizeof line - 1) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long left = 5000 - ((long)(now.tv_sec - t0.tv_sec) * 1000
                                + (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (left <= 0) break; /* whole-line budget spent: drop it */
            struct pollfd cp = { c, POLLIN, 0 };
            if (poll(&cp, 1, (int)left) <= 0) break;
            ssize_t n = read(c, line + off, sizeof line - 1 - off);
            if (n <= 0) break;
            off += n;
            line[off] = 0;
            if (strchr(line, '\n')) break;
        }
        if (off > 0) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char resp[4096];
            handle_cmd(srv_p, line, resp, sizeof resp);
            size_t rl = strlen(resp);
            resp[rl++] = '\n';
            ssize_t off2 = 0;
            while (off2 < (ssize_t)rl) {
                ssize_t w = write(c, resp + off2, rl - off2);
                if (w <= 0) break;
                off2 += w;
            }
        }
        close(c);
    }
    return NULL;
}

/* 1 if a live listener at path belongs to this uid. a connect failure
   (stale socket) or a foreign peer returns 0 and the caller proceeds to
   unlink+bind. */
static int self_listener(const char *path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return 0;
    int self = 0;
    if (connect(probe, (struct sockaddr *)&sa, sizeof sa) == 0)
        self = peer_is_self(probe);
    close(probe);
    return self;
}

int srv_start(Player *p) {
    srv_p = p;
    srv_quit = 0;
    char path[512];
    lv_sock_path(path, sizeof path);
    int fallback = lv_tmp_fallback();
    /* if a live self-owned listener answers, do not steal its path:
       unlink+bind would leave the first instance playing but unreachable
       by CLI/MCP. a foreign peer is evicted via unlink+bind. */
    if (fallback) {
        /* /tmp is world-writable and sticky: another user can pre-bind the
           deterministic name and unlink of THEIR file then fails with EPERM,
           so eviction is impossible and the squat disables us. Bind an
           unpredictable name instead and publish it in our config dir,
           which no other user can write; the squat becomes useless. */
        char pub[512];
        if (lv_sock_pointer(pub, sizeof pub) && self_listener(pub)) {
            fprintf(stderr, "levari: another instance is already running; daemon disabled\n");
            return -1;
        }
        unsigned rnd = 0;
        FILE *u = fopen("/dev/urandom", "rb");
        if (u) { if (fread(&rnd, 1, sizeof rnd, u) != sizeof rnd) rnd = 0; fclose(u); }
        if (!rnd) rnd = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
        snprintf(path, sizeof path, "/tmp/levari-%d-%08x.sock", (int)getuid(), rnd);
    } else if (self_listener(path)) {
        fprintf(stderr, "levari: another instance is already running; daemon disabled\n");
        return -1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    unlink(path);   /* only our own stale socket can sit at this name */
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
    if (fallback) {
        char pf[512];
        lv_sock_ptr_file(pf, sizeof pf);
        char dir[512];
        lv_strlcpy(dir, pf, sizeof dir);
        if (lv_dirname(dir)) mkdir(dir, 0755);
        /* publish atomically: a client resolving the pointer must never read a
           half-written path. Write a per-pid temp in the same directory, then
           rename() over the target (atomic within one filesystem). */
        int wrote = 0;
        if (pf[0]) {
            char tmp[600];
            snprintf(tmp, sizeof tmp, "%s.%ld.tmp", pf, (long)getpid());
            FILE *w = fopen(tmp, "w");
            if (w) {
                fputs(path, w);
                if (fclose(w) == 0 && chmod(tmp, 0600) == 0 && rename(tmp, pf) == 0)
                    wrote = 1;
                else
                    unlink(tmp);
            }
        }
        if (wrote) lv_strlcpy(srv_ptr_pub, pf, sizeof srv_ptr_pub);
        else fprintf(stderr, "levari: cannot publish socket path to %s; "
                             "CLI/MCP cannot reach this instance\n", pf);
    }
    lv_strlcpy(srv_path, path, sizeof srv_path);
    pthread_create(&srv_th, NULL, srv_thread, NULL);
    return 0;
}

void srv_stop(void) {
    if (listen_fd < 0) return;
    srv_quit = 1;
    pthread_join(srv_th, NULL);
    close(listen_fd);
    listen_fd = -1;
    unlink(srv_path);   /* the published path may differ from the default */
    /* the pointer file is ours to remove too: a stale one makes every
       client resolve a dead path before falling back */
    if (srv_ptr_pub[0]) { unlink(srv_ptr_pub); srv_ptr_pub[0] = 0; }
}

#endif
