# Win98 legacy cross toolchain (branch Win98; parked-features dossier
# "Windows 98 legacy build"). NEVER use for the flagship win artifact.
#
# Shape: mingw-w64 runtime on the i686 target with the win95/98/ME OS
# floor declared (0x0410) so headers refuse post-9x declarations at our
# call sites; our own era guards compile under -DLV_WIN9X=1. Era SDL2
# (static, see this branch's notes) must be supplied via -DSDL2_DIR.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32 /usr/i686-w64-mingw32/sys-root/mingw)
# Strict ONLY modes: a cross build must never resolve headers/libs from
# the host root (an earlier BOTH probe pulled the host zlib in, whose
# include dir then shadowed the sysroot pthread.h with glibc's).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 0x0410 = Windows 98 SE floor. Note SDL_windows.h #undef-s _WIN32_WINNT
# to its own 0x501 inside SDL TUs; ours (daemon.c etc.) keep 0x0410.
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINDOWS=0x0410 -DWINVER=0x0410")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
