#include "mcp_server.h"
#include "../config/config.h"
#ifdef _WIN32
#include "../compat/win32.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    cfg_load();
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        char *resp = mcp_handle(line);
        if (resp) {
            fputs(resp, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            free(resp);
        }
    }
    free(line);
    return 0;
}
