#include "daemon.h"
#include "../common.h"
#include "../config/config.h"
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

#ifdef _WIN32
typedef HANDLE conn_t;
#define CONN_BAD INVALID_HANDLE_VALUE

static conn_t conn_open(const char *path) {
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}
static int conn_write(conn_t c, const void *b, size_t n) {
    DWORD w = 0;
    return WriteFile(c, b, (DWORD)n, &w, NULL) && w == (DWORD)n ? 0 : -1;
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
    return c;
}
static int conn_write(conn_t c, const void *b, size_t n) {
    return write(c, b, n) < 0 ? -1 : 0;
}
static long conn_read(conn_t c, void *b, size_t n) {
    ssize_t r = read(c, b, n);
    return r < 0 ? -1 : (long)r;
}
static void conn_close(conn_t c) { close(c); }
#endif

int cli_request(const char *cmd, char *resp, size_t rn) {
    if (rn) resp[0] = 0;
    char path[512];
    sock_path(path, sizeof path);
    conn_t c = conn_open(path);
    if (c == CONN_BAD) return 2;
    char line[8192];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (conn_write(c, line, strlen(line)) < 0) { conn_close(c); return 2; }
    char buf[8192];
    long n = conn_read(c, buf, sizeof buf - 1);
    conn_close(c);
    if (n <= 0) return 2;
    buf[n] = 0;
    lv_strlcpy(resp, buf, rn);
    return resp[0] == 'E' && resp[1] == 'R' ? 1 : 0;
}

int cli_send_line(const char *cmd) {
    char resp[8192];
    int rc = cli_request(cmd, resp, sizeof resp);
    if (rc == 2) {
        char path[512];
        sock_path(path, sizeof path);
        fprintf(stderr, "levari: no running instance (no socket at %s)\n", path);
        return 2;
    }
    fputs(resp, stdout);
    return rc;
}

int cli_run(int argc, char **argv) {
    char line[2048] = "";
    for (int i = 1; i < argc; i++) {
        size_t used = strlen(line);
        size_t argn = strlen(argv[i]);
        size_t sep = used ? 1 : 0;
        /* sep + arg + the newline appended below + NUL */
        if (used + sep + argn + 2 > sizeof line) {
            fprintf(stderr, "levari: command line truncated at %d args\n", i - 1);
            break;
        }
        if (sep) strcat(line, " ");
        strcat(line, argv[i]);
    }
    char path[512];
    sock_path(path, sizeof path);
    conn_t c = conn_open(path);
    if (c == CONN_BAD) {
        fprintf(stderr, "levari: no running instance (no socket at %s)\n", path);
        return 2;
    }
    strcat(line, "\n");
    if (conn_write(c, line, strlen(line) + 1) < 0) { conn_close(c); return 2; }
    char resp[8192];
    long n = conn_read(c, resp, sizeof resp - 1);
    conn_close(c);
    if (n <= 0) { fprintf(stderr, "levari: no response\n"); return 2; }
    resp[n] = 0;
    fputs(resp, stdout);
    return resp[0] == 'E' && resp[1] == 'R' ? 1 : 0;
}
