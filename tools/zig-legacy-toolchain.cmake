# zig-legacy-toolchain.cmake — legacy x86-64 Linux builds via zig cc.
#
# Floor: x86-64 baseline CPU (zig's generic x86_64 target; verified by the
# objdump ISA scan, not trusted from flags) + glibc 2.17 (RHEL/CentOS 7 era,
# i.e. the binary drops onto any x86-64 Linux from ~2014 onward). zig ships
# per-version glibc import libraries, so every referenced symbol resolves to
# its oldest-versioned form; the crt objects come from zig, not the distro,
# so the "ISA needed: v3" note surgery of the gcc legacy build is not needed
# here (still verified with readelf -n).
#
# The host's /usr/lib64 is deliberately NOT on the linker search path (zig
# enforces this for cross targets): distro shared libs at RHEL-10 symbol
# floors would silently defeat the point. Only two external libs join, both
# by absolute path, both floor-safe because their own ABIs are ancient and
# unversioned, so the TARGET machine's copy satisfies them at runtime:
#   - libz (found by CMake's FindZLIB, absolute path already)
#   - libdbus-1 (resolved to an absolute path in CMakeLists for the
#     optional MPRIS support; libdbus-1.so.3 ABI stable since 2007)
# All other dependencies (SDL2 static, libmodplug, snes_spc, libc++) are
# compiled at the floor and linked in; X11/ALSA/Pulse/ffmpeg/opus stay
# dlopen'd at runtime.
#
# Usage (SDL2 must be built at the same floor first — see docs/build.md):
#   PKG_CONFIG_PATH=<sdl2-legacy>/lib64/pkgconfig cmake -B build-legacy-zig \
#     -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=tools/zig-legacy-toolchain.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cc-2.17")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cxx-2.17")

# zig 0.16's lld segfaults on CMake's linker dependency-file generation for
# this cross target; disable the feature (only costs rebuild precision).
# Must be a FORCEd cache entry: plain set() is overwritten by compiler-id
# detection after the toolchain file is read.
set(CMAKE_C_COMPILER_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)

# static SDL2 cross-built at the floor; keep it ahead of any system dir
set(SDL2_LEGACY_PREFIX "$ENV{SDL2_LEGACY_PREFIX}" CACHE PATH "prefix of the floor-built static SDL2")
if(SDL2_LEGACY_PREFIX)
    set(ENV{PKG_CONFIG_PATH} "${SDL2_LEGACY_PREFIX}/lib64/pkgconfig")
endif()
