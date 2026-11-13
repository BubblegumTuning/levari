#!/usr/bin/env bash
# tools/build_sdl2_win98.sh — bootstrap the era SDL 2.0.8 static prefix
# for the Win98 legacy build (branch Win98; the tracked recipe replacing
# the hand-built ~/sdl2-win98 prefix). The prefix itself stays OUTSIDE
# git (house convention: SDL2 is never vendored, the tarball is pinned
# here by URL + sha256, mirroring the legacy floor SDL treatment).
#
# Requires the EPEL mingw32 cross:
#   dnf install mingw32-gcc mingw32-gcc-c++ mingw32-zlib mingw32-zlib-static
# (the static zlib comes from CRB; without libz.a the final exe imports
# zlib1.dll).
#
# usage: build_sdl2_win98.sh [prefix]      (default: $HOME/sdl2-win98)
# Offline: SDL2_WIN98_TARBALL=/path/to/SDL2-2.0.8.tar.gz skips the fetch
# (the sha256 is verified either way).
#
# Why 2.0.8 is the era floor (measured, not recalled): 2.0.2 lacks
# SDL_CreateRGBSurfaceWithFormat + SDL_PIXELFORMAT_RGBA32 (used by
# skin.c) and its <DXGI.h> include dies on case-sensitive filesystems;
# every SDL2 vintage forces UNICODE=1, so the runtime wide-API question
# is MSLU territory and never an SDL-version choice. Zero source patches
# at 2.0.8. -std=gnu11 is REQUIRED: modern gcc defaults to C23 where
# SDL 2.0.8's implicit declarations are hard errors.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
prefix=${1:-$HOME/sdl2-win98}
ver=2.0.8
sha=edc77c57308661d576e843344d8638e025a7818bff73f8fbfab09c3c5fd092ec
tarball=SDL2-$ver.tar.gz

command -v i686-w64-mingw32-gcc >/dev/null 2>&1 || {
    echo "i686-w64-mingw32-gcc missing: dnf install mingw32-gcc mingw32-gcc-c++ mingw32-zlib mingw32-zlib-static" >&2
    exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src=${SDL2_WIN98_TARBALL:-}
if [ -z "$src" ]; then
    src=$tmp/$tarball
    echo "== fetching $tarball"
    curl -sSL -o "$src" "https://www.libsdl.org/release/$tarball"
fi
[ -f "$src" ] || { echo "tarball not found: $src" >&2; exit 1; }
echo "$sha  $src" | sha256sum -c -

tar xzf "$src" -C "$tmp"
cd "$tmp/SDL2-$ver"
echo "== configuring (host i686-w64-mingw32, static, era CFLAGS)"
./configure --host=i686-w64-mingw32 --build=x86_64-pc-linux-gnu \
    --prefix="$prefix" --enable-static --disable-shared \
    CFLAGS="-O2 -std=gnu11" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null

# the era consumer reads SDL2_INCLUDE_DIRS/SDL2_LIBDIR and uses BOTH
# include styles (<SDL2/SDL.h> and <SDL.h>) — the tracked template
# exposes the parent AND the SDL2 dir.
mkdir -p "$prefix/cmake"
cp "$root/tools/win98-sdl2-config.cmake" "$prefix/cmake/sdl2-config.cmake"

echo "== era SDL $ver static prefix ready: $prefix"
echo "== levari era configure: cmake -B build-win98 \\"
echo "     -DCMAKE_TOOLCHAIN_FILE=$root/tools/mingw-w64-win98-toolchain.cmake \\"
echo "     -DSDL2_DIR=$prefix/cmake -DLV_WIN9X=ON -DCMAKE_BUILD_TYPE=Release"
