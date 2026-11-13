#!/usr/bin/env bash
# tools/build_sdl2_legacy32.sh — bootstrap the floor-built static zlib +
# SDL2 prefix for the legacy 32-bit x86 build (the "zig treatment" of
# the i386 route: pentium2-class ISA, musl static, zero distro deps).
# The prefix lives OUTSIDE git (house convention: SDL2 is never
# vendored; tarballs are pinned by URL + sha256).
#
# Requires zig >= 0.16 (EPEL package works).
#
# usage: build_sdl2_legacy32.sh [prefix]    (default: $HOME/sdl2-legacy-i386)
# Offline: SDL2_TARBALL / ZLIB_TARBALL=/path skips the fetch (the sha256
# is verified either way).
#
# ISA facts (measured, do not "optimize" away): the zig x86 target
# DEFAULTS to SSE-carrying libc code (167 xmm refs measured) — on a
# Pentium II that SIGILLs. -mcpu=pentium2 is load-bearing (0 xmm,
# cmov allowed = i686, the P2 baseline). The libc floor is the gnu
# importlib pin (glibc >= 2.17), NOT musl: static musl cannot dlopen,
# which would dead-code the SDL dlopen-only X11/ALSA backends while
# the binary still exits 0 (measured on real hardware — see the
# toolchain file).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
prefix=${1:-$HOME/sdl2-legacy-i386}

sdl_ver=2.32.10
sdl_sha=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
zlib_ver=1.3.1
zlib_sha=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

command -v zig >/dev/null 2>&1 || { echo "zig missing (EPEL package)" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fetch() { # fetch <env-var> <url> <sha> -> echoes the file path
    local ev=$1 url=$2 sha=$3 f
    local local_path=${!ev:-}
    if [ -n "$local_path" ]; then f=$local_path; else
        f=$tmp/$(basename "$url"); curl -sSL -o "$f" "$url"; fi
    [ -f "$f" ] || { echo "tarball missing: $f" >&2; exit 1; }
    echo "$sha  $f" | sha256sum -c - >/dev/null && echo "$f"
}

echo "== zlib $zlib_ver static (zig i386-p2)"
# Two zlib-1.3.1-cmake traps: (1) its CMakeLists builds the SHARED zlib
# target UNCONDITIONALLY (no ZLIB_BUILD_SHARED option exists at this
# version — passing one is a dead name; BUILD_SHARED_LIBS only steers
# WIN32 import libs) — a libz.so in the prefix makes FindZLIB hand the
# levari link a shared object, which silently flips zig to DYNAMIC musl
# (NEEDED libz.so.1 + libc.so, musl crt SSE baseline; measured 12406
# xmm hits vs 0 static), so the .so files are deleted after install;
# (2) the static objects are built WITHOUT -fPIC — a PIE final link then
# dies on R_386_32 relocations. Both pinned here.
zt=$(fetch ZLIB_TARBALL "https://github.com/madler/zlib/releases/download/v$zlib_ver/zlib-$zlib_ver.tar.gz" "$zlib_sha")
tar xzf "$zt" -C "$tmp"
cmake -S "$tmp/zlib-$zlib_ver" -B "$tmp/zlib-b" \
    -DCMAKE_C_COMPILER="$root/tools/zig-cc-i386-p2" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
cmake --build "$tmp/zlib-b" -j"$(nproc)" >/dev/null
cmake --install "$tmp/zlib-b" >/dev/null
rm -f "$prefix/lib/libz.so" "$prefix/lib/libz.so.1" "$prefix/lib/libz.so.$zlib_ver"

echo "== SDL2 $sdl_ver static (zig i386-p2, minimal config)"
st=$(fetch SDL2_TARBALL "https://www.libsdl.org/release/SDL2-$sdl_ver.tar.gz" "$sdl_sha")
tar xzf "$st" -C "$tmp"
# X11 headers are arch-neutral, but SDL2's CheckX11 does
# find_path(X_INCLUDEDIR NAMES X11/Xlib.h) and a host-root hit returns
# /usr/include — every compile after it gets -I/usr/include and the
# glibc headers mask musl's (the gnu/stubs-32.h signature, same
# contamination family as the win98 zlib story). Stage the headers under
# a clean dir and prime the cache var; X11 links nothing here (
# SDL_X11_SHARED=ON records only SONAMEs and dlopens at runtime).
# CMAKE_LIBRARY_PATH: for a 32-bit CMAKE_SYSTEM_PROCESSOR cross, CMake's
# find_library searches /usr/lib only — on this RHEL-family host the
# 64-bit devel .so symlinks live in /usr/lib64, so point at them (they
# are used for SONAMEs in shared/dlopen mode, never linked).
x11inc=$tmp/x11inc
mkdir -p "$x11inc"
cp -a /usr/include/X11 "$x11inc/"
# ALSA joins the same way: FindALSA's /usr/include lands on the
# INTERFACE_INCLUDE_DIRECTORIES of ALSA::ALSA, which SDL spreads over
# every later compile. The x86-64 floor SDL carries exactly one audio
# dlopen name (libasound, measured), so the 32-bit floor keeps ALSA
# (headers staged clean; asound is dlopen'd at runtime, never linked)
# and drops PulseAudio/PipeWire detection (their pkg-config dirs are
# /usr/include too, and old boxes will not have them anyway).
alsainc=$tmp/alsainc
mkdir -p "$alsainc"
cp -a /usr/include/alsa "$alsainc/" 2>/dev/null || { echo "no host /usr/include/alsa — install alsa-lib-devel" >&2; exit 1; }
# Two cross-build traps in SDL2's CheckX11: (1) X11_LIB must be non-empty
# for X11 to join at all, and FindLibraryAndSONAME derives the runtime
# dlopen name purely from the FILENAME (no objdump) — so the stub
# archive below is named exactly like the real runtime soname; (2) the
# generic-events support check LINKS ${X11_LIB}, which no musl cross can
# satisfy (zig refuses host dynamic libs by name), and a false there
# typedef-collides SDL_x11xinput2.h at compile time. The link is
# therefore impossible, not informative: HAVE_XGENERICEVENT is seeded
# TRUE (XFree86/Xorg has shipped XGetEventData since X1.2, 2004 — far
# under the era floor; the call itself rides SDL's dlopen wrappers).
# Checks that merely degrade from the refused link: XKB-symbol fast path,
# XInput2 multitouch, XFixes — all modern irrelevancies, off by choice.
mkdir -p "$tmp/stub32"
cat > "$tmp/x11stub.c" <<'STUB'
void XNextEvent(void) {}
void XGetEventData(void) {}
void XFreeEventData(void) {}
STUB
"$root/tools/zig-cc-i386-p2" -c -fPIC "$tmp/x11stub.c" -o "$tmp/x11stub.o"
zig ar rcs "$tmp/stub32/libX11.so.6" "$tmp/x11stub.o"
# SDL2's SIMD defaults ABOVE the pentium2 floor in two ways, both
# measured: (1) SDL_SSE/SDL_SSE2/SDL_SSE3/SDL_SSEMATH default ON for
# x86 — each feature-checks its -m flag via try_compile (pinned clang
# always passes) then APPENDS the flag to EXTRA_CFLAGS GLOBALLY, so
# EVERY SDL unit compiles above the pin (13441 xmm refs across
# libSDL2.a, even the math units). Killed with per-knob OFF.
# (2) SDL's MMX blitters (SDL_blit_A.c etc.) are guarded by the COMPILER
# MACRO __MMX__, which the pentium2 CPU defines — but clang on 32-bit
# implements the packed __m64 intrinsics as SSE2 (measured: _mm_xor_si64
# "requires target feature 'sse2'"), so they cannot compile below SSE2
# at all. SDL_ASSEMBLY=OFF is NOT the fix (the blit guards ignore it).
# -U__MMX__ undefines the macro for every SDL unit: the intrinsic
# guards select their pure-C fallbacks and SDL's own HAVE_MMX check
# fails cleanly (no -mmmx joins). P2 hardware HAS MMX; opting out is a
# perf choice forced by clang, pure C is correctness-safe.
cmake -S "$tmp/SDL2-$sdl_ver" -B "$tmp/sdl-b" \
    -DX_INCLUDEDIR="$x11inc" \
    -DALSA_INCLUDE_DIR="$alsainc" \
    -DX11_LIB="$tmp/stub32/libX11.so.6" \
    -DHAVE_XGENERICEVENT=TRUE \
    -DCMAKE_LIBRARY_PATH=/usr/lib64 \
    -DSDL_SSE=OFF -DSDL_SSE2=OFF -DSDL_SSE3=OFF -DSDL_SSEMATH=OFF \
    -DCMAKE_C_FLAGS="-U__MMX__" -DCMAKE_CXX_FLAGS="-U__MMX__" \
    -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
    -DCMAKE_C_COMPILER="$root/tools/zig-cc-i386-p2" \
    -DCMAKE_CXX_COMPILER="$root/tools/zig-cxx-i386-p2" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
    -DSDL_DBUS=OFF -DSDL_WAYLAND=OFF -DSDL_OPENGL=OFF \
    -DSDL_OPENGLES=OFF \
    -DSDL_TESTS=OFF -DSDL_TEST=OFF \
    -DSDL_X11_SHARED=ON -DSDL_X11_XCB=OFF \
    -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF \
    -DSDL_HIDAPI=OFF -DSDL_RENDER=OFF -DSDL_VULKAN=OFF >/dev/null
cmake --build "$tmp/sdl-b" -j"$(nproc)" >/dev/null
cmake --install "$tmp/sdl-b" >/dev/null

echo "== floor prefix ready: $prefix"
grep -q 'x86' "$prefix/lib/pkgconfig/sdl2.pc" 2>/dev/null || true
echo "== levari: cmake -B build-legacy32 -DCMAKE_BUILD_TYPE=Release \\"
echo "     -DCMAKE_TOOLCHAIN_FILE=$root/tools/zig-legacy32-toolchain.cmake"
