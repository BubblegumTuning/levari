#ifndef LEVARI_DAEMON_H
#define LEVARI_DAEMON_H

#include "../player/player.h"
#include "../platform/sock.h"

/* server side (runs inside the player process) */
int  srv_start(Player *p);
void srv_stop(void);

/* endpoint paths live in platform/sock.h (lv_sock_path, /tmp-fallback
   pointer helpers) — refactor item 5 moved them out of this header. */

/* era file IPC (batch 29): this box's kernel32 refuses
   CreateNamedPipeA outright (hardware: gle=120 on the SYNCHRONOUS
   create too — same dead-kernel-object family as CreateSemaphore).
   The era conversation therefore runs over two files next to the
   exe — the one directory hardware PROVED writable (it holds the
   trace log). Overwrite-only token protocol (no deletes, no locks,
   no new imports): the client stamps its request "T<tick> ", the
   server echoes the token as the first field of the answer; stale
   files die by token mismatch and by the next writer's overwrite. */
#if defined(LV_WIN9X) && defined(_WIN32)
int lv_ipc_file(char *out, size_t n, const char *what);
#endif

/* client side: returns process exit code; prints response */
int  cli_run(int argc, char **argv);
/* client side: send one command line, capture response into resp;
   0 = ok, 1 = ERR response, 2 = transport error */
int  cli_request(const char *line, char *resp, size_t rn);
/* append " arg" to the command being built: args with a space or a double
   quote travel quoted, embedded quotes escaped as \" (the play tokenizer
   and add handler unescape inside quotes). -1 = would not fit, no write */
int  cli_append_arg(char *cmd, size_t n, const char *arg);

#endif
