#!/usr/bin/env python3
"""tagwords regression harness for levari tag decoding (src/tag/tag.c).

Reads words.txt (one entry per line: MODE<TAB>TEXT), generates one ID3v2/ID3v1
fixture per entry in a temp dir, runs the probe compiled against the tag.c
under test, and compares the decoded title byte-exact with TEXT's UTF-8
bytes. MODE 'obs' pins nothing: the actual decode is reported for review.

Usage (from anywhere):
  python3 test/tagwords/tagcheck.py [--tagc path/to/tag.c] [--keep]
Default --tagc is <repo>/src/tag/tag.c (the worktree). Pass an extracted
older tag.c to baseline past versions (tag.c includes "../common.h", so
extract it keeping that relative layout).

Modes:
  l1    ID3v2.3 TIT2, enc=0, latin-1 bytes of TEXT   (expected: TEXT)
  l1m   same, synthetic matrix entry (byte-trap probe)
  cyr   ID3v2.3 TIT2, enc=0, cp1251 bytes of TEXT    (expected: TEXT)
  u8    ID3v2.3 TIT2, enc=0, field holds REAL UTF-8 of TEXT (expected: TEXT)
  u8t   ID3v2.3 TIT2, enc=0, TEXT then a truncated 4-byte utf-8 tail
        (expected: TEXT; exercises the clamp that 0.3.2 added for emoji)
  u8cut / u8cut2  TIT2: utf-8 of TEXT minus its final char, then that char's
        lead byte (u8cut) or lead+1 continuation (u8cut2) — a fixed-width
        field cut at the char boundary (expected: TEXT minus final char)
  v1cut ID3v1 30-byte title: cp1251 of TEXT cut at byte 30 (expected: TEXT
        minus the trailing partial char)
  v1u8  ID3v1 30-byte title: real UTF-8 of TEXT cut at byte 30 (expected:
        TEXT minus any trailing partial sequence)
  bigc / bigl TIT2: two words joined by one space, cp1251 / latin-1 bytes
        (expected: TEXT) — word-boundary adjacency rules
  l1w / v1w  TIT2 / id3v1: windows-1252 bytes of TEXT (the bytes 0x80-0x9F
        where cp1251 and windows-1252 AGREE — smart quotes, dashes, ellipsis;
        the latin-1 fallback must read them as punctuation, never C1 controls)
  au8 / au8t / au8cut / au8cut2  same shapes as u8/u8t/u8cut/u8cut2 but in
        the TPE1 frame — compared against the decoded ARTIST field
  v1l / v1c  ID3v1 tail (30-byte title, NUL-padded), latin-1 / cp1251
  obs   decode printed, never asserted
id3v1-mode expectations are rstripped: copy_fixed trims the trailing space
run by design (ID3v1 fields pad with spaces; see SECTION 7).
Exit 0 iff every pinned entry matches.
"""
import argparse, os, subprocess, struct, sys, tempfile

# mirror of src/tag/tag.c cp1251_hi — keep in sync; note the Ukrainian
# extensions (0x95->0x0490, 0xB4->0x0491) that the standard cp1251 codec
# lacks, and 0x98 which is undefined there.
CP1251_HI = [
0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
0xFFFD,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
]
CYR_ENC = {}
for b, cp in enumerate(CP1251_HI):
    CYR_ENC.setdefault(cp, 0x80 + b)  # first byte wins, matching tag.c's decode

def enc_latin1(s):
    for c in s:
        if ord(c) > 0xFF:
            raise SystemExit("char U+%04X out of latin-1 range" % ord(c))
    return bytes(ord(c) for c in s)

def enc_cyr(s):
    out = bytearray()
    for c in s:
        cp = ord(c)
        if cp < 0x80: out.append(cp)
        elif cp in CYR_ENC: out.append(CYR_ENC[cp])
        else: raise SystemExit("char U+%04X not encodable via tag.c cp1251 table" % cp)
    return bytes(out)

# bytes 0x80-0x9F where windows-1252 and cp1251 AGREE — tag.c's copy_fixed
# decodes these as punctuation (the w1252 tier); mirror of tag.c's table,
# keep in sync. Disagreeing bytes (0x80 Ђ, 0x9C ѕ/œ, 0x90/0x98 undefined,
# ...) keep the latin-1 C1 reading and are NOT encodable here.
W1252 = {0x82:0x201A, 0x84:0x201E, 0x85:0x2026, 0x86:0x2020, 0x87:0x2021,
         0x88:0x20AC, 0x89:0x2030, 0x8B:0x2039,
         0x91:0x2018, 0x92:0x2019, 0x93:0x201C, 0x94:0x201D, 0x95:0x2022,
         0x96:0x2013, 0x97:0x2014, 0x99:0x2122, 0x9B:0x203A}
W1252_REV = {cp: b for b, cp in W1252.items()}

def enc_w1252(s):
    out = bytearray()
    for c in s:
        cp = ord(c)
        if cp < 0x80 or 0xA0 <= cp <= 0xFF: out.append(cp)
        elif cp in W1252_REV: out.append(W1252_REV[cp])
        else: raise SystemExit("char U+%04X outside the w1252-agreed set" % cp)
    return bytes(out)

def ss(n): return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])
def frame(fid, payload): return fid + struct.pack('>I', len(payload)) + b'\x00\x00' + payload
def tag23(payload): return b'ID3\x03\x00\x00' + ss(len(payload)) + payload

def ext_hdr_v23():  # size 6 EXCLUDES its own 4 bytes (spec): skip = 4+6 = 10
    return b'\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00'
def ext_hdr_v24():  # syncsafe size 10 INCLUDES itself (spec): skip = 10
    return ss(10) + b'\x00\x01\x00\x00\x00\x00'

def fixture(mode, text):
    exp = text.encode('utf-8')
    if mode == 'ext23':
        return (b'ID3\x03\x00\x40' + ss(10 + 10 + len(text.encode('latin-1')) + 1) + \
               ext_hdr_v23() + frame(b'TIT2', b'\x00' + text.encode('latin-1')), exp)
    if mode == 'ext24':
        b4 = text.encode('latin-1')
        ext = ext_hdr_v24()
        fr = b'TIT2' + ss(1 + len(b4)) + b'\x00\x00' + b'\x00' + b4
        return (b'ID3\x04\x00\x40' + ss(len(ext) + len(fr)) + ext + fr, exp)
    if mode in ('l1', 'l1m'):
        b = enc_latin1(text)
        return (tag23(frame(b'TIT2', b'\x00' + b)), exp)
    if mode == 'cyr':
        return (tag23(frame(b'TIT2', b'\x00' + enc_cyr(text))), exp)
    if mode == 'l1w':
        return (tag23(frame(b'TIT2', b'\x00' + enc_w1252(text))), exp)
    if mode in ('au8', 'au8t', 'au8cut', 'au8cut2'):
        m = mode[1:]
        if m == 'u8':
            return (tag23(frame(b'TPE1', b'\x00' + text.encode('utf-8'))), exp)
        if m == 'u8t':
            return (tag23(frame(b'TPE1', b'\x00' + text.encode('utf-8') + b'\xf0\x9f\x98')), exp)
        prev = text[:-1].encode('utf-8')
        tail = text[-1:].encode('utf-8')[:2 if m == 'u8cut2' else 1]
        return (tag23(frame(b'TPE1', b'\x00' + prev + tail)), prev)
    if mode == 'u8':
        return (tag23(frame(b'TIT2', b'\x00' + text.encode('utf-8'))), exp)
    if mode == 'u8t':
        return (tag23(frame(b'TIT2', b'\x00' + text.encode('utf-8') + b'\xf0\x9f\x98')), exp)
    if mode in ('u8cut', 'u8cut2'):
        prev = text[:-1].encode('utf-8')
        tail = text[-1:].encode('utf-8')[:2 if mode == 'u8cut2' else 1]
        return (tag23(frame(b'TIT2', b'\x00' + prev + tail)), prev)
    if mode in ('v1cut', 'v1u8'):
        fn = (lambda ch: enc_cyr(ch)) if mode == 'v1cut' else (lambda ch: ch.encode('utf-8'))
        acc_text = ''
        acc_len = 0
        for ch in text:
            n = len(fn(ch))
            if acc_len + n > 30: break
            acc_len += n; acc_text += ch
        title = (b''.join(fn(ch) for ch in text) + b'\x00' * 30)[:30]
        v1 = b'TAG' + title + b'\x00' * (30 + 30) + b'1999' + b'\x00' * 31
        return (b'\xff\xfb\x90\x64' * 64 + v1, acc_text.rstrip(' ').encode('utf-8'))
    if mode in ('bigc', 'bigl'):
        a, _, b2 = text.partition(' ')
        fn = enc_cyr if mode == 'bigc' else enc_latin1
        return (tag23(frame(b'TIT2', b'\x00' + fn(a) + b' ' + fn(b2))), exp)
    if mode in ('v1l', 'v1c', 'v1w'):
        b = (enc_latin1(text) if mode == 'v1l' else
             enc_cyr(text) if mode == 'v1c' else enc_w1252(text))
        title = b + b'\x00' * (30 - len(b))
        v1 = b'TAG' + title + b'\x00' * (30 + 30) + b'1999' + b'\x00' * 31
        return (b'\xff\xfb\x90\x64' * 64 + v1, text.rstrip(' ').encode('utf-8'))
    if mode == 'obs':
        try:
            blob = tag23(frame(b'TIT2', b'\x00' + enc_cyr(text)))
        except SystemExit:
            blob = tag23(frame(b'TIT2', b'\x00' + enc_latin1(text)))
        return (blob, exp)
    raise SystemExit("unknown mode " + mode)

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ap.add_argument('--corpus', default=os.path.join(here, 'words.txt'))
    ap.add_argument('--tagc', default=os.path.join(repo, 'src', 'tag', 'tag.c'))
    ap.add_argument('--keep', action='store_true', help='keep the fixture dir')
    a = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix='tagwords-')
    obj = os.path.join(tmp, 'probe')
    r = subprocess.run(['gcc', '-std=gnu11', '-I' + os.path.dirname(a.tagc),
                        '-o', obj, os.path.join(here, 'probe.c'), a.tagc])
    if r.returncode: sys.exit('probe compile failed')

    entries, fixtures, names = [], [], []
    for lineno, raw in enumerate(open(a.corpus, encoding='utf-8'), 1):
        line = raw.rstrip('\n')
        if not line or line.startswith('#'): continue
        mode, _, text = line.partition('\t')
        if mode not in ('l1','l1m','l1w','cyr','u8','u8t','u8cut','u8cut2','v1cut','v1u8',
                        'bigc','bigl','v1l','v1c','v1w','ext23','ext24',
                        'au8','au8t','au8cut','au8cut2','obs'):
            sys.exit('line %d: unknown mode %r' % (lineno, mode))
        path = os.path.join(tmp, 'f%05d' % lineno)
        blob, exp = fixture(mode, text)
        with open(path, 'wb') as f: f.write(blob)
        entries.append((mode, text, path, exp)); names.append(path)

    # argv size: big corpora blow the single-exec path list; chunk it
    sout = []
    for i in range(0, len(names), 512):
        r = subprocess.run([obj] + names[i:i+512], capture_output=True, text=True)
        if r.returncode: sys.exit('probe chunk %d failed' % i)
        sout.append(r.stdout)
    got = {}
    for chunk in sout:
        for line in chunk.splitlines():
            p, th, ah = line.split('\t')
            got[p] = (bytes.fromhex(th), bytes.fromhex(ah))

    fails, observes, bymode = [], [], {}
    for mode, text, path, exp in entries:
        pair = got.get(path)
        if pair is None: fails.append((mode, text, b'<no result>', exp)); continue
        act = pair[1] if mode.startswith('au') else pair[0]
        st = 'PASS' if act == exp else 'FAIL'
        bymode.setdefault(mode, [0, 0])
        bymode[mode][st == 'PASS'] += 1
        if mode == 'obs':
            observes.append((text, act.decode('utf-8', 'replace')))
        elif st == 'FAIL':
            fails.append((mode, text, act, exp))
        print('%s %-5s %r -> %r' % (st, mode, text, act.decode('utf-8', 'replace')))

    print('\n== per-mode pass/fail ==')
    for m in sorted(bymode):
        f, p = bymode[m]; print('  %-4s %4d pass  %4d fail' % (m, p, f))
    print('\n== OBSERVE (not asserted) ==')
    for t, act_s in observes: print('  %r decodes to %r' % (t, act_s))
    if fails:
        print('\n== %d PINNED FAILURES (expected pre-fix: see docs/fixlist-0.3.3.md) ==' % len(fails))
        for m, t, act, exp in fails:
            print('  %s %r: got %r want %r' % (m, t, act.decode('utf-8','replace'), exp.decode('utf-8','replace')))
    total_fail = len(fails)
    if not a.keep:
        subprocess.run(['rm', '-rf', tmp])
    else:
        print('\nfixtures kept in ' + tmp)
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
