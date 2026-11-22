/* win98 era startup forensics — see win98_diag.h for the why.
   Era-safe: A-APIs only, no SIMD ever compiled in (era flags), every
   write flushed, nothing here may fault (the handler must survive a
   faulting process; all buffers static). */
#include "compat/win98_diag.h"
#if defined(LV_WIN9X) && defined(_WIN32)

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

static char g_logpath[MAX_PATH * 2];
static HANDLE g_logh = INVALID_HANDLE_VALUE;

static void lv98_write(const char *buf, int n)
{
    DWORD w;
    if (g_logh == INVALID_HANDLE_VALUE)
        return;
    WriteFile(g_logh, buf, (DWORD)n, &w, NULL);
}

void lv98_tracef(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (g_logh == INVALID_HANDLE_VALUE)
        return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 3, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = (int)(sizeof buf - 3);
    buf[n++] = '\r';
    buf[n++] = '\n';
    buf[n] = 0;
    lv98_write(buf, n);
}

/* Runs instead of Watson on an unhandled exception. Logs enough to
   map the fault to objdump output of THIS build:
     rva = eip - imagebase   ->   objdump addr = rva + 0x400000 */
static LONG WINAPI lv98_crash(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    DWORD base = (DWORD)(uintptr_t)GetModuleHandleA(NULL);
    lv98_tracef("CRASH code=%08lx eip=%08lx base=%08lx rva=%08lx "
                "ea0=%08lx ea1=%08lx",
                (unsigned long)r->ExceptionCode,
                (unsigned long)r->ExceptionAddress, base,
                (unsigned long)((char *)r->ExceptionAddress - (char *)base),
                r->NumberParameters > 0
                    ? (unsigned long)r->ExceptionInformation[0] : 0,
                r->NumberParameters > 1
                    ? (unsigned long)r->ExceptionInformation[1] : 0);
    lv98_tracef("trace end (crash)");
    if (g_logh != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_logh);
    return EXCEPTION_EXECUTE_HANDLER; /* exit, no Watson dialog */
}

void lv98_diag_init(void)
{
    char exe[MAX_PATH];
    char *slash;
    DWORD attr = GetFileAttributesA("levari_w98.log");
    HMODULE u;
    OSVERSIONINFOA ov;
    DEVMODEA dm;

    /* If a previous run left a log, don't overwrite the corpse:
       append a separator instead (tester can send every round). */
    lstrcpynA(g_logpath, "levari_w98.log", MAX_PATH * 2);
    if (GetModuleFileNameA(NULL, exe, sizeof exe)) {
        slash = strrchr(exe, '\\');
        if (slash) {
            snprintf(g_logpath, sizeof g_logpath, "%.*s\\levari_w98.log",
                     (int)(slash - exe), exe);
        }
    }
    (void)attr;
    /* batch 29: FILE_APPEND_DATA (not GENERIC_WRITE) — the GUI holds
       this log open for its whole life with its pointer advancing
       locally; a second process (CLI verb) appending with plain
       write access writes from ITS end-offset-at-open and CLOBBERS
       the GUI's newer bytes (hardware proof: CLI blocks — attcon/
       cli: lines — vanished wholesale from the batch-28 log, and
       one line was found bisected mid-bytes at a session boundary).
       FILE_APPEND_DATA re-positions at every write: concurrent
       appenders can interleave but never overwrite. */
    g_logh = CreateFileA(g_logpath, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logh != INVALID_HANDLE_VALUE)
        SetFilePointer(g_logh, 0, NULL, FILE_END);
    SetUnhandledExceptionFilter(lv98_crash);

    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    GetVersionExA(&ov);
    u = GetModuleHandleA("unicows.dll");
    lv98_tracef("=== levari w98 boot os=%ld.%ld.%ld unicows=%ld "
                "cmdline=[%s]",
                (long)ov.dwMajorVersion, (long)ov.dwMinorVersion,
                (long)ov.dwBuildNumber, (long)(u != NULL),
                GetCommandLineA());
    /* display truth at boot (palette-dark reports need the mode) */
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        lv98_tracef("display %ldx%ld@%lubpp hz=%lu",
                    (long)dm.dmPelsWidth, (long)dm.dmPelsHeight,
                    (unsigned long)dm.dmBitsPerPel,
                    (unsigned long)dm.dmDisplayFrequency);
    else
        lv98_tracef("display: EnumDisplaySettingsA FAILED err=%lu",
                    GetLastError());
}

#endif /* LV_WIN9X && _WIN32 */
