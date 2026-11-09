#!/usr/bin/env python3
"""ffmatrix — cross-major ABI matrix for the ffmpeg backend (2026-11-07).

Proves the multi-ABI pick (src/decode/ffabi.h + ffab.c + vendor/ffmpeg):
the SAME binary must decode ffmpeg formats on several libav majors and
honestly refuse majors it carries no variant for.

Legs (all run the same ffver harness: probe triad + m4a open/read/seek/
re-read; see test/ffabi/ffver.c for the link recipe):
  ubuntu:24.04    ffmpeg 6.1.x  lavu 58 -> probe=1, m4a decodes
  debian:trixie   ffmpeg 7.1.x  lavu 59 -> probe=1, m4a decodes
  debian:bookworm ffmpeg 5.1.x  lavu 57 -> probe=1, m4a decodes
  ffmpeg-8.1.3    built from the vendored tarball inside trixie (no
                  distro ships 8.x) lavu 60 -> probe=1, m4a decodes
  host            native ffver          -> probe=1, m4a decodes
  debian:bullseye ffmpeg 4.3.x  lavu 56 -> probe=3, FAIL open (outside the set)
  --mac mac112    ffmpeg 9.x    lavu 61 -> probe=1 (optional ssh leg)

ffver for the container legs is linked with tools/zig-cc-2.17 against a
glibc-floor (legacy zig) build tree so it runs on every distro here.

Usage:  python3 test/ffabi/ffmatrix.py [--mac] [--images-only-pulled]
Requires: podman, the legacy tree built (build-rel-linux-legacy-zig),
test-assets present (git-ignored copyright fixtures).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# image -> (expected probe state, expect decode ok?)
LEGS = [
    ("docker.io/library/ubuntu:24.04",   1, True),
    ("docker.io/library/debian:trixie",  1, True),
    ("docker.io/library/debian:bookworm", 1, True),
    ("docker.io/library/debian:bullseye", 3, False),
]

# 8.x runtime leg: no distro ships ffmpeg 8 — build the vendored tarball
FF8_VER = "8.1.3"
FF8_SHA = "7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"
FF8_URL = f"https://ffmpeg.org/releases/ffmpeg-{FF8_VER}.tar.xz"

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), **kw)

def link_portable_ffver(legacy, out):
    """zig-cc link of ffver against the legacy tree: glibc-2.17 floor."""
    objs = [f"{legacy}/CMakeFiles/levari.dir/src/decode/decode.c.o",
            f"{legacy}/CMakeFiles/levari.dir/src/config/config.c.o",
            f"{legacy}/CMakeFiles/levari.dir/src/tag/tag.c.o",
            f"{legacy}/CMakeFiles/levari.dir/third_party/stb_vorbis.c.o"]
    objs += [f"{legacy}/CMakeFiles/dec_ffab{a}.dir/src/decode/ffab.c.o" for a in (5, 6, 7, 8, 9)]
    cmd = ([f"{ROOT}/tools/zig-cc-2.17", "-O2", "-DNDEBUG", "-D_FILE_OFFSET_BITS=64",
            "-DHAVE_FFMPEG", "-Isrc", "-o", out, "test/ffabi/ffver.c"] + objs +
           [f"{legacy}/libmodplug.a", f"{legacy}/libsnes_spc.a",
            "-lpthread", "-ldl", "-lm", "-lc++"])
    r = sh(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit("ffver link failed:\n" + r.stderr[-2000:])

def frames_of(output):
    m = re.search(r"(\d+) frames", output)
    return int(m.group(1)) if m else None

def check(output, want_probe, want_decode, label, collect=None):
    m = re.search(r"probe=(\d+)", output)
    if not m:
        print(f"FAIL {label}: no probe line\n{output}"); return False
    ok = True
    got = int(m.group(1))
    if got != want_probe:
        print(f"FAIL {label}: probe={got} want {want_probe}"); ok = False
    decoded = re.search(r"seek=ok", output) is not None
    refused = "FAIL open" in output
    if want_decode and not decoded:
        print(f"FAIL {label}: m4a did not decode\n{output}"); ok = False
    if not want_decode and not refused:
        print(f"FAIL {label}: expected FAIL open refusal\n{output}"); ok = False
    if collect is not None:
        collect.append((label, frames_of(output)))
    if ok:
        first = output.strip().splitlines()[0]
        print(f"PASS {label}: {first}")
    return ok

def container_leg(stage, image, want_probe, want_decode, collect=None):
    # bullseye is fully archived: deb.debian.org pools rot (stale
    # security indexes 404), route its apt at archive.debian.org
    arch = ("sed -i -e 's|security.debian.org|archive.debian.org/debian-security|g' "
            "-e 's|deb.debian.org/debian-security|archive.debian.org/debian-security|g' "
            "-e 's|deb.debian.org/debian|archive.debian.org/debian|g' /etc/apt/sources.list; ") \
           if "bullseye" in image else ""
    inner = ("export DEBIAN_FRONTEND=noninteractive HOME=/w; "
             + arch +
             "apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null 2>&1; "
             "apt-get install -y -qq ffmpeg >/dev/null 2>&1; "
             "ffmpeg -version 2>/dev/null | head -1 | cut -d, -f1; "
             "/w/ffver /w/test.m4a")
    r = sh(["podman", "run", "--rm", "-v", f"{stage}:/w:Z", image, "bash", "-c", inner],
           capture_output=True, text=True)
    return check(r.stdout, want_probe, want_decode, image.split("/")[-1], collect)

def ff8_tarball():
    """Cache the FF8_VER tarball host-side, sha256-checked against the
    vendor README pin (the leg runs the EXACT source the ff8 variant was
    compiled against — same-source/same-major is the point)."""
    import hashlib, urllib.request
    cache = os.path.join(tempfile.gettempdir(), "ffmatrix-cache")
    os.makedirs(cache, exist_ok=True)
    tp = os.path.join(cache, f"ffmpeg-{FF8_VER}.tar.xz")
    if os.path.exists(tp):
        h = hashlib.sha256(open(tp, "rb").read()).hexdigest()
        if h == FF8_SHA:
            return tp
        os.remove(tp)
    urllib.request.urlretrieve(FF8_URL, tp)
    if hashlib.sha256(open(tp, "rb").read()).hexdigest() != FF8_SHA:
        sys.exit(f"ffmpeg-{FF8_VER} tarball sha256 != vendor pin")
    return tp

def ff8_leg(stage, collect=None):
    """lavu-60 runtime leg: build ffmpeg FF8_VER shared libs from the
    vendored tarball inside debian:trixie (no distro ships 8.x), then run
    the same ffver against them via LD_LIBRARY_PATH. Minimal pure-C build
    (mov demux + aac decoder + file protocol — all the harness opens)."""
    shutil.copy(ff8_tarball(), stage)
    with open(f"{stage}/ff8.sha256", "w") as f:
        f.write(f"{FF8_SHA}  ffmpeg-{FF8_VER}.tar.xz\n")
    inner = ("export DEBIAN_FRONTEND=noninteractive HOME=/w; "
             "apt-get update -qq >/dev/null 2>&1; "
             "apt-get install -y -qq gcc make pkg-config xz-utils >/dev/null 2>&1; "
             "cd /w && sha256sum -c ff8.sha256 >/dev/null || exit 1; "
             f"tar xJf ffmpeg-{FF8_VER}.tar.xz && cd ffmpeg-{FF8_VER} && "
             "./configure --disable-everything --enable-shared --disable-static "
             "--disable-programs --disable-doc --disable-autodetect --disable-asm "
             "--disable-network --enable-protocol=file --enable-demuxer=mov "
             "--enable-decoder=aac --enable-decoder=alac --prefix=/w/ff8 > cfg.log 2>&1 || "
             "{ echo CONFIGURE-FAIL; tail -15 cfg.log; exit 1; }; "
             "make -j$(nproc) > build.log 2>&1 || { echo BUILD-FAIL; tail -15 build.log; exit 1; }; "
             "make install > /dev/null 2>&1 || exit 1; "
             "LD_LIBRARY_PATH=/w/ff8/lib /w/ffver /w/test.m4a")
    r = sh(["podman", "run", "--rm", "-v", f"{stage}:/w:Z",
            "docker.io/library/debian:trixie", "bash", "-c", inner],
           capture_output=True, text=True)
    label = f"ffmpeg-{FF8_VER}-src"
    ok = check(r.stdout, 1, True, label, collect)
    if f"ver={FF8_VER}" not in r.stdout:
        print(f"FAIL {label}: runtime ver != {FF8_VER}"); ok = False
    return ok

def mac_leg(host, collect=None):
    """build ffver on the Mac against build-mac's objects, run it there.
    mac trees are pre-tier-split: decode.c.o is canonical, no dispatch."""
    link = """
cd ~/levari || exit 1
gcc -std=gnu11 -Isrc -o /tmp/ffver-mac test/ffabi/ffver.c \\
  build-mac/CMakeFiles/levari.dir/src/decode/decode.c.o \\
  build-mac/CMakeFiles/levari.dir/src/config/config.c.o \\
  build-mac/CMakeFiles/levari.dir/src/tag/tag.c.o \\
  $(find build-mac/CMakeFiles -path '*dec_ffab*.dir*' -name '*.o') \\
  build-mac/CMakeFiles/levari.dir/third_party/stb_vorbis.c.o \\
  $(find build-mac -name 'libmodplug.a' | head -1) \\
  $(find build-mac -name 'libsnes_spc.a' | head -1) \\
  -lpthread -ldl -lm -lstdc++ -undefined dynamic_lookup 2>&1 | tail -5
[ -x /tmp/ffver-mac ] && /tmp/ffver-mac "$(ls ~/levari/test-assets/*.m4a | head -1)"
"""
    # pipe the script via bash -s: shell-quoting a remote command in a
    # python string is a trap; stdin is not
    r = sh(["ssh", host, "bash -s"], input=link, capture_output=True, text=True)
    return check(r.stdout, 1, True, host, collect)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mac", nargs="?", const="mac112", default=None)
    ap.add_argument("--legacy", default=f"{ROOT}/build-rel-linux-legacy-zig")
    args = ap.parse_args()
    m4a = glob_m4a()
    stage = tempfile.mkdtemp(prefix="scratch-ffmatrix-")
    counts = []  # (label, frame-count) per leg, for the cross-check
    link_portable_ffver(args.legacy, f"{stage}/ffver")
    shutil.copy(m4a, f"{stage}/test.m4a")
    ok = all([container_leg(stage, img, p, d, counts) for img, p, d in LEGS])
    ok &= ff8_leg(stage, counts)
    host_out = sh([f"{ROOT}/build/ffver", m4a], capture_output=True, text=True).stdout
    ok &= check(host_out, 1, True, "host", counts)
    if args.mac:
        ok &= mac_leg(args.mac, counts)
    frame_vals = [f for _, f in counts if f is not None]
    if len(set(frame_vals)) > 1:
        print(f"FAIL frame-count mismatch across decode legs: {counts}")
        ok = False
    elif frame_vals:
        print(f"frame counts agree: {frame_vals[0]} x{len(frame_vals)} legs")
    shutil.rmtree(stage, ignore_errors=True)
    print("FFMATRIX: " + ("ALL PASS" if ok else "FAILURES"))
    sys.exit(0 if ok else 1)

def glob_m4a():
    d = os.path.join(ROOT, "test-assets")
    for f in sorted(os.listdir(d)):
        if f.endswith(".m4a"):
            return os.path.join(d, f)
    sys.exit("no .m4a fixture in test-assets/ (copyright-ignored; copy first)")

if __name__ == "__main__":
    main()
