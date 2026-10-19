#ifndef LEVARI_DAEMON_H
#define LEVARI_DAEMON_H

#include "../player/player.h"

/* server side (runs inside the player process) */
int  srv_start(Player *p);
void srv_stop(void);

/* client side: returns process exit code; prints response */
int  cli_run(int argc, char **argv);
/* client side: send one prepared command line */
int  cli_send_line(const char *line);
/* client side: send one command line, capture response into resp;
   0 = ok, 1 = ERR response, 2 = transport error */
int  cli_request(const char *line, char *resp, size_t rn);

#endif
