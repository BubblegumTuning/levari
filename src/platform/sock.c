/* struct ucred (SO_PEERCRED) is a GNU extension in glibc headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "sock.h"
#include "platform.h"
#include "../config/config.h"
#include "../common.h"
#include "../compat/win98_diag.h" /* lv98_tracef (no-op off-era) */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>   /* struct xucred lives here, not in sys/un.h */
#endif
#endif

/* ---- endpoint paths (shared by server and client) ---- */

void lv_sock_path(char *out, size_t n) {
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

#ifndef _WIN32
int lv_tmp_fallback(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    return !cfg.socket_path[0] && !(xdg && *xdg);
}

/* where the daemon publishes its /tmp-fallback socket path: the config
   dir is the user's own — no other local user can write it */
void lv_sock_ptr_file(char *out, size_t n) {
    char d[LV_MAX_PATH];
    cfg_path(d, sizeof d);
    if (!lv_dirname(d)) { out[0] = 0; return; }
    snprintf(out, n, "%s/socket", d);
}

int lv_sock_pointer(char *out, size_t n) {
    if (!lv_tmp_fallback()) return 0;
    char f[LV_MAX_PATH];
    lv_sock_ptr_file(f, sizeof f);
    if (!f[0]) return 0;
    FILE *h = fopen(f, "r");
    if (!h) return 0;
    char line[512];
    int ok = 0;
    if (fgets(line, sizeof line, h)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        /* must fit sun_path or connect would silently truncate */
        if (line[0] && strlen(line) < sizeof ((struct sockaddr_un *)0)->sun_path) {
            lv_strlcpy(out, line, n);
            ok = 1;
        }
    }
    fclose(h);
    return ok;
}

/* Unix-domain socket peer-uid check. The spelling is platform-specific:
   SO_PEERCRED + struct ucred on Linux, LOCAL_PEERCRED + struct xucred
   on Apple. Best effort like the original call sites: a failed query
   passes, since the socket file is already owner-only (umask 0077 +
   chmod 0600); the check is defense against a pre-bound impostor on the
   /tmp fallback path. */
static int peer_is_self(lv_conn_t c) {
#ifdef __APPLE__
    struct xucred xc;
    socklen_t xl = sizeof xc;
    xc.cr_version = XUCRED_VERSION;
    if (getsockopt(c, SOL_LOCAL, LOCAL_PEERCRED, &xc, &xl) != 0) return 1;
    return xc.cr_uid == getuid();
#else
    struct ucred uc;
    socklen_t ul = sizeof uc;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &uc, &ul) != 0) return 1;
    return uc.uid == getuid();
#endif
}
#endif /* !_WIN32 */

/* ---- client-side connection primitives ---- */

#ifdef _WIN32
lv_conn_t lv_conn_open(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        /* batch 27: why the CLI could not reach the instance —
           gle 2 = nobody listens there, 5 = DACL, 231 = all busy */
        lv98_tracef("cli:open FAIL gle=%lu", GetLastError());
    return h;
}
long lv_conn_read(lv_conn_t c, void *b, size_t n) {
    DWORD r = 0;
    return ReadFile(c, b, (DWORD)n, &r, NULL) ? (long)r : -1;
}
void lv_conn_close(lv_conn_t c) { CloseHandle(c); }
int lv_listener_own(const char *path) {
    lv_conn_t p = lv_conn_open(path);
    if (p == LV_CONN_BAD) return 0;
    lv_conn_close(p);
    return 1;
}
#else
lv_conn_t lv_conn_open(const char *path) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) return LV_CONN_BAD;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    if (connect(c, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(c);
        return LV_CONN_BAD;
    }
    /* with XDG_RUNTIME_DIR unset the path falls back to /tmp: verify the
       listener we just connected to is ours, not a pre-bound impostor */
    if (!peer_is_self(c)) {
        close(c);
        return LV_CONN_BAD;
    }
    return c;
}
long lv_conn_read(lv_conn_t c, void *b, size_t n) {
    ssize_t r = read(c, b, n);
    return r < 0 ? -1 : (long)r;
}
void lv_conn_close(lv_conn_t c) { close(c); }
int lv_listener_own(const char *path) {
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
#endif

int lv_conn_write(lv_conn_t c, const void *b, size_t n) {
    /* loop: a short write would leave the daemon waiting for the rest
       of the line (and the newline with it) */
    const char *p = b;
    while (n) {
        long w;
#ifdef _WIN32
        DWORD d = 0;
        w = WriteFile(c, p, (DWORD)n, &d, NULL) ? (long)d : -1;
#else
        w = (long)write(c, p, n);
#endif
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* the server answers with one '\n'-terminated line; a single read can
   come back short (segmented sockets, the 4 KB pipe buffer vs a 4096+B
   response), so loop until the newline or EOF */
long lv_conn_read_line(lv_conn_t c, char *buf, size_t n) {
    size_t off = 0;
    while (off < n - 1) {
        long r = lv_conn_read(c, buf + off, n - 1 - off);
        if (r <= 0) break;
        off += (size_t)r;
        buf[off] = 0;
        if (strchr(buf, '\n')) break;
    }
    return (long)off;
}

/* ---- POSIX server-side listener ---- */

#ifndef _WIN32
int lv_listener_bind(const char *path) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    unlink(path);   /* only our own stale socket can sit at this name */
    /* bind with a private umask so the socket is never group/other
       connectable, even briefly before the chmod */
    mode_t old_mask = umask(0077);
    int bind_rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    umask(old_mask);
    if (bind_rc < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 4) < 0) { close(fd); return -1; }
    return fd;
}

lv_conn_t lv_conn_accept(int listen_fd) {
    int c = accept(listen_fd, NULL, NULL);
    if (c < 0) return LV_CONN_BAD;
    /* with XDG_RUNTIME_DIR unset the socket path falls back to
       world-writable /tmp: refuse a peer owned by another uid so a
       pre-bound impostor cannot harvest our status/title responses */
    if (!peer_is_self(c)) { close(c); return LV_CONN_BAD; }
    return c;
}
#endif /* !_WIN32 */
