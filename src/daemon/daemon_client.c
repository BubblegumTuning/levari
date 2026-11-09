/* struct ucred (SO_PEERCRED) is a GNU extension in glibc headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "daemon.h"
#include "../common.h"
#include "../config/config.h"
#include "peercred.h"
#ifdef _WIN32
#include "../compat/win32.h"
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

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
#endif

/* client-side resolution: published /tmp path first, then the default */
static void sock_resolve(char *out, size_t n) {
#ifndef _WIN32
    if (lv_sock_pointer(out, n)) return;
#endif
    lv_sock_path(out, n);
}

#ifdef _WIN32
typedef HANDLE conn_t;
#define CONN_BAD INVALID_HANDLE_VALUE

static conn_t conn_open(const char *path) {
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}
static long conn_read(conn_t c, void *b, size_t n) {
    DWORD r = 0;
    return ReadFile(c, b, (DWORD)n, &r, NULL) ? (long)r : -1;
}
static void conn_close(conn_t c) { CloseHandle(c); }
#else
typedef int conn_t;
#define CONN_BAD (-1)

static conn_t conn_open(const char *path) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) return CONN_BAD;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    lv_strlcpy(sa.sun_path, path, sizeof sa.sun_path);
    if (connect(c, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(c);
        return CONN_BAD;
    }
    /* with XDG_RUNTIME_DIR unset the path falls back to /tmp: verify the
       listener we just connected to is ours, not a pre-bound impostor */
    if (!peer_is_self(c)) {
        close(c);
        return CONN_BAD;
    }
    return c;
}
static long conn_read(conn_t c, void *b, size_t n) {
    ssize_t r = read(c, b, n);
    return r < 0 ? -1 : (long)r;
}
static void conn_close(conn_t c) { close(c); }
#endif

static int conn_write(conn_t c, const void *b, size_t n) {
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
static long conn_read_line(conn_t c, char *buf, size_t n) {
    size_t off = 0;
    while (off < n - 1) {
        long r = conn_read(c, buf + off, n - 1 - off);
        if (r <= 0) break;
        off += (size_t)r;
        buf[off] = 0;
        if (strchr(buf, '\n')) break;
    }
    return (long)off;
}

int cli_request(const char *cmd, char *resp, size_t rn) {
    if (rn) resp[0] = 0;
    char path[512];
    sock_resolve(path, sizeof path);
    conn_t c = conn_open(path);
    if (c == CONN_BAD) return 2;
    char line[8192];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (conn_write(c, line, strlen(line)) < 0) { conn_close(c); return 2; }
    char buf[8192];
    long n = conn_read_line(c, buf, sizeof buf);
    conn_close(c);
    if (n <= 0) return 2;
    buf[n] = 0;
    lv_strlcpy(resp, buf, rn);
    return resp[0] == 'E' && resp[1] == 'R' ? 1 : 0;
}

/* append " arg": args with a space or a double quote travel quoted, with
   embedded " and \ escaped as \" and \\ (the daemon's play tokenizer and
   add handler unescape inside quotes). -1 = no room in cmd, nothing
   written. This is how a path like 'a b"c.mp3' survives transport —
   the old scheme refused to quote args that contained a quote at all,
   so the tokenizer spliced one path into several bogus entries. */
int cli_append_arg(char *cmd, size_t n, const char *arg) {
    size_t used = strlen(cmd);
    int quote = strpbrk(arg, " \"") != NULL;
    size_t elen = used ? 1 : 0; /* the separating space, not before arg 1 */
    for (const char *p = arg; *p; p++) {
        elen++;
        if (quote && (*p == '"' || *p == '\\')) elen++;
    }
    if (quote) elen += 2;
    if (used + elen + 1 > n) return -1;
    if (used) strcat(cmd, " ");
    if (!quote) {
        strcat(cmd, arg);
        return 0;
    }
    strcat(cmd, "\"");
    for (const char *p = arg; *p; p++) {
        char esc[3] = { '\\', *p, 0 };
        if (*p == '"' || *p == '\\') strcat(cmd, esc);
        else esc[0] = *p, esc[1] = 0, strcat(cmd, esc);
    }
    strcat(cmd, "\"");
    return 0;
}

int cli_run(int argc, char **argv) {
    char line[8192] = "";   /* matches the daemon's 8 KB line budget */
    for (int i = 1; i < argc; i++) {
        /* leave a byte for the newline appended before send */
        if (cli_append_arg(line, sizeof line - 1, argv[i]) != 0) {
            fprintf(stderr, "levari: command line truncated at %d args\n", i - 1);
            break;
        }
    }
    char path[512];
    sock_resolve(path, sizeof path);
    conn_t c = conn_open(path);
    if (c == CONN_BAD) {
        fprintf(stderr, "levari: no running instance (no socket at %s)\n", path);
        return 2;
    }
    strcat(line, "\n");
    if (conn_write(c, line, strlen(line) + 1) < 0) { conn_close(c); return 2; }
    char resp[8192];
    long n = conn_read_line(c, resp, sizeof resp);
    conn_close(c);
    if (n <= 0) { fprintf(stderr, "levari: no response\n"); return 2; }
    resp[n] = 0;
    fputs(resp, stdout);
    return resp[0] == 'E' && resp[1] == 'R' ? 1 : 0;
}
