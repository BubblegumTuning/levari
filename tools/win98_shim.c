/* win98 import interposition (Win98 branch; era builds ONLY — compiled
   when LV_WIN9X, never on flagship builds).

   Modern mingw-w64 import libraries keep per-symbol archive members,
   and an archive member is pulled ONLY for symbols still undefined at
   link time: defining the __imp__ thunk here preempts the kernel32
   member entirely, so the final PE imports NONE of these entries.
   (Verified by import census, not assumed.)

   Each replacement is era-safe on both floors we care about: 9x gets
   the emulation, NT-class systems keep working (mainline never links
   this object, so this is the win98 artifact's story only). */
#include <windows.h>
#include <mmsystem.h> /* batch 14: winmm audio trio signatures */
#include <stddef.h>   /* offsetof on the szPname marshal */
#include "compat/win98_diag.h" /* era boot forensics (LV_WIN9X builds) */

/* ---- TryEnterCriticalSection (Win2000+) ----
   Referenced by winpthreads' cond object AND by SDL's sysmutex. On
   2000+ we forward to the real function resolved lazily (it is absent
   from 9x kernel32; GetProcAddress never imports it). On 9x: the
   classic Interlocked emulation over the CRITICAL_SECTION fields that
   the 9x layout also has (LockCount/RecursionCount/OwningThread),
   matching the real accounting: one LockCount increment per hold, free
   at -1. Known limitation of this era-standard hack: the recursive
   OwningThread test races a concurrent release — the same window the
   era's own porting guides accepted. Our two callers (a thread locking
   its own cond mutex; SDL event-queue trylocks) do not contend that
   window. */
typedef BOOL(WINAPI *lv_te_cs)(CRITICAL_SECTION *);

BOOL WINAPI lv98_TryEnterCriticalSection(CRITICAL_SECTION *cs)
{
    static lv_te_cs real;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (lv_te_cs)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                        "TryEnterCriticalSection");
    }
    if (real)
        return real(cs);
    if (InterlockedIncrement(&cs->LockCount) == 0) { /* was -1: acquired */
        cs->OwningThread = (PVOID)(ULONG_PTR)GetCurrentThreadId();
        cs->RecursionCount = 1;
        return TRUE;
    }
    if (cs->OwningThread == (PVOID)(ULONG_PTR)GetCurrentThreadId()) {
        /* recursive: keep the increment — one LockCount per hold */
        cs->RecursionCount++;
        return TRUE;
    }
    InterlockedDecrement(&cs->LockCount);
    return FALSE;
}

/* ---- InitializeCriticalSectionAndSpinCount (Win2000+) ----
   SDL sysmutex's normal init path. The spin count is a pure
   performance hint: plain init is the era behavior. */
BOOL WINAPI lv98_InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs,
                                                       DWORD spin)
{
    (void)spin;
    InitializeCriticalSection(cs);
    return TRUE;
}

/* ---- RegisterRawInputDevices (Vista+) ----
   Static import of SDL's windowsmouse backend (every SDL2 vintage).
   Always report failure: era builds must ride the classic mouse path,
   and the measured code path in SDL_windowsmouse.c treats a FALSE
   return from the register call as "raw input off". */
BOOL WINAPI lv98_RegisterRawInputDevices(void *rids, UINT n, UINT sz)
{
    (void)rids;
    (void)n;
    (void)sz;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- AttachConsole (Win2000+) ----
   Static import of main.c's CLI console re-attach (the call site is
   guarded by _WIN32 only, not by the era guard). On 9x kernel32 has no
   such export and a static import aborts the LOADER outright
   ("missing export kernel32.dll AttachConsole" — reported on real
   hardware). Batch 28 replaces the old always-fail stub: on the 9x
   console model the Win32 console is GLOBAL per session — a
   GUI-subsystem exe has no console to attach, but it can open the
   console output device CONOUT$ directly (that is why NT later needed
   an attach call at all). Probe the very device main.c's freopen
   targets: if it opens, claiming success lets the freopen block run
   and CLI verb output becomes visible; if not, keep the old honest
   failure and the silent behavior. Probed-and-reported, never
   assumed: attcon ok / attcon FAIL gle= lands in the trace log. */
BOOL WINAPI lv98_AttachConsole(DWORD dwProcessId)
{
    (void)dwProcessId;
    HANDLE h = CreateFileA("CONOUT$", GENERIC_WRITE,
                           FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        lv98_tracef("attcon ok");
        return TRUE;
    }
    lv98_tracef("attcon FAIL gle=%lu", GetLastError());
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- VerSetConditionMask + VerifyVersionInfoW (XP+/2003) ----
   Referenced by SDL_windows.c IsWindowsVersionOrGreater (the Vista-or-
   not gates in audio/joystick backends). Era emulation:
   VerSetConditionMask packs one op byte per condition in call order;
   VerifyVersionInfoW compares against the real version from
   GetVersionExA (95-safe). The documented packing is one mask byte
   per SET BIT of the type mask, ascending bit order, one byte per
   condition (that is what the caller's triple-VerSetConditionMask
   produces); fields by bit: 1 major, 2 minor, 4 build, 8 sp-major,
   16 sp-minor, 32 suite, 64 product-type, 128 platform. */
typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor, wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType, wReserved;
} lv98_osviexw; /* layout mirrors OSVERSIONINFOEXW */

#define LV_VER_EQ 1
#define LV_VER_GT 2
#define LV_VER_LT 3
#define LV_VER_GE 4
#define LV_VER_LE 5

static BOOL lv98_cmp(LONG have, LONG want, BYTE op)
{
    switch (op) {
    case LV_VER_EQ: return have == want;
    case LV_VER_GT: return have > want;
    case LV_VER_LT: return have < want;
    case LV_VER_GE: return have >= want;
    case LV_VER_LE: return have <= want;
    default: return TRUE; /* unset/AND/OR: SDL never uses them here */
    }
}

DWORDLONG WINAPI lv98_VerSetConditionMask(DWORDLONG mask, DWORD type, BYTE op)
{
    /* the real function packs one op byte per CALL, in call order; the
       caller's mask accumulates by setting its type bit, so the op for
       a condition lands in the byte slot = number of condition bits
       already packed. counting set bits in the incoming accumulated
       mask's TYPE half is impossible from here (only the packed value
       crosses the boundary) — the observed convention is simply the
       lowest free byte, i.e. pack sequentially per call */
    int slot = 0;
    while (slot < 8 && ((mask >> (8 * slot)) & 0xFF) != 0)
        slot++; /* first free byte */
    (void)type;
    if (slot >= 8)
        return mask;
    mask |= (DWORDLONG)op << (8 * slot);
    return mask;
}

BOOL WINAPI lv98_VerifyVersionInfoW(lv98_osviexw *vi, DWORD typeMask,
                                    DWORDLONG mask)
{
    OSVERSIONINFOA ov;
    int bit, slot = 0;
    const DWORD bits[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    ZeroMemory(&ov, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    if (!GetVersionExA(&ov))
        return FALSE;
    for (bit = 0; bit < 8; bit++) {
        LONG have = -1, want = -1;
        if (!(typeMask & bits[bit]))
            continue;
        switch (bits[bit]) {
        case 1:   have = ov.dwMajorVersion;      want = vi->dwMajorVersion; break;
        case 2:   have = ov.dwMinorVersion;      want = vi->dwMinorVersion; break;
        case 4:   have = ov.dwBuildNumber;       want = vi->dwBuildNumber; break;
        case 8:   have = vi->wServicePackMajor;  break; /* 9x sp: from szCSDVersion era-wise, treat 0 */
        case 16:  have = vi->wServicePackMinor;  break;
        case 32:  have = ov.dwPlatformId;        want = vi->wSuiteMask; break;
        case 64:  have = ov.dwPlatformId;        want = vi->wProductType; break;
        case 128: have = ov.dwPlatformId;        want = vi->dwPlatformId; break;
        }
        if (bits[bit] == 8 || bits[bit] == 16) {
            /* service pack fields: GetVersionExA carries no numeric SP on
               9x; the era floor (4.10 SP1+) satisfies any SP ask SDL makes
               (SDL only asks SP0) — pass. */
            BYTE op = (BYTE)((mask >> (8 * slot)) & 0xFF);
            if (op == LV_VER_GT || op == LV_VER_LT) { /* strict asks: fail */
                SetLastError(ERROR_OLD_WIN_VERSION);
                return FALSE;
            }
            slot++;
            continue;
        }
        {
            BYTE op = (BYTE)((mask >> (8 * slot)) & 0xFF);
            if (op != 0 && !lv98_cmp(have, want, op)) {
                SetLastError(ERROR_OLD_WIN_VERSION);
                return FALSE;
            }
        }
        slot++;
    }
    return TRUE;
}


/* ---- SetFilePointerEx (Win2000+) ----
   Static import of SDL_rwops.o's 64-bit seek. Loader-killer on 9x if
   left imported. Forward to the real function where it exists; on 9x
   emulate by the documented SetFilePointer split (high word optional). */
typedef BOOL(WINAPI *lv_sfpe)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);

static FARPROC lv98_real(const char *dll, const char *name)
{
    FARPROC p = GetProcAddress(GetModuleHandleA(dll), name);
    /* 9x MECHANISM (hardware-decided 2026-11-17, batch 8): on the 9x
       platform family every *W export is a dead stub returning
       ERROR_CALL_NOT_IMPLEMENTED (MSLU rewrites the loader-patched IAT
       but only for its supported subset — EnumDisplaySettingsW stayed
       dead with unicows loaded). Forwarding to such a stub returns
       failure where our era fallback would succeed, so on 9x a W name
       NEVER counts as present. NT family: resolve normally. */
    if (p) {
        size_t n = strlen(name);
        if (n && name[n - 1] == 'W') {
            static int is9x = -1;
            if (is9x < 0) {
                OSVERSIONINFOA ov;
                memset(&ov, 0, sizeof ov);
                ov.dwOSVersionInfoSize = sizeof ov;
                is9x = GetVersionExA(&ov) &&
                       ov.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
            }
            if (is9x)
                return NULL;
        }
    }
    return p;
}

BOOL WINAPI lv98_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist,
                                  PLARGE_INTEGER newPos, DWORD method)
{
    static lv_sfpe real;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = lv98_real("kernel32.dll", "SetFilePointerEx");
    }
    if (real)
        return real(h, dist, newPos, method);
    {
        LONG hi = (LONG)(dist.QuadPart >> 32);
        LONG lo = (LONG)(dist.QuadPart & 0xFFFFFFFF);
        LONG newhi = hi;
        DWORD ret = SetFilePointer(h, lo, newPos ? &newhi : NULL, method);
        if (ret == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
            return FALSE;
        if (newPos) {
            newPos->LowPart = ret;
            newPos->HighPart = newhi;
        }
        return TRUE;
    }
}

/* ---- WaitForSingleObjectEx (Vista+) ----
   Static import of SDL_syssem.o/SDL_systhread.o/SDL_wasapi.o waits.
   Forward where present; 9x falls back to WaitForSingleObject. Era
   caveat (documented): the alertable flag is dropped — none of our
   callers wait alertable, so the wait semantics they rely on are
   identical. */
typedef DWORD(WINAPI *lv_wsoe)(HANDLE, DWORD, BOOL);

DWORD WINAPI lv98_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    static lv_wsoe real;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (lv_wsoe)lv98_real("kernel32.dll", "WaitForSingleObjectEx");
    }
    (void)alertable;
    if (real)
        return real(h, ms, alertable);
    return WaitForSingleObject(h, ms);
}

/* ---- GetThreadContext / SetThreadContext (XP+; 9x kernel32 has no
   such exports — provenance: libwinpthread_la-thread.o's deferred-
   cancellation EIP probe). Forward where present; else report failure
   — winpthreads treats a failed probe as "cannot check", the documented
   fallback, and cancellation rides its exception path. */
typedef BOOL(WINAPI *lv_gtc)(HANDLE, LPCONTEXT);

BOOL WINAPI lv98_GetThreadContext(HANDLE h, LPCONTEXT ctx)
{
    static lv_gtc real;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = lv98_real("kernel32.dll", "GetThreadContext");
    }
    if (real)
        return real(h, ctx);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI lv98_SetThreadContext(HANDLE h, LPCONTEXT ctx)
{
    static lv_gtc real;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = lv98_real("kernel32.dll", "SetThreadContext");
    }
    if (real)
        return real(h, ctx);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- ToUnicode (Win2000+) ----
   Static import of SDL_windowskeyboard.o's text generation (SDL2 since
   2.0.4 builds TEXTINPUT from WM_KEYDOWN + ToUnicode for AltGr
   correctness — there is no WM_CHAR fallback at this vintage, so
   without an emulation era users could not type into fields at all).
   Forward where present; 9x rides ToAscii (Win95+): the era API with
   the same key-state contract, widened through the ACP. Multi-byte
   dead-key sequences are simplified to their byte chars — era-accurate
   enough for the ASCII range, honest caveat for CJK ACPs. */
int WINAPI lv98_ToUnicode(WPARAM vk, UINT scan, const BYTE *keys,
                          LPWSTR buf, int cch, UINT flags)
{
    static int(WINAPI *real)(WPARAM, UINT, const BYTE *, LPWSTR, int, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (int(WINAPI *)(WPARAM, UINT, const BYTE *, LPWSTR, int, UINT))
                   lv98_real("user32.dll", "ToUnicode");
    }
    if (real)
        return real(vk, scan, keys, buf, cch, flags);
    {
        CHAR chars[2];
        int n = ToAscii((UINT)vk, scan, keys, (WORD *)chars, flags);
        if (n <= 0)
            return n; /* 0 no char, -1 dead key: same contract */
        if (cch < n)
            return -1;
        if (MultiByteToWideChar(CP_ACP, 0, chars, n, buf, n) != n) {
            SetLastError(ERROR_NO_UNICODE_TRANSLATION);
            return -1;
        }
        return n;
    }
}

/* ---- GetClipboardSequenceNumber (XP+) ----
   Static import of SDL_windowsclipboard.o's cache invalidation.
   Forward where present; on 9x return an ever-increasing counter so
   SDL refreshes the clipboard on every query: the cache it invalidates
   is a perf optimization, losing it costs one paste-time re-read,
   never correctness. */
DWORD WINAPI lv98_GetClipboardSequenceNumber(void)
{
    static LONG n;
    static DWORD(WINAPI *real)(void);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (DWORD(WINAPI *)(void))lv98_real("user32.dll",
                                                "GetClipboardSequenceNumber");
    }
    if (real)
        return real();
    return (DWORD)InterlockedIncrement(&n);
}

/* ---- SetLayeredWindowAttributes (Win2000+; "not supported" on 9x) ----
   Static import of SDL_windowswindow.o's opacity path. Forward where
   present; era FALSE fails the opacity call cleanly (the feature does
   not exist on 9x anyway). */
BOOL WINAPI lv98_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha,
                                            DWORD flags)
{
    static BOOL(WINAPI *real)(HWND, COLORREF, BYTE, DWORD);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HWND, COLORREF, BYTE, DWORD))
                   lv98_real("user32.dll", "SetLayeredWindowAttributes");
    }
    if (real)
        return real(hwnd, key, alpha, flags);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- RegisterDeviceNotificationW / UnregisterDeviceNotification
   (Win2000+) ----
   Static imports of SDL_windowsjoystick.o's hotplug watcher. Forward
   where present; era NULL/FAILURE: SDL's measured error path proceeds
   with polling joystick attach detection instead of WM_DEVICECHANGE
   (hotplug degrades, init does not). */
HDEVNOTIFY WINAPI lv98_RegisterDeviceNotificationW(HWND hwnd, UINT filter,
                                                   BOOL inherit)
{
    /* real typed LONG-returning: casting FARPROC (int-returning) to a
       void*-returning pointer trips -Wcast-function-type; the handle
       crosses as an integer instead */
    static LONG(WINAPI *real)(HWND, UINT, BOOL);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LONG(WINAPI *)(HWND, UINT, BOOL))
                   lv98_real("user32.dll", "RegisterDeviceNotificationW");
    }
    if (real)
        return (HDEVNOTIFY)(UINT_PTR)real(hwnd, filter, inherit);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

BOOL WINAPI lv98_UnregisterDeviceNotification(HDEVNOTIFY handle)
{
    static BOOL(WINAPI *real)(HDEVNOTIFY);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HDEVNOTIFY))
                   lv98_real("user32.dll", "UnregisterDeviceNotification");
    }
    if (real)
        return real(handle);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- GetRawInputData / GetRawInputDeviceList / GetRawInputDeviceInfoA
   (XP+) ----
   Static imports of SDL's event queue and BOTH joystick backends.
   Pure era stubs (no forward) to match the standing
   RegisterRawInputDevices posture — raw input is off on era builds
   everywhere, so these must only keep the loader happy and return the
   "empty" value the callers already treat as "raw input off"
   (GetRawInputDeviceList must return 0, not (UINT)-1: the callers'
   checks are `<= 0` on an unsigned, which a -1 would sail through
   into a 4-billion-entry alloc). */
UINT WINAPI lv98_GetRawInputData(HRAWINPUT h, UINT cmd, void *data,
                                 UINT *size, UINT cbHdr)
{
    (void)h;
    (void)cmd;
    (void)data;
    (void)cbHdr;
    if (size)
        *size = 0;
    return 0;
}

UINT WINAPI lv98_GetRawInputDeviceList(void *list, UINT *num, UINT cbSize)
{
    (void)list;
    (void)cbSize;
    if (num)
        *num = 0;
    return 0;
}

UINT WINAPI lv98_GetRawInputDeviceInfoA(HANDLE dev, UINT cmd, void *data,
                                        UINT *size)
{
    (void)dev;
    (void)cmd;
    (void)data;
    if (size)
        *size = 0;
    return 0;
}

/* ---- PropVariantClear (Win2000+; absent from 9x ole32/oleaut32) ----
   Static import of SDL_wasapi_win32.o (provenance by LINKER MAP — the
   nm -A archive scan MISSED this member; map is the authority).
   Forward where present. The era fallback is nearly unreachable on 98
   (the WASAPI COM class the caller needs does not exist there, SDL
   declines the backend first), but is written honest for the common
   vectors so a mis-provenanced call can never corrupt the heap:
   release the payloads oleaut32 95 already provides, then zero. */
HRESULT WINAPI lv98_PropVariantClear(PROPVARIANT *pv)
{
    static HRESULT(WINAPI *real)(PROPVARIANT *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HRESULT(WINAPI *)(PROPVARIANT *))lv98_real("ole32.dll",
                                                           "PropVariantClear");
    }
    if (real)
        return real(pv);
    if (!pv)
        return E_INVALIDARG;
    if (pv->vt & VT_VECTOR) {
        /* every CA* vector header shares { cElems, pElems } (mingw-w64
           names the generic one cac) */
        void *base = (void *)pv->cac.pElems;
        ULONG n = pv->cac.cElems;
        WORD elem = pv->vt & VT_TYPEMASK;
        if (base) {
            ULONG i;
            switch (elem) {
            case VT_LPWSTR:
                for (i = 0; i < n; i++)
                    CoTaskMemFree(((LPWSTR *)base)[i]);
                break;
            case VT_BSTR:
                for (i = 0; i < n; i++)
                    SysFreeString(((BSTR *)base)[i]);
                break;
            default:
                break; /* scalar vectors: one block, freed below */
            }
            CoTaskMemFree(base);
        }
    }
    switch (pv->vt & ~VT_ARRAY) {
    case VT_UNKNOWN:
        if (pv->punkVal)
            pv->punkVal->lpVtbl->Release(pv->punkVal);
        break;
    case VT_DISPATCH:
        if (pv->pdispVal)
            pv->pdispVal->lpVtbl->Release(pv->pdispVal);
        break;
    case VT_BSTR:
        if (pv->bstrVal)
            SysFreeString(pv->bstrVal);
        break;
    case VT_ARRAY:
        if (pv->parray)
            SafeArrayDestroy(pv->parray);
        break;
    default:
        break;
    }
    ZeroMemory(pv, sizeof *pv);
    return S_OK;
}

/* ================= batch 3 (hardware rerun, GetFileSizeEx abort) ====
   The tester's new exe aborted at GetFileSizeEx — an import the
   allow-list had adjudicated "95-era" from memory and MSDN calls
   Win2000. Lesson recorded: MS Learn's modern "Minimum supported
   client" is retro-raised for the 95/98 era (it claims CloseHandle
   "requires Windows 2000"), so name-by-memory is out; everything
   below is interposed with the forward-then-fallback shape, which is
   behavior-preserving whether or not the name ships on 98. */

/* ---- GetFileSizeEx (Win2000+; SDL_rwops' 64-bit size probe) ----
   Fallback: the documented GetFileSize split. FAT32 caps files at
   4 GiB, so on 98 hardware this is exact. */
BOOL WINAPI lv98_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    static BOOL(WINAPI *real)(HANDLE, PLARGE_INTEGER);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HANDLE, PLARGE_INTEGER))
                   lv98_real("kernel32.dll", "GetFileSizeEx");
    }
    if (real)
        return real(h, size);
    {
        DWORD hi = 0;
        DWORD lo = GetFileSize(h, &hi);
        if (lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
            return FALSE;
        size->QuadPart = (LONGLONG)lo | ((LONGLONG)hi << 32);
        return TRUE;
    }
}

/* ---- GetCurrentProcessId (XP+ per MSDN; provenance: winpthread) ----
   Forward where present. Era fallback: one constant per process —
   winpthread only compares copies of this same value (its thread-GC
   liveness bookkeeping), and the app-side uses (getpid seeds, tmp
   names) tolerate any stable value; 98 is effectively single-user. */
DWORD WINAPI lv98_GetCurrentProcessId(void)
{
    static DWORD(WINAPI *real)(void);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (DWORD(WINAPI *)(void))lv98_real("kernel32.dll",
                                                "GetCurrentProcessId");
    }
    if (real)
        return real();
    return 1;
}

/* ---- GetHandleInformation (Win2000+ per MSDN; winpthread) ----
   Its only use here is winpthread's pthread_kill liveness probe
   (_pthread_check): failure means "thread gone", success means alive.
   Emulating TRUE with zero flags keeps that truth: on 9x there is no
   inheritance flag to report, and a valid handle stays valid. */
BOOL WINAPI lv98_GetHandleInformation(HANDLE h, PULONG flags)
{
    static BOOL(WINAPI *real)(HANDLE, PULONG);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HANDLE, PULONG))
                   lv98_real("kernel32.dll", "GetHandleInformation");
    }
    if (real)
        return real(h, flags);
    (void)h;
    if (flags)
        *flags = 0;
    return TRUE;
}

/* ---- IsDebuggerPresent (NT4-era; winpthread + SDL_systhread) ----
   Forward where present; era FALSE ("no debugger attached") is the
   only consumer's benign branch (SDL's thread-failure debug print;
   winpthread's SEH path). The dossier lists it as 98-delivered — the
   forward then makes that a no-op; the interposition only insures
   against the docs' NT-only claim being the truth. */
BOOL WINAPI lv98_IsDebuggerPresent(void)
{
    static BOOL(WINAPI *real)(void);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(void))lv98_real("kernel32.dll",
                                               "IsDebuggerPresent");
    }
    if (real)
        return real();
    return FALSE;
}

/* ---- IsDBCSLeadByteEx (provenance: libmsvcrt's mbrtowc) ----
   Forward where present; era fallback IsDBCSLeadByte (Win 3.1/95
   kernel32 export). Honest caveat: the ACP-only CodePage — mingw's
   mbrtowc reaches here for the current code page, which is exactly
   what the 95 API tests. */
BOOL WINAPI lv98_IsDBCSLeadByteEx(UINT codepage, BYTE ch)
{
    static BOOL(WINAPI *real)(UINT, BYTE);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(UINT, BYTE))
                   lv98_real("kernel32.dll", "IsDBCSLeadByteEx");
    }
    if (real)
        return real(codepage, ch);
    (void)codepage;
    return IsDBCSLeadByte(ch);
}

/* ---- PostThreadMessageW (SDL_windowsjoystick shutdown signal) ----
   levari inits SDL_INIT_VIDEO only, so the joystick thread that this
   is meant for never exists here — but the import would still load-
   abort. Forward W, then A (GetProcAddress, no new import: pre-95
   APIs ship their A name whenever the W name exists), else FALSE. */
BOOL WINAPI lv98_PostThreadMessageW(DWORD thread, UINT msg, WPARAM w,
                                    LPARAM l)
{
    static BOOL(WINAPI *real)(DWORD, UINT, WPARAM, LPARAM);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(DWORD, UINT, WPARAM, LPARAM))
                   lv98_real("user32.dll", "PostThreadMessageW");
        if (!real)
            real = (BOOL(WINAPI *)(DWORD, UINT, WPARAM, LPARAM))
                       lv98_real("user32.dll", "PostThreadMessageA");
    }
    if (real)
        return real(thread, msg, w, l);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- EnumDisplayDevicesW (batch 4: import interposer; batch 8: real
   A fallback) ----
   Used to name a display (SDL_windowsmodes WIN_AddDisplay). The batch
   4 comment claimed the enumeration ran "on EnumDisplaySettingsW,
   which stays imported (98-delivered)" — that by-memory claim is what
   batch 8 hardware disproved: it is a dead 9x W stub MSLU does not
   cover, and it is now interposed too (see lv98_EnumDisplaySettingsW). */
BOOL WINAPI lv98_EnumDisplayDevicesW(void *dev, DWORD idx, void *devinfo,
                                     DWORD flags)
{
    static BOOL(WINAPI *real)(void *, DWORD, void *, DWORD);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(void *, DWORD, void *, DWORD))
                   lv98_real("user32.dll", "EnumDisplayDevicesW");
    }
    if (real)
        return real(dev, idx, devinfo, flags);
    /* era: EnumDisplayDevicesA (98 stock) + struct marshal. SDL uses
       this only for display NAMES (SDL_windowsmodes.c WIN_AddDisplay)
       — the A call is the real thing, just narrower strings. */
    {
        DISPLAY_DEVICEA da;
        DISPLAY_DEVICEW *dw = devinfo;
        const char *devA = NULL;
        char devAb[256];
        if (dev) {
            if (!WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)dev, -1, devAb,
                                     sizeof devAb, NULL, NULL))
                return FALSE;
            devA = devAb;
        }
        memset(&da, 0, sizeof da);
        da.cb = sizeof da;
        if (!EnumDisplayDevicesA(devA, idx, &da, flags))
            return FALSE;
        dw->StateFlags = da.StateFlags;
        dw->DeviceID[0] = dw->DeviceString[0] = dw->DeviceName[0] = 0;
        MultiByteToWideChar(CP_ACP, 0, da.DeviceID, -1, dw->DeviceID,
                            ARRAYSIZE(dw->DeviceID));
        MultiByteToWideChar(CP_ACP, 0, da.DeviceString, -1, dw->DeviceString,
                            ARRAYSIZE(dw->DeviceString));
        MultiByteToWideChar(CP_ACP, 0, da.DeviceName, -1, dw->DeviceName,
                            ARRAYSIZE(dw->DeviceName));
        return TRUE;
    }
}

/* ---- GetMonitorInfoW (SDL_windowsmodes' display enumeration) ----
   The one name here whose era fallback must SUCCEED: SDL's
   EnumDisplayMonitors callback drops any monitor GetMonitorInfo
   refuses — FALSE would mean zero video displays and an SDL_Init
   failure. Era fallback is therefore the honest single-monitor truth
   from GetSystemMetrics (rcMonitor=rcWork=the screen, PRIMARY flag;
   MONITORINFOEX callers' szDevice stays the zero they passed in).
   Multi-monitor 98 boxes stack their bounds — documented era caveat,
   never a startup failure. */
BOOL WINAPI lv98_GetMonitorInfoW(HMONITOR mon, LPMONITORINFO mi)
{
    static BOOL(WINAPI *real)(HMONITOR, LPMONITORINFO);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HMONITOR, LPMONITORINFO))
                   lv98_real("user32.dll", "GetMonitorInfoW");
    }
    if (real)
        return real(mon, mi);
    (void)mon;
    if (!mi)
        return FALSE;
    mi->rcMonitor.left = 0;
    mi->rcMonitor.top = 0;
    mi->rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
    mi->rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = MONITORINFOF_PRIMARY;
    /* SDL passes MONITORINFOEX and hands info->szDevice straight to
       EnumDisplaySettingsW (SDL_windowsmodes.c:155+121) — an EMPTY
       device name fails on 9x the same way ("" is not NULL), so the
       real primary name must travel with the bounds. cbSize-guarded:
       plain MONITORINFO callers (WIN_GetDisplayBounds) must not get
       written past their 40-byte struct. */
    if (mi->cbSize >= sizeof(MONITORINFOEXW)) {
        MONITORINFOEXW *ex = (MONITORINFOEXW *)mi;
        DISPLAY_DEVICEA dd;
        ex->szDevice[0] = 0;
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (EnumDisplayDevicesA(NULL, 0, &dd, 0))
            MultiByteToWideChar(CP_ACP, 0, dd.DeviceName, -1, ex->szDevice,
                                ARRAYSIZE(ex->szDevice));
    }
    return TRUE;
}

/* ---- EnumDisplaySettingsW (SDL_windowsmodes WIN_GetDisplayMode —
   THE batch-8 killer: 98's user32 exports this as a dead W stub, and
   MSLU does NOT cover it (hardware 2026-11-17: with unicows loaded
   every display add failed "No displays available"). The earlier
   "98-delivered" comment was the by-memory error class again. ---- */
static void lv98_devmode_a2w(const DEVMODEA *a, DEVMODEW *w)
{
    w->dmSpecVersion = a->dmSpecVersion;
    w->dmDriverVersion = a->dmDriverVersion;
    w->dmSize = sizeof(DEVMODEW);
    w->dmDriverExtra = 0;
    w->dmFields = a->dmFields;
    w->dmBitsPerPel = a->dmBitsPerPel;
    w->dmPelsWidth = a->dmPelsWidth;
    w->dmPelsHeight = a->dmPelsHeight;
    w->dmDisplayFlags = a->dmDisplayFlags;
    w->dmDisplayFrequency = a->dmDisplayFrequency;
    w->dmICMMethod = a->dmICMMethod;
    w->dmICMIntent = a->dmICMIntent;
    w->dmMediaType = a->dmMediaType;
    w->dmDitherType = a->dmDitherType;
    w->dmReserved1 = a->dmReserved1;
    w->dmReserved2 = a->dmReserved2;
#ifdef DM_PANNINGWIDTH /* mingw W-only trailing pair */
    w->dmPanningWidth = 0;
    w->dmPanningHeight = 0;
#endif
    w->dmDeviceName[0] = w->dmFormName[0] = 0;
    MultiByteToWideChar(CP_ACP, 0, (const char *)a->dmDeviceName, -1,
                        w->dmDeviceName, ARRAYSIZE(w->dmDeviceName));
    MultiByteToWideChar(CP_ACP, 0, (const char *)a->dmFormName, -1,
                        w->dmFormName, ARRAYSIZE(w->dmFormName));
}

BOOL WINAPI lv98_EnumDisplaySettingsW(LPCWSTR devW, DWORD idx,
                                      DEVMODEW *dmW)
{
    static BOOL(WINAPI *real)(LPCWSTR, DWORD, DEVMODEW *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(LPCWSTR, DWORD, DEVMODEW *))
                   lv98_real("user32.dll", "EnumDisplaySettingsW");
    }
    if (real)
        return real(devW, idx, dmW);
    /* era: EnumDisplaySettingsA (Win95 stock) + marshal. Empty name
       maps to NULL = display device settings, matching NT's W
       semantics SDL relies on. */
    {
        DEVMODEA dmA;
        const char *nameA = NULL;
        char nameAb[256];
        if (devW && devW[0]) {
            if (!WideCharToMultiByte(CP_ACP, 0, devW, -1, nameAb,
                                     sizeof nameAb, NULL, NULL))
                return FALSE;
            nameA = nameAb;
        }
        memset(&dmA, 0, sizeof dmA);
        dmA.dmSize = sizeof dmA;
        if (!EnumDisplaySettingsA(nameA, idx, &dmA))
            return FALSE;
        lv98_devmode_a2w(&dmA, dmW);
        return TRUE;
    }
}

/* ---- window lifecycle (batch 10, hardware "Couldn't create window:
   b"): SDL's create path is CreateWindowExW -> SDL WndProc ->
   DefWindowProcW/CallWindowProcW during WM_(N)CCREATE, all W imports
   left to MSLU. With class registration proven good (SDL_Init ok),
   the create is the last unproven link; own it on era A-APIs so the
   create path needs no MSLU windowing support at all. On failure the
   REAL error code goes to levari_w98.log (SDL's own FormatMessage
   path proved useless there — trailing "b" was uninitialized-buffer
   garbage from a failed MSLU FormatMessageW). ---- */

/* text-message marshalling shared by the three below: only the
   string-carrying window messages need it (levari's titles are
   short; 4 KiB bounds the conversion, oversize passes unconverted
   — a truncation, never an overrun). */
static int lv98_wmsg_istext(UINT msg)
{
    return msg == WM_SETTEXT || msg == WM_GETTEXT ||
           msg == WM_GETTEXTLENGTH;
}

static const char *lv98_w2a(LPCWSTR w, char *buf, int cap)
{
    buf[0] = 0;
    if (!w)
        return NULL;
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, buf, cap, NULL, NULL)) {
        buf[0] = 0;
        return NULL;
    }
    return buf;
}

/* ---- RegisterClassExW (batch 11) ----
   Hardware (unofficial 98SE SP-era box, os build 2734, unicows=0):
   SDL_Init passes on its W-side class registration, but our era
   CreateWindowExA cannot see that class, and CreateWindowExW itself
   failed too - last-error 203 (ERROR_ENVVAR_NOT_FOUND) is nonsense
   for a window call: stale residue, because the W stubs fail WITHOUT
   SetLastError. Both worlds are half-broken; we make ours fully A:
   register the class in the era's native namespace so the whole SDL
   window chain (A class, A create, A def-proc) is self-consistent
   and never consults the box's W layer. */
ATOM WINAPI lv98_RegisterClassExW(const WNDCLASSEXW *wcx)
{
    static ATOM(WINAPI *real)(const WNDCLASSEXW *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (ATOM(WINAPI *)(const WNDCLASSEXW *))(void *)
            lv98_real("user32.dll", "RegisterClassExW");
    }
    if (real)
        return real(wcx);
    {
        WNDCLASSEXA a;
        char clsA[256], menuA[256];
        ATOM atom;
        memset(&a, 0, sizeof a);
        a.cbSize = sizeof a;
        a.style = wcx->style;
        a.lpfnWndProc = wcx->lpfnWndProc;
        a.cbClsExtra = wcx->cbClsExtra;
        a.cbWndExtra = wcx->cbWndExtra;
        a.hInstance = wcx->hInstance;
        a.hIcon = wcx->hIcon;
        a.hCursor = wcx->hCursor;
        a.hbrBackground = wcx->hbrBackground;
        a.lpszMenuName =
            (wcx->lpszMenuName && !IS_INTRESOURCE(wcx->lpszMenuName))
                ? (lv98_w2a(wcx->lpszMenuName, menuA, sizeof menuA) ? menuA
                                                                    : NULL)
                : (LPCSTR)wcx->lpszMenuName;
        a.lpszClassName =
            (wcx->lpszClassName && !IS_INTRESOURCE(wcx->lpszClassName))
                ? (lv98_w2a(wcx->lpszClassName, clsA, sizeof clsA) ? clsA
                                                                   : NULL)
                : (LPCSTR)wcx->lpszClassName;
        /* wcx->lpszIcon is W-only (NT; SDL passes NULL): no era slot. */
        a.hIconSm = wcx->hIconSm;
        atom = RegisterClassExA(&a);
        lv98_tracef("RegisterClassExA cls=[%s] atom=%u err=%lu",
                    a.lpszClassName ? a.lpszClassName : "NULL",
                    (unsigned)atom, atom ? 0UL : GetLastError());
        return atom;
    }
}

HWND WINAPI lv98_CreateWindowExW(DWORD exStyle, LPCWSTR clsW, LPCWSTR nameW,
                                 DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst,
                                 LPVOID param)
{
    static HWND(WINAPI *real)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int,
                              int, HWND, HMENU, HINSTANCE, LPVOID);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HWND(WINAPI *)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int,
                               int, HWND, HMENU, HINSTANCE, LPVOID))(
                   void *)lv98_real("user32.dll", "CreateWindowExW");
    }
    if (real)
        return real(exStyle, clsW, nameW, style, x, y, w, h, parent, menu,
                    inst, param);
    /* era: CreateWindowExA (Win95 stock). Class and title are the
       only string params; lpParam (SDL_Window*) is passed through —
       SDL's WndProc reads it from the CREATESTRUCT untyped. */
    {
        HWND hwnd;
        char clsA[256], nameA[4096];
        const char *pcls = NULL, *pname = NULL;
        if (clsW) {
            /* atom-encoded class names (MAKEINTATOM) pass through */
            if (IS_INTRESOURCE(clsW))
                pcls = (const char *)clsW;
            else if (!(pcls = lv98_w2a(clsW, clsA, sizeof clsA)) || !*pcls)
                pcls = (const char *)clsW; /* shouldn't happen; let it fail visibly */
        }
        if (nameW)
            pname = lv98_w2a(nameW, nameA, sizeof nameA) ? nameA : "";
        hwnd = CreateWindowExA(exStyle, pcls, pname, style, x, y, w, h,
                               parent, menu, inst, param);
        if (!hwnd)
            lv98_tracef("CreateWindowExA FAIL err=%lu style=%08lx ex=%08lx "
                        "cls=[%s] name=[%s] g=%dx%d+%d+%d par=%p",
                        GetLastError(), style, exStyle,
                        pcls ? (IS_INTRESOURCE(pcls) ? "#atom" : pcls) : "NULL",
                        pname ? pname : "NULL", w, h, x, y, (void *)parent);
        return hwnd;
    }
}

LRESULT WINAPI lv98_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp,
                                   LPARAM lp)
{
    static LRESULT(WINAPI *real)(HWND, UINT, WPARAM, LPARAM);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LRESULT(WINAPI *)(HWND, UINT, WPARAM, LPARAM))
                   lv98_real("user32.dll", "DefWindowProcW");
    }
    if (real)
        return real(hwnd, msg, wp, lp);
    if (!lv98_wmsg_istext(msg))
        return DefWindowProcA(hwnd, msg, wp, lp);
    {
        char a[4096];
        if (msg == WM_SETTEXT) {
            if (!lv98_w2a((LPCWSTR)lp, a, sizeof a))
                return DefWindowProcA(hwnd, msg, wp, 0);
            return DefWindowProcA(hwnd, msg, wp, (LPARAM)a);
        }
        if (msg == WM_GETTEXT) {
            LRESULT n = DefWindowProcA(hwnd, WM_GETTEXTLENGTH, 0, 0);
            char tmp[4096];
            if (n < 0)
                n = 0;
            if (n >= (LONG)sizeof tmp)
                n = (LONG)sizeof tmp - 1;
            if (!n) {
                ((LPWSTR)lp)[0] = 0;
                return 0;
            }
            n = DefWindowProcA(hwnd, WM_GETTEXT, n + 1, (LPARAM)tmp);
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, (LPWSTR)lp, n + 1);
            return n;
        }
        /* WM_GETTEXTLENGTH: width == length == count */
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

LRESULT WINAPI lv98_CallWindowProcW(void *orig, HWND hwnd, UINT msg,
                                    WPARAM wp, LPARAM lp)
{
    static LRESULT(WINAPI *real)(void *, HWND, UINT, WPARAM, LPARAM);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LRESULT(WINAPI *)(void *, HWND, UINT, WPARAM, LPARAM))
                   lv98_real("user32.dll", "CallWindowProcW");
    }
    if (real)
        return real(orig, hwnd, msg, wp, lp);
    /* The callee is always SDL's OWN WndProc in our process (static
       SDL): it handles wide text messages directly, so do not marshal
       — forward raw to the A entry, same pointer path as W on NT. */
    return CallWindowProcA((WNDPROC)orig, hwnd, msg, wp, lp);
}

/* ---- window text + property + vk trio (batch 12) ----
   Hardware ladder proved creation itself now works (no
   CreateWindowExA FAIL line; wmain died downstream): WIN_RegisterData
   fails at SetPropW (the box's W half-world), and WndProc does
   GetPropW("SDL_WindowData") on EVERY message — without these the
   window can never live. Title APIs ride the same trio pattern (the
   foreseen mojibake follow-up), MapVirtualKeyW feeds SDL's keyboard
   map (keyboard init; W is a dead stub here). All era twins are
   95-stock; ASCII round-trip makes length conversions exact. */
BOOL WINAPI lv98_SetPropW(HWND hwnd, LPCWSTR name, HANDLE data)
{
    static BOOL(WINAPI *real)(HWND, LPCWSTR, HANDLE);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HWND, LPCWSTR, HANDLE))(void *)
            lv98_real("user32.dll", "SetPropW");
    }
    if (real)
        return real(hwnd, name, data);
    {
        char a[256];
        if (name && !IS_INTRESOURCE(name) &&
            !lv98_w2a(name, a, sizeof a))
            return FALSE;
        return SetPropA(hwnd, name && IS_INTRESOURCE(name) ? (LPCSTR)name
                                                            : a,
                        data);
    }
}

HANDLE WINAPI lv98_GetPropW(HWND hwnd, LPCWSTR name)
{
    static HANDLE(WINAPI *real)(HWND, LPCWSTR);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HANDLE(WINAPI *)(HWND, LPCWSTR))(void *)
            lv98_real("user32.dll", "GetPropW");
    }
    if (real)
        return real(hwnd, name);
    {
        char a[256];
        if (name && !IS_INTRESOURCE(name) &&
            !lv98_w2a(name, a, sizeof a))
            return NULL;
        return GetPropA(hwnd,
                        name && IS_INTRESOURCE(name) ? (LPCSTR)name : a);
    }
}

HANDLE WINAPI lv98_RemovePropW(HWND hwnd, LPCWSTR name)
{
    static HANDLE(WINAPI *real)(HWND, LPCWSTR);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HANDLE(WINAPI *)(HWND, LPCWSTR))(void *)
            lv98_real("user32.dll", "RemovePropW");
    }
    if (real)
        return real(hwnd, name);
    {
        char a[256];
        if (name && !IS_INTRESOURCE(name) &&
            !lv98_w2a(name, a, sizeof a))
            return NULL;
        return RemovePropA(hwnd,
                           name && IS_INTRESOURCE(name) ? (LPCSTR)name : a);
    }
}

BOOL WINAPI lv98_SetWindowTextW(HWND hwnd, LPCWSTR w)
{
    static BOOL(WINAPI *real)(HWND, LPCWSTR);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HWND, LPCWSTR))(void *)
            lv98_real("user32.dll", "SetWindowTextW");
    }
    if (real)
        return real(hwnd, w);
    {
        char a[4096];
        static char lastA[4096];
        if (w && !lv98_w2a(w, a, sizeof a))
            return FALSE;
        if (strcmp(lastA, w ? a : "") != 0) {
            /* title census (hardware batch 12: taskbar showed "?????" -
               catch what the conversion actually produced) */
            lstrcpynA(lastA, w ? a : "", sizeof lastA);
            lv98_tracef("title=[%s]", lastA);
        }
        return SetWindowTextA(hwnd, w ? a : "");
    }
}

int WINAPI lv98_GetWindowTextW(HWND hwnd, LPWSTR buf, int maxlen)
{
    static int(WINAPI *real)(HWND, LPWSTR, int);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (int(WINAPI *)(HWND, LPWSTR, int))(void *)
            lv98_real("user32.dll", "GetWindowTextW");
    }
    if (real)
        return real(hwnd, buf, maxlen);
    {
        char a[4096];
        int n;
        if (!buf || maxlen <= 0)
            return 0;
        n = GetWindowTextA(hwnd, a, (int)sizeof a);
        if (n < 0)
            return n;
        if (MultiByteToWideChar(CP_ACP, 0, a, -1, buf, maxlen) <= 0)
            return 0;
        return n;
    }
}

int WINAPI lv98_GetWindowTextLengthW(HWND hwnd)
{
    static int(WINAPI *real)(HWND);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (int(WINAPI *)(HWND))(void *)
            lv98_real("user32.dll", "GetWindowTextLengthW");
    }
    if (real)
        return real(hwnd);
    /* ASCII-era titles: character count == ANSI length */
    return GetWindowTextLengthA(hwnd);
}

UINT WINAPI lv98_MapVirtualKeyW(UINT code, UINT map)
{
    static UINT(WINAPI *real)(UINT, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (UINT(WINAPI *)(UINT, UINT))(void *)
            lv98_real("user32.dll", "MapVirtualKeyW");
    }
    if (real)
        return real(code, map);
    return MapVirtualKeyA(code, map);
}

/* ---- cursor + window-long pair (batch 14: the blind-window batch) ----
   Decoded hardware log (batch 13): pump alive (peek advancing) yet
   lastevt frozen at 512 and the system cursor VANISHES over our
   windows (hardware report 2026-11-17). Two W stubs explain both:
   WIN_CreateDefaultCursor's LoadCursor(NULL, IDC_ARROW) compiles to
   LoadCursorW -> NULL here, so SDL_cursor stays NULL and SDL's own
   WM_SETCURSOR branch runs SetCursor(SDL_cursor) = invisible over the
   client; and SetupWindowData binds SDL's WndProc with
   SetWindowLongPtr(GWL_WNDPROC) = SetWindowLongW: if that stub fails
   silently, DefWindowProcA eats every mouse/key message (WndProc
   never installed) = deaf to interaction, while paint (SDL-side)
   keeps working. GetWindowLongW is the immediate probe beside it
   (data->wndproc). Era twins 95-stock. */
HCURSOR WINAPI lv98_LoadCursorW(HINSTANCE inst, LPCWSTR name)
{
    static HCURSOR(WINAPI *real)(HINSTANCE, LPCWSTR);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HCURSOR(WINAPI *)(HINSTANCE, LPCWSTR))(void *)
            lv98_real("user32.dll", "LoadCursorW");
    }
    if (real)
        return real(inst, name);
    {
        char a[256];
        const char *pname =
            (name && !IS_INTRESOURCE(name))
                ? (lv98_w2a(name, a, sizeof a) ? a : "")
                : (const char *)name;
        HCURSOR hc = LoadCursorA(inst, pname);
        lv98_tracef("loadcur name=%s hc=%p",
                    (pname && !IS_INTRESOURCE((void *)pname)) ? pname
                                                              : "#atom",
                    (void *)hc);
        return hc;
    }
}

LONG WINAPI lv98_GetWindowLongW(HWND hwnd, int idx)
{
    static LONG(WINAPI *real)(HWND, int);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LONG(WINAPI *)(HWND, int))(void *)
            lv98_real("user32.dll", "GetWindowLongW");
    }
    if (real)
        return real(hwnd, idx);
    return GetWindowLongA(hwnd, idx);
}

LONG WINAPI lv98_SetWindowLongW(HWND hwnd, int idx, LONG value)
{
    static LONG(WINAPI *real)(HWND, int, LONG);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LONG(WINAPI *)(HWND, int, LONG))(void *)
            lv98_real("user32.dll", "SetWindowLongW");
    }
    if (real)
        return real(hwnd, idx, value);
    {
        LONG old = SetWindowLongA(hwnd, idx, value);
        if (idx == GWL_WNDPROC)
            lv98_tracef("setwndproc old=%p new=%p", (void *)old,
                        (void *)value);
        return old;
    }
}

/* ---- winmm audio trio (batch 14; the dossier's named audio frontier) ----
   The winmm backend's first words are waveOut/InGetDevCaps(W) (device
   enumeration) and waveOutGetErrorText(W) (every winmm failure
   diagnostic via SetMMerror - the mojibake-error family); a dead W
   stub here = the backend never finds a device = silent null
   fallback. Era twins 95-stock; only szPname (MAXPNAMELEN) is string
   (ASCII names make the conversion exact); call with cb < struct
   refused rather than fill short. */
MMRESULT WINAPI lv98_waveOutGetDevCapsW(UINT dev, WAVEOUTCAPSW *woc, UINT cb)
{
    static MMRESULT(WINAPI *real)(UINT, WAVEOUTCAPSW *, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (MMRESULT(WINAPI *)(UINT, WAVEOUTCAPSW *, UINT))(void *)
            lv98_real("winmm.dll", "waveOutGetDevCapsW");
    }
    if (real)
        return real(dev, woc, cb);
    {
        WAVEOUTCAPSA a;
        MMRESULT r;
        if (cb < sizeof *woc)
            return MMSYSERR_INVALPARAM;
        r = waveOutGetDevCapsA(dev, &a, sizeof a);
        if (r == MMSYSERR_NOERROR) {
            memcpy(woc, &a, offsetof(WAVEOUTCAPSA, szPname));
            MultiByteToWideChar(CP_ACP, 0, a.szPname, -1, woc->szPname,
                                MAXPNAMELEN);
        }
        lv98_tracef("wavecaps odev=%u r=%d", dev, (int)r);
        return r;
    }
}

MMRESULT WINAPI lv98_waveInGetDevCapsW(UINT dev, WAVEINCAPSW *wic, UINT cb)
{
    static MMRESULT(WINAPI *real)(UINT, WAVEINCAPSW *, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (MMRESULT(WINAPI *)(UINT, WAVEINCAPSW *, UINT))(void *)
            lv98_real("winmm.dll", "waveInGetDevCapsW");
    }
    if (real)
        return real(dev, wic, cb);
    {
        WAVEINCAPSA a;
        MMRESULT r;
        if (cb < sizeof *wic)
            return MMSYSERR_INVALPARAM;
        r = waveInGetDevCapsA(dev, &a, sizeof a);
        if (r == MMSYSERR_NOERROR) {
            memcpy(wic, &a, offsetof(WAVEINCAPSA, szPname));
            MultiByteToWideChar(CP_ACP, 0, a.szPname, -1, wic->szPname,
                                MAXPNAMELEN);
        }
        lv98_tracef("wavecaps idev=%u r=%d", dev, (int)r);
        return r;
    }
}

MMRESULT WINAPI lv98_waveOutGetErrorTextW(MMRESULT err, LPWSTR buf, UINT cch)
{
    static MMRESULT(WINAPI *real)(MMRESULT, LPWSTR, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (MMRESULT(WINAPI *)(MMRESULT, LPWSTR, UINT))(void *)
            lv98_real("winmm.dll", "waveOutGetErrorTextW");
    }
    if (real)
        return real(err, buf, cch);
    {
        char a[MAXERRORLENGTH];
        MMRESULT r = waveOutGetErrorTextA(err, a, sizeof a);
        if (r == MMSYSERR_NOERROR)
            MultiByteToWideChar(CP_ACP, 0, a, -1, buf, (int)cch);
        return r;
    }
}

/* ---- PeekMessageW (batch 13 pump liveness probe) ----
   SDL_PumpEvents (reached from SDL_WaitEventTimeout EVERY loop
   iteration) pumps the queue via PeekMessageW; WIN_PumpEvents hung
   or never-reached shows as stopped probe lines while the app thread
   itself may still heartbeat-log. Era twin = PeekMessageA (dispatch
   semantics identical for our all-A window chain). First 3 + every
   250th call log keeps the log human-scale (~1 line/second live). */
BOOL WINAPI lv98_PeekMessageW(LPMSG msg, HWND hwnd, UINT first, UINT last,
                              UINT flags)
{
    static BOOL(WINAPI *real)(LPMSG, HWND, UINT, UINT, UINT);
    static int resolved;
    BOOL rc;
    static unsigned peek_n;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(LPMSG, HWND, UINT, UINT, UINT))(void *)
            lv98_real("user32.dll", "PeekMessageW");
    }
    if (real)
        return real(msg, hwnd, first, last, flags);
    rc = PeekMessageA(msg, hwnd, first, last, flags);
    peek_n++;
    if (rc && msg) {
        /* batch 15: first-arrival census per Win32 message ID the
           pump actually dequeues — answers where a click dies: no
           peekmsg id=513 = the OS never sent WM_LBUTTONDOWN to this
           queue; present but no evt/click echo = SDL-side. 24
           distinct cap. Batch 16: +hwnd and per-click lines (12 cap)
           with coords — batch 15 proved 512/513/514/256/257 all
           arrive; which HWND carries them decides dispatch guilt. */
        static UINT wseen[24];
        static int wseen_n;
        static int clkq_n;
        int i;
        if (clkq_n < 12 &&
            (msg->message == WM_LBUTTONDOWN || msg->message == WM_LBUTTONUP ||
             msg->message == WM_KEYDOWN || msg->message == WM_KEYUP)) {
            clkq_n++;
            lv98_tracef("clkq id=%u hwnd=%p x=%d y=%d", msg->message,
                        (void *)msg->hwnd, (int)(short)LOWORD(msg->lParam),
                        (int)(short)HIWORD(msg->lParam));
        }
        for (i = 0; i < wseen_n; i++)
            if (wseen[i] == msg->message)
                break;
        if (i == wseen_n) {
            if (wseen_n < (int)(sizeof wseen)) {
                wseen[wseen_n++] = msg->message;
                lv98_tracef("peekmsg first id=%u hwnd=%p", msg->message,
                            (void *)msg->hwnd);
            }
        }
    }
    if (peek_n <= 3 || (peek_n % 250) == 0)
        lv98_tracef("peek n=%u rc=%d", peek_n, (int)rc);
    return rc;
}

/* ---- DispatchMessageW + GetMessageExtraInfo (batch 16: click war) ----
   Batch 15 PROVED the OS queues WM_MOUSEMOVE/LBUTTONDOWN/UP and
   KEYDOWN/UP, yet SDL translates motion only. Two suspects remain on
   the SDL side, both W/extra-info layer:
   (1) SDL pumps via DispatchMessageW - an un-interposed call into the
       SP's half-world (the only pump hop we never owned). Era twin is
       the A dispatch; for our all-A window chain semantics are
       identical (wndproc pointer is namespace-blind). Logged first
       arrival per ID: proves WHICH hwnd SDL dispatches to.
   (2) The button cases tag every event by GetMessageExtraInfo()'s
       touch signature: if a driver returns the FROMTOUCH signature
       (0xFF515700 bit pattern - some era MS-mouse drivers use -1
       high-word sentinels!), mouseID becomes SDL_TOUCH_MOUSEID and
       SDL_PrivateSendMouseButton DROPS the event (touch_mouse_events
       is off), while motion has no such filter - exactly the
       observed motion-live/buttons-dead split. Era has no touch:
       mask to 0 (non-touch, per input docs' pre-Win8 contract). */
LRESULT WINAPI lv98_DispatchMessageW(const MSG *msg)
{
    static LRESULT(WINAPI *real)(const MSG *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (LRESULT(WINAPI *)(const MSG *))(void *)
            lv98_real("user32.dll", "DispatchMessageW");
    }
    if (real)
        return real(msg);
    if (msg) {
        static UINT dseen[24];
        static int dseen_n;
        int i;
        for (i = 0; i < dseen_n; i++)
            if (dseen[i] == msg->message)
                break;
        if (i == dseen_n && dseen_n < (int)(sizeof dseen)) {
            dseen[dseen_n++] = msg->message;
            lv98_tracef("disp first id=%u hwnd=%p", msg->message,
                        (void *)msg->hwnd);
        }
    }
    return DispatchMessageA(msg);
}

DWORD WINAPI lv98_GetMessageExtraInfo(void)
{
    /* Era floor: no touch stack exists. SDL reads this value only to
       test the MOUSEEVENTF_FROMTOUCH signature (0xFF515700 mask), and
       a driver sentinel matching that mask silently reclassifies every
       click as a touch event — which SDL then DROPS (motion has no
       such filter: the exact motion-live/buttons-dead split from the
       batch 15 log). Return 0 = "no extra info", the documented
       default, unconditionally; one-shot proof line. */
    static int logged;
    if (!logged) {
        logged = 1;
        lv98_tracef("extinfo masked->0");
    }
    return 0;
}

/* ---- SetWindowRgn (SDL_windowsshape only; Winamp-region-era API) ----
   levari never calls SDL_SetWindowShape. Forward where present; era
   FALSE is ignored by the sole caller — worst case a shape request
   silently no-ops. */
BOOL WINAPI lv98_SetWindowRgn(HWND hwnd, HRGN rgn, BOOL redraw)
{
    static BOOL(WINAPI *real)(HWND, HRGN, BOOL);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(HWND, HRGN, BOOL))
                   lv98_real("user32.dll", "SetWindowRgn");
    }
    if (real)
        return real(hwnd, rgn, redraw);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ================= batch 5 (hardware abort #3: GlobalMemoryStatusEx) ==
   Same wave as GetFileSizeEx/SetFilePointerEx: the Win2000 "Ex" family,
   which the old MSDN explicitly marked NT-only with "use the plain API
   on 9x" — my "98-delivered" memory had it wrong for the whole family.
   Interposed for the same reason as the rest: the forward makes them
   inert where they ship, and NONE of these four code paths is ever
   entered by levari (no SDL_GetSystemRAM/GetPowerInfo/GetPrefPath
   calls in src, mouse-track's return value is ignored) — this batch
   exists purely to make the import table loadable, so every fallback
   is written era-truthful anyway. */

/* ---- GlobalMemoryStatusEx (Win2000+; SDL_cpuinfo) ----
   The documented 9x answer is GlobalMemoryStatus; widening is exact
   (9x addresses are 32-bit by construction). MEMORYSTATUSEX is spelled
   out here because the era headers hide it below NT5 guards. */
typedef struct {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
} lv98_memstatusex;

BOOL WINAPI lv98_GlobalMemoryStatusEx(lv98_memstatusex *msx)
{
    static BOOL(WINAPI *real)(lv98_memstatusex *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(lv98_memstatusex *))
                   lv98_real("kernel32.dll", "GlobalMemoryStatusEx");
    }
    if (real)
        return real(msx);
    {
        MEMORYSTATUS ms;
        ZeroMemory(&ms, sizeof ms);
        ms.dwLength = sizeof ms;
        GlobalMemoryStatus(&ms);
        msx->dwLength = sizeof *msx;
        msx->dwMemoryLoad = ms.dwMemoryLoad;
        msx->ullTotalPhys = ms.dwTotalPhys;
        msx->ullAvailPhys = ms.dwAvailPhys;
        msx->ullTotalPageFile = ms.dwTotalPageFile;
        msx->ullAvailPageFile = ms.dwAvailPageFile;
        msx->ullTotalVirtual = ms.dwTotalVirtual;
        msx->ullAvailVirtual = ms.dwAvailVirtual;
        msx->ullAvailExtendedVirtual = 0;
        return TRUE;
    }
}

/* ---- GetSystemPowerStatus (SDL_syspower; era-desktop fallback) ----
   Plugged-in, no-battery is the honest desktop-98 state; SDL's power
   query then reports NO_BATTERY instead of failing. Forward makes the
   interposition inert where the API ships (it is old — 95 docs claim
   it, but this family's claims are exactly what has burned twice). */
BOOL WINAPI lv98_GetSystemPowerStatus(LPSYSTEM_POWER_STATUS sps)
{
    static BOOL(WINAPI *real)(LPSYSTEM_POWER_STATUS);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(LPSYSTEM_POWER_STATUS))
                   lv98_real("kernel32.dll", "GetSystemPowerStatus");
    }
    if (real)
        return real(sps);
    if (!sps)
        return FALSE;
    sps->ACLineStatus = 1;   /* online */
    sps->BatteryFlag = 8;    /* no system battery */
    sps->BatteryLifePercent = 255;
    sps->SystemStatusFlag = 0; /* mingw name for the field MSDN calls Reserved1 */
    sps->BatteryLifeTime = (DWORD)-1;
    sps->BatteryFullLifeTime = (DWORD)-1;
    return TRUE;
}

/* ---- TrackMouseEvent (98-era; SDL_windowswindow mouse tracking) ----
   The sole caller ignores the return value; era fallback reports
   failure, which costs only mouse-leave tracking. */
typedef struct {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
} lv98_tme; /* layout mirrors TRACKMOUSEEVENT */

BOOL WINAPI lv98_TrackMouseEvent(lv98_tme *tme)
{
    static BOOL(WINAPI *real)(lv98_tme *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (BOOL(WINAPI *)(lv98_tme *))
                   lv98_real("user32.dll", "TrackMouseEvent");
    }
    if (real)
        return real(tme);
    (void)tme;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ---- SHGetFolderPathW (98-era; SDL_sysfilesystem GetPrefPath) ----
   levari never calls GetPrefPath (it rides XDG/registry), so only the
   loader cares — but the fallback is still the 95/98 truth: APPDATA
   was %WINDIR%\Application Data on those shells (CSIDL_FLAG_CREATE
   honored), every other folder reports failure. */
#define LV98_CSIDL_APPDATA 26
#define LV98_CSIDL_FLAG_CREATE 0x8000

HRESULT WINAPI lv98_SHGetFolderPathW(HWND owner, int csidl, HANDLE token,
                                     DWORD flags, LPWSTR path)
{
    static HRESULT(WINAPI *real)(HWND, int, HANDLE, DWORD, LPWSTR);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (HRESULT(WINAPI *)(HWND, int, HANDLE, DWORD, LPWSTR))
                   lv98_real("shell32.dll", "SHGetFolderPathW");
    }
    if (real)
        return real(owner, csidl, token, flags, path);
    (void)owner;
    (void)token;
    if (!path || (csidl & 0x7FFF) != LV98_CSIDL_APPDATA)
        return E_FAIL;
    {
        CHAR base[MAX_PATH] = "";
        if (!GetWindowsDirectoryA(base, sizeof base))
            return E_FAIL;
        lstrcatA(base, "\\Application Data");
        if (flags & LV98_CSIDL_FLAG_CREATE)
            CreateDirectoryA(base, NULL);
        if (MultiByteToWideChar(CP_ACP, 0, base, -1, path, MAX_PATH) <= 0)
            return E_FAIL;
        return S_OK;
    }
}

/* ---- ExtractIconExW (SDL 2.0.8 WIN_RegisterClass, SDL_windowsevents.c
   :1166 — the exe's first icon as the SDL_app class default, only the
   no-INTRESOURCE-hint branch) ----
   Hardware report 2026-11-19: a field 9x box's shell32 predates this
   export (our 98-SE reference box resolves it — the 4.72 export set is
   NOT universal: downgraded shell32 DLL-hell or a 95/97-class shell
   behind a "98" label), and the loader aborts on the first missing
   export, so the whole app dies at load. Interposing is immunity
   across shell vintages, and the era fallback replicates the exact
   behavior our own box has always had: the call there is a dead W
   stub, the outputs stay NULL, the class registers iconless. NT-class
   forwards to the real export (lv98_real policy). Zero new imports:
   the shim deletes the name; the gate ALLOW excises it, so any
   reappearance fails the gate. */
UINT WINAPI lv98_ExtractIconExW(LPCWSTR lpszFile, int nIconIndex,
                                HICON *phiconLarge, HICON *phiconSmall,
                                UINT nIcons)
{
    static UINT(WINAPI *real)(LPCWSTR, int, HICON *, HICON *, UINT);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        real = (UINT(WINAPI *)(LPCWSTR, int, HICON *, HICON *, UINT))
                   lv98_real("shell32.dll", "ExtractIconExW");
    }
    if (real)
        return real(lpszFile, nIconIndex, phiconLarge, phiconSmall, nIcons);
    (void)lpszFile;
    (void)nIconIndex;
    (void)nIcons;
    if (phiconLarge)
        *phiconLarge = NULL;
    if (phiconSmall)
        *phiconSmall = NULL;
    return 0;
}

/* thunk aliases: what the import tables actually name (i386 stdcall
   decoration — including the ole32 PropVariantClear thunk, which the
   linker map renders one-underscore-shorter than its raw COFF name
   __imp__PropVariantClear@4). Emitted as .data words holding the
   replacement's address — same shape as a real import thunk, so the
   relocation is a normal data reloc (an `=` absolute would bake
   base-dependent addresses). */
__asm__(".data\n"
        ".globl __imp__TryEnterCriticalSection@4\n"
        "__imp__TryEnterCriticalSection@4: .long _lv98_TryEnterCriticalSection@4\n"
        ".globl __imp__InitializeCriticalSectionAndSpinCount@8\n"
        "__imp__InitializeCriticalSectionAndSpinCount@8: "
        ".long _lv98_InitializeCriticalSectionAndSpinCount@8\n"
        ".globl __imp__RegisterRawInputDevices@12\n"
        "__imp__RegisterRawInputDevices@12: .long _lv98_RegisterRawInputDevices@12\n"
        ".globl __imp__AttachConsole@4\n"
        "__imp__AttachConsole@4: .long _lv98_AttachConsole@4\n"
        ".globl __imp__VerSetConditionMask@16\n"
        "__imp__VerSetConditionMask@16: .long _lv98_VerSetConditionMask@16\n"
        ".globl __imp__VerifyVersionInfoW@16\n"
        "__imp__VerifyVersionInfoW@16: .long _lv98_VerifyVersionInfoW@16\n"
        ".globl __imp__SetFilePointerEx@20\n"
        "__imp__SetFilePointerEx@20: .long _lv98_SetFilePointerEx@20\n"
        ".globl __imp__WaitForSingleObjectEx@12\n"
        "__imp__WaitForSingleObjectEx@12: .long _lv98_WaitForSingleObjectEx@12\n"
        ".globl __imp__GetThreadContext@8\n"
        "__imp__GetThreadContext@8: .long _lv98_GetThreadContext@8\n"
        ".globl __imp__SetThreadContext@8\n"
        "__imp__SetThreadContext@8: .long _lv98_SetThreadContext@8\n"
        ".globl __imp__ToUnicode@24\n"
        "__imp__ToUnicode@24: .long _lv98_ToUnicode@24\n"
        ".globl __imp__GetClipboardSequenceNumber@0\n"
        "__imp__GetClipboardSequenceNumber@0: "
        ".long _lv98_GetClipboardSequenceNumber@0\n"
        ".globl __imp__SetLayeredWindowAttributes@16\n"
        "__imp__SetLayeredWindowAttributes@16: "
        ".long _lv98_SetLayeredWindowAttributes@16\n"
        ".globl __imp__RegisterDeviceNotificationW@12\n"
        "__imp__RegisterDeviceNotificationW@12: "
        ".long _lv98_RegisterDeviceNotificationW@12\n"
        ".globl __imp__UnregisterDeviceNotification@4\n"
        "__imp__UnregisterDeviceNotification@4: "
        ".long _lv98_UnregisterDeviceNotification@4\n"
        ".globl __imp__GetRawInputData@20\n"
        "__imp__GetRawInputData@20: .long _lv98_GetRawInputData@20\n"
        ".globl __imp__GetRawInputDeviceList@12\n"
        "__imp__GetRawInputDeviceList@12: .long _lv98_GetRawInputDeviceList@12\n"
        ".globl __imp__GetRawInputDeviceInfoA@16\n"
        "__imp__GetRawInputDeviceInfoA@16: .long _lv98_GetRawInputDeviceInfoA@16\n"
        ".globl __imp__PropVariantClear@4\n"
        "__imp__PropVariantClear@4: .long _lv98_PropVariantClear@4\n"
        ".globl __imp__GetFileSizeEx@8\n"
        "__imp__GetFileSizeEx@8: .long _lv98_GetFileSizeEx@8\n"
        ".globl __imp__GetCurrentProcessId@0\n"
        "__imp__GetCurrentProcessId@0: .long _lv98_GetCurrentProcessId@0\n"
        ".globl __imp__GetHandleInformation@8\n"
        "__imp__GetHandleInformation@8: .long _lv98_GetHandleInformation@8\n"
        ".globl __imp__IsDebuggerPresent@0\n"
        "__imp__IsDebuggerPresent@0: .long _lv98_IsDebuggerPresent@0\n"
        ".globl __imp__IsDBCSLeadByteEx@8\n"
        "__imp__IsDBCSLeadByteEx@8: .long _lv98_IsDBCSLeadByteEx@8\n"
        ".globl __imp__PostThreadMessageW@16\n"
        "__imp__PostThreadMessageW@16: .long _lv98_PostThreadMessageW@16\n"
        ".globl __imp__EnumDisplayDevicesW@16\n"
        "__imp__EnumDisplayDevicesW@16: .long _lv98_EnumDisplayDevicesW@16\n"
        ".globl __imp__EnumDisplaySettingsW@12\n"
        "__imp__EnumDisplaySettingsW@12: .long _lv98_EnumDisplaySettingsW@12\n"
        ".globl __imp__GetMonitorInfoW@8\n"
        "__imp__GetMonitorInfoW@8: .long _lv98_GetMonitorInfoW@8\n"
        ".globl __imp__CreateWindowExW@48\n"
        "__imp__CreateWindowExW@48: .long _lv98_CreateWindowExW@48\n"
        ".globl __imp__RegisterClassExW@4\n"
        "__imp__RegisterClassExW@4: .long _lv98_RegisterClassExW@4\n"
        ".globl __imp__DefWindowProcW@16\n"
        "__imp__DefWindowProcW@16: .long _lv98_DefWindowProcW@16\n"
        ".globl __imp__CallWindowProcW@20\n"
        "__imp__CallWindowProcW@20: .long _lv98_CallWindowProcW@20\n"
        ".globl __imp__SetPropW@12\n"
        "__imp__SetPropW@12: .long _lv98_SetPropW@12\n"
        ".globl __imp__GetPropW@8\n"
        "__imp__GetPropW@8: .long _lv98_GetPropW@8\n"
        ".globl __imp__RemovePropW@8\n"
        "__imp__RemovePropW@8: .long _lv98_RemovePropW@8\n"
        ".globl __imp__SetWindowTextW@8\n"
        "__imp__SetWindowTextW@8: .long _lv98_SetWindowTextW@8\n"
        ".globl __imp__GetWindowTextW@12\n"
        "__imp__GetWindowTextW@12: .long _lv98_GetWindowTextW@12\n"
        ".globl __imp__GetWindowTextLengthW@4\n"
        "__imp__GetWindowTextLengthW@4: .long _lv98_GetWindowTextLengthW@4\n"
        ".globl __imp__MapVirtualKeyW@8\n"
        "__imp__MapVirtualKeyW@8: .long _lv98_MapVirtualKeyW@8\n"
        ".globl __imp__LoadCursorW@8\n"
        "__imp__LoadCursorW@8: .long _lv98_LoadCursorW@8\n"
        ".globl __imp__GetWindowLongW@8\n"
        "__imp__GetWindowLongW@8: .long _lv98_GetWindowLongW@8\n"
        ".globl __imp__SetWindowLongW@12\n"
        "__imp__SetWindowLongW@12: .long _lv98_SetWindowLongW@12\n"
        ".globl __imp__waveOutGetDevCapsW@12\n"
        "__imp__waveOutGetDevCapsW@12: .long _lv98_waveOutGetDevCapsW@12\n"
        ".globl __imp__waveInGetDevCapsW@12\n"
        "__imp__waveInGetDevCapsW@12: .long _lv98_waveInGetDevCapsW@12\n"
        ".globl __imp__waveOutGetErrorTextW@12\n"
        "__imp__waveOutGetErrorTextW@12: .long _lv98_waveOutGetErrorTextW@12\n"
        ".globl __imp__PeekMessageW@20\n"
        "__imp__PeekMessageW@20: .long _lv98_PeekMessageW@20\n"
        ".globl __imp__DispatchMessageW@4\n"
        "__imp__DispatchMessageW@4: .long _lv98_DispatchMessageW@4\n"
        ".globl __imp__GetMessageExtraInfo@0\n"
        "__imp__GetMessageExtraInfo@0: .long _lv98_GetMessageExtraInfo@0\n"
        ".globl __imp__SetWindowRgn@12\n"
        "__imp__SetWindowRgn@12: .long _lv98_SetWindowRgn@12\n"
        ".globl __imp__GlobalMemoryStatusEx@4\n"
        "__imp__GlobalMemoryStatusEx@4: .long _lv98_GlobalMemoryStatusEx@4\n"
        ".globl __imp__GetSystemPowerStatus@4\n"
        "__imp__GetSystemPowerStatus@4: .long _lv98_GetSystemPowerStatus@4\n"
        ".globl __imp__TrackMouseEvent@4\n"
        "__imp__TrackMouseEvent@4: .long _lv98_TrackMouseEvent@4\n"
        ".globl __imp__SHGetFolderPathW@20\n"
        "__imp__SHGetFolderPathW@20: .long _lv98_SHGetFolderPathW@20\n"
        ".globl __imp__ExtractIconExW@20\n"
        "__imp__ExtractIconExW@20: .long _lv98_ExtractIconExW@20\n");
