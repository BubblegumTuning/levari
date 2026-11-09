#!/usr/bin/env python3
"""Regression pin: scan-thread SIGFPE on frameless MP3 (2026-11-05).

A tag-only .mp3 (ID3v2 header, zero audio frames) OPENS SUCCESSFULLY in
minimp3_ex with samples == 0 AND channels == 0. dec_open()'s
`samples / ch` then divided by zero — integer 0/0 traps like n/0 — and
SIGFPE'd the lvscan thread mid-scan of a network share (kernel journal:
"traps: lvscan[...] trap divide error"). dec_open() now skips the
division when ch == 0 and its existing validity gate rejects the
decoder, so probe and scan report failure instead of dying.

Checks (all against the real binary, no daemon needed — `library scan`
runs in-process):
  1. --probe on the tag-only fixture -> exit 0, "FAIL open"
     (pre-fix: exit -8 / SIGFPE)
  2. library scan over a dir holding the fixture -> exit 0 and the row
     lands in the index (pre-fix: the scan process dies, rc -8)
  3. --probe on test/sample.mp3 still decodes (seek=ok, frames > 0) —
     the guard must not disturb the valid MP3 path
  4. the index header carries a creation marker ("<version> <YYMMDD>")
     and fits pre-0.3.4 readers' 32-byte header probe invisibly; with an
     optional second build dir, that older binary must list the marker
     index byte-identically and append to it cleanly
  5. hand-muxed Ogg fixtures: Opus/Vorbis/Ogg-FLAC durations come from the
     end-of-stream granulepos (Opus minus preskip); a tag-only stream
     reports 0, never a guess

Usage: python3 test/probe/probecheck.py [build-dir] [old-build-dir]
       (default build dir: ./build)
"""
import os, re, subprocess, struct, sys, tempfile, shutil

build = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build")
old_build = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(build, "levari")
if not os.path.exists(BIN):
    sys.exit(f"no binary at {BIN} — build first")

def tagonly_fixture(path):
    """ID3v2.3 tag (TIT2 + NUL padding), no MPEG frames — sniff routes
    'ID3' to F_MP3, minimp3_ex opens it and leaves info zeroed."""
    def syncsafe(n):
        return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])
    body = b"TIT2" + struct.pack(">I", 12) + b"\x00\x00" + b"\x00" + b"Ghost Track"
    body += b"\x00" * 256
    with open(path, "wb") as f:
        f.write(b"ID3\x03\x00\x00" + syncsafe(len(body)) + body)

# --- hand-muxed Ogg fixtures (same CRC/lacing rules as test/songinfo) ----
def _ogg_tbl():
    tbl = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
        tbl.append(r)
    return tbl
_OGG_TBL = _ogg_tbl()

def ogg_page(htype, seq, granule, body):
    nseg = len(body) // 255 + 1
    lacing = bytes([255] * (nseg - 1)) + bytes([len(body) % 255])
    page = (b"OggS" + bytes([0, htype]) +
            granule.to_bytes(8, "little", signed=True) +
            (1234).to_bytes(4, "little") + seq.to_bytes(4, "little") +
            (0).to_bytes(4, "little") + bytes([nseg]) + lacing + body)
    crc = 0
    for b in page:
        crc = ((crc << 8) ^ _OGG_TBL[((crc >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return page[:22] + crc.to_bytes(4, "little") + page[26:]

def comment_packet(entries):
    pkt = (len("levari-probecheck").to_bytes(4, "little") + b"levari-probecheck" +
           len(entries).to_bytes(4, "little"))
    for e in entries:
        b = e.encode("utf-8")
        pkt += len(b).to_bytes(4, "little") + b
    return pkt

def ogg_opus(path, secs, preskip=312):
    head = (b"OpusHead" + bytes([1, 2]) + preskip.to_bytes(2, "little") +
            (48000).to_bytes(4, "little") + b"\x00\x00\x00")
    pages = (ogg_page(2, 0, 0, head) +
             ogg_page(0, 1, 0, b"OpusTags" + comment_packet(["TITLE=Granule Opus"])))
    if secs:
        pages += ogg_page(0, 2, preskip + 48000 * secs, b"\x00" * 40)  # fake audio
    open(path, "wb").write(pages)

def ogg_vorbis(path, secs):
    ident = (b"\x01vorbis" + (0).to_bytes(4, "little") + b"\x02" +
             (44100).to_bytes(4, "little") + (0).to_bytes(12, "little") + b"\x01")
    # trailing 0x01 = the comment packet's framing flag (ffmpeg writes it;
    # mutagen demands it — verified against both this session)
    cmt = b"\x03vorbis" + comment_packet(["TITLE=Granule Vorbis"]) + b"\x01"
    pages = (ogg_page(2, 0, 0, ident) +
             ogg_page(0, 1, 0, cmt) +
             ogg_page(0, 2, 44100 * secs, b"\x00" * 40))
    open(path, "wb").write(pages)

def ogg_flac(path, secs):
    si = bytearray(34)                      # ffmpeg shape: total_samples == 0
    sr, ch, bps, total = 44100, 2, 16, 0
    si[10] = sr >> 12
    si[11] = (sr >> 4) & 0xFF
    si[12] = ((sr & 0xF) << 4) | ((ch - 1) << 1) | ((bps - 1) >> 4)
    si[13] = (total >> 32) & 0xF
    si[14:18] = (total & 0xFFFFFFFF).to_bytes(4, "big")
    # ffmpeg's exact shape: marker embeds fLaC + STREAMINFO only (packet
    # count 1), the VCBLOCK travels in its own packet, header 0x84 (last|4)
    marker = b"\x7fFLAC\x01\x00\x00\x01" + b"fLaC" + b"\x00\x00\x00\x22" + bytes(si)
    vcb = comment_packet(["TITLE=Granule OggFLAC"])
    pages = (ogg_page(2, 0, 0, marker) +
             ogg_page(0, 1, 0, b"\x84" + len(vcb).to_bytes(3, "big") + vcb) +
             ogg_page(0, 2, 44100 * secs, b"\x00" * 40))
    open(path, "wb").write(pages)

def run(home, args, binary=None):
    env = dict(os.environ, HOME=home,
               XDG_CONFIG_HOME=os.path.join(home, ".config"),
               XDG_RUNTIME_DIR=home)
    return subprocess.run([binary or BIN] + args, capture_output=True,
                          text=True, timeout=30, env=env)

fails = []
def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {name}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        fails.append(name)

scratch = tempfile.mkdtemp(prefix="probecheck-")
try:
    tagonly = os.path.join(scratch, "tagonly.mp3")
    tagonly_fixture(tagonly)

    # 1. probe must fail-open, not die
    r = run(scratch, ["--probe", tagonly])
    check("probe tag-only exits 0 (not SIGFPE)", r.returncode == 0,
          f"rc={r.returncode} out={r.stdout!r}")
    check("probe tag-only prints FAIL open", "FAIL open" in r.stdout, r.stdout)

    # 2. the user's exact path: library scan over a dir holding the fixture
    home2 = os.path.join(scratch, "scan-home")
    scan_dir = os.path.join(scratch, "lib")
    os.makedirs(home2); os.makedirs(scan_dir)
    shutil.copy(tagonly, scan_dir)
    r = run(home2, ["library", "scan", scan_dir])
    check("library scan exits 0 (not SIGFPE)", r.returncode == 0,
          f"rc={r.returncode}")
    idx = os.path.join(home2, ".config", "levari", "index")
    if not os.path.exists(idx):
        idx = os.path.join(home2, ".levari", "index")  # pre-XDG fallback shape
    row = ""
    head1 = ""
    if os.path.exists(idx):
        head1 = open(idx).readline().rstrip("\n")
        row = next((l for l in open(idx) if "tagonly.mp3" in l), "")
    check("scan indexed the fixture row with dur 0",
          "\t0\t" in row, f"index row: {row!r}")

    # 2b. creation marker: header names the writing version and date (UTC)
    check("index header carries creation marker",
          re.match(r"^#levari-index v2 \d+\.\d+\.\d+ \d{6}$", head1) is not None,
          f"header: {head1!r}")
    check("marker line fits old readers' 32-byte probe (<=31 incl NL)",
          len(head1) + 1 <= 31, f"{len(head1) + 1} bytes")

    # 2c. with an optional old build dir: the pre-marker binary must read
    # (and append to) the marker index with zero visible difference
    if old_build:
        old_bin = os.path.join(old_build, "levari")
        new_list = run(home2, ["library", "list"])
        old_list = run(home2, ["library", "list"], binary=old_bin)
        check("old binary list output byte-identical (marker invisible)",
              old_list.stdout == new_list.stdout,
              f"new={new_list.stdout!r} old={old_list.stdout!r}")
        r_old = run(home2, ["library", "scan", scan_dir], binary=old_bin)
        after = run(home2, ["library", "list"])
        check("old binary appends to marker index, rows intact",
              r_old.returncode == 0 and after.stdout == new_list.stdout,
              f"rc={r_old.returncode}")

    # 2d. urlIndex is a NEW file OUTSIDE the index format (streaming D5):
    # scan/list must never read, write or disturb it. Seed a tampered row
    # (embedded CR = evidence class) next to the index and prove byte-
    # identity across a scan and a list — probecheck never plays URLs,
    # and must not even see the file.
    urlidx = os.path.join(home2, ".levari", "urlIndex")
    os.makedirs(os.path.dirname(urlidx), exist_ok=True)
    with open(urlidx, "w") as f:
        f.write("file:///evil\rx\n")
    before = open(urlidx, "rb").read()
    r = run(home2, ["library", "scan", scan_dir])
    r2 = run(home2, ["library", "list"])
    check("scan/list leave urlIndex untouched (outside index format)",
          r.returncode == 0 and r2.returncode == 0 and
          open(urlidx, "rb").read() == before, urlidx)

    # 5. Ogg duration from the end-of-stream granulepos: Opus/Vorbis carry
    # no length in their headers and ffmpeg zeroes total_samples in Ogg
    # FLAC's STREAMINFO — the last page's granule is the only source.
    # Opus must also subtract the preskip. A tag-only stream (no page ever
    # ends a packet past zero samples) must report 0, never a guess.
    home3 = os.path.join(scratch, "ogg-home")
    oggdir = os.path.join(scratch, "ogglib")
    os.makedirs(home3); os.makedirs(oggdir)
    ogg_opus(os.path.join(oggdir, "g.opus"), secs=10)
    ogg_opus(os.path.join(oggdir, "bare.opus"), secs=0)
    ogg_vorbis(os.path.join(oggdir, "g.ogg"), secs=3)
    ogg_flac(os.path.join(oggdir, "g.flac"), secs=3)
    r = run(home3, ["library", "scan", oggdir])
    idx3 = os.path.join(home3, ".config", "levari", "index")
    if not os.path.exists(idx3):
        idx3 = os.path.join(home3, ".levari", "index")
    rows = {}
    if os.path.exists(idx3):
        for l in open(idx3):
            parts = l.split("\t")
            if len(parts) >= 5:
                rows[os.path.basename(parts[0])] = parts[4]
    for name, want in (("g.opus", "10"), ("g.ogg", "3"), ("g.flac", "3"),
                       ("bare.opus", "0")):
        check(f"ogg tail-granule duration {name}", rows.get(name) == want,
              f"got {rows.get(name)!r}, scan rc={r.returncode}, rows={rows!r}")

    # 3. valid MP3 path untouched
    r = run(scratch, ["--probe", os.path.join(repo, "test", "sample.mp3")])
    ok = r.returncode == 0 and "FAIL open" not in r.stdout and "seek=ok" in r.stdout
    check("valid sample.mp3 still probes ok", ok, f"rc={r.returncode} out={r.stdout!r}")
finally:
    shutil.rmtree(scratch, ignore_errors=True)

print(f"{len(fails)} failure(s)" if fails else "all green")
sys.exit(1 if fails else 0)
