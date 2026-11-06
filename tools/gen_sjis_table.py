#!/usr/bin/env python3
"""Generate src/tag/sjis_table.h — the Shift_JIS decode table for tag.c.

PROVENANCE — READ BEFORE TOUCHING THIS FILE OR ITS OUTPUT:
The table source is the python 'cp932' codec, i.e. the WHATWG `shift_jis`
mapping (CP932 / Windows-31J). This is DELIBERATE and is decision D4 of
the SJIS feature: real Windows-era tags carry NEC specials, NEC-selected
IBM extensions and vendor kanji (髙 0xEEE0, 﨑 0xED95) and symbols (① 0x8740,
㈱ 0x878A). A strict JIS X 0208 table REJECTS OR REPLACES exactly these
codepoints — switching to a strict-JIS or 'shift_jis'(-fallback) source
here is a REGRESSION, not a cleanup. Do not "tidy" the codec choice.
The corpus in test/tagwords/words.txt SECTION 15 pins the vendor
codepoints; a strict table turns those pins red.

Layout: two-stage. sjis_lead[byte] -> row index, 0xFF = NOT a lead byte.
The AUTO classifier (tag.c sjis_scan) and the forced decoder share the
grid: leads {0x81-0x9F, 0xE0-0xEF}, trails {0x40-0x7E, 0x80-0xFC}
(the 0x7F slot is kept for the trail-0x40 index arithmetic and is always
unassigned). 0x0000 = pair unassigned in CP932 -> the decoder emits
U+FFFD, which is what Windows tools do. CP932 fills parts of the grid a
strict table leaves empty (e.g. the 0xFA/0xFB columns and the 0x80 trail
band of rows like 0x87/0x88); the two-stage shape carries that through
unchanged.

Half-width kana (bytes 0xA1-0xDF): decoded only under the FORCED
SHIFT-JIS mode (decision D2 — dense cp1251 collision), from a 63-entry
table generated from the SAME cp932 codec. NOTE: the SJIS plan drafted
this as the formula U+FF61+(byte-0xA0); measurement shows cp932 truth
diverges (0xA1 = U+FFE5 yen, 0xB1 = U+FF71 'ァ'), so the table is
generated, never formula'd — cp932 stays the source of truth.

Measured facts kept current (2026-11-05, python 3.13, CPython 'cp932'):
  - 1856 of the lead x trail scan grid are undecodable by python cp932:
    table entry 0x0000, decoder emits U+FFFD (the Windows-tool behavior).
  - python cp932 has a quirk on the 0xFD/0xFE leads (PUA-synthesis
    rows); it never matters: sjis_scan fail-closes on 0x80, 0xA0, 0xFD-0xFF
    as non-lead bytes BEFORE the table is consulted (decision D6), and
    0xF0-0xFC are non-lead for the same reason.
  - every codepoint the table can hold is BMP (measured max U+FFE5 in the
    cp932 grid; the SJIS plan's draft note "<= U+FF9F" underestimated the
    full-width form band FFE0-FFE6): one output codepoint is at most 3
    UTF-8 bytes, so worst case field expansion is 1.5x (2 SJIS bytes ->
    3 UTF-8 bytes). The self-check below asserts BMP and no surrogates.

Usage (from the repo root or anywhere):
  python3 tools/gen_sjis_table.py        # regenerate + self-check
  python3 tools/gen_sjis_table.py --check  # self-check current header only
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'src', 'tag', 'sjis_table.h')

CODEC = 'cp932'          # THE source of truth — see provenance header above
LEADS = [b for b in range(0x81, 0xA0)] + [b for b in range(0xE0, 0xF0)]
TRAILS = [t for t in range(0x40, 0x80)] + [t for t in range(0x80, 0xFD)]

def pair_cp(lead, trail):
    """Unicode codepoint python's cp932 assigns to (lead, trail), or None
    if unassigned. WHATWG shift_jis: a lead followed by an invalid trail
    byte must never leak a half-pair, so this only ever sees grid trails."""
    try:
        s = bytes([lead, trail]).decode(CODEC)
    except UnicodeDecodeError:
        return None
    if len(s) != 1:
        raise SystemExit('cp932 decoded (%02X %02X) to %d chars — table '
                         'shape assumption broken' % (lead, trail, len(s)))
    return ord(s)

def build():
    lead = [0xFF] * 256
    rows = []
    for i, l in enumerate(LEADS):
        lead[l] = i
        rows.append([pair_cp(l, t) or 0 for t in TRAILS])
    hw = []
    for b in range(0xA1, 0xE0):
        try:
            cp = ord(bytes([b]).decode(CODEC))
        except UnicodeDecodeError:
            cp = 0xFFFD       # cp932 leaves no hole in A1-DF; defensive
        hw.append(cp)
    return lead, rows, hw

VENDOR = [(0xEE, 0xE0, 0x9AD9, '髙'), (0xED, 0x95, 0xFA11, '﨑'),
          (0x87, 0x40, 0x2460, '①'),  (0x87, 0x8A, 0x3231, '㈱')]
TITLES = ['ボーナストラック', 'PE-RI-CA （地底の王国）', 'Melty Blood （OPテーマ）',
          '今 この瞬間がすべて', 'PROMISED　DAWN']

def table_lookup(lead_tab, rows, lead, trail):
    r = lead_tab[lead]
    return None if r == 0xFF else rows[r][trail - 0x40]

def selfcheck(lead_tab, rows, hw):
    fails = 0
    # 1) round-trip the five real titles through the generated table
    for t in TITLES:
        b = t.encode(CODEC)
        out = []
        i = 0
        while i < len(b):
            c = b[i]
            if c < 0x80:
                out.append(c); i += 1; continue
            cp = table_lookup(lead_tab, rows, c, b[i + 1])
            if cp is None or cp == 0:
                print('FAIL title %r: pair %02X %02X not table-decodable'
                      % (t, c, b[i + 1])); fails += 1; break
            out += list(chr(cp).encode('utf-8'))
            i += 2
        if bytes(out) != t.encode('utf-8'):
            print('FAIL round-trip %r' % t); fails += 1
    # 2) CP932 vendor pins — the VENDOR codepoint, never a strict-JIS
    #    equivalent and never U+FFFD (decision D4 made honest)
    for lead_b, trail_b, want, ch in VENDOR:
        got = table_lookup(lead_tab, rows, lead_b, trail_b)
        if got != want:
            print('FAIL vendor %s %02X %02X: table %04X want %04X'
                  % (ch, lead_b, trail_b, got or 0, want)); fails += 1
    # 3) half-width table vs codec
    for b, cp in zip(range(0xA1, 0xE0), hw):
        want = ord(bytes([b]).decode(CODEC))
        if cp != want:
            print('FAIL halfwidth %02X: table %04X want %04X' % (b, cp, want)); fails += 1
    # 4) 1000-pair random sweep vs the reference mapping
    rnd = random.Random(0x5C15)
    for _ in range(1000):
        l = rnd.choice(LEADS); t = rnd.choice(TRAILS)
        got = table_lookup(lead_tab, rows, l, t) or 0
        want = pair_cp(l, t) or 0
        if got != want:
            print('FAIL sweep %02X %02X: table %04X want %04X' % (l, t, got, want)); fails += 1
    # 5) every codepoint is BMP (cp932 grid max measured U+FFE5) and not
    #    a surrogate: the decoder's 3-byte UTF-8 ceiling depends on it
    over = [cp for row in rows for cp in row if cp and (cp > 0xFFFF or 0xD800 <= cp <= 0xDFFF)] + \
           [cp for cp in hw if cp > 0xFFFF or 0xD800 <= cp <= 0xDFFF]
    if over:
        print('FAIL non-BMP/surrogate codepoints in table: %s' % ['%04X' % c for c in over[:8]]); fails += 1
    # 6) the D6 edge bytes are NOT leads (fail-closed before the table)
    for b in [0x80, 0xA0, 0xFD, 0xFE, 0xFF] + list(range(0xF0, 0xFC)):
        if lead_tab[b] != 0xFF:
            print('FAIL edge byte %02X marked as lead' % b); fails += 1
    return fails

def emit(lead_tab, rows, hw):
    def arr(vals, per):
        out = []
        for i in range(0, len(vals), per):
            out.append(','.join('0x%04X' % v for v in vals[i:i+per]))
        return ',\n'.join(out)
    un = sum(1 for r in rows for c in r if c == 0)
    hdr = []
    hdr.append('/* GENERATED by tools/gen_sjis_table.py — DO NOT EDIT. Source:')
    hdr.append('   python\'s "%s" codec = WHATWG shift_jis (CP932/Windows-31J).' % CODEC)
    hdr.append('   DELIBERATELY not strict JIS X 0208 (decision D4 of the SJIS plan):')
    hdr.append('   NEC specials, NEC-selected IBM extensions and vendor kanji 髙 﨑 瀨 ㈱')
    hdr.append('   plus circled symbols ① are the POINT — a strict-JIS mapping here is a')
    hdr.append('   REGRESSION. Corpus pins: test/tagwords/words.txt SECTION 15. */')
    hdr.append('#ifndef LEVARI_SJIS_TABLE_H')
    hdr.append('#define LEVARI_SJIS_TABLE_H')
    hdr.append('')
    hdr.append('/* lead byte -> row index into sjis_rows[]; 0xFF = NOT a lead.')
    hdr.append('   tag.c sjis_scan fail-closes BEFORE consulting this table on the')
    hdr.append('   non-lead bytes 0x80, 0xA0, 0xF0-0xFF (decision D6); half-width')
    hdr.append('   kana 0xA1-0xDF decode via sjis_hw[] under FORCED mode only')
    hdr.append('   (decision D2). */')
    hdr.append('static const unsigned char sjis_lead[256] = {')
    for r in range(0, 256, 16):
        hdr.append(','.join('0x%02X' % b for b in lead_tab[r:r+16]) + ',')
    hdr.append('};')
    hdr.append('')
    hdr.append('/* rows for leads {0x81-0x9F, 0xE0-0xEF} (row = sjis_lead[lead]).')
    hdr.append('   Index = trail - 0x40 over {0x40-0x7E, 0x80-0xFC}; the 0x7F slot is')
    hdr.append('   structurally unreachable (sjis_scan rejects it) and kept unassigned.')
    hdr.append('   0x0000 = pair unassigned in CP932 (%d of %d grid slots) — decoder' % (un, len(rows) * len(TRAILS)))
    hdr.append('   emits U+FFFD, the Windows-tool behavior. */')
    hdr.append('static const unsigned short sjis_rows[%d][%d] = {' % (len(rows), len(TRAILS)))
    for ri, row in enumerate(rows):
        hdr.append('{')
        hdr.append(arr(row, 21))
        hdr.append('},')
    hdr.append('};')
    hdr.append('')
    hdr.append('/* half-width kana 0xA1-0xDF -> unicode (index = byte - 0xA1), cp932')
    hdr.append('   truth (0xA1 is U+FFE5 yen, NOT U+FF61+off — do not formula this).')
    hdr.append('   Used only when the verdict is FORCED shiftjis (decision D2). */')
    hdr.append('static const unsigned short sjis_hw[63] = {')
    hdr.append(arr(hw, 14))
    hdr.append('};')
    hdr.append('')
    hdr.append('#endif')
    return '\n'.join(hdr) + '\n'

def main():
    check = '--check' in sys.argv
    if check:
        # re-derive and diff against the committed header
        src = emit(*build())
        old = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else ''
        if src != old:
            sys.exit('sjis_table.h is not what the generator produces — '
                     're-run tools/gen_sjis_table.py')
        fails = selfcheck(*build())
        sys.exit(1 if fails else 0)
    lead_tab, rows, hw = build()
    fails = selfcheck(lead_tab, rows, hw)
    if fails:
        sys.exit('generator self-check: %d failures' % fails)
    src = emit(lead_tab, rows, hw)
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(src)
    import subprocess
    v = sys.version.split()[0]
    print('generated %s: python %s, codec %s, %d rows x %d, %d bytes header'
          % (OUT, v, CODEC, len(rows), len(TRAILS), len(src.encode('utf-8'))))
    print('self-check green: titles, vendor set, halfwidth, 1000-pair sweep, '
          'BMP bound, D6 non-lead edges')

if __name__ == '__main__':
    main()
