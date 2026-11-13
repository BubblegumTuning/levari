#!/usr/bin/env python3
"""win98 release gate (Win98 branch; parked-features dossier TODO 8).

Asserts the era artifact's floor the same way the legacy ISA gate
asserts the i586 one:

  1. import denylist scan == 0 hits  (every entry is loader-rejecting
     on 9x kernel32/advapi32: absent exports, not deprecated ones.
     GetTickCount64/CancelIoEx/CancelSynchronousIo are Vista+;
     TryEnterCriticalSection/InitializeCriticalSectionAndSpinCount/
     ConvertStringSecurityDescriptor*/VerifyVersionInfo*/
     VerSetConditionMask are Win2000-XP era; RegisterRawInputDevices
     Vista+. GetSystemPowerStatus/SetErrorMode/IsDebuggerPresent were
     investigated and are 98-DELIVERED — deliberately NOT denied.)
  2. PE is PE32 (not PE32+), machine Intel 80386
  3. subsystem Windows GUI, subsystem version 4.x (not 5.x/6.x floors)
  4. embedded version token (win GUI embeds "v"+version; LV_ARCH x86)

usage: win98_gate.py <objdump> <exe> [version]
exit 0 = all gates green."""
import re
import subprocess
import sys

DENY = [
    'GetTickCount64', 'CancelIoEx', 'CancelSynchronousIo',
    'InitializeCriticalSectionAndSpinCount', 'TryEnterCriticalSection',
    'SleepConditionVariable', 'CreateEventEx',
    'GetSystemTimePreciseAsFileTime', 'SetProcessDEPPolicy',
    'ConvertStringSecurityDescriptor', 'VerifyVersionInfo',
    'VerSetConditionMask', 'GetModuleHandleEx',
    'RegisterRawInputDevices', 'K32',
]


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
    hits = []
    for ln in dump.splitlines():
        m = re.search(r'DLL Name:\s*(\S+)', ln)
        if m:
            cur = m.group(1).lower()
            continue
        m = re.match(r'\s*[0-9a-f]+\s+(?:<none>|\d+)\s+[0-9a-f]+\s+'
                     r'([A-Za-z_?][A-Za-z0-9_?.]*)', ln)
        if m and cur:
            sym = m.group(1)
            for d in DENY:
                if d in sym or sym.startswith('K32'):
                    hits.append(f'{cur}!{sym}')
    if hits:
        fails.append('denylist hits: ' + ', '.join(sorted(set(hits))))

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

    for f in fails:
        print('FAIL:', f)
    print('win98 gate:', 'FAIL' if fails else
          'PASS — import denylist 0 hits, PE32 i386, GUI subsystem 4.x' +
          (f', token v{ver}' if ver else ''))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
