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

## Legacy Linux build (x86-64, "drop it anywhere")

Targets the oldest realistic x86-64 Linux: CPU at the original AMD64
baseline and glibc >= 2.17 (RHEL/CentOS 7 era, ~2014 onward). Built with
zig cc, which ships per-glibc-version import libraries: every symbol
resolves to its 2.17-vintage form, and the crt objects come from zig
(no distro ISA notes to strip). SDL2 is statically linked from a
floor-built copy; X11/ALSA/Pulse/ffmpeg/opus stay dlopen'd at runtime,
so the binary needs only libc/libm/libz/libdbus (its own soname ABI is
stable since 2007) on the target.

Host prereqs (RHEL-family): `sudo dnf install zig libX11-devel
libXext-devel libXcursor-devel libXinerama-devel libXScrnSaver-devel
libXrandr-devel libXi-devel alsa-lib-devel` (zig >= 0.16; EPEL package
works).

One-time SDL2 bootstrap (SDL2 must join at the same floor — the official
SDL2 2.32.10 release tarball from libsdl.org, the exact bytes the floor
build was made with, sha256
`5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165`):

    curl -sfO https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
    tar xzf SDL2-2.32.10.tar.gz
    cmake -S SDL2-2.32.10 -B sdl2-legacy-build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$PWD/levari/tools/zig-cc-2.17 \
      -DCMAKE_INSTALL_PREFIX=$HOME/sdl2-legacy-x86_64 \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
      -DSDL_DBUS=OFF -DSDL_WAYLAND=OFF -DSDL_OPENGL=OFF \
      -DSDL_GLES1=OFF -DSDL_GLES2=OFF -DSDL_TESTS=OFF -DSDL_TEST=OFF
    cmake --build sdl2-legacy-build -j"$(nproc)" && cmake --install sdl2-legacy-build

Then the player (tools/zig-legacy-toolchain.cmake wraps `zig cc/c++
-target x86_64-linux-gnu.2.17`; zig rejects `-march=x86-64`/`-mtune` —
the generic x86_64 target IS the baseline, verify with the scan below):

    SDL2_LEGACY_PREFIX=$HOME/sdl2-legacy-x86_64 cmake -B build-legacy-zig \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=tools/zig-legacy-toolchain.cmake
    cmake --build build-legacy-zig -j"$(nproc)"

Verify the floors before shipping (release.sh runs the first two as hard
gates):

    objdump -T build-legacy-zig/levari | grep -oE 'GLIBC[A-Z+]*_[0-9.]+' | sort -uV | tail -3
    #   highest GLIBC_* must be 2.17; no GLIBCXX_* at all (libc++ static)
    objdump -d build-legacy-zig/levari | grep -cE \
      $'\tv[a-z0-9]+([ \t]|$)|\b(movbe|tzcnt|pshufb|pblendvb|palignr|lddqu|movntdqa|ptest|roundps|pminud|pminsb|packusdw|pcmpgtq|popcnt|crc32|andn|bextr|blsr|mulx)\b' || true   # must be 0
    #   lzcnt appears ~23x, always inside dr_flac's rice decoder behind its
    #   runtime CPUID guard (drflac__is_lzcnt_supported) — benign; do not
    #   add lzcnt to the gate. readelf -n shows no property notes.
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build-legacy-zig/levari --selftest 3 test/example.ogg
    #   success signature: state=1 pos>0

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

Cross-building the pi0 target (release.sh's pi block, via
`tools/armv6-toolchain.cmake`) needs `arm-linux-gnueabihf-gcc` AND
`-g++` AND ARM-built SDL2 (distro `/usr/arm-linux-gnueabihf/lib`, or
`SDL2_PI0_DIR` pointing at an SDL2 cmake package dir); a missing piece
skips the target with a notice. CAUTION: the distro cross userland is
compiled for ARMv7 — its crt/libc/SDL2 contain NEON/VFPv3 and the
result may SIGILL on a real Pi Zero. release.sh therefore runs an ISA
floor scan on the binary (q registers, vld1..4/vst1..4, vdiv/vsqrt);
when it fires the pi0 zip is NOT packaged. A shippable pi0 needs
`LV_PI0_SYSROOT=<path>` with a raspbian-style ARMv6 hardfp sysroot
(headers/libs/crt — and compilers if the tree ships them under
`bin/`). The pi0 zip has never been run on real Pi Zero hardware here.

## Release packaging

    tools/release.sh

Builds the release targets (native linux, legacy linux via the zig
glibc-2.17 toolchain — needs the floor-built static SDL2 from the legacy
section above, default prefix `~/sdl2-legacy-x86_64`; MinGW win;
`SDL2_DIR` points at the SDL2-mingw cmake dir) and writes
`dist/release/levari-<target>.<arch>.<version>.zip`
(arch tokens match the in-app `LV_ARCH` display: `x86-64`, `arm32`,
`arm64`, `x86`)
(version read from CMakeLists.txt) — one folder with every artifact,
ready to drag into a GitHub release upload. The pi0 target joins when
its cross prerequisites are installed and only packages if its ISA floor
scan passes (see the ARM section and the pi block in the script).

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

## macOS build

Build natively on Apple Silicon (verified on arm64, macOS 26).
Requires the Xcode Command Line Tools plus Homebrew:

    brew install cmake pkg-config sdl2-compat opus

(Homebrew's SDL2 on Apple Silicon is the `sdl2-compat` formula;
CMake's SDL2 lookup and the Homebrew link paths are handled in
CMakeLists.) The standard invocation then applies:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

The result is a proper `levari.app` GUI bundle (double-clickable, no
Terminal window, exempt from App Nap), not a bare binary. CLI control
keeps working through the binary inside the bundle:

    build/levari.app/Contents/MacOS/levari status

Runtime shape on macOS: audio goes through the SDL backend
(CoreAudio). Config and the media-library index live at
`~/.config/levari`, the same as on Linux (a set `XDG_CONFIG_HOME`
wins there too). Daemon/CLI IPC uses POSIX sockets; with no
`XDG_RUNTIME_DIR` (the usual on macOS) the daemon binds a
randomly-named `/tmp/levari-<uid>-*.sock` and publishes the path in
the config dir, so clients find it without guessing. Opus and
aac/m4a/wma are runtime `dlopen` probes, not build options: opus
resolves as `libopus.0.dylib`/`libopus.dylib`, and libav* is searched
in the Homebrew keg dirs (`/opt/homebrew/opt/ffmpeg/lib`,
`/usr/local/opt/ffmpeg/lib`) including versioned
`libavformat.N.dylib` variants — another install location can be
pointed at with `libav.dir = <dir>` in the config. MPRIS is
Linux-only and is not built here.
