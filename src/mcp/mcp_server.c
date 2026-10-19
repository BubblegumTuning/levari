#include "mcp_server.h"
#include "mcp_tools.h"
#include "cJSON.h"
#include "../common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MCP_LATEST_VERSION "2024-11-05"
#define LEVARI_MCP_VERSION LV_VERSION /* single source: CMake project version */

static const char *SUPPORTED[] = {"2024-11-05", "2025-03-26", "2025-06-18"};

static cJSON *dup_id(const cJSON *id) {
    if (!id) return cJSON_CreateNull();
    if (cJSON_IsNumber(id)) return cJSON_CreateNumber(id->valuedouble);
    if (cJSON_IsString(id)) return cJSON_CreateString(id->valuestring);
    return cJSON_CreateNull();
}

static char *finish(cJSON *resp) {
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *error_resp(const cJSON *id, int code, const char *msg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", dup_id(id));
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return finish(r);
}

static char *result_resp(const cJSON *id, cJSON *result) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", dup_id(id));
    cJSON_AddItemToObject(r, "result", result);
    return finish(r);
}

static char *handle_initialize(const cJSON *id, const cJSON *params) {
    const cJSON *pv = params ? cJSON_GetObjectItemCaseSensitive(params, "protocolVersion") : NULL;
    const char *ver = MCP_LATEST_VERSION;
    if (pv && cJSON_IsString(pv))
        for (size_t i = 0; i < sizeof SUPPORTED / sizeof SUPPORTED[0]; i++)
            if (!strcmp(pv->valuestring, SUPPORTED[i])) { ver = SUPPORTED[i]; break; }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "protocolVersion", ver);
    cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
    cJSON_AddStringToObject(info, "name", "levari");
    cJSON_AddStringToObject(info, "version", LEVARI_MCP_VERSION);
    return result_resp(id, res);
}

static char *handle_tools_list(const cJSON *id) {
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "tools", mcp_tools_list());
    return result_resp(id, res);
}

static char *handle_tools_call(const cJSON *id, const cJSON *params) {
    const cJSON *name = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    if (!name || !cJSON_IsString(name))
        return error_resp(id, -32602, "tools/call: missing tool name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    char text[16000];
    int rc = mcp_tools_call(name->valuestring, args, text, sizeof text);
    if (rc < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "unknown tool '%s'", name->valuestring);
        return error_resp(id, -32602, msg);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(res, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    if (rc == 1) cJSON_AddBoolToObject(res, "isError", 1);
    return result_resp(id, res);
}

char *mcp_handle(const char *line) {
    cJSON *req = cJSON_Parse(line);
    if (!req) return error_resp(NULL, -32700, "parse error");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    char *resp = NULL;
    if (!method || !cJSON_IsString(method)) {
        resp = error_resp(id, -32600, "invalid request");
    } else {
        const char *m = method->valuestring;
        if (!strcmp(m, "initialize")) resp = handle_initialize(id, params);
        else if (!strcmp(m, "ping")) resp = result_resp(id, cJSON_CreateObject());
        else if (!strcmp(m, "tools/list")) resp = handle_tools_list(id);
        else if (!strcmp(m, "tools/call")) resp = handle_tools_call(id, params);
        else if (!id) resp = NULL; /* unknown notification: ignore */
        else resp = error_resp(id, -32601, "method not found");
    }
    cJSON_Delete(req);
    return resp;
}
