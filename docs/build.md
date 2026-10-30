# Build

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Requires: SDL2 dev headers, a C++ compiler (for the vendored libmodplug
module decoder). Everything else is vendored in `third_party/`.
If `libavformat/libavcodec/libavutil` dev packages are present, CMake
enables them automatically for aac/m4a/wma support; without them the
player builds and runs, just without those formats. On Linux, if
`dbus-devel` (pkg-config `dbus-1`) is present, MPRIS support is built
in (desktop media keys, player widgets, `playerctl`); without it the
player runs the same, just off the desktop's radar.

Optional: `-DENABLE_MCP=ON` also builds `levari-mcp`, a stdio MCP
(Model Context Protocol) server for LLM control of the player
(see [usage.md](usage.md)). Off by default, so release builds don't
contain it.

## Linux install

    sudo cmake --install build --prefix /usr

Installs the binary plus `levari.desktop` and the hicolor icon. The
dock/alt-tab icon on GNOME is resolved from the installed desktop file
(matched via `StartupWMClass=levari`), not from the window icon the app
sets at runtime, so it only appears after installing (per-user works
too: copy the desktop file to `~/.local/share/applications/` and the
svg to `~/.local/share/icons/hicolor/scalable/apps/`).

## ARM build (Raspberry Pi)

Build natively on the Pi (or in an arm64/armhf Linux container matching
the Pi's OS). Pass `-DLV_TUNE=<cpu>` to tune for the board:

    cmake -B build -DCMAKE_BUILD_TYPE=Release -DLV_TUNE=arm1176jzf-s # Pi Zero 1 / Pi 1 (armv6)
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DLV_TUNE=cortex-a7   # Pi 2 (32-bit)
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DLV_TUNE=cortex-a72  # Pi 4 (64-bit)

On 32-bit armv7 `LV_TUNE` also enables NEON, which switches the mp3
decoder to its SIMD path (~2-3x faster). The Pi Zero 1 and Pi 1 are
armv6 (ARM1176: VFPv2, no NEON): they get plain VFP instead and the mp3
decoder stays on its scalar path, which still decodes in real time at
1 GHz; NEON flags on those boards would SIGILL. On aarch64 NEON is
used automatically with or without `LV_TUNE`.

## Release packaging

    tools/release.sh

Builds the release targets (native linux, MinGW win; `SDL2_DIR` points
at the SDL2-mingw cmake dir) and writes `dist/release/levari-<target>.<arch>.<version>.zip`
(arch tokens match the in-app `LV_ARCH` display: `x86-64`, `arm32`,
`arm64`, `x86`)
(version read from CMakeLists.txt) — one folder with every artifact,
ready to drag into a GitHub release upload. Pi zips join the set once
their cross toolchains are set up (see the pi block in the script).

## Windows build

Cross-compile with MinGW-w64 (or build natively in MSYS2). Requires the
SDL2 MinGW devel package from https://libsdl.org (SDL2-devel-mingw.zip)
and a MinGW zlib (static preferred):

    x86_64-w64-mingw32-... # install mingw-w64 gcc/g++ and mingw-zlib
    cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64-toolchain.cmake \
          -DSDL2_DIR=/path/to/SDL2-2.32.10/cmake -DCMAKE_BUILD_TYPE=Release
    cmake --build build-win

Produces a self-contained `levari.exe` (console app; SDL2 and zlib are
linked statically, only system DLLs are required at runtime).
On Windows the player uses the SDL audio backend (WASAPI under the
hood), named pipes (`\\.\pipe\levari-<user>`) for daemon/CLI IPC,
`%APPDATA%\levari` for config and `%LOCALAPPDATA%\levari` for the
library index. Opus playback works if `libopus-0.dll` is next to the
exe; aac/m4a/wma need the libav* DLLs (not supported in the Windows
build yet).
