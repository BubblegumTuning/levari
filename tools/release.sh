#!/usr/bin/env bash
# tools/release.sh — build the release targets and package them as
# dist/release/levari-<target>.<arch>.<version>.zip: one folder holding
# every artifact, ready to drag into a GitHub release upload box.
# arch tokens match LV_ARCH in src/common.h (x86-64, arm32, arm64, x86).
#
# Targets built here: linux (native x86-64) and win (MinGW cross; SDL2_DIR
# overrides the SDL2-mingw cmake dir). Pi targets join the table below
# once their cross toolchains are set up (see docs/build.md).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ver=$(sed -n 's/^project(levari VERSION \([0-9.]\+\).*/\1/p' CMakeLists.txt)
[ -n "$ver" ] || { echo "no version found in CMakeLists.txt" >&2; exit 1; }

out=dist/release
rm -rf "$out"
mkdir -p "$out"

build() { # build <builddir> [extra cmake args...]
    local bdir=$1; shift
    echo "== configuring $bdir"
    cmake -B "$bdir" -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
    echo "== building $bdir"
    cmake --build "$bdir" -j"$(nproc)" >/dev/null
}

package() { # package <target> <arch> <builddir> <binary> [extra files...]
    local target=$1 arch=$2 bdir=$3 bin=$4; shift 4
    [ -f "$bdir/$bin" ] || { echo "missing $bdir/$bin" >&2; exit 1; }
    local stem=levari-$target.$arch.$ver
    local stage=$out/$stem
    mkdir -p "$stage"
    cp "$bdir/$bin" "$stage/"
    local f
    for f in "$@"; do cp "$f" "$stage/"; done
    (cd "$out" && zip -qr "$stem.zip" "$stem")
    rm -rf "$stage"
    echo "== $out/$stem.zip"
}

build build-rel-linux
package linux x86-64 build-rel-linux levari README.md assets/levari.desktop assets/levari.svg

# legacy x86-64: the "drop it anywhere" build. zig cc pins BOTH floors in one
# toolchain: ISA floor at the original AMD64 baseline (zig's generic x86_64
# target; this host's gcc defaults to -march=x86-64-v3 which SIGILLs on
# pre-2008 CPUs — the X61s/2007 Merom is the reference machine) and glibc
# floor at 2.17 (RHEL/CentOS 7 era: zig resolves every symbol to its
# 2.17-vintage form via its bundled per-version import libs). crt objects
# come from zig, so the old gcc path's objcopy ISA-note surgery is not
# needed — readelf -n is empty on the output. Needs the floor-built static
# SDL2 bootstrapped once (see docs/build.md, legacy section).
export SDL2_LEGACY_PREFIX=${SDL2_LEGACY_PREFIX:-$HOME/sdl2-legacy-x86_64}
[ -f "$SDL2_LEGACY_PREFIX/lib64/libSDL2.a" ] || {
    echo "floor-built static SDL2 missing at $SDL2_LEGACY_PREFIX (see docs/build.md, legacy section)" >&2; exit 1; }
build build-rel-linux-legacy-zig -DCMAKE_TOOLCHAIN_FILE="$root/tools/zig-legacy-toolchain.cmake" -DLEVAR_SIMD_TIERS=OFF
# floor gates (hard fails — a silent regression here ships a binary that
# refuses to load or SIGILLs on the legacy target):
#   glibc: highest GLIBC_* symbol version must be 2.17 (no GLIBCXX_* at
#   all: libc++/libmodplug are linked statically).
#   ISA: zero AVX, zero SSE3+ scalars. The umbrella '\tv[a-z0-9]+\t'
#   matches every AT&T AVX/AVX2 mnemonic (all start with 'v') in objdump's
#   tab-delimited mnemonic column; the enumeration covers the SSE3-SSE4.2/
#   BMI instructions without the 'v' prefix. lzcnt appears ~23x, inside
#   dr_flac's rice decoder behind its runtime CPUID guard
#   (drflac__is_lzcnt_supported) — benign; do NOT add lzcnt to the gate.
#   NOTE the '|| true': on a clean binary grep -c prints 0 AND exits 1 —
#   under set -euo pipefail that would kill the script before packaging.
floor=$(objdump -T build-rel-linux-legacy-zig/levari | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
[ "$floor" = "GLIBC_2.17" ] || { echo "legacy glibc floor is $floor, expected GLIBC_2.17" >&2; exit 1; }
# QUOTING PITFALL: the $'...' covers ONLY the tab-umbrella half.  bash
# turns \b inside $'...' into a BACKSPACE character, so wrapping the
# word-boundary half would silently match NOTHING (verified both ways:
# the corrected split pattern hits 8063 on the host v3-native build and
# 0 on the legacy zig build; the fully-$'...' pattern hit 0 on both —
# a dead scalar gate).  No tzcnt/rep-bsf false positive exists on the
# zig binary (measured 0).  The two pattern segments are ADJACENT quoted
# words (no space between them) — one space would hand grep the second
# word as a FILE argument.
n=$(objdump -d build-rel-linux-legacy-zig/levari | grep -cE $'\tv[a-z0-9]+([ \t]|$)|''\b(movbe|tzcnt|pshufb|pblendvb|palignr|lddqu|movntdqa|ptest|roundps|pminud|pminsb|packusdw|pcmpgtq|popcnt|crc32|andn|bextr|blsr|mulx)\b' || true)
[ "$n" = 0 ] || { echo "legacy ISA scan: $n AVX/SSE3+ refs found" >&2; exit 1; }
package linux-legacy x86-64 build-rel-linux-legacy-zig levari README.md assets/levari.desktop assets/levari.svg

sdl2=${SDL2_DIR:-$root/../sdl2-mingw/cmake}
[ -f "$sdl2/sdl2-config.cmake" ] || { echo "SDL2-mingw cmake dir not found: $sdl2 (set SDL2_DIR)" >&2; exit 1; }
build build-rel-win -DCMAKE_TOOLCHAIN_FILE="$root/tools/mingw-w64-toolchain.cmake" -DSDL2_DIR="$sdl2"
package win x86-64 build-rel-win levari.exe README.md

# Pi Zero 1 / Pi 1 (ARMv6 hard-float). Wired up via tools/armv6-toolchain.cmake.
# Needs the whole cross stack — gcc AND g++ AND ARM-built SDL2 (distro tree
# under /usr/arm-linux-gnueabihf/lib, or SDL2_PI0_DIR pointing at an SDL2
# cmake package dir). A missing piece SKIPS the target with a notice —
# discovering it mid-cmake would abort the whole release under set -e.
# Cortex-a7/a15 Pi 2-4 still need their own toolchain files to join.
pi0skip=""
command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || \
    pi0skip="arm-linux-gnueabihf-gcc not installed"
command -v arm-linux-gnueabihf-g++ >/dev/null 2>&1 || \
    pi0skip="$pi0skip${pi0skip:+; }arm-linux-gnueabihf-g++ not installed"
if [ -z "$pi0skip" ]; then
    if [ -n "${SDL2_PI0_DIR:-}" ]; then
        [ -f "$SDL2_PI0_DIR/sdl2-config.cmake" ] || \
            pi0skip="SDL2_PI0_DIR cmake dir not found: $SDL2_PI0_DIR"
    else
        ls /usr/arm-linux-gnueabihf/lib/libSDL2* >/dev/null 2>&1 || \
            pi0skip="$pi0skip${pi0skip:+; }ARM SDL2 (no /usr/arm-linux-gnueabihf/lib/libSDL2*, set SDL2_PI0_DIR)"
    fi
fi
if [ -z "$pi0skip" ]; then
    pi0cmake=(-DCMAKE_TOOLCHAIN_FILE="$root/tools/armv6-toolchain.cmake" -DLV_TUNE=arm1176jzf-s)
    if [ -n "${LV_PI0_SYSROOT:-}" ]; then pi0cmake+=(-DLV_PI0_SYSROOT="$LV_PI0_SYSROOT"); fi
    if [ -n "${SDL2_PI0_DIR:-}" ]; then pi0cmake+=(-DSDL2_DIR="$SDL2_PI0_DIR"); fi
    build build-rel-pi0 "${pi0cmake[@]}"
    # ISA floor gate for the ARM1176JZF-S (Pi Zero 1 / Pi 1): the board is
    # ARMv6Z + VFPv3-D16. Doubles, vmla and vdiv/vsqrt are HARDWARE there
    # (vdiv/vsqrt date back to VFPv1, and the ARM1176 has VFPv3 regardless
    # — a correct ARMv6 build of dsp.c/player.c CONTAINS them: the gate
    # must NOT flag them; see the ARM note in src/dsp/dsp.c). What must
    # never appear: NEON (q registers, vld1..4/vst1..4, vdup, vshl, vtbl,
    # vmov.i*/vmov.f16 — plain vmov.f32/f64 are VFPv1 moves and must NOT
    # be flagged), the full-VFPv3 register bank d16-d31, and Thumb-2
    # (movw/movt plus .w/.n encoding suffixes — the ARM1176 has
    # no Thumb-2 at all; this is the pattern that catches distro
    # crt/libgcc statically linked into the binary as ARMv7 thumb2).
    # Known residual: NEON vmov.f32/f64 against d0-d15 is indistinguish-
    # able from VFP moves and not flagged; the -march pin is the floor,
    # this scan is the backstop.
    # The HOST objdump cannot disassemble an ARM binary at all (0 ins —
    # packaging would skip on every build): always use the cross binutils,
    # sysroot bin/ first, then PATH. EXPECT the scan to FIRE on distro
    # arm-linux-gnueabihf userland — Debian/Ubuntu armhf is compiled for
    # ARMv7 (NEON memcpy in libc etc.): that is the forcing function, the
    # fix is LV_PI0_SYSROOT, never relaxing the gate. A firing gate — or a
    # missing/failed cross objdump — SKIPS pi0 packaging; a hard fail here
    # would abort the whole release under set -e. NOTE the '|| true': on a
    # clean scan grep -c prints 0 AND exits 1.
    pi0objdump=arm-linux-gnueabihf-objdump
    if [ -n "${LV_PI0_SYSROOT:-}" ] &&
       [ -x "$LV_PI0_SYSROOT/bin/arm-linux-gnueabihf-objdump" ]; then
        pi0objdump="$LV_PI0_SYSROOT/bin/arm-linux-gnueabihf-objdump"
    fi
    dis=build-rel-pi0/pi0-isa-scan.txt
    ins=0 n=0 scanned=0
    if { command -v "$pi0objdump" >/dev/null 2>&1 || [ -x "$pi0objdump" ]; } &&
       "$pi0objdump" -d build-rel-pi0/levari > "$dis" 2>/dev/null; then
        scanned=1
        ins=$(grep -cE '^[[:space:]]+[0-9a-f]+:' "$dis" || true)
        # NOTE the operand guards on d/q: objdump prints small addresses as
        # bare hex labels (d18:, d24 <sym+...>) which otherwise read as
        # d16-d31 register refs; a register token is always followed by
        # ',', '-', '}' or ']', a bare label by ':' or '<'.
        n=$(grep -cE '\bq(1[0-5]|[0-9])(,|-|}|\])|\bv(ld|st)[1-4]\b|\b(vdup|vshl|vtbl)\b|\bvmov\.(i[0-9]+|f16)|\bd(1[6-9]|2[0-9]|3[01])(,|-|}|\])|\b(movw|movt)\b|\.[wn]\b' "$dis" || true)
    fi
    rm -f "$dis"
    if [ "$scanned" = 0 ]; then
        echo "== pi0 ISA scan: cross objdump ($pi0objdump) missing or cannot disassemble build-rel-pi0/levari; pi0 packaging skipped"
    elif [ "$ins" = 0 ]; then
        echo "== pi0 ISA scan: objdump found no code in build-rel-pi0/levari; pi0 packaging skipped"
    elif [ "$n" != 0 ]; then
        echo "== pi0 ISA scan: $n ARMv7/NEON refs (distro armhf userland is ARMv7; pass LV_PI0_SYSROOT for a shippable pi0); pi0 packaging skipped"
    else
        package pi0 arm32 build-rel-pi0 levari README.md
    fi
else
    echo "== pi0 skipped: $pi0skip"
fi

echo
echo "release artifacts for v$ver (drag everything in $out/ into the release):"
ls -1 "$out"
