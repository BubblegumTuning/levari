#!/usr/bin/env python3
# Generate src/skin/cjk_font.c: JIS X 0208 + latin-1 + Cyrillic + KS X 1001
# Hangul + common punctuation + audited title decorations as 16x16 bitmaps,
# bitmaps taken from GNU Unifont (unifont.hex), keyed by Unicode codepoint.
# Bitmaps are stored as a raw-LZMA1 stream over a byte-plane permutation
# (hi byte of every uint16 row first, then lo byte) and inflated at first
# lookup by the vendored 7-Zip SDK decoder in src/skin/cjk_font.c.
#
#   python3 tools/gen_cjk_font.py /path/to/unifont.hex
#
# License of generated bitmaps: GNU Unifont is GPL-2.0+ with the font
# embedding exception, which covers embedding the glyphs in this program.
import lzma
import struct
import sys

def jis0208_codepoints():
    cps = set()
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                s = bytes([hi, lo]).decode('euc_jp')
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                cps.add(ord(s))
    return cps

def latin1_supplement():
    # python's euc_jp codec leaves the accented JIS X 0208 cells unmapped, so
    # the JIS set alone misses a o u A O U etc.; add the whole latin-1
    # supplement so western-european titles render instead of falling back to '?'
    return set(range(0xA0, 0x100))

def cyrillic():
    # JIS X 0208 carries only the Russian alphabet + yo; Ukrainian/Serbian/
    # Belarusian letters (i yi ye ghe-upshortedje, dje tshe dzhe, short u)
    # live outside it. Take the whole Cyrillic block + supplements so any
    # slavic title renders. Unifont's cyrillic glyphs are 8-wide, so they
    # land on the half-width table automatically like the russian set.
    return set(range(0x0400, 0x0530))

def korean():
    # KS X 1001 Hangul syllables: the euc_kr lead grid 0xB0-0xC8 decodes to
    # exactly the 2,350 standard syllables (verified against the codec, not
    # memory). Same legacy-grid policy as the JIS set — the full 11,172-block
    # would add ~285 KB of raw bitmaps for archaic syllables no title uses.
    # All are 16-wide in unifont, so they land on the fullwidth table.
    cps = set()
    for hi in range(0xB0, 0xC9):
        for lo in range(0xA1, 0xFF):
            try:
                s = bytes([hi, lo]).decode('euc_kr')
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                cps.add(ord(s))
    return cps

def common_punctuation():
    # curly quotes, dashes, ellipsis etc. show up constantly in track titles
    return {0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201B, 0x201C, 0x201D,
            0x201E, 0x201F, 0x2020, 0x2021, 0x2022, 0x2026, 0x2039, 0x203A,
            0x20AC}

def title_decorations():
    # codepoints found by auditing 11,868 real song titles from
    # ja.wikipedia Category:日本語の楽曲 against the shipped cjk_cp table
    # (2026-11; recipe: levari skill references/wiki-glyph-coverage-audit.md).
    # All are present in Unifont; none is reachable from the legacy grids.
    return {
        0x2661, 0x2665, 0x2667, 0x266C,  # ♡ ♥ ♧ ♬ decor in titles (33/9/1/1x)
        0x2248,                          # ≈ absent from JIS grids and from the
                                         # 11,868-title audit; requested 2026-11
        0xFF5E,                          # fullwidth tilde: python's euc_jp maps
                                         # the JIS X 0208 tilde cell to U+301C
                                         # only (CP932 tilde problem), so UTF-8
                                         # titles using FF5E fall through
        0x3094,                          # ゔ — kana added post-JIS X 0208
        0xFA1F,                          # 﨟 CJK compat kanji (わが﨟たし悪の華)
        0x2F63,                          # ⽣ kangxi radical used decoratively
        0x014D, 0x01DD,                  # ō ǝ romanization flourishes
    }

def load_unifont(path):
    # returns codepoint -> list of 16 uint16 row bitmaps (bit 0x8000 = leftmost)
    # and the set of codepoints whose unifont entry is 8-wide (half-width)
    glyphs = {}
    half = set()
    with open(path, encoding='ascii') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp, _, hexs = line.partition(':')
            if len(hexs) == 64:  # 16-wide: 16 rows x 2 bytes
                glyphs[int(cp, 16)] = [int(hexs[i:i+4], 16) for i in range(0, 64, 4)]
            elif len(hexs) == 32:  # 8-wide: center in 16
                rows = [int(hexs[i:i+2], 16) for i in range(0, 32, 2)]
                glyphs[int(cp, 16)] = [(r << 4) & 0xFFFF for r in rows]
                half.add(int(cp, 16))
    return glyphs, half

# the only encoder pin for the shipped blob: raw LZMA1, build-best settings
LZMA_FILTERS = [dict(id=lzma.FILTER_LZMA1, preset=9 | lzma.PRESET_EXTREME,
                     lc=3, lp=0, pb=0, dict_size=524288)]

def lzma_props():
    # LZMA1 props (current SDK LzmaProps_Decode): byte0 = (pb*5+lp)*9+lc;
    # bytes1..4 LE = dictionary size in bytes. Round-trip verified in main().
    f = LZMA_FILTERS[0]
    return bytes([(f['pb'] * 5 + f['lp']) * 9 + f['lc']]) + \
        f['dict_size'].to_bytes(4, 'little')

def main():
    uni, half = load_unifont(sys.argv[1])
    want = (jis0208_codepoints() | latin1_supplement() | cyrillic() |
            korean() | common_punctuation() | title_decorations())
    cps = sorted(c for c in want if c in uni)
    missing = len(want) - len(cps)
    sys.stderr.write('font: %d glyphs, %d missing in unifont\n' % (len(cps), missing))

    out = []
    out.append('/* generated by tools/gen_cjk_font.py from GNU Unifont 16.0.01 (unifont.hex) */\n'
               '/* GNU Unifont: GPL-2.0+ with font embedding exception */\n'
               '#include <stdint.h>\n#include <stdlib.h>\n#include "cjk_font.h"\n#include "LzmaDec.h"\n\n')
    out.append('#define CJK_N %d\n' % len(cps))
    out.append('static const uint16_t cjk_cp[CJK_N] = {\n')
    row = []
    for i, c in enumerate(cps):
        row.append('0x%04X' % c)
        if len(row) == 12:
            out.append(','.join(row) + ',\n'); row = []
    if row:
        out.append(','.join(row) + '\n')
    out.append('};\n\n')
    # half-width (8-wide in unifont) codepoints in the table: latin-1, common
    # punctuation etc. must advance half-width like ascii, not like cjk
    half_cps = sorted(c for c in cps if c in half)
    out.append('#define CJK_HALF_N %d\n' % len(half_cps))
    out.append('static const uint16_t cjk_half_cp[CJK_HALF_N] = {\n')
    row = []
    for c in half_cps:
        row.append('0x%04X' % c)
        if len(row) == 12:
            out.append(','.join(row) + ',\n'); row = []
    if row:
        out.append(','.join(row) + '\n')
    out.append('};\n\n')
    # glyph bitmaps as little-endian rows permuted to byte planes: the hi
    # byte of every uint16 row (left 8 pixels, odd byte of the LE stream)
    # precedes the lo byte plane, so vertical stroke continuity spans the
    # whole corpus instead of a 2-byte window; raw-LZMA1 over that beats
    # both plain-order LZMA and the former zlib-9 by wide margins
    raw = b''.join(struct.pack('<16H', *uni[c]) for c in cps)
    plane = raw[1::2] + raw[0::2]
    blob = lzma.compress(plane, format=lzma.FORMAT_RAW, filters=LZMA_FILTERS)
    props = lzma_props()
    # generator-side gate: the shipped C decoder must be able to reverse
    # exactly what we just wrote — python round-trip plus byte-exactness
    back = lzma.decompress(blob, format=lzma.FORMAT_RAW, filters=LZMA_FILTERS)
    assert back == plane, 'lzma round-trip failed'
    assert props == bytes([LZMA_FILTERS[0]['lc']]) + \
        LZMA_FILTERS[0]['dict_size'].to_bytes(4, 'little'), 'props math drift'
    out.append('/* raw-LZMA1 of the byte-plane permutation of CJK_N*16 little-endian\n'
               '   uint16 rows (hi-byte plane then lo-byte plane); decoder props\n'
               '   lc=3 lp=0 pb=0 dict 512 KiB — see third_party/lzma */\n')
    out.append('static const unsigned char cjk_glyphs_lz[] = {\n')
    row = []
    for i, b in enumerate(blob):
        row.append('0x%02X' % b)
        if len(row) == 20:
            out.append(','.join(row) + ',\n'); row = []
    if row:
        out.append(','.join(row) + '\n')
    out.append('};\n#define CJK_GLYPHS_LZ_LEN %d\n\n' % len(blob))
    out.append('#define CJK_RAW_LEN (CJK_N * 32)\n\n')
    out.append('static const Byte cjk_lzma_props[LZMA_PROPS_SIZE] = {'
               + ','.join('0x%02X' % b for b in props) + '};\n\n')
    out.append('static uint16_t cjk_glyphs[CJK_N][16];\n'
               'static int cjk_glyphs_ready;\n'
               'static int cjk_glyphs_ok;\n\n'
               'static void *cjk_lz_alloc(ISzAllocPtr p, size_t size)\n'
               '{ (void)p; return malloc(size); }\n'
               'static void cjk_lz_free(ISzAllocPtr p, void *address)\n'
               '{ (void)p; free(address); }\n'
               'static ISzAlloc cjk_lz_if = { cjk_lz_alloc, cjk_lz_free };\n\n'
               'static void cjk_glyphs_init(void)\n{\n'
               '    CLzmaDec dec;\n'
               '    LzmaDec_Construct(&dec);\n'
               '    unsigned char *plane = malloc(CJK_RAW_LEN);\n'
               '    if (plane && LzmaDec_Allocate(&dec, cjk_lzma_props,\n'
               '            LZMA_PROPS_SIZE, &cjk_lz_if) == SZ_OK) {\n'
               '        LzmaDec_Init(&dec);\n'
               '        SizeT in_pos = 0, out_pos = 0;\n'
               '        ELzmaStatus status;\n'
               '        SRes rc = SZ_OK;\n'
               '        while (out_pos < CJK_RAW_LEN) {\n'
               '            SizeT in_used = CJK_GLYPHS_LZ_LEN - in_pos;\n'
               '            SizeT out_used = CJK_RAW_LEN - out_pos;\n'
               '            rc = LzmaDec_DecodeToBuf(&dec, plane + out_pos, &out_used,\n'
               '                cjk_glyphs_lz + in_pos, &in_used,\n'
               '                in_pos + in_used >= CJK_GLYPHS_LZ_LEN\n'
               '                    ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);\n'
               '            if (rc != SZ_OK || (in_used == 0 && out_used == 0)) break;\n'
               '            in_pos += in_used;\n'
               '            out_pos += out_used;\n'
               '        }\n'
               '        if (rc == SZ_OK && out_pos == CJK_RAW_LEN) {\n'
               '            uint16_t *flat = &cjk_glyphs[0][0];\n'
               '            SizeT half = (SizeT)CJK_N * 16;\n'
               '            for (SizeT i = 0; i < half; i++)\n'
               '                flat[i] = (uint16_t)(plane[half + i] |\n'
               '                    ((uint16_t)plane[i] << 8));\n'
               '            cjk_glyphs_ok = 1;\n'
               '        }\n'
               '        LzmaDec_Free(&dec, &cjk_lz_if);\n'
               '    }\n'
               '    free(plane);\n'
               '    cjk_glyphs_ready = 1;\n}\n\n')
    # half-width ASCII from Unifont (8x16, left-aligned in the 16px grid)
    ascii_cps = list(range(0x20, 0x7F))
    out.append('static const uint16_t ascii16[95][16] = {\n')
    for c in ascii_cps:
        g = uni.get(c)
        if g is None:
            out.append('{0},\n')
        else:
            # stored centered (r<<4); shift back up to left-align the 8-wide rows
            rows = [(r << 4) & 0xFF00 for r in g]
            out.append('{' + ','.join('0x%04X' % r for r in rows) + '},\n')
    out.append('};\n\nconst uint16_t *ascii16_glyph(unsigned cp)\n{\n'
               '    if (cp < 0x20 || cp > 0x7E) return 0;\n'
               '    return ascii16[cp - 0x20];\n}\n\n')
    out.append('const uint16_t *cjk_glyph(unsigned cp)\n{\n'
               '    if (!cjk_glyphs_ready) cjk_glyphs_init();\n'
               '    if (!cjk_glyphs_ok) return 0;\n'
               '    int lo = 0, hi = CJK_N - 1;\n'
               '    while (lo <= hi) {\n'
               '        int mid = (lo + hi) / 2;\n'
               '        if (cjk_cp[mid] == cp) return cjk_glyphs[mid];\n'
               '        if (cjk_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;\n'
               '    }\n'
               '    return 0;\n}\n\n'
               'int cjk_halfwidth(unsigned cp)\n{\n'
               '    int lo = 0, hi = CJK_HALF_N - 1;\n'
               '    while (lo <= hi) {\n'
               '        int mid = (lo + hi) / 2;\n'
               '        if (cjk_half_cp[mid] == cp) return 1;\n'
               '        if (cjk_half_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;\n'
               '    }\n'
               '    return 0;\n}\n')
    with open('src/skin/cjk_font.c', 'w') as f:
        f.write(''.join(out))
    sys.stderr.write('wrote src/skin/cjk_font.c\n')

if __name__ == '__main__':
    main()
