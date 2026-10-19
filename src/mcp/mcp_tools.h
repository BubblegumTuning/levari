#ifndef LEVARI_MCP_TOOLS_H
#define LEVARI_MCP_TOOLS_H

#include <stddef.h>
#include "cJSON.h"

/* JSON array of MCP tool descriptors for tools/list */
cJSON *mcp_tools_list(void);

/* execute a tool; fills text with the result message.
   returns 0 = ok, 1 = tool error (isError), -1 = unknown tool */
int mcp_tools_call(const char *name, const cJSON *args, char *text, size_t n);

#endif
