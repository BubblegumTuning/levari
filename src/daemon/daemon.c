/* struct ucred (SO_PEERCRED) is a GNU extension in glibc headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "daemon.h"
#include "../playlist/playlist.h"
#include "../library/library.h"
#include "../config/config.h"
#include "../common.h"
#include "../platform/sock.h"
#include "../platform/platform.h"   /* lv_user_tag (era lv_ipc_file); shims */
#ifdef _WIN32
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include "../compat/win98_diag.h" /* lv98_tracef (no-op off-era; WIN32
                                     srv probes compile in flagship-win
                                     too, so the include cannot be era-only) */
#if defined(LV_WIN9X) && defined(_WIN32)
#include <process.h>              /* _beginthreadex (batch 26) */
#endif
#ifndef _WIN32
#include <time.h>
#endif

#define VOL_STEP 8

#if defined(LV_WIN9X) && defined(_WIN32)
/* era: winpthread pthread_create is dead on the box (EAGAIN every
   boot, batch 21) — the handle lives natively (player.c pattern) */
static void *srv_th;
#else
static pthread_t srv_th;
#endif
static Player *srv_p;
static volatile int srv_quit;

#ifdef _WIN32
static HANDLE cur_pipe;
/* srv_stop reads cur_pipe while the server thread assigns and closes it:
   unsynchronized, CancelIoEx could target a handle value already closed
   and recycled for another object. the thread clears cur_pipe under this
   lock *before* CloseHandle, so a handle srv_stop sees is still open. */
static pthread_mutex_t pipe_mtx = PTHREAD_MUTEX_INITIALIZER;

#ifdef LV_WIN9X
/* ---- win98 era floor (Win98 branch; parked-features dossier): replaces
   the post-9x kernel32/advapi32 entries this file uses, all confirmed
   loader-rejecting imports by objdump census (GetTickCount64 Vista+,
   CancelIoEx Vista+, CancelSynchronousIo Vista+,
   ConvertStringSecurityDescriptor* Win2000+). Mainline win builds never
   define LV_WIN9X and keep the modern shape untouched. ---- */
#include <mmsystem.h>

/* era-aware monotonic ms: 9x has no GetTickCount64; winmm timeGetTime is
   32-bit and wraps every ~49.7 days — wrap-extend so every 5 s deadline
   below keeps its meaning across a rollover. called only from this serial
   server thread (and srv_stop only joins it), so plain statics are safe. */
static ULONGLONG lv_tick_ms(void) {
    static DWORD last_lo, epoch;
    DWORD lo = timeGetTime();
    if (lo < last_lo) epoch++; /* 32-bit rollover */
    last_lo = lo;
    return ((ULONGLONG)epoch << 32) | lo;
}
#define GetTickCount64() lv_tick_ms()

/* 9x CancelIo cancels EVERY pending op on the handle. Equivalent here:
   the server loop is serial per instance (one pending OVERLAPPED at a
   time) and srv_stop's CancelIoEx(cp, NULL) cancelled everything anyway. */
#define CancelIoEx(h, o) CancelIo((h))

/* No CancelSynchronousIo before Vista. The write-path comment already
   names DisconnectNamedPipe as the sure unblock; era builds lean on it.
   The call sites keep running: the cancel becomes a no-op, the bounded
   reap with its deliberate-leak escape stays the safety net. */
static BOOL lv_cancel_sync(HANDLE h) {
    (void)h;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
#define CancelSynchronousIo(h) lv_cancel_sync(h)
#endif
#else
static int listen_fd = -1;
static char srv_path[512];   /* the path actually bound (random in /tmp fallback) */
static char srv_ptr_pub[512]; /* pointer file this instance published ('' if none) */
#endif

static void status_line(Player *p, char *out, size_t n) {
    long long base = p->track_start;
    long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
    long long pos = p->rate ? (p->pos_frames - base) / p->rate : 0;
    long long tot = p->rate ? (endf - base) / p->rate : 0;
    char ps[16], ts[16];
    lv_fmt_time(ps, sizeof ps, pos);
    lv_fmt_time(ts, sizeof ts, tot);
    const char *st = p->state == ST_PLAY ? "playing" : p->state == ST_PAUSE ? "paused" : "stopped";
    char title[256];
    /* count/cur read under p->mtx: the daemon's own play handler swaps
       the playlist under that lock; pl_current_title takes it internally,
       so grab the scalars first and release before calling it */
    pthread_mutex_lock(&p->mtx);
    int cur = p->cur, count = p->pl->count;
    pthread_mutex_unlock(&p->mtx);
    pl_current_title(p, title, sizeof title);
    lv_utf8_trim(title);
    snprintf(out, n, "%s %d/%d pos=%s/%s shuffle=%d repeat=%d vol=%d title=%s",
             st, cur + 1, count, ps, ts,
             p->shuffle, p->repeat, p->vol, title);
    lv_utf8_trim(out);
}

static void handle_cmd(Player *p, char *line, char *resp, size_t rn) {
    char cmd[64] = "";
    sscanf(line, "%63s", cmd);
    resp[0] = 0;

    if (!strcmp(cmd, "status")) {
        status_line(p, resp, rn);
    } else if (!strcmp(cmd, "play")) {
        char *rest = line + 4;
        while (*rest == ' ') rest++;
        if (*rest) {
            /* build into a temp playlist WITHOUT holding p->mtx: a
               multi-MB m3u on a network share can take a while, and the
               UI draw loop blocks on that mutex every frame -- holding
               it through the load makes the desktop declare the window
               unresponsive. only the swap is locked. */
            Playlist np;
            pl_init(&np);
            /* split on spaces but honor double quotes: strtok(" ") made
               every path containing a space into several bogus paths.
               inside quotes \" and \\ decode to " and \, so paths that
               contain both spaces and quotes survive transport */
            for (char *s = rest; *s; ) {
                char *tok = s;
                if (*tok == '"') {
                    tok++;
                    char *r = tok, *w = tok;
                    while (*r && *r != '"') {
                        if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                            *w++ = r[1];
                            r += 2;
                        } else {
                            *w++ = *r++;
                        }
                    }
                    /* the closing quote must be tested BEFORE the NUL goes
                       in: with no escapes w lands on it and would erase
                       the only marker of where the token ended */
                    int closed = *r == '"';
                    *w = 0;
                    s = closed ? r + 1 : r;
                } else {
                    char *sp = strchr(tok, ' ');
                    if (sp) { *sp = 0; s = sp + 1; }
                    else s = tok + strlen(tok);
                }
                while (*s == ' ') s++;
                if (!*tok) continue;
                PlKind kind = pl_ext_kind(tok);
                if (lv_is_url(tok)) pl_add(&np, tok);   /* transient row:
                    an URL ending in .m3u is still a stream, not a local
                    playlist to fopen (D3, same guard as main.c argv) */
                else if (kind == PL_M3U) pl_load_m3u(&np, tok);
                else if (kind == PL_PLS) pl_load_pls(&np, tok);
                else if (kind == PL_CUE) pl_load_cue(&np, tok);
                else pl_add(&np, tok);
            }
            /* index first: library queries arrive as bare paths, and the
               index already holds tags + duration for scanned files */
            lib_fill_playlist(&np);
            pthread_mutex_lock(&p->mtx);
            pl_clear(p->pl);              /* frees the old rows AND their pool */
            p->pl->items = np.items;
            p->pl->count = np.count;
            p->pl->cap = np.cap;
            p->pl->pool = np.pool;        /* adopt: every row string rides it */
            pl_touch(p->pl);   /* the swap rewrites every row: any cache on the old gen dies */
            pthread_mutex_unlock(&p->mtx);
            pl_post(p, CMD_PLAY, 0);
        } else {
            if (p->state == ST_PAUSE) pl_post(p, CMD_TOGGLE_PAUSE, 0);
            else pl_post(p, CMD_PLAY, p->cur >= 0 ? p->cur : 0);
        }
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "toggle") || !strcmp(cmd, "pause") || !strcmp(cmd, "resume")) {
        pl_post(p, CMD_TOGGLE_PAUSE, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "stop")) {
        pl_post(p, CMD_STOP, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "next")) {
        pl_post(p, CMD_NEXT, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "prev")) {
        pl_post(p, CMD_PREV, 0);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "seek")) {
        long long base = p->track_start;
        long long endf = p->track_end > 0 ? p->track_end : p->total_frames;
        double cur = p->rate ? (double)(p->pos_frames - base) / p->rate : 0;
        char *v = line + 4;
        while (*v == ' ') v++;
        double target = cur;
        if (*v == '+' || *v == '-') target = cur + atof(v);
        else target = atof(v);
        if (endf > base) {
            pl_post_seek(p, (target * p->rate) / (double)(endf - base));
            lv_strlcpy(resp, "OK", rn);
        } else {
            /* live stream / unknown length: house polite refusal (D-total
               policy) — pl_post_seek would be inert anyway */
            lv_strlcpy(resp, "ERR live or unknown length: seek not available", rn);
        }
    } else if (!strcmp(cmd, "vol")) {
        char *v = line + 3;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: vol <0-128>"); }
        else {
            int x = atoi(v);
            if (x < 0) x = 0;
            if (x > 128) x = 128;
            p->vol = x;
            lv_strlcpy(resp, "OK", rn);
        }
    } else if (!strcmp(cmd, "volup") || !strcmp(cmd, "voldown")) {
        int v = p->vol + (!strcmp(cmd, "volup") ? VOL_STEP : -VOL_STEP);
        if (v < 0) v = 0;
        if (v > 128) v = 128;
        p->vol = v;
        snprintf(resp, rn, "vol %d", v);
    } else if (!strcmp(cmd, "mute")) {
        if (p->vol > 0) { p->vol_prev = p->vol; p->vol = 0; }
        else p->vol = p->vol_prev > 0 ? p->vol_prev : 100;
        snprintf(resp, rn, "vol %d", p->vol);
    } else if (!strcmp(cmd, "pan")) {
        char *v = line + 3;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: pan <-32..32>"); }
        else {
            int x = atoi(v);
            if (x < -32) x = -32;
            if (x > 32) x = 32;
            p->pan = x;
            lv_strlcpy(resp, "OK", rn);
        }
    } else if (!strcmp(cmd, "shuffle") || !strcmp(cmd, "repeat")) {
        /* the on/off/one token is positional: strstr over the whole line
           matched "on" inside paths and arguments */
        char tok[16] = "";
        sscanf(line + strlen(cmd), "%15s", tok);
        int on = !strcmp(tok, "on");
        int off = !strcmp(tok, "off");
        int one = !strcmp(cmd, "repeat") && !strcmp(tok, "one");
        _Atomic int *flag = !strcmp(cmd, "shuffle") ? &p->shuffle : &p->repeat;
        if (one) *flag = 2;
        else if (on) *flag = 1; else if (off) *flag = 0; else *flag = !*flag;
        if (flag == &p->shuffle && cfg.shuffle_remember > 0) {
            cfg_set_shuffle(*flag);
        }
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "eq")) {
        char tok[16] = "";
        sscanf(line + strlen(cmd), "%15s", tok);
        int on = !strcmp(tok, "on");
        int off = !strcmp(tok, "off");
        if (on) eq_set_enabled(&p->eq, 1);
        else if (off) eq_set_enabled(&p->eq, 0);
        else eq_toggle(&p->eq);
        lv_strlcpy(resp, "OK", rn);
    } else if (!strcmp(cmd, "speed")) {
        char *v = line + 5;
        while (*v == ' ') v++;
        if (!*v) { snprintf(resp, rn, "ERR usage: speed <0.5-2.0>"); }
        else {
            double x = atof(v);
            if (x < 0.5) x = 0.5;
            if (x > 2.0) x = 2.0;
            pl_post(p, CMD_SPEED, (int)(x * 100 + 0.5));
            lv_strlcpy(resp, "OK", rn);
        }
    } else if (!strcmp(cmd, "pl")) {
        pthread_mutex_lock(&p->mtx);
        int off = 0;
        for (int i = 0; i < p->pl->count && off < (int)rn - 64; i++) {
            Track *t = &p->pl->items[i];
            char pretty[LV_MAX_PATH];
            const char *name;
            if (t->tags.title[0]) name = t->tags.title;
            else { lv_pretty_name(t->path, pretty, sizeof pretty); name = pretty; }
            /* snprintf returns the would-be length: off could grow past
               the buffer while the loop bound only re-checks next turn,
               making the next resp + off arithmetic fragile */
            int w = snprintf(resp + off, rn - off, "%d\t%s\t%s\n", i + 1, name, t->path);
            if (w < 0 || (size_t)w >= rn - off) break;
            off += w;
        }
        pthread_mutex_unlock(&p->mtx);
        if (!off) lv_strlcpy(resp, "(empty)", rn);
        else lv_utf8_trim(resp);
    } else if (!strcmp(cmd, "add")) {
        char *rest = line + 3;
        while (*rest == ' ' || *rest == '\t') rest++;
        char *end = rest + strlen(rest);
        while (end > rest && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            *--end = 0;
        /* the CLI quotes args containing spaces or quotes (same convention
           as the play tokenizer above); add keeps the rest of the line as
           ONE path, so strip exactly one surrounding quote pair and undo
           the \" \\ escapes it may contain */
        size_t rl = (size_t)(end - rest);
        if (rl >= 2 && rest[0] == '"' && end[-1] == '"') {
            end[-1] = 0;
            rest++;
            char *r = rest, *w = rest;
            while (*r) {
                if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    *w++ = r[1];
                    r += 2;
                } else {
                    *w++ = *r++;
                }
            }
            *w = 0;
        }
        if (*rest) {
            if (lv_is_url(rest)) {
                /* D3 (design ruling): URLs are never playlist rows — the
                   transient play verb above is the sanctioned door; the
                   house polite refusal mirrors the live-seek wording */
                snprintf(resp, rn, "ERR url rows are not allowed in playlists: use play <url> (transient)");
            } else {
                LibIndex ix = lib_index_load(); /* disk I/O off the player lock */
                pthread_mutex_lock(&p->mtx);
                pl_add(p->pl, rest);
                lib_fill_playlist_ix(p->pl, &ix);
                pthread_mutex_unlock(&p->mtx);
                lib_index_free(&ix);
                lv_strlcpy(resp, "OK", rn);
            }
        } else lv_strlcpy(resp, "OK", rn);
    } else {
        snprintf(resp, rn, "ERR unknown command '%s'", cmd);
    }
}

#ifdef _WIN32

/* FlushFileBuffers on a pipe blocks until the client drains the out
   buffer — forever if the client never reads. run it on a helper thread
   so the caller can bound the wait; what unblocks a stuck flush is the
   explicit CancelSynchronousIo and/or the DisconnectNamedPipe at
   teardown (the flush fails against a disconnected instance) — NOT
   object destruction, since the thread's own duplicate keeps the pipe
   object alive across the caller's CloseHandle. the arg block is
   heap-owned because the thread can still be running after the caller's
   scope ends; the caller always reaps it after closing the pipe handle,
   with a BOUNDED wait (see teardown: a pathological stall leaks two
   handles rather than hanging the server thread forever).
   The thread owns a DUPLICATED pipe handle, never the caller's h: with
   a small remaining budget the caller reaches DisconnectNamedPipe +
   CloseHandle(h) while the thread may not even have entered
   FlushFileBuffers yet — a closed handle VALUE can be recycled by any
   other thread's open and the flush would then run against the wrong
   object. With its own duplicate, the pipe object stays valid under it
   (references outlive closes), the teardown still unblocks it, and the
   thread closes its own copy on the way out. */
typedef struct { HANDLE h; HANDLE ev; } PipeFlush;
static DWORD WINAPI pipe_flush_thr(void *arg) {
    PipeFlush *pf = (PipeFlush *)arg;
    FlushFileBuffers(pf->h);
    CloseHandle(pf->h);
    SetEvent(pf->ev);
    free(pf);
    return 0;
}

#if defined(LV_WIN9X) && defined(_WIN32)
int lv_ipc_file(char *out, size_t n, const char *what) {
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = NULL;
    char user[256];
    char *p;
    if (!k) return -1;
    for (p = exe; *p; p++)
        if (*p == '\\') slash = p;
    if (!slash) return -1;
    *slash = 0;
    lv_user_tag(user, sizeof user);
    snprintf(out, n, "%s\\levari-%s.%s", exe, user, what);
    return 0;
}
#endif

static void *srv_thread(void *arg) {
    (void)arg;
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 29: this box's kernel32 refuses CreateNamedPipeA outright
       (batch-28 hardware: gle=120 ERROR_CALL_NOT_IMPLEMENTED even on
       the SYNCHRONOUS create — the same dead-kernel-object family as
       CreateSemaphore). The era server therefore conducts the CLI
       conversation over two plain files next to the exe (module dir
       = hardware-proven writable: the trace log lives there),
       polling at 250 ms with box-accepted Sleep. Protocol (see
       daemon.h): request file "T<tick> <cmd>", answer echoes the
       token; the server processes a request once by tracking the
       last token. No deletes, no locks, no new imports. */
    char reqp[MAX_PATH], respp[MAX_PATH];
    if (lv_ipc_file(reqp, sizeof reqp, "req") != 0 ||
        lv_ipc_file(respp, sizeof respp, "resp") != 0) {
        lv98_tracef("srv:ipc NOPATH");
        return NULL;
    }
    {   static int ok_n;
        if (ok_n < 1) { ok_n++; lv98_tracef("srv:ipc OK"); } }
    unsigned last_tok = 0;
    while (!srv_quit) {
        char line[8192];
        DWORD n = 0;
        HANDLE f = CreateFileA(reqp, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ReadFile(f, line, (DWORD)sizeof line - 1, &n, NULL);
            CloseHandle(f);
        }
        if (n > 0) {
            line[n] = 0;
            char *sp = strchr(line, ' ');
            unsigned tok = sp && line[0] == 'T'
                               ? (unsigned)strtoul(line + 1, NULL, 10) : 0;
            if (sp && tok != 0 && tok != last_tok) {
                char *nl;
                char *cmd = sp + 1;
                last_tok = tok;
                nl = strchr(cmd, '\n');
                if (nl) *nl = 0;
                {   /* first token of the accepted command (cap 3; high
                       bytes dotted — the log stays 7-bit) */
                    static int cmd_n;
                    if (cmd_n < 3) {
                        cmd_n++;
                        char tk[25];
                        size_t ti = 0;
                        while (ti < 24 && cmd[ti] && cmd[ti] != ' ' &&
                               cmd[ti] != '\t') {
                            tk[ti] = (unsigned char)cmd[ti] >= 128
                                         ? '.' : cmd[ti];
                            ti++;
                        }
                        tk[ti] = 0;
                        lv98_tracef("srv:cmd [%s]", tk);
                    }
                }
                {   char resp[4096];
                    char stamped[4160];
                    int sl;
                    DWORD w = 0;
                    handle_cmd(srv_p, cmd, resp, sizeof resp);
                    sl = snprintf(stamped, sizeof stamped, "T%u %s\n",
                                  tok, resp);
                    f = CreateFileA(respp, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (f == INVALID_HANDLE_VALUE)
                        lv98_tracef("srv:wfail gle=%lu", GetLastError());
                    else {
                        if (sl > 0)
                            WriteFile(f, stamped, (DWORD)strlen(stamped),
                                      &w, NULL);
                        CloseHandle(f);
                        {   static int conn_n;
                            if (conn_n < 3) {
                                conn_n++;
                                lv98_tracef("srv:conn n=%d", conn_n);
                            }
                        }
                    }
                }
            }
        }
        Sleep(250);
    }
    return NULL;
#else
    char path[512];
    lv_sock_path(path, sizeof path);
    /* owner-only pipe: with a NULL security descriptor the pipe inherits the
       token's default DACL, which lets other local users open the single
       instance (read-only, but enough to lock the real CLI/MCP out). The
       owner-SID ACE blocks them at open while the same user's CLI/MCP keep
       access even from another logon session. */
    PSECURITY_DESCRIPTOR psd = NULL;
    SECURITY_ATTRIBUTES sa, *sap = NULL;
#ifndef LV_WIN9X
    /* the SDDL converter is Win2000+ ADVAPI32: importing it at all would
       loader-reject on 9x. era builds keep the NULL descriptor (permissive
       default DACL — the posture the security notes already document for
       Windows) and stay unverified-until-hardware on this item. */
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;OW)", SDDL_REVISION_1, &psd, NULL)) {
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;
        sap = &sa;
    }
#endif
    while (!srv_quit) {
        /* FILE_FLAG_OVERLAPPED: the only supported way to bound a pipe
           read. SetCommTimeouts is documented for communications devices
           (not named pipes) and its return value was not even checked —
           the old "bounded read" comment claimed more than the code did. */
        HANDLE h = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    1, 0, 4096, 1000, sap);
        if (h == INVALID_HANDLE_VALUE) {
            /* batch 27: era pipe honesty — FILE_FLAG_OVERLAPPED named
               pipes are an open era question; if creation dies the
               thread exits and this is the ONLY trace that says so */
            lv98_tracef("srv:pipe FAIL gle=%lu", GetLastError());
            if (psd) LocalFree(psd);
            return NULL;
        }
        {   /* first successful creation = server listening (cap 1) */
            static int ok_n;
            if (ok_n < 1) { ok_n++; lv98_tracef("srv:pipe OK"); }
        }
        pthread_mutex_lock(&pipe_mtx);
        cur_pipe = h;
        pthread_mutex_unlock(&pipe_mtx);
        OVERLAPPED conn = {0};
        conn.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        BOOL cok = ConnectNamedPipe(h, &conn);
        DWORD ce = GetLastError();
        if (!cok && ce == ERROR_IO_PENDING) {
            if (WaitForSingleObject(conn.hEvent, INFINITE) == WAIT_OBJECT_0) {
                DWORD ign = 0;
                /* the event only says "a completion arrived"; GetOverlappedResult
                   reaps it and reports the true connect status/error. Without it
                   the overlapped operation is left unreaped. */
                cok = GetOverlappedResult(h, &conn, &ign, FALSE);
                if (!cok) ce = GetLastError();
            }
        }
        if (!cok && ce != ERROR_PIPE_CONNECTED) {
            pthread_mutex_lock(&pipe_mtx);
            cur_pipe = INVALID_HANDLE_VALUE;
            pthread_mutex_unlock(&pipe_mtx);
            if (conn.hEvent) CloseHandle(conn.hEvent);
            CloseHandle(h);
            continue;
        }
        {   /* batch 27: a client arrived (cap 3) */
            static int conn_n;
            if (conn_n < 3) { conn_n++; lv98_tracef("srv:conn n=%d", conn_n); }
        }
        /* read one newline-framed line under a deadline over the WHOLE
           line: the per-read 1 s wait only bounds silence — a client
           dribbling bytes just under 1 s apart kept this serial loop
           busy byte-by-byte. 5 s total: an idle, dribbling or stalled
           client is dropped and the loop re-accepts. */
        /* pending bounded-flush thread/event, or NULL: declared out here so
           the reap after CloseHandle(h) can see it (see write path below) */
        HANDLE fth = NULL, fev = NULL;
        if (!srv_quit) {
            OVERLAPPED rd = {0};
            rd.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            char line[8192];
            ULONGLONG t0 = GetTickCount64();
            DWORD n = 0, off = 0;
            while (off < sizeof line - 1) {
                ULONGLONG spent = GetTickCount64() - t0;
                DWORD left = spent >= 5000 ? 0 : (DWORD)(5000 - spent);
                if (left == 0) break; /* whole-line budget spent: drop it */
                ResetEvent(rd.hEvent);
                if (!ReadFile(h, line + off, (DWORD)(sizeof line - 1 - off), &n, &rd)) {
                    if (GetLastError() != ERROR_IO_PENDING) break;
                    if (WaitForSingleObject(rd.hEvent, left) != WAIT_OBJECT_0) {
                        CancelIoEx(h, &rd);
                        WaitForSingleObject(rd.hEvent, INFINITE);
                        break; /* budget/silence expired: drop the client */
                    }
                    if (!GetOverlappedResult(h, &rd, &n, FALSE)) break;
                }
                if (n == 0) break;
                off += n;
                line[off] = 0;
                if (strchr(line, '\n')) break;
            }
            if (rd.hEvent) CloseHandle(rd.hEvent);
            if (off > 0) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                {   /* batch 27: first token of the accepted command
                       (cap 3; OEM/high bytes dotted — log stays 7-bit) */
                    static int cmd_n;
                    if (cmd_n < 3) {
                        cmd_n++;
                        char tok[25];
                        size_t ti = 0;
                        while (ti < 24 && line[ti] && line[ti] != ' ' &&
                               line[ti] != '\t') {
                            tok[ti] = (unsigned char)line[ti] >= 128
                                          ? '.' : line[ti];
                            ti++;
                        }
                        tok[ti] = 0;
                        lv98_tracef("srv:cmd [%s]", tok);
                    }
                }
                char resp[4096];
                handle_cmd(srv_p, line, resp, sizeof resp);
                size_t rl = strlen(resp);
                resp[rl++] = '\n';
                /* the pipe is FILE_FLAG_OVERLAPPED: a synchronous WriteFile
                   (lpOverlapped == NULL) is invalid on an async handle. Drive
                   the write with its own OVERLAPPED + event, bounded by the
                   same 5 s budget the read uses. */
                OVERLAPPED wr = {0};
                wr.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
                DWORD w = 0;
                int wdone = 1;
                ULONGLONG wt0 = GetTickCount64();
                if (!WriteFile(h, resp, (DWORD)rl, &w, &wr) &&
                    GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(wr.hEvent, 5000) == WAIT_OBJECT_0)
                        GetOverlappedResult(h, &wr, &w, FALSE);
                    else {
                        /* cancel AND wait for the cancellation to complete, the
                           read path's shape above: a cancelled-but-unreaped IRP
                           still references wr (this frame) and its event, so
                           CloseHandle-ing the event here races it. */
                        CancelIoEx(h, &wr);
                        WaitForSingleObject(wr.hEvent, INFINITE);
                        wdone = 0;
                    }
                }
                if (wr.hEvent) CloseHandle(wr.hEvent);
                /* why any flush at all: a COMPLETED WriteFile only lands the
                   bytes in the pipe's out buffer — a client that closes
                   without reading still loses them. but FlushFileBuffers is
                   synchronous, and on a pipe the client is not reading it can
                   block forever, defeating the 5 s deadline this whole loop
                   enforces. so: skip it when the write was cancelled (the
                   client is not reading anyway; the bytes are undeliverable
                   and a flush would only block), and on the completed path
                   run it on the helper thread bounded by the same 5 s budget.
                   on timeout: CancelSynchronousIo is best-effort (a pipe
                   flush may ignore it); the DisconnectNamedPipe below is
                   the sure unblock — CloseHandle(h) no longer destroys
                   the object (the thread's duplicate holds a reference,
                   which is exactly what prevents the recycled-handle
                   race), so the teardown reap below is BOUNDED. honest
                   residual caveat: a
                   client that stalls past the budget can still lose already
                   written bytes. */
                if (wdone) {
                    ULONGLONG spent = GetTickCount64() - wt0;
                    DWORD left = spent >= 5000 ? 0 : (DWORD)(5000 - spent);
                    /* no budget left (the write ate the whole 5 s): skip the
                       flush too — a helper thread spawned only to time out
                       instantly could reach FlushFileBuffers AFTER the
                       CloseHandle(h) below, flushing a recycled handle
                       value. the client is not draining either way. */
                    if (left > 0) {
                        PipeFlush *pf = malloc(sizeof *pf);
                        fev = pf ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL;
                        if (!fev) free(pf);
                        else {
                            /* the thread gets its own reference (see
                               pipe_flush_thr): CloseHandle(h) below can
                               then never race the flush over a recycled
                               handle value, at any budget size */
                            pf->ev = fev;
                            if (!DuplicateHandle(GetCurrentProcess(), h,
                                                 GetCurrentProcess(), &pf->h,
                                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                pf->h = NULL;
                            }
                            fth = pf->h ? CreateThread(NULL, 0, pipe_flush_thr,
                                                       pf, 0, NULL) : NULL;
                            if (!fth) {
                                if (pf->h) CloseHandle(pf->h);
                                CloseHandle(fev);
                                free(pf);
                            }
                        }
                        if (fth) {
                            if (WaitForSingleObject(fev, left) == WAIT_OBJECT_0) {
                                CloseHandle(fth); CloseHandle(fev);
                                fth = fev = NULL;
                            } else {
                                CancelSynchronousIo(fth);
                                /* fth/fev stay live: reaped after the
                                   DisconnectNamedPipe + CloseHandle(h)
                                   below. */
                            }
                        }
                    }
                }
            }
        }
        if (conn.hEvent) CloseHandle(conn.hEvent);
        DisconnectNamedPipe(h);
        pthread_mutex_lock(&pipe_mtx);
        cur_pipe = INVALID_HANDLE_VALUE;
        pthread_mutex_unlock(&pipe_mtx);
        CloseHandle(h);
        if (fth) {
            /* what ends the stuck flush: the cancellation(s) and/or the
               flush failing against the disconnected instance — not the
               CloseHandle(h) above, which only drops the caller's
               reference (the thread's duplicate keeps the object alive;
               that IS the recycled-handle fix). cancel once more: the
               budget-expiry cancel above can miss if the thread entered
               FlushFileBuffers only after it ran. the reap is BOUNDED:
               a client that neither drains nor dies could keep the flush
               wedged past every unblock, and the accept loop must not
               inherit that hang — on timeout fth/fev (and the thread's
               own duplicate + arg block) leak deliberately; two handles
               beat a dead daemon, and the next pipe instance is a fresh
               object unaffected by the stuck one. */
            CancelSynchronousIo(fth);
            if (WaitForSingleObject(fev, 5000) == WAIT_OBJECT_0) {
                CloseHandle(fth); CloseHandle(fev);
            }
        }
    }
    if (psd) LocalFree(psd);
    return NULL;
#endif
}

#if defined(LV_WIN9X) && defined(_WIN32)
static unsigned __stdcall lv98_srv_entry(void *a) {
    srv_thread(a);
    return 0;
}
#endif

int srv_start(Player *p) {
    srv_p = p;
    srv_quit = 0;
    cur_pipe = INVALID_HANDLE_VALUE;
#if defined(LV_WIN9X) && defined(_WIN32)
    /* batch 26: era-native spawn — _beginthreadex default stack +
       real tid is THE hardware-proven shape (t1, won three boots
       running); no matrix, the shape decided. If it ever fails the
       era daemon simply has no server thread, exactly the pre-batch
       behavior — never a regression. */
    unsigned btid = 0;
    srv_th = (void *)_beginthreadex(NULL, 0, lv98_srv_entry, NULL, 0, &btid);
    lv98_tracef("srv:thr h=%p gle=%lu", srv_th,
                srv_th ? 0UL : GetLastError());
#else
    pthread_create(&srv_th, NULL, srv_thread, NULL);
#endif
    return 0;
}

void srv_stop(void) {
    srv_quit = 1;
    /* cancel under the lock: the thread cannot close the handle it just
       handed us while we hold it, so CancelIoEx never sees a closed or
       recycled value */
    pthread_mutex_lock(&pipe_mtx);
    HANDLE cp = cur_pipe;
    if (cp != INVALID_HANDLE_VALUE) CancelIoEx(cp, NULL);
    pthread_mutex_unlock(&pipe_mtx);
#if defined(LV_WIN9X) && defined(_WIN32)
    if (srv_th) {
        /* batch 28: the era server may sit in a BLOCKING synchronous
           ConnectNamedPipe that CancelIo need not interrupt (9x pipe
           cancel semantics are not adjudicated from memory) — cap the
           reap at 2 s and let process exit kill the thread; closing
           the handle of a running thread is legal. Shutdown must
           never hang on the pipe. */
        DWORD rc = WaitForSingleObject((HANDLE)srv_th, 2000);
        CloseHandle((HANDLE)srv_th);
        srv_th = NULL;
        lv98_tracef("srv:stop rc=%lu", rc);
    }
#else
    pthread_join(srv_th, NULL);
#endif
}

#else /* POSIX */

static void *srv_thread(void *arg) {
    (void)arg;
    while (!srv_quit) {
        struct pollfd pf = {listen_fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0) continue;
        int c = lv_conn_accept(listen_fd);   /* peer-verified (sock.h) */
        if (c < 0) continue;
        /* bound the read: a client that connects and sends nothing must
           not park the server thread forever (that deadlocks CLI/MCP),
           and the client sends up to 8 KB in one line — one read() into
           2 KB used to truncate long commands mid-path. The bound is a
           deadline over the WHOLE line read: SO_RCVTIMEO bounds a single
           read() only, so a client dribbling a byte every few seconds
           kept this serial accept loop from serving anyone else for
           minutes. */
        struct timespec t0, now;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        char line[8192];
        ssize_t off = 0;
        while (off < (ssize_t)sizeof line - 1) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long left = 5000 - ((long)(now.tv_sec - t0.tv_sec) * 1000
                                + (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (left <= 0) break; /* whole-line budget spent: drop it */
            struct pollfd cp = { c, POLLIN, 0 };
            if (poll(&cp, 1, (int)left) <= 0) break;
            ssize_t n = read(c, line + off, sizeof line - 1 - off);
            if (n <= 0) break;
            off += n;
            line[off] = 0;
            if (strchr(line, '\n')) break;
        }
        if (off > 0) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char resp[4096];
            handle_cmd(srv_p, line, resp, sizeof resp);
            size_t rl = strlen(resp);
            resp[rl++] = '\n';
            ssize_t off2 = 0;
            while (off2 < (ssize_t)rl) {
                ssize_t w = write(c, resp + off2, rl - off2);
                if (w <= 0) break;
                off2 += w;
            }
        }
        close(c);
    }
    return NULL;
}

int srv_start(Player *p) {
    srv_p = p;
    srv_quit = 0;
    char path[512];
    lv_sock_path(path, sizeof path);
    int fallback = lv_tmp_fallback();
    /* if a live self-owned listener answers, do not steal its path:
       unlink+bind would leave the first instance playing but unreachable
       by CLI/MCP. a foreign peer is evicted via unlink+bind. */
    if (fallback) {
        /* /tmp is world-writable and sticky: another user can pre-bind the
           deterministic name and unlink of THEIR file then fails with EPERM,
           so eviction is impossible and the squat disables us. Bind an
           unpredictable name instead and publish it in our config dir,
           which no other user can write; the squat becomes useless. */
        char pub[512];
        if (lv_sock_pointer(pub, sizeof pub) && lv_listener_own(pub)) {
            fprintf(stderr, "levari: another instance is already running; daemon disabled\n");
            return -1;
        }
        unsigned rnd = 0;
        FILE *u = fopen("/dev/urandom", "rb");
        if (u) { if (fread(&rnd, 1, sizeof rnd, u) != sizeof rnd) rnd = 0; fclose(u); }
        if (!rnd) rnd = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
        snprintf(path, sizeof path, "/tmp/levari-%d-%08x.sock", (int)getuid(), rnd);
    } else if (lv_listener_own(path)) {
        fprintf(stderr, "levari: another instance is already running; daemon disabled\n");
        return -1;
    }
    listen_fd = lv_listener_bind(path);
    if (listen_fd < 0) return -1;
    if (fallback) {
        char pf[512];
        lv_sock_ptr_file(pf, sizeof pf);
        char dir[512];
        lv_strlcpy(dir, pf, sizeof dir);
        if (lv_dirname(dir)) mkdir(dir, 0755);
        /* publish atomically: a client resolving the pointer must never read a
           half-written path. Write a per-pid temp in the same directory, then
           rename() over the target (atomic within one filesystem). */
        int wrote = 0;
        if (pf[0]) {
            char tmp[600];
            snprintf(tmp, sizeof tmp, "%s.%ld.tmp", pf, (long)getpid());
            FILE *w = fopen(tmp, "w");
            if (w) {
                fputs(path, w);
                if (fclose(w) == 0 && chmod(tmp, 0600) == 0 && rename(tmp, pf) == 0)
                    wrote = 1;
                else
                    unlink(tmp);
            }
        }
        if (wrote) lv_strlcpy(srv_ptr_pub, pf, sizeof srv_ptr_pub);
        else fprintf(stderr, "levari: cannot publish socket path to %s; "
                             "CLI/MCP cannot reach this instance\n", pf);
    }
    lv_strlcpy(srv_path, path, sizeof srv_path);
    pthread_create(&srv_th, NULL, srv_thread, NULL);
    return 0;
}

void srv_stop(void) {
    if (listen_fd < 0) return;
    srv_quit = 1;
    pthread_join(srv_th, NULL);
    close(listen_fd);
    listen_fd = -1;
    unlink(srv_path);   /* the published path may differ from the default */
    /* the pointer file is ours to remove too: a stale one makes every
       client resolve a dead path before falling back */
    if (srv_ptr_pub[0]) { unlink(srv_ptr_pub); srv_ptr_pub[0] = 0; }
}

#endif
