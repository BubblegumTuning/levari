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


/* thunk aliases: what the import tables actually name (i386 stdcall
   decoration). Emitted as .data words holding the replacement's address
   — same shape as a real import thunk, so the relocation is a normal
   data reloc (an `=` absolute would bake base-dependent addresses). */
__asm__(".data\n"
        ".globl __imp__TryEnterCriticalSection@4\n"
        "__imp__TryEnterCriticalSection@4: .long _lv98_TryEnterCriticalSection@4\n"
        ".globl __imp__InitializeCriticalSectionAndSpinCount@8\n"
        "__imp__InitializeCriticalSectionAndSpinCount@8: "
        ".long _lv98_InitializeCriticalSectionAndSpinCount@8\n"
        ".globl __imp__RegisterRawInputDevices@12\n"
        "__imp__RegisterRawInputDevices@12: .long _lv98_RegisterRawInputDevices@12\n"
        ".globl __imp__VerSetConditionMask@16\n"
        "__imp__VerSetConditionMask@16: .long _lv98_VerSetConditionMask@16\n"
        ".globl __imp__VerifyVersionInfoW@16\n"
        "__imp__VerifyVersionInfoW@16: .long _lv98_VerifyVersionInfoW@16\n");
