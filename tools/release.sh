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

# legacy x86-64: pin the ISA floor to the original AMD64 baseline (SSE2 only).
# This host's gcc defaults to -march=x86-64-v3 (AVX2/MOVBE/LZCNT) which SIGILLs
# on pre-2008 CPUs; the X61s (2007 Merom) is the reference machine.
build build-rel-linux-legacy -DCMAKE_C_FLAGS="-march=x86-64 -mtune=generic" -DCMAKE_CXX_FLAGS="-march=x86-64 -mtune=generic"
# The distro crt objects carry an "x86 ISA needed: v3" property note that the
# linker propagates to the output, so glibc's loader aborts with "CPU ISA level
# is lower than required" on old CPUs even though our code is baseline
# ("x86 ISA used: baseline"). Strip the note from the legacy binary; we use no
# CET IBT/SHSTK features that live in it.
objcopy --remove-section=.note.gnu.property build-rel-linux-legacy/levari
package linux-legacy x86-64 build-rel-linux-legacy levari README.md assets/levari.desktop assets/levari.svg

sdl2=${SDL2_DIR:-$root/../sdl2-mingw/cmake}
[ -f "$sdl2/sdl2-config.cmake" ] || { echo "SDL2-mingw cmake dir not found: $sdl2 (set SDL2_DIR)" >&2; exit 1; }
build build-rel-win -DCMAKE_TOOLCHAIN_FILE="$root/tools/mingw-w64-toolchain.cmake" -DSDL2_DIR="$sdl2"
package win x86-64 build-rel-win levari.exe README.md

# pi targets join when their toolchains are ready, e.g.:
#   build build-rel-pi0 -DCMAKE_TOOLCHAIN_FILE=tools/armv6-toolchain.cmake -DLV_TUNE=arm1176jzf-s
#   package pi0 arm32 build-rel-pi0 levari README.md

echo
echo "release artifacts for v$ver (drag everything in $out/ into the release):"
ls -1 "$out"
