#include "mcp_tools.h"
#include "../daemon/daemon.h"
#include "../library/library.h"
#include "../common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the daemon socket protocol splits arguments on whitespace,
   so single path/query tokens must not contain spaces */
static int bad_token(const char *s) {
    if (!s || !*s) return 1;
    for (const char *p = s; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') return 1;
    return 0;
}

static const char *str_arg(const cJSON *args, const char *key) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    if (!v || cJSON_IsNull(v)) return NULL;
    if (!cJSON_IsString(v)) return NULL;
    return v->valuestring;
}

static int run_cmd(const char *cmd, char *text, size_t n) {
    char resp[8000];
    int rc = cli_request(cmd, resp, sizeof resp);
    if (rc == 2) {
        snprintf(text, n, "error: no running levari instance (is the player up?)");
        return 1;
    }
    size_t l = strlen(resp);
    while (l && (resp[l - 1] == '\n' || resp[l - 1] == '\r')) resp[--l] = 0;
    lv_strlcpy(text, resp, n);
    return rc;
}

static int run_lib(const char *sub, const char *arg, char *text, size_t n) {
    static char buf[16000];
    FILE *mem = tmpfile(); /* portable stand-in for fmemopen */
    if (!mem) { snprintf(text, n, "error: out of memory"); return 1; }
    lib_run(sub, arg, mem);
    fflush(mem);
    rewind(mem);
    buf[fread(buf, 1, sizeof buf - 1, mem)] = 0;
    fclose(mem);
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
    lv_strlcpy(text, buf, n);
    return 0;
}

static int on_off_cmd(const cJSON *args, const char *base, char *text, size_t n) {
    const char *state = str_arg(args, "state");
    char cmd[64];
    if (!state || !strcmp(state, "toggle")) snprintf(cmd, sizeof cmd, "%s toggle", base);
    else if (!strcmp(state, "on")) snprintf(cmd, sizeof cmd, "%s on", base);
    else if (!strcmp(state, "off")) snprintf(cmd, sizeof cmd, "%s off", base);
    else { snprintf(text, n, "error: state must be \"on\", \"off\" or \"toggle\""); return 1; }
    return run_cmd(cmd, text, n);
}

static int tool_play(const cJSON *args, char *text, size_t n) {
    const cJSON *files = args ? cJSON_GetObjectItemCaseSensitive(args, "files") : NULL;
    if (!files || !cJSON_IsArray(files) || cJSON_GetArraySize(files) < 1) {
        snprintf(text, n, "error: files must be a non-empty array of paths");
        return 1;
    }
    char cmd[8000] = "play";
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, files) {
        if (!cJSON_IsString(f) || bad_token(f->valuestring)) {
            snprintf(text, n, "error: every path must be a single string without spaces");
            return 1;
        }
        if (strlen(cmd) + strlen(f->valuestring) + 2 > sizeof cmd) {
            snprintf(text, n, "error: too many files");
            return 1;
        }
        strcat(cmd, " ");
        strcat(cmd, f->valuestring);
    }
    return run_cmd(cmd, text, n);
}

static int tool_seek(const cJSON *args, char *text, size_t n) {
    const char *v = str_arg(args, "value");
    if (!v || !*v) { snprintf(text, n, "error: value is required (e.g. \"90\" or \"+30\")"); return 1; }
    for (const char *p = v; *p; p++)
        if (*p < '0' || *p > '9') {
            if (*p != '+' && *p != '-' && *p != '.') {
                snprintf(text, n, "error: value must be a number, optionally prefixed with + or -");
                return 1;
            }
        }
    char cmd[64];
    snprintf(cmd, sizeof cmd, "seek %s", v);
    return run_cmd(cmd, text, n);
}

static int tool_volume(const cJSON *args, char *text, size_t n) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, "level") : NULL;
    if (!v || !cJSON_IsNumber(v)) { snprintf(text, n, "error: level must be an integer 0-128"); return 1; }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "vol %d", (int)v->valuedouble);
    return run_cmd(cmd, text, n);
}

static int tool_pan(const cJSON *args, char *text, size_t n) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, "value") : NULL;
    if (!v || !cJSON_IsNumber(v)) { snprintf(text, n, "error: value must be an integer -32..32"); return 1; }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "pan %d", (int)v->valuedouble);
    return run_cmd(cmd, text, n);
}

static int tool_speed(const cJSON *args, char *text, size_t n) {
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, "value") : NULL;
    if (!v || !cJSON_IsNumber(v)) { snprintf(text, n, "error: value must be a number 0.5-2.0"); return 1; }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "speed %.2f", v->valuedouble);
    return run_cmd(cmd, text, n);
}

static int tool_add(const cJSON *args, char *text, size_t n) {
    const char *p = str_arg(args, "path");
    if (bad_token(p)) { snprintf(text, n, "error: path is required and must not contain spaces"); return 1; }
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "add %s", p);
    return run_cmd(cmd, text, n);
}

static int tool_lib_scan(const cJSON *args, char *text, size_t n) {
    const char *d = str_arg(args, "dir");
    if (!d || !*d) { snprintf(text, n, "error: dir is required"); return 1; }
    return run_lib("scan", d, text, n);
}

static int tool_lib_list(const cJSON *args, char *text, size_t n) {
    return run_lib("list", str_arg(args, "filter"), text, n);
}

static int tool_lib_play(const cJSON *args, char *text, size_t n) {
    return run_lib("play", str_arg(args, "query"), text, n);
}

typedef int (*tool_fn)(const cJSON *args, char *text, size_t n);

typedef struct {
    const char *name;
    const char *desc;
    const char *schema;
    tool_fn fn;
} Tool;

static const Tool TOOLS[] = {
    {"status", "Current track, playback state, position, volume, shuffle/repeat flags.",
     "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"play", "Load files into the playlist and play them (replaces the playlist).",
     "{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
     "\"description\":\"audio file or .m3u paths; no spaces in paths\"}},\"required\":[\"files\"]}", tool_play},
    {"toggle", "Toggle play/pause.", "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"stop", "Stop playback.", "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"next", "Skip to the next track.", "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"prev", "Go to the previous track.", "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"seek", "Seek: absolute seconds (\"90\") or relative (\"+30\", \"-10\").",
     "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},\"required\":[\"value\"]}", tool_seek},
    {"set_volume", "Set volume 0-128.",
     "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":128}},"
     "\"required\":[\"level\"]}", tool_volume},
    {"volume_up", "Raise volume by one step (8/128).",
     "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"volume_down", "Lower volume by one step (8/128).",
     "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"mute", "Mute (volume 0); invoke again to restore the previous volume.",
     "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"set_pan", "Set left/right balance, -32..32.",
     "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\",\"minimum\":-32,\"maximum\":32}},"
     "\"required\":[\"value\"]}", tool_pan},
    {"set_speed", "Set pitch-preserving tempo, 0.5-2.0.",
     "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\",\"minimum\":0.5,\"maximum\":2.0}},"
     "\"required\":[\"value\"]}", tool_speed},
    {"set_shuffle", "Shuffle on/off/toggle.",
     "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"toggle\"]}}}",
     NULL},
    {"set_repeat", "Repeat on/off/toggle.",
     "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"toggle\"]}}}",
     NULL},
    {"set_eq", "Equalizer on/off/toggle.",
     "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"toggle\"]}}}",
     NULL},
    {"playlist", "Print the playlist: index, title, path per line.",
     "{\"type\":\"object\",\"properties\":{}}", NULL},
    {"add", "Append one file to the playlist without playing.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}", tool_add},
    {"library_scan", "Scan a directory into the media library (tags + duration).",
     "{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\"}},\"required\":[\"dir\"]}", tool_lib_scan},
    {"library_list", "List media library entries, optional substring filter.",
     "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}", tool_lib_list},
    {"library_play", "Play media library entries matching a query.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}", tool_lib_play},
};

#define NTOOLS (sizeof TOOLS / sizeof TOOLS[0])

cJSON *mcp_tools_list(void) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < NTOOLS; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(TOOLS[i].schema));
        cJSON_AddItemToArray(arr, t);
    }
    return arr;
}

int mcp_tools_call(const char *name, const cJSON *args, char *text, size_t n) {
    for (size_t i = 0; i < NTOOLS; i++) {
        if (strcmp(TOOLS[i].name, name)) continue;
        if (TOOLS[i].fn) return TOOLS[i].fn(args, text, n);
        char cmd[64];
        if (!strcmp(name, "status")) lv_strlcpy(cmd, "status", sizeof cmd);
        else if (!strcmp(name, "toggle")) lv_strlcpy(cmd, "toggle", sizeof cmd);
        else if (!strcmp(name, "stop")) lv_strlcpy(cmd, "stop", sizeof cmd);
        else if (!strcmp(name, "next")) lv_strlcpy(cmd, "next", sizeof cmd);
        else if (!strcmp(name, "prev")) lv_strlcpy(cmd, "prev", sizeof cmd);
        else if (!strcmp(name, "playlist")) lv_strlcpy(cmd, "pl", sizeof cmd);
        else if (!strcmp(name, "volume_up")) lv_strlcpy(cmd, "volup", sizeof cmd);
        else if (!strcmp(name, "volume_down")) lv_strlcpy(cmd, "voldown", sizeof cmd);
        else if (!strcmp(name, "mute")) lv_strlcpy(cmd, "mute", sizeof cmd);
        else if (!strcmp(name, "set_shuffle")) return on_off_cmd(args, "shuffle", text, n);
        else if (!strcmp(name, "set_repeat")) return on_off_cmd(args, "repeat", text, n);
        else if (!strcmp(name, "set_eq")) return on_off_cmd(args, "eq", text, n);
        else return -1;
        return run_cmd(cmd, text, n);
    }
    return -1;
}
