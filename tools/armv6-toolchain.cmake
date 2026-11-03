# Pi Zero 1 / Pi 1 cross build: ARMv6 (ARM1176JZF-S), hard-float VFP.
# release.sh passes -DLV_TUNE=arm1176jzf-s which adds -mcpu=arm1176jzf-s;
# the flags here pin the ABI floor so the codegen (and release.sh's pi0
# ISA floor scan) matches the oldest supported board. No NEON: minimp3
# keeps its plain scalar decoder on this board (see CMakeLists LV_TUNE
# block).
#
# USERLAND CAVEAT: the distro arm-linux-gnueabihf tree (Debian/Ubuntu
# /usr/arm-linux-gnueabihf) is compiled for ARMv7 — its crt/libc/SDL2
# contain NEON/VFPv3 (NEON memcpy etc.) and the resulting binary may
# SIGILL on a real ARMv6 Pi Zero. Pass -DLV_PI0_SYSROOT=<path> with a
# raspbian-style ARMv6 hardfp sysroot: headers/libs (and crt) come from
# there; if the tree also ships host-runnable cross compilers under
# <path>/bin they are picked up, else the distro cross compilers are
# used. release.sh's ISA floor scan is the forcing function: without a
# sysroot it fires on the ARMv7 userland and pi0 packaging is SKIPPED,
# never silently shipped.
set(CMAKE_SYSTEM_NAME Linux)
# armv6l, not plain 'arm': that is what makes the ^armv6 branch of the
# LV_TUNE block in CMakeLists run. checked consumer: the only other read
# of CMAKE_SYSTEM_PROCESSOR is the x86 tier gate (matches ^(x86_64|AMD64)$)
# — unaffected.
set(CMAKE_SYSTEM_PROCESSOR armv6l)

# The file is re-included inside try_compile scratch projects (via the
# saved CMakeSystem.cmake), but command-line knobs like LV_PI0_SYSROOT
# are NOT carried over there — branching on them again would flip the
# else branch, re-warn and repoint the find roots mid-configure. The
# compiler, flags and system name are already forwarded from the main
# project by then, so leave a scratch project alone.
if(CMAKE_BINARY_DIR MATCHES "CMakeScratch")
    return()
endif()


# -march=armv6 is pinned here and -mcpu=<tune> comes from LV_TUNE: a
# non-ARMv6 tune (e.g. someone reuses this file with -DLV_TUNE=cortex-a7)
# is a contradictory request the compiler only grumbles about. refuse it.
if(DEFINED LV_TUNE AND NOT LV_TUNE STREQUAL "" AND NOT LV_TUNE MATCHES "^arm1176")
    message(FATAL_ERROR
        "armv6-toolchain: LV_TUNE='${LV_TUNE}' is not an ARMv6 core. "
        "this file pins -march=armv6; use arm1176jzf-s (Pi Zero 1 / Pi 1) "
        "or a matching toolchain file for newer boards.")
endif()

if(LV_PI0_SYSROOT)
    set(CMAKE_SYSROOT ${LV_PI0_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${LV_PI0_SYSROOT})
    if(NOT CMAKE_C_COMPILER)
        find_program(PI0_CC NAMES arm-linux-gnueabihf-gcc
                     HINTS "${LV_PI0_SYSROOT}/bin" NO_DEFAULT_PATH)
        if(NOT PI0_CC)
            find_program(PI0_CC NAMES arm-linux-gnueabihf-gcc
                         NO_CMAKE_FIND_ROOT_PATH)
        endif()
        if(NOT PI0_CC)
            message(FATAL_ERROR
                "armv6-toolchain: no arm-linux-gnueabihf-gcc found "
                "(LV_PI0_SYSROOT=${LV_PI0_SYSROOT})")
        endif()
        set(CMAKE_C_COMPILER ${PI0_CC})
    endif()
    if(NOT CMAKE_CXX_COMPILER)
        find_program(PI0_CXX NAMES arm-linux-gnueabihf-g++
                     HINTS "${LV_PI0_SYSROOT}/bin" NO_DEFAULT_PATH)
        if(NOT PI0_CXX)
            find_program(PI0_CXX NAMES arm-linux-gnueabihf-g++
                         NO_CMAKE_FIND_ROOT_PATH)
        endif()
        if(NOT PI0_CXX)
            message(FATAL_ERROR
                "armv6-toolchain: no arm-linux-gnueabihf-g++ found "
                "(LV_PI0_SYSROOT=${LV_PI0_SYSROOT})")
        endif()
        set(CMAKE_CXX_COMPILER ${PI0_CXX})
    endif()
else()
    set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
    if(NOT CMAKE_C_COMPILER)
        set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
    endif()
    if(NOT CMAKE_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
    endif()
    message(WARNING
        "pi0 build WITHOUT LV_PI0_SYSROOT: the distro arm-linux-gnueabihf "
        "userland is ARMv7 (NEON/VFPv3 in crt/libc/SDL2) — the binary may "
        "SIGILL on a real Pi Zero. release.sh's ISA scan will skip "
        "packaging when it fires; pass -DLV_PI0_SYSROOT=<armv6-hardfp-root> "
        "for a shippable pi0.")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv6 -mfpu=vfp -mfloat-abi=hard")
