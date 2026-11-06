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

    # 3. valid MP3 path untouched
    r = run(scratch, ["--probe", os.path.join(repo, "test", "sample.mp3")])
    ok = r.returncode == 0 and "FAIL open" not in r.stdout and "seek=ok" in r.stdout
    check("valid sample.mp3 still probes ok", ok, f"rc={r.returncode} out={r.stdout!r}")
finally:
    shutil.rmtree(scratch, ignore_errors=True)

print(f"{len(fails)} failure(s)" if fails else "all green")
sys.exit(1 if fails else 0)
