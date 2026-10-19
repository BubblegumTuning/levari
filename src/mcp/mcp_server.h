#ifndef LEVARI_MCP_SERVER_H
#define LEVARI_MCP_SERVER_H

/* handle one JSON-RPC request line; returns malloc'd response string,
   or NULL when no response is due (notifications) */
char *mcp_handle(const char *line);

#endif
