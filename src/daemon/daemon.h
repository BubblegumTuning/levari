#ifndef LEVARI_DAEMON_H
#define LEVARI_DAEMON_H

#include "../player/player.h"

/* server side (runs inside the player process) */
int  srv_start(Player *p);
void srv_stop(void);

/* default socket path shared by server and client: XDG_RUNTIME_DIR when
   set, /tmp fallback otherwise; Windows: per-user named pipe */
void lv_sock_path(char *out, size_t n);
#ifndef _WIN32
int  lv_tmp_fallback(void);                 /* 1 = the /tmp fallback is on */
void lv_sock_ptr_file(char *out, size_t n); /* publish file in config dir */
int  lv_sock_pointer(char *out, size_t n);  /* published path; 0 = none */
#endif

/* client side: returns process exit code; prints response */
int  cli_run(int argc, char **argv);
/* client side: send one prepared command line */
int  cli_send_line(const char *line);
/* client side: send one command line, capture response into resp;
   0 = ok, 1 = ERR response, 2 = transport error */
int  cli_request(const char *line, char *resp, size_t rn);
/* append " arg" to the command being built: args with a space or a double
   quote travel quoted, embedded quotes escaped as \" (the play tokenizer
   and add handler unescape inside quotes). -1 = would not fit, no write */
int  cli_append_arg(char *cmd, size_t n, const char *arg);

#endif
