#include "daemon.h"
#include "../common.h"
#include "../config/config.h"
#include "../compat/win98_diag.h" /* lv98_tracef (no-op off-era) */
#include "../platform/sock.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* client-side resolution: published /tmp path first, then the default */
static void sock_resolve(char *out, size_t n) {
#ifndef _WIN32
    if (lv_sock_pointer(out, n)) return;
#endif
    lv_sock_path(out, n);
}

int cli_request(const char *cmd, char *resp, size_t rn) {
    if (rn) resp[0] = 0;
    char path[512];
    sock_resolve(path, sizeof path);
    lv_conn_t c = lv_conn_open(path);
    if (c == LV_CONN_BAD) return 2;
    char line[8192];
    snprintf(line, sizeof line, "%s\n", cmd);
    if (lv_conn_write(c, line, strlen(line)) < 0) { lv_conn_close(c); return 2; }
    char buf[8192];
    long n = lv_conn_read_line(c, buf, sizeof buf);
    lv_conn_close(c);
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
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 29: this box has no named pipes (daemon.h note) — the
       client half of the file conversation: stamp the request,
       poll for the stamped answer (server polls at 250 ms, so the
       full round is bounded ~1 s; 12 s client patience). */
    char reqp[MAX_PATH], respp[MAX_PATH];
    if (lv_ipc_file(reqp, sizeof reqp, "req") != 0 ||
        lv_ipc_file(respp, sizeof respp, "resp") != 0) {
        fprintf(stderr, "levari: cannot locate module dir for IPC\n");
        return 2;
    }
    {
        unsigned tok = (unsigned)GetTickCount();
        char stamped[8208];
        HANDLE f;
        char resp[8192];
        char rtok[32];
        int t, got = 0;
        snprintf(stamped, sizeof stamped, "T%u %s\n", tok, line);
        f = CreateFileA(reqp, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            lv98_tracef("cli:open FAIL gle=%lu", GetLastError());
            fprintf(stderr, "levari: cannot write %s (exe dir not writable?)\n",
                    reqp);
            return 2;
        }
        {
            DWORD w;
            WriteFile(f, stamped, (DWORD)strlen(stamped), &w, NULL);
        }
        CloseHandle(f);
        lv98_tracef("cli:file req tok=%u", tok);
        snprintf(rtok, sizeof rtok, "T%u ", tok);
        for (t = 0; t < 48 && !got; t++) {
            DWORD n = 0;
            f = CreateFileA(respp, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                ReadFile(f, resp, (DWORD)sizeof resp - 1, &n, NULL);
                CloseHandle(f);
                resp[n] = 0;
                if (n > 0 && strncmp(resp, rtok, strlen(rtok)) == 0)
                    got = 1;
            }
            if (!got) Sleep(250);
        }
        if (!got) {
            lv98_tracef("cli:timeout");
            fprintf(stderr,
                    "levari: no response after 12 s (is a GUI instance running?)\n");
            return 2;
        }
        {
            char *body = resp + strlen(rtok);
            char *nl = strchr(body, '\n');
            if (nl) *nl = 0;
            lv98_tracef("cli:ok n=%d", (int)strlen(body));
            fputs(body, stdout);
            fputs("\n", stdout);
            return body[0] == 'E' && body[1] == 'R' ? 1 : 0;
        }
    }
#else
    char path[512];
    sock_resolve(path, sizeof path);
    lv_conn_t c = lv_conn_open(path);
    if (c == LV_CONN_BAD) {
        fprintf(stderr, "levari: no running instance (no socket at %s)\n", path);
        return 2;
    }
    strcat(line, "\n");
    if (lv_conn_write(c, line, strlen(line) + 1) < 0) { lv_conn_close(c); return 2; }
    char resp[8192];
    long n = lv_conn_read_line(c, resp, sizeof resp);
    lv_conn_close(c);
    if (n <= 0) { fprintf(stderr, "levari: no response\n"); return 2; }
    resp[n] = 0;
    /* batch 27: the conversation happened — bytes counted BEFORE the
       trailing newline is echoed; the user saw the resp on console */
    lv98_tracef("cli:ok n=%ld", n);
    fputs(resp, stdout);
    return resp[0] == 'E' && resp[1] == 'R' ? 1 : 0;
#endif
}
