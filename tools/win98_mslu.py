#!/usr/bin/env python3
"""Set the Unicode-Aware PE flag (DllCharacteristics bit 0x0002).

The Win98 loader only consults the Microsoft Layer for Unicode
(unicows.dll beside the exe) for images that declare this bit. With
MSLU active, the *W* Win32 entry points that 9x exports as dead
ERROR_CALL_NOT_IMPLEMENTED stubs are serviced by real A-call
translations at RUNTIME — which is the only way era SDL (which
forces UNICODE mode in every release; measured 2.0.0..2.32) can
register classes and create windows on 98.

House rule: unicows.dll is NEVER shipped in the zip (user-supplied;
docs/WIN98-NOTES.txt says how) — but the flag must always be set,
or 98 ignores MSLU even when the DLL is present. Setting it without
the DLL is still progress: the 98 loader then shows its own
"requires Microsoft Layer for Unicode" dialog instead of the app
dying silently.

Idempotent. usage: win98_mslu.py <exe>"""
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    with open(path, 'r+b') as f:
        data = f.read()
        pe = int.from_bytes(data[0x3c:0x40], 'little')
        if data[pe:pe + 4] != b'PE\x00\x00':
            print('win98_mslu: not a PE — refusing')
            return 1
        magic = int.from_bytes(data[pe + 24:pe + 26], 'little')
        if magic != 0x10b:
            print(f'win98_mslu: not PE32 (magic {magic:#x}) — era exe '
                  'floor is PE32; refusing')
            return 1
        off = pe + 24 + 70  # DllCharacteristics, PE32 optional header
        dc = int.from_bytes(data[off:off + 2], 'little')
        if dc & 0x2:
            print(f'win98_mslu: Unicode-Aware already set ({dc:#06x})')
            return 0
        f.seek(off)
        f.write((dc | 0x2).to_bytes(2, 'little'))
        print(f'win98_mslu: Unicode-Aware set {dc:#06x} -> {dc | 2:#06x}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
