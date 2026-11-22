#ifndef LV_PLATFORM_SOCK_H
#define LV_PLATFORM_SOCK_H

/* IPC transport (refactor item 5): the raw endpoint ops shared by the
   daemon server, the daemon client, and mcp. Windows: per-user named
   pipe; POSIX: unix-domain socket. All platform ifdefs live in sock.c —
   call sites see one shape. Peer identity is verified INSIDE the ops
   that matter (lv_conn_open, lv_conn_accept): the socket is already
   owner-only (umask 0077 + chmod 0600), the check is defense against a
   pre-bound impostor on the /tmp fallback path, and a failed query
   passes best-effort as the original call sites did. */

#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE lv_conn_t;
#define LV_CONN_BAD INVALID_HANDLE_VALUE
#else
typedef int lv_conn_t;
#define LV_CONN_BAD ((lv_conn_t)-1)
#endif

/* endpoint path: cfg override, XDG_RUNTIME_DIR (POSIX) / per-user pipe
   name (Windows), /tmp fallback otherwise */
void lv_sock_path(char *out, size_t n);

#ifndef _WIN32
int  lv_tmp_fallback(void);                 /* 1 = the /tmp fallback is on */
void lv_sock_ptr_file(char *out, size_t n); /* publish file in config dir */
int  lv_sock_pointer(char *out, size_t n);  /* published path; 0 = none */
#endif

/* ---- client side ---- */
/* open the endpoint; peer-verified on POSIX, closed + LV_CONN_BAD on a
   foreign listener */
lv_conn_t lv_conn_open(const char *path);
long lv_conn_read(lv_conn_t c, void *b, size_t n);    /* -1 = error */
int  lv_conn_write(lv_conn_t c, const void *b, size_t n); /* 0 = ok; loops short writes */
void lv_conn_close(lv_conn_t c);
/* read until '\n' or EOF; returns bytes stored (NUL-terminated) */
long lv_conn_read_line(lv_conn_t c, char *buf, size_t n);

/* ---- server side (POSIX; the Windows server owns its pipe lifecycle
   in daemon.c) ---- */
/* socket+bind at path with a private umask + chmod 0600; 0 = ok */
int  lv_listener_bind(const char *path);
/* accept one connection; peer-verified (unwanted peers are closed);
   LV_CONN_BAD on failure */
lv_conn_t lv_conn_accept(int listen_fd);
/* 1 if a live listener at path belongs to this uid (a connect failure —
   stale socket — or a foreign peer returns 0 and the caller may
   unlink+bind); on Windows, 1 if the pipe currently opens */
int  lv_listener_own(const char *path);

#endif /* LV_PLATFORM_SOCK_H */
