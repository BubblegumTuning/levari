#!/usr/bin/env python3
"""win98 release gate (Win98 branch).

Asserts the era artifact's floor the same way the legacy ISA gate
asserts the i586 one:

  1. import ALLOW-list: every imported (dll, name) must be in the
     adjudicated 98-SE-delivered table below. An allow-list replaces
     the old hand-written deny-list — the deny-list MISSED loader-
     killers twice (the daemon.c trio in the dossier era, AttachConsole
     on real hardware 2026-11-16), a full-table sweep then named 13
     more, and a hardware rerun caught a 14th (GetFileSizeEx) that a
     by-memory sweep had passed. Do NOT re-adjudicate names from
     memory or from modern MS Learn (its "Minimum supported client"
     is retro-raised — it claims CloseHandle "requires Windows 2000"):
     the forward-then-fallback interposition is the correct answer to
     any doubt. Every interposed name stays OUT of this table on
     purpose: a shimmed name reappearing in an import table means the
     interposition broke.
  2. PE is PE32 (not PE32+), machine Intel 80386
  3. subsystem Windows GUI, subsystem version 4.x (not 5.x/6.x floors)
  4. embedded version token (win GUI embeds "v"+version; LV_ARCH x86)
  5. ISA scan: zero %xmm/%mmx in the full disassembly. Hardware proved
     the test box is a Pentium II (no SSE): the Nov 11 era SDL prefix
     carried 3182 SSE insns and SIGILL'd at movss in early init. Root
     cause: SDL 2.0.8's configure probes -mmmx/-msse/-msse2 and, on
     pass, APPENDS them to CFLAGS for EVERY file — overriding any
     -mno-*. Guard: bootstrap passes --disable-{sse2,sse,mmx,amd3dnow}
     and the toolchain file pins -march=pentiumpro -mno-sse -mno-sse2.
     Both hold even on compilers whose defaults are already clean.
  6. MSLU: DllCharacteristics Unicode-Aware bit (0x2) set — era SDL
     is UNICODE-only and 9x *W stubs fail at RUNTIME; the flag is
     what makes the 98 loader use user-supplied unicows.dll.

usage: win98_gate.py <objdump> <exe> [version]
exit 0 = all gates green."""
import re
import subprocess
import sys

# Adjudicated era import table (ground-truth ILT walk of the verified
# build-win98 artifact, regenerated after each shim batch — batch 14
# 2026-11-17: LoadCursorW/Get-SetWindowLongW and the winmm string trio
# interposed, their Win95-stock A twins admitted (same ruling as every
# earlier A twin: 98-SE-delivered, imported since Win95). Batch 16:
# DispatchMessageW and GetMessageExtraInfo interposed and REMOVED
# (reappearance = interposition broke); DispatchMessageA admitted
# (Win95-stock, product of the dispatch replacement).)
# GetSystemPowerStatus/SetErrorMode investigated and 98-delivered; the
# affinity pair is Win95-era; ToAscii and IsDBCSLeadByte (added by shim
# fallbacks) are Win95-era; the ADVAPI32/GDI32/IMM32 blocks (imported
# before KERNEL32) have now twice PASSED a real 98 SE loader — W-stub
# exports included, empirically settled on hardware.
ALLOW = {
    'advapi32.dll': {
        'GetUserNameA', 'RegCloseKey', 'RegOpenKeyExW', 'RegQueryValueExW'
    },
    'gdi32.dll': {
        'BitBlt', 'ChoosePixelFormat', 'CombineRgn', 'CreateBitmap', 'CreateCompatibleBitmap', 'CreateCompatibleDC',
        'CreateDCW', 'CreateDIBSection', 'CreateFontIndirectW', 'CreateFontW', 'CreatePen', 'CreateRectRgn',
        'CreateSolidBrush', 'DeleteDC', 'DeleteObject', 'DescribePixelFormat', 'ExtTextOutW', 'GetDIBits',
        'GetDeviceCaps', 'GetDeviceGammaRamp', 'GetPixelFormat', 'GetTextExtentPoint32A', 'GetTextExtentPoint32W',
        'GetTextMetricsW', 'Rectangle', 'SelectObject', 'SetBkMode', 'SetDeviceGammaRamp', 'SetPixelFormat',
        'SetTextColor', 'SwapBuffers'
    },
    'imm32.dll': {
        'ImmAssociateContext', 'ImmGetCandidateListW', 'ImmGetCompositionStringW', 'ImmGetContext',
        'ImmGetIMEFileNameA', 'ImmNotifyIME', 'ImmReleaseContext', 'ImmSetCompositionStringW', 'ImmSetCompositionWindow'
    },
    'kernel32.dll': {
        'CancelIo', 'CloseHandle', 'CompareStringA', 'ConnectNamedPipe', 'CreateDirectoryA', 'CreateDirectoryW',
        'CreateEventA', 'CreateEventW', 'CreateFileA', 'CreateFileMappingA', 'CreateFileW', 'CreateNamedPipeA',
        'CreateSemaphoreA', 'CreateSemaphoreW', 'CreateThread', 'DeleteCriticalSection', 'DisconnectNamedPipe',
        'DuplicateHandle', 'EnterCriticalSection', 'ExitProcess', 'FlushFileBuffers', 'FormatMessageW', 'FreeLibrary',
        'GetCommandLineA', 'GetCurrentProcess', 'GetCurrentThread', 'GetCurrentThreadId', 'GetEnvironmentVariableA',
        'GetFileAttributesA', 'GetFileSize', 'GetFullPathNameA', 'GetLastError', 'GetLogicalDrives',
        'GetModuleFileNameA', 'GetModuleFileNameW', 'GetModuleHandleA', 'GetModuleHandleW', 'GetOverlappedResult',
        'GetProcAddress', 'GetProcessAffinityMask', 'GetSystemInfo', 'GetSystemTimeAsFileTime', 'GetThreadPriority',
        'GetTickCount', 'GetVersionExA', 'GetWindowsDirectoryA', 'GlobalAlloc', 'GlobalLock', 'GlobalMemoryStatus',
        'GlobalUnlock', 'InitializeCriticalSection', 'IsDBCSLeadByte', 'IsProcessorFeaturePresent',
        'LeaveCriticalSection', 'LoadLibraryA', 'LoadLibraryW', 'MapViewOfFile', 'MulDiv', 'MultiByteToWideChar',
        'OpenProcess', 'OutputDebugStringA', 'OutputDebugStringW', 'QueryPerformanceCounter',
        'QueryPerformanceFrequency', 'RaiseException', 'ReadFile', 'ReleaseSemaphore', 'ResetEvent', 'ResumeThread',
        'SetEnvironmentVariableA', 'SetErrorMode', 'SetEvent', 'SetFilePointer', 'SetLastError',
        'SetProcessAffinityMask', 'SetThreadPriority', 'SetUnhandledExceptionFilter', 'Sleep', 'SuspendThread',
        'TerminateProcess', 'TlsAlloc', 'TlsGetValue', 'TlsSetValue', 'UnmapViewOfFile', 'VirtualProtect',
        'VirtualQuery', 'WaitForMultipleObjects', 'WaitForSingleObject', 'WideCharToMultiByte', 'WriteFile',
        # batch 18 (win98_fs.c): the era file layer owns directory walk +
        # stat on these three 95-stock kernel32 A entry points (all are
        # Win32s/NT3.1-original, in 98's kernel32 since retail 95)
        'FindClose', 'FindFirstFileA', 'FindNextFileA',
        'lstrcatA', 'lstrcatW', 'lstrcpyA', 'lstrcpynA', 'lstrlenW'
    },
    'msvcrt.dll': {
        '__getmainargs', '__initenv', '__mb_cur_max', '__p__commode', '__p__fmode', '__set_app_type',
        '__setusermatherr', '_amsg_exit', '_beginthreadex', '_cexit', '_endthreadex', '_errno', '_filelengthi64',
        '_fileno', '_fullpath', '_getcwd', '_getpid', '_i64toa', '_initterm',
        '_iob', '_itoa', '_lock', '_ltoa', '_mkdir', '_onexit', '_setjmp3', '_strdup', '_stricmp',
        '_strlwr', '_strnicmp', '_strrev', '_strupr', '_ui64toa', '_ultoa', '_unlock', '_vsnprintf', '_wfopen',
        '_write', 'abort', 'acos', 'asin', 'atan', 'atof', 'atoi', 'bsearch', 'calloc', 'exit', 'fclose', 'feof',
        'ferror', 'fflush', 'fgetc', 'fgetpos', 'fgets', 'fopen', 'fprintf', 'fputc', 'fputs', 'fread', 'free',
        'freopen', 'fseek', 'fsetpos', 'ftell', 'fwrite', 'getc', 'getenv', 'gmtime', 'islower', 'isspace', 'isupper',
        'isxdigit', 'localeconv', 'log10', 'longjmp', 'malloc', 'memcmp', 'memcpy', 'memmove', 'memset', 'printf',
        'qsort', 'rand', 'realloc', 'remove', 'rename', 'setlocale', 'signal', 'srand', 'strcat', 'strchr', 'strcmp',
        'strcpy', 'strcspn', 'strerror', 'strftime', 'strlen', 'strncat', 'strncmp', 'strpbrk', 'strrchr', 'strstr',
        'strtok', 'strtol', 'strtoul', 'tan', 'time', 'tolower', 'toupper', 'ungetc', 'vfprintf', 'wcscmp', 'wcslen'
    },
    'ole32.dll': {
        'CoCreateInstance', 'CoInitializeEx', 'CoTaskMemFree', 'CoUninitialize'
    },
    'oleaut32.dll': {
        'SysFreeString'
    },
    'shell32.dll': {
        # ExtractIconExW interposed 2026-11-19 (field box's shell32 predates
        # the export -> loader abort; SDL 2.0.8 WIN_RegisterClass icon path,
        # era fallback = iconless, the reference box's measured behavior).
        # If the name reappears in any ILT the shim leaked: FAIL.
        'DragAcceptFiles', 'DragFinish', 'DragQueryFileW'
    },
    'user32.dll': {
        'AdjustWindowRectEx', 'CallWindowProcA', 'ChangeDisplaySettingsExW', 'ClientToScreen', 'ClipCursor',
        'CloseClipboard', 'CreateIconFromResource', 'CreateIconIndirect', 'CreateWindowExA', 'DefWindowProcA',
        'DestroyIcon', 'DestroyWindow', 'DialogBoxIndirectParamW', 'DispatchMessageA', 'DrawTextW', 'EmptyClipboard',
        'EndDialog', 'EnumDisplayDevicesA', 'EnumDisplayMonitors', 'EnumDisplaySettingsA', 'GetAsyncKeyState',
        'GetClassInfoExW', 'GetClientRect', 'GetClipboardData', 'GetCursorPos', 'GetDC', 'GetDesktopWindow',
        'GetDoubleClickTime', 'GetFocus', 'GetKeyState', 'GetKeyboardLayout', 'GetKeyboardState', 'GetMenu',
        'GetMessageW', 'GetParent', 'GetPropA', 'GetSystemMetrics', 'GetUpdateRect',
        'GetWindowLongA', 'GetWindowRect', 'GetWindowTextA', 'GetWindowTextLengthA', 'InvalidateRect',
        'IsClipboardFormatAvailable', 'IsIconic', 'IsRectEmpty', 'KillTimer', 'LoadCursorA', 'LoadIconW',
        'MapVirtualKeyA', 'MessageBoxA', 'OpenClipboard', 'PeekMessageA', 'RegisterClassExA', 'RegisterClassW',
        'ReleaseDC', 'RemovePropA', 'ScreenToClient', 'SendMessageW', 'SetClipboardData', 'SetCursor', 'SetCursorPos',
        'SetForegroundWindow', 'SetPropA', 'SetTimer', 'SetWindowLongA', 'SetWindowPos', 'SetWindowTextA',
        'ShowWindow', 'SystemParametersInfoA', 'SystemParametersInfoW', 'ToAscii', 'TranslateMessage',
        'UnregisterClassW', 'ValidateRect', 'WindowFromPoint', 'wsprintfA'
    },
    'version.dll': {
        'GetFileVersionInfoA', 'GetFileVersionInfoSizeA', 'VerQueryValueA'
    },
    'winmm.dll': {
        'timeBeginPeriod', 'timeEndPeriod', 'timeGetTime', 'waveInAddBuffer', 'waveInClose', 'waveInGetDevCapsA',
        'waveInGetNumDevs', 'waveInOpen', 'waveInPrepareHeader', 'waveInReset', 'waveInStart', 'waveInUnprepareHeader',
        'waveOutClose', 'waveOutGetDevCapsA', 'waveOutGetErrorTextA', 'waveOutGetNumDevs', 'waveOutOpen',
        'waveOutPrepareHeader', 'waveOutReset', 'waveOutUnprepareHeader', 'waveOutWrite'
    },

}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    objdump, exe = sys.argv[1], sys.argv[2]
    ver = sys.argv[3] if len(sys.argv) > 3 else None
    dump = subprocess.run([objdump, '-p', exe], capture_output=True,
                          text=True).stdout
    fails = []

    cur = None
    unknown = []
    total = 0
    for ln in dump.splitlines():
        m = re.search(r'DLL Name:\s*(\S+)', ln)
        if m:
            cur = m.group(1).lower()
            continue
        m = re.match(r'\s*[0-9a-f]+\s+(?:<none>|\d+)\s+[0-9a-f]+\s+'
                     r'([A-Za-z_?#][A-Za-z0-9_?.]*)', ln)
        if m and cur:
            sym = m.group(1)
            total += 1
            if sym not in ALLOW.get(cur, ()):
                unknown.append(f'{cur}!{sym}')
    if not total:
        fails.append('no imports parsed (gate broken or not a PE)')
    if unknown:
        fails.append('unadjudicated imports: ' + ', '.join(sorted(set(unknown))))

    if 'PE32+' in dump.split('\n', 1)[0] or \
            re.search(r'(PE32\+)', dump[:2000]):
        fails.append('PE32+ image (64-bit) — 98 cannot load')
    if not re.search(r'file format pei-i386', dump) and \
            not re.search(r'machine.*80386', dump, re.I):
        fails.append('machine is not i386 (pei-i386)')
    if not re.search(r'Subsystem\s+00000002\s+\(Windows GUI\)', dump):
        fails.append('not Windows GUI subsystem')
    m = re.search(r'MajorSubsystemVersion\s+(\d+)', dump)
    if not m or int(m.group(1)) > 4:
        fails.append('MajorSubsystemVersion not 4 (98-class floor)')

    if ver:
        raw = open(exe, 'rb').read()
        if f'v{ver}'.encode() not in raw:
            fails.append(f'version token v{ver} not embedded')
        if b'x86' not in raw:
            fails.append('LV_ARCH x86 token not embedded')

    # 5. ISA scan — every SIMD register mention in the disassembly is a
    # SIGILL waiting on a Pentium II (SSE) or Pentium Pro (MMX). 3DNow
    # always names %mmx operands, so the mmx regex covers it too.
    disasm = subprocess.run([objdump, '-d', exe], capture_output=True,
                            text=True).stdout
    simd = sum(1 for ln in disasm.splitlines()
               if re.search(r'%(xmm|mmx)\d*\b', ln))
    if simd:
        fails.append(f'{simd} SIMD (SSE/MMX/3DNow) instruction sites — '
                     'era floor is Pentium Pro class, no SIMD')

    # 6. MSLU declaration: era SDL forces UNICODE mode and 9x exports
    # the *W entry points as dead stubs — the image MUST declare
    # Unicode-Aware (DllCharacteristics bit 0x2) or the 98 loader
    # never consults unicows.dll even when the user supplied it
    # (tools/win98_mslu.py sets it post-link).
    m = re.search(r'DllCharacteristics\s+0*([0-9a-f]+)', dump)
    if not m or not int(m.group(1), 16) & 0x2:
        fails.append('MSLU Unicode-Aware bit not set (tools/win98_mslu.py '
                     'post-build step missing or skipped?)')

    for f in fails:
        print('FAIL:', f)
    print('win98 gate:', 'FAIL' if fails else
          f'PASS — {total} imports all adjudicated 98-class, PE32 i386, '
          'GUI subsystem 4.x' +
          (f', token v{ver}' if ver else ''))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
