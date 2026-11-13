# zig-legacy32-toolchain.cmake — legacy 32-bit x86 Linux builds via zig cc.
#
# Floor: PENTIUM II class — zig's x86 target pinned -mcpu=pentium2
# (i686: CMOV yes, SSE NO; the SSE-free result is verified by the
# objdump ISA scan on the final binary, never trusted from the flag —
# the generic x86 libc carries SSE and would SIGILL on a P2, measured
# 167 xmm refs vs 0 pinned). LIBC FLOOR: glibc >= the importlib pin
# (currently 2.17, measured on the final binary with objdump -T like
# the x86-64 legacy zip). NOT musl: a static musl binary cannot
# dlopen ("Dynamic loading not supported", measured on zig's musl,
# static AND static-pie, i386 AND x86_64) — and the floor SDL's
# X11/ALSA backends are dlopen-only (SDL_X11_SHARED=ON), so a musl
# static link silently ships a player that cannot open a window or a
# sound device while still exiting 0 (SDL falls back to dummy video;
# measured on the pentium4: SDL_Init "x11 not available" only when
# forced, silent dummy otherwise — black Xvfb framebuffer, zero
# windows in xwininfo, fake-green selftests). The gnu target links
# dynamically against the pinned floor: dlopen works and the whole
# X11/ALSA runtime is the system's own.
#
# Cross from x86-64 host (RHEL-10 has no i686 packages). zig enforces
# its own sysroot for cross targets, so host x86_64 libs cannot link;
# PKG_CONFIG_LIBDIR REPLACES the default search dirs (PATH would
# only extend them) so the host's x86_64 dbus-1.pc cannot turn MPRIS on
# and poison the link with a 64-bit libdbus — 32-bit legacy builds ship
# without MPRIS by design (old boxes have no desktop media-key stack
# worth the dependency anyway).
#
# Usage (SDL2 + zlib must be built at the same floor first — run
# tools/build_sdl2_legacy32.sh, then):
#   cmake -B build-legacy32 -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/zig-legacy32-toolchain.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cc-i386-p2")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cxx-i386-p2")

# zig 0.16's lld segfaults on CMake's linker dependency-file generation
# for cross targets; disable the feature (only costs rebuild precision).
# Must be FORCEd cache entries (compiler-id detection resets plain set()).
set(CMAKE_C_COMPILER_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_LINKER_DEPFILE_SUPPORTED FALSE CACHE BOOL "" FORCE)

# the floor-built static prefix (tools/build_sdl2_legacy32.sh; default
# ~/sdl2-legacy-i386): pkg-config sees ONLY it, and FindZLIB takes the
# floor static libz (ZLIB::ZLIB becomes the .a — the target needs no
# shared libs at all).
set(SDL2_LEGACY32_PREFIX "$ENV{SDL2_LEGACY32_PREFIX}" CACHE PATH
    "prefix of the floor-built static SDL2/zlib for 32-bit x86")
if(NOT SDL2_LEGACY32_PREFIX)
    set(SDL2_LEGACY32_PREFIX "$ENV{HOME}/sdl2-legacy-i386" CACHE PATH "" FORCE)
endif()
if(EXISTS "${SDL2_LEGACY32_PREFIX}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_LIBDIR} "${SDL2_LEGACY32_PREFIX}/lib/pkgconfig")
    set(ZLIB_ROOT "${SDL2_LEGACY32_PREFIX}" CACHE PATH "" FORCE)
    # TRAP: FindZLIB PREFERS the shared lib when the prefix carries both
    # .so and .a — resolving libz.so.1 flips the zig link to DYNAMIC musl
    # (interpreter /lib/ld-musl-i386.so.1, NEEDED libz.so.1 + libc.so, and
    # the dynamic musl crt/SSE baseline SIGILLs the pentium2 floor;
    # measured 12406 xmm-scan hits vs 0 with the static link). Pin .a.
    set(ZLIB_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif()
