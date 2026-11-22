cmake_minimum_required(VERSION 3.16)
project(levari VERSION 0.3.8 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Dev identity: a build that does not sit exactly on a release cut commit
# embeds a "-dev-<shortsha>" suffix into the build-ID string (options title
# bar, MCP version), so a dev artifact can never masquerade as a shipped
# one. A release cut is the convention already kept by the commit protocol:
# HEAD's subject starts with "Version <project-version>." AND the tree is
# clean (untracked files do not count; they do not enter the binaries).
# The suffix NEVER enters LV_VERSION itself — the library index header
# embeds the bare version inside its 31-byte budget and must not churn per
# commit. The verdict is computed at CONFIGURE time: commit without
# re-running cmake and the running build keeps the old string. A checkout
# without .git (a git-archive sync copy, an extracted package) cannot
# self-identify and defaults to no suffix — pass -DLV_DEV_SUFFIX="-dev-<sha>"
# explicitly there when reproducing a dev build.
set(LV_DEV_SUFFIX "" CACHE STRING
    "build-ID suffix for trees that cannot self-identify (no .git); auto-computed from git otherwise")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
        OUTPUT_VARIABLE lv_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        OUTPUT_VARIABLE lv_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --format=%s
        OUTPUT_VARIABLE lv_subject OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT lv_sha STREQUAL "" AND (NOT lv_dirty STREQUAL ""
            OR NOT lv_subject MATCHES "^Version ${PROJECT_VERSION}\\. "))
        set(LV_DEV_SUFFIX "-dev-${lv_sha}")
    else()
        set(LV_DEV_SUFFIX "")
    endif()
endif()
if(LV_DEV_SUFFIX)
    add_compile_definitions(LV_DEV_SUFFIX="${LV_DEV_SUFFIX}")
endif()

# -DLV_SANITIZE=ON builds the whole tree with ASan+UBSan (dev builds only;
# catches the races/overflows that the AFL front finds only via its harness)
option(LV_SANITIZE "build with ASan+UBSan" OFF)
if(LV_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# 64-bit off_t/fseeko everywhere: MinGW defaults off_t to 32-bit (long), so
# the RIFF chunk-skip math in tag.c could go negative and spin on files with
# chunks >= 2 GB; 32-bit Linux (Pi arm32/armv6) needs it for LFS too.
add_compile_definitions(_FILE_OFFSET_BITS=64)

# LV_STREAMING=OFF compiles URL playback out entirely (the per-user
# no-streaming option): URLs fail as plain nonexistent files and the URL
# button/window are absent from the UI. The define MUST reach every
# decode.c compile (levari, every SIMD tier object, levari-mcp) or the
# FF/struct layouts differ between objects — an ODR/ABI disaster — so it
# is directory-scoped, exactly where the tier compiles inherit. Default
# keeps streaming on; release.sh ships only the default.
option(LV_STREAMING "build network streaming (URL playback)" ON)
option(LV_WIN9X "win98 era floor guards: no post-9x kernel32/advapi32 imports (Win98-branch pipeline only; NEVER set on flagship win builds)" OFF)
if(LV_WIN9X)
    add_compile_definitions(LV_WIN9X)
    # NOTE: with the core/engine/ui static split, _CRTIMP= (see the WIN32
    # block below) must be applied to every PROJECT target individually —
    # it used to ride the single levari target that held all sources.
endif()
if(LV_STREAMING)
    add_compile_definitions(LV_URL_ENABLED=1)
endif()

if(WIN32)
    # MinGW cross builds: point SDL2_DIR at the SDL2-mingw cmake dir, e.g.
    #   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64-toolchain.cmake \
    #         -DSDL2_DIR=/path/to/SDL2-2.32.10/cmake
    find_package(SDL2 CONFIG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()
# NOTE: no zlib — the glyph blob moved to the vendored raw-LZMA1 decoder
# (third_party/lzma). If the Windows build ever needs zlib again, it is
# SDL2's own dependency, not levari's — prove it with the link line first.

# vendored libmodplug (public domain): mod/s3m/xm/it -> 44.1kHz pcm
# MODPLUG_BASIC_SUPPORT drops the exotic loaders (669/abc/amf/... kept
# out of the build); exotic variants fall back to the libav* backend
#
# these two libs parse untrusted files directly: they get the same
# hardening as levari itself (canaries, FORTIFY, PIE) and warnings stay
# visible (-Wall -Wextra with the vendored-noise subset suppressed), so
# an upstream regression cannot land silently behind a blanket -w
enable_language(CXX)
set(CMAKE_CXX_STANDARD 11)
set(VEND_WARN -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-comment
    -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough)
set(VEND_HARD -fstack-protector-strong -D_FORTIFY_SOURCE=2)
add_library(modplug STATIC
    third_party/libmodplug/fastmix.cpp
    third_party/libmodplug/load_it.cpp
    third_party/libmodplug/load_mod.cpp
    third_party/libmodplug/load_s3m.cpp
    third_party/libmodplug/load_wav.cpp
    third_party/libmodplug/load_xm.cpp
    third_party/libmodplug/mmcmp.cpp
    third_party/libmodplug/modplug.cpp
    third_party/libmodplug/snd_dsp.cpp
    third_party/libmodplug/snd_flt.cpp
    third_party/libmodplug/snd_fx.cpp
    third_party/libmodplug/sndfile.cpp
    third_party/libmodplug/sndmix.cpp
)
target_include_directories(modplug PRIVATE
    third_party/libmodplug third_party/libmodplug/libmodplug)
target_compile_definitions(modplug PRIVATE MODPLUG_BUILD HAVE_CONFIG_H MODPLUG_BASIC_SUPPORT)
if(WIN32)
    # no -flto on MinGW: LTO IR hides main() from archive member selection,
    # so mingw's crtexewin.o (main -> WinMain shim) wins and the link fails
    target_compile_options(modplug PRIVATE -Os ${VEND_HARD} ${VEND_WARN})
elseif(APPLE)
    target_compile_options(modplug PRIVATE -Os -flto ${VEND_HARD} ${VEND_WARN})
else()
    # -fPIE required so the static archive can join a PIE link
    target_compile_options(modplug PRIVATE -Os -flto -fPIE ${VEND_HARD} ${VEND_WARN})
endif()

# vendored snes_spc (LGPL 2.1, see third_party/snes_spc/LICENSE): SPC ->
# stereo 32kHz pcm through the C API in spc.h; fast DSP core (the one
# blargg recommends for music players)
add_library(snes_spc STATIC
    third_party/snes_spc/spc.cpp
    third_party/snes_spc/SNES_SPC.cpp
    third_party/snes_spc/SNES_SPC_misc.cpp
    third_party/snes_spc/SNES_SPC_state.cpp
    third_party/snes_spc/SPC_DSP.cpp
    third_party/snes_spc/SPC_Filter.cpp
)
target_include_directories(snes_spc PRIVATE third_party/snes_spc)

# vendored LZMA decoder (public domain 7-Zip SDK, see third_party/lzma/
# README.vendored.md): inflates the embedded glyph blob for src/skin/cjk_font.c
add_library(lzma STATIC third_party/lzma/LzmaDec.c)
target_include_directories(lzma PUBLIC third_party/lzma)
target_compile_options(lzma PRIVATE -Os ${VEND_HARD} ${VEND_WARN})
if(LV_SANITIZE AND UNIX AND NOT APPLE)
    # uninstrumented LzmaDec is small-code-model RIP-relative and links into
    # the PIE exe as-is, but ASan instrumentation emits absolute R_X86_64_32
    # .data refs and the link dies — same trap as modplug/snes_spc above,
    # which carry -fPIE unconditionally. Sanitize-only so flagship objects
    # stay byte-identical (the zig legacy tree must not see a flag change).
    target_compile_options(lzma PRIVATE -fPIE)
endif()
if(WIN32)
    target_compile_options(snes_spc PRIVATE -Os ${VEND_HARD} ${VEND_WARN})
elseif(APPLE)
    target_compile_options(snes_spc PRIVATE -Os -flto ${VEND_HARD} ${VEND_WARN})
else()
    # -fPIE required so the static archive can join a PIE link
    target_compile_options(snes_spc PRIVATE -Os -flto -fPIE ${VEND_HARD} ${VEND_WARN})
endif()

# x86-64 decoder codegen tiers: decode.c + stb_vorbis.c compiled once per
# tier (SSE2 baseline, AVX+FMA, AVX2); tier_rename.h suffixes every tier's
# symbols and src/decode/dispatch.c owns the public API, picking the best
# tier the CPU supports at runtime (LEVAR_DECODE=sse2|avx|avx2 to force).
# LEVAR_TIERS selects WHICH tiers ship: each tier costs roughly a full
# second copy of the decode stack, so a build should ship only what its
# target audience can execute — release.sh ships the flagship linux/win
# zips AVX2-only; the legacy zip is tier-less (plain decode.c at the
# baseline ISA IS its SSE2 floor). The lowest tier in the list is the
# runtime floor: a CPU below it cannot run the binary at all (dispatch.c
# picks the best COMPILED tier the CPU supports). Same source,
# bit-identical output ONLY with -ffp-contract=off: with the default
# (fast) contraction the compiler folds a multiply+add into one FMA
# rounding, and the avx/avx2 Vorbis output drifts +/-1 LSB from sse2
# (FLAC and WAV are unaffected — no fused ops). Tiers stay OFF for the
# legacy x86-64 build: tools/release.sh's ISA floor gate must not see VEX
# instructions, and old CPUs run the baseline code anyway.
set(LEVAR_TIERS "sse2;avx;avx2" CACHE STRING
    "decoder tiers to compile: any of sse2 avx avx2 (lowest shipped is the runtime floor; empty = tier-less build)")
option(LEVAR_SIMD_TIERS "runtime-dispatched x86 decoder tiers" ON)
set(LEVAR_TIERS_REQ "${LEVAR_TIERS}")  # the cache request
set(LEVAR_TIERS "")                    # resolved list of tiers to compile
if(LEVAR_TIERS_REQ AND LEVAR_SIMD_TIERS
        AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    include(CheckCCompilerFlag)
    # every tier compiles with LEVAR_TIER_BASE's -march=x86-64, and not
    # every compiler that reports "Clang" accepts it: zig cc is clang at
    # heart but spells CPU selection -mcpu, so it rejects the flag and
    # every tier then fails at BUILD time (a misconfigured legacy dir
    # used to die exactly that way). probe the floor flag too and fall
    # back to the tier-less shape (plain decode.c, no dispatch) when it
    # does not compile
    check_c_compiler_flag("-march=x86-64" LEVAR_CC_MARCH)
    if(LEVAR_CC_MARCH)
        check_c_compiler_flag("-mavx -mfma" LEVAR_CC_AVX)
        check_c_compiler_flag("-mavx2 -mfma -mbmi -mbmi2 -mlzcnt" LEVAR_CC_AVX2)
        foreach(t IN LISTS LEVAR_TIERS_REQ)
            if(NOT t MATCHES "^(sse2|avx|avx2)$")
                message(FATAL_ERROR "LEVAR_TIERS: unknown tier '${t}' (allowed: sse2 avx avx2; empty list = tier-less)")
            endif()
        endforeach()
        # canonical order regardless of the request's spelling
        foreach(t sse2 avx avx2)
            if(t IN_LIST LEVAR_TIERS_REQ)
                # a requested tier the compiler cannot spell is a hard
                # error, never a silent downgrade: release.sh pins avx2
                # for the flagship zips and a quietly SSE2-only "flagship"
                # would be a wrong artifact with a right exit code
                if(t STREQUAL "avx" AND NOT LEVAR_CC_AVX)
                    message(FATAL_ERROR "LEVAR_TIERS: 'avx' requested but the compiler rejects -mavx -mfma")
                endif()
                if(t STREQUAL "avx2" AND NOT LEVAR_CC_AVX2)
                    message(FATAL_ERROR "LEVAR_TIERS: 'avx2' requested but the compiler rejects -mavx2 -mfma -mbmi -mbmi2 -mlzcnt")
                endif()
                list(APPEND LEVAR_TIERS ${t})
            endif()
        endforeach()
    else()
        message(STATUS "LEVAR_SIMD_TIERS: requested but off — compiler rejects -march=x86-64 (zig cc)")
    endif()
endif()

# Shared hardening/optimization flags, identical for BOTH executables
# (levari, levari-mcp) — the two copies were verbatim except that the
# levari WIN32 link also carried -mwindows. That one stays with the GUI
# target alone (see its explicit target_link_options): levari-mcp is a
# stdio server and must never grow a GUI subsystem.
# Split note: the compile flags live in lv_harden_compile so the three
# static libs below get EXACTLY the per-TU flags every source always had
# (CMake rejects the plain-signature target_link_options CMake uses here
# on STATIC archives — link options stay executables-only, and the two
# executables carry the same link line they always did).
function(lv_harden_compile tgt)
    if(WIN32)
        target_compile_options(${tgt} PRIVATE -Os -ffunction-sections -fdata-sections -fstack-protector-strong -Wall -Wextra -Wno-unused-parameter)
    elseif(APPLE)
        target_compile_options(${tgt} PRIVATE -Os -flto -ffunction-sections -fdata-sections -fstack-protector-strong -Wall -Wextra -Wno-unused-parameter)
    else()
        # hardening: PIE (ASLR for the image), stack canaries, full RELRO
        # (-z now makes the GOT read-only), FORTIFY source-level buffer checks
        target_compile_options(${tgt} PRIVATE -Os -flto -ffunction-sections -fdata-sections -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(WIN32 AND LV_WIN9X)
        # _CRTIMP= neutralizes dllimport (the headers guard it with
        # #ifndef, _mingw.h). Why: mingw-w64 calls _stati64 THROUGH the
        # __imp__ thunk, so win98_fs.c's definition (attached to the exe
        # below) could never intercept stat() call sites and the thunk
        # member collided with it (multiple definition). Direct calls +
        # our definition = the CRT _stati64 import dies. Verified: no
        # prebuilt lib (SDL2, winpthread, modplug, snes_spc) references
        # _stati64, so nothing else regrows the import. It must reach
        # every PROJECT target — era builds only, NEVER flagship.
        target_compile_definitions(${tgt} PRIVATE _CRTIMP=)
    endif()
endfunction()
function(lv_harden tgt)
    lv_harden_compile(${tgt})
    if(WIN32)
        target_link_options(${tgt} PRIVATE -Wl,--gc-sections)
    elseif(APPLE)
        target_link_options(${tgt} PRIVATE -flto -Wl,-dead_strip)
    else()
        target_link_options(${tgt} PRIVATE -flto -pie -Wl,--gc-sections -Wl,-s -Wl,-z,relro,-z,now)
    endif()
endfunction()

# ---- static library targets (refactor item 3: one compile, two links) ----
# levari_core:  the ingest spine — config, tag, playlist, library + CLI,
#               daemon client, and the decode stack (dispatch.c when tiers
#               ship, plain decode.c+stb_vorbis.c otherwise; the tier and
#               ffab OBJECT libs are compiled once and attached to each
#               final executable, exactly as before the split, so mcp and
#               levari share every compile and no archive gains duplicate
#               member names).
# levari_engine: the playback spine — audio, player, dsp, viz, daemon,
#               and mpris when dbus was found.
# levari_ui:    the SDL skin layer — the ui.c split TUs, search, textedit,
#               x8bit, skin, cjk_font (LV_X8BIT/LV_PROFILE live here).
# The dependency direction is a strict DAG: ui -> engine -> core; nothing
# below ui is ever referenced upward (verified by include graph).
add_library(levari_core STATIC
    src/config/config.c
    src/tag/tag.c
    src/playlist/playlist.c
    src/library/library.c
    src/library/library_cli.c
    src/daemon/daemon_client.c
    src/platform/sock.c)
if(LEVAR_TIERS)
    target_sources(levari_core PRIVATE src/decode/dispatch.c)
else()
    target_sources(levari_core PRIVATE src/decode/decode.c third_party/stb_vorbis.c)
endif()
if(WIN32)
    # on Windows modplug.h otherwise declares the DLL-import form
    # (__imp_ModPlug_*) and the static modplug archive does not match;
    # decode.c is where modplug.h gets included
    target_compile_definitions(levari_core PRIVATE MODPLUG_BUILD)
endif()

add_library(levari_engine STATIC
    src/audio/audio.c
    src/player/player.c
    src/dsp/dsp.c
    src/viz/viz.c
    src/daemon/daemon.c)

add_library(levari_ui STATIC
    src/ui/ui.c
    src/ui/ui_drag.c
    src/ui/ui_pl.c
    src/ui/ui_eq.c
    src/ui/ui_file.c
    src/ui/ui_url.c
    src/ui/ui_opt.c
    src/ui/ui_info.c
    src/ui/textedit.c
    src/ui/search.c
    src/ui/x8bit.c
    src/skin/skin.c
    src/skin/cjk_font.c)

foreach(lv_lib levari_core levari_engine levari_ui)
    target_include_directories(${lv_lib} PUBLIC src third_party ${SDL2_INCLUDE_DIRS})
    lv_harden_compile(${lv_lib})
endforeach()
target_link_libraries(levari_engine PUBLIC levari_core)
target_link_libraries(levari_ui PUBLIC levari_engine)
# cjk_font.c inflates its blob through the vendored LZMA decoder
target_link_libraries(levari_ui PRIVATE lzma)

add_executable(levari src/main.c)
target_link_libraries(levari levari_ui levari_engine levari_core)

# 8-bit PseudoColor/GrayScale presentation path (src/ui/x8bit.c): needs
# X11 headers at COMPILE time only — Xlib is dlopen'd at runtime, so the
# legacy artifact's zero-link-time-X11 shape survives. Absent headers just
# disable the takeover (the Tier-0 guard in ui.c names the cause instead
# of crashing).
if(UNIX AND NOT APPLE)
    include(CheckIncludeFile)
    check_include_file(X11/Xlib.h HAVE_X11_XLIB_H)
    if(HAVE_X11_XLIB_H)
        target_compile_definitions(levari_ui PRIVATE LV_X8BIT)
    endif()
endif()

# LV_PROFILE=ON embeds the main-window draw-path instrument (per-producer
# tick counts, dirty-rect px totals, per-stage microseconds printed to
# stderr at exit — the standing measurement tool for low-floor CPU claims;
# dev builds only, never shipped on).
option(LV_PROFILE "main-window draw instrumentation at exit (stderr)" OFF)
if(LV_PROFILE)
    target_compile_definitions(levari_ui PRIVATE LV_PROFILE)
endif()

if(WIN32)
    # link the SDL2 import lib directly rather than the SDL2::SDL2 target
    # (which injects -mwindows via CMake interface; we set it explicitly
    # below so the intent is visible: GUI subsystem, no console window on
    # double-click; CLI verbs re-attach to the parent console via
    # platform/console.c).
    target_compile_definitions(levari PRIVATE SDL_MAIN_HANDLED)
    # console re-attach for CLI verbs (platform layer, item 5)
    target_sources(levari PRIVATE src/platform/console.c)
    if(LV_WIN9X)
        # _CRTIMP= (the anti-dllimport that lets win98_fs.c's direct
        # _stati64 definition win) now comes from lv_harden_compile for
        # EVERY project target — it used to ride this single target because
        # this single target held all sources. The interposition definitions
        # stay on the EXE: import interposition is a link-time race with the
        # CRT's archive members and the exe's object list wins it.
        # (see the file header for why the import archive never wins).
        # win98_fs.c preempts libmsvcrt.a's dirent/_stat64 members the
        # same way: the stock CRT walks the box's W layer through
        # msvcrt's OWN imports (invisible to this gate); we own the
        # file layer on FindFirstFileA instead (batch 18).
        target_sources(levari PRIVATE tools/win98_shim.c
                         src/compat/win98_diag.c src/compat/win98_fs.c)
        # MSLU: declare Unicode-Aware so the 98 loader consults
        # unicows.dll (user-supplied; see tools/win98_mslu.py) —
        # without the flag 98 never runs MSLU and SDL's W calls stay
        # dead stubs; with it, they work when the DLL is present.
        add_custom_command(TARGET levari POST_BUILD
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/win98_mslu.py
                    $<TARGET_FILE:levari>
            COMMENT "win98: setting MSLU Unicode-Aware PE flag"
            VERBATIM)
    endif()
    target_include_directories(levari PRIVATE src third_party ${SDL2_INCLUDE_DIRS})
    # static SDL2: single self-contained exe, no SDL2.dll needed
    target_link_libraries(levari "${SDL2_LIBDIR}/libSDL2.a"
        modplug snes_spc lzma winpthread m advapi32
        user32 gdi32 winmm imm32 ole32 oleaut32 setupapi version uuid
        dinput8 dxguid)
else()
    target_include_directories(levari PRIVATE src third_party ${SDL2_INCLUDE_DIRS})
    # Homebrew's libdir is not on the default linker search path
    target_link_directories(levari PRIVATE ${SDL2_LIBRARY_DIRS})
    if(APPLE)
        # no libdl on macOS (dlopen is in libSystem); ld64 has no --gc-sections/-s
        target_link_libraries(levari ${SDL2_LIBRARIES} modplug snes_spc lzma pthread m)
    else()
        target_link_libraries(levari ${SDL2_LIBRARIES} modplug snes_spc lzma dl pthread m)
    endif()
endif()
target_compile_definitions(levari PRIVATE LV_VERSION="${PROJECT_VERSION}")
if(APPLE)
    # proper GUI app: double-clickable, no Terminal window, not App-Napped
    set_target_properties(levari PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in")
endif()
lv_harden(levari)
if(WIN32)
    # -mwindows: GUI subsystem (no console on double-click). GUI target
    # ONLY — lv_harden gives levari-mcp the same flags without it.
    # No -Wl,-s: stripped exes trip heuristic AV scans
    # (Behavior:Win32/DefenseEvasion) and the symbol table costs little
    # here. MinGW-w64 links PE executables DYNAMICBASE (ASLR) by default.
    target_link_options(levari PRIVATE -mwindows)
endif()

# CPU tuning for ARM boards: -DLV_TUNE=arm1176jzf-s (Pi Zero 1 / Pi 1),
# cortex-a7 (Pi 2) or cortex-a72 (Pi 4). On 32-bit armv7 the NEON flags
# also switch minimp3 to its SIMD mp3 decoder; the armv6 boards (Pi Zero 1,
# Pi 1: ARM1176, VFPv2, no NEON) get plain VFP instead, so minimp3 keeps
# its scalar path and the binary runs on them (NEON flags would SIGILL).
set(LV_TUNE "" CACHE STRING "-mcpu target, e.g. arm1176jzf-s (Pi Zero) / cortex-a7 (Pi 2) / cortex-a72 (Pi 4)")
if(LV_TUNE AND NOT WIN32)
    # every PROJECT target compiles tuned code (the split made the old
    # single levari target into 3 libs + exe — losing the tune on a lib
    # would ship Pi 4 code to a Pi Zero, exactly the SIGILL this block
    # exists to prevent); link-time pins stay on the executable.
    foreach(lv_tuned levari_core levari_engine levari_ui levari modplug snes_spc)
        target_compile_options(${lv_tuned} PRIVATE -mcpu=${LV_TUNE})
    endforeach()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
        foreach(lv_tuned levari_core levari_engine levari_ui levari modplug)
            target_compile_options(${lv_tuned} PRIVATE -mfpu=neon-vfpv4 -mfloat-abi=hard)
        endforeach()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv6")
        foreach(lv_tuned levari_core levari_engine levari_ui levari modplug)
            target_compile_options(${lv_tuned} PRIVATE -mfpu=vfp -mfloat-abi=hard)
        endforeach()
    endif()
    # LTO RE-COMPILES THE IR AT LINK TIME with the driver's default
    # -march — the `lto1: warning: switch -mcpu... conflicts with
    # -march=armv6+fp` line: per-object compile flags are replayed but
    # the link driver's defaults WIN for new codegen, so the final
    # binary gains VFPv3 encodings (vdiv.f64 = undefined instruction on
    # the ARM1176's VFPv2) even though every .o was compiled -mfpu=vfp.
    # Pin -mcpu (and the FPU/ABI pair) at link time so lto1 generates
    # for the same silicon we ship to.
    target_link_options(levari PRIVATE -mcpu=${LV_TUNE})
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
        target_link_options(levari PRIVATE -mfpu=neon-vfpv4 -mfloat-abi=hard)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv6")
        target_link_options(levari PRIVATE -mfpu=vfp -mfloat-abi=hard)
    endif()
endif()

# decoders dominate CPU: let these TUs optimize for speed, not size.
# player.c belongs here too (opt report 2026-11-17): its per-chunk loops
# (apply_vol, the copy-to-output loop) are the playback hot path the
# decoder hands off to — the report measured 7.1 -> 1.0 us/chunk for the
# volume loop at -O3; the -Os LTO link does not undo the per-file -O3.
# stb_vorbis belongs here as much as decode.c: the tier comment below
# already assumes the -O3 source property covers both, and without it
# stb_vorbis ran -Os in plain builds and NO -O at all in the tier OBJECT
# libs (object libs do not inherit levari's -Os) — 20-25% slower ogg
set_source_files_properties(src/decode/decode.c third_party/stb_vorbis.c src/dsp/dsp.c src/viz/viz.c src/player/player.c
    PROPERTIES COMPILE_OPTIONS "-O3")

if(LEVAR_TIERS)
    # mirror the per-platform hardening flags of the levari target (minus
    # -Os/-flto: the tier code is reached through function pointers, size
    # and cross-TU LTO buy nothing; -O3 already comes via the source prop).
    # -march=x86-64 pins the ISA floor explicitly: some toolchains default
    # above the baseline (this host's gcc ships x86-64-v3), and the sse2
    # tier is what the dispatcher hands to old CPUs — its name must match
    # its instructions. -ffp-contract=off disables FMA contraction so the
    # three tiers produce bit-identical Vorbis output (the imdct/filterbank
    # multiply+add is not merged into one rounding step).
    if(WIN32)
        set(LEVAR_TIER_BASE -march=x86-64 -ffp-contract=off -ffunction-sections -fdata-sections -fstack-protector-strong)
    elseif(APPLE)
        set(LEVAR_TIER_BASE -march=x86-64 -ffp-contract=off -ffunction-sections -fdata-sections -fstack-protector-strong)
    else()
        set(LEVAR_TIER_BASE -march=x86-64 -ffp-contract=off -ffunction-sections -fdata-sections -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    endif()
    foreach(tier IN LISTS LEVAR_TIERS)
        add_library(dec_${tier} OBJECT src/decode/decode.c third_party/stb_vorbis.c)
        # MODPLUG_BUILD: on Windows modplug.h otherwise declares the DLL-import
        # form (__imp_ModPlug_*) and the static modplug archive does not match
        target_compile_definitions(dec_${tier} PRIVATE LEVAR_DECODE_SUFFIX=_${tier})
        if(WIN32)
            target_compile_definitions(dec_${tier} PRIVATE MODPLUG_BUILD)
        endif()
        set(tierflags "")
        if(tier STREQUAL "avx")
            set(tierflags -mavx -mfma -DDRFLAC_API=static -DDRFLAC_PRIVATE=static -DDRWAV_API=static -DDRWAV_PRIVATE=static)
        elseif(tier STREQUAL "avx2")
            # v3-class set, not bare AVX2: FMA is free throughput in the
            # imdct/filterbank loops and BMI/LZCNT pay off in the bitstream
            # walkers (measured: bare -mavx2 loses ~7% to this set). Every
            # AVX2-capable CPU worth a tier (Haswell/Excavator and newer)
            # has them all, and the dispatcher gates on every bit we use
            set(tierflags -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -DDRFLAC_API=static -DDRFLAC_PRIVATE=static -DDRWAV_API=static -DDRWAV_PRIVATE=static)
        endif()
        target_compile_options(dec_${tier} PRIVATE
            -include ${CMAKE_CURRENT_SOURCE_DIR}/src/decode/tier_rename.h
            ${LEVAR_TIER_BASE} ${tierflags})
        # target_sources (not target_link_libraries): levari's link line uses
        # the plain signature above, and keyword/plain mixing is an error
        target_sources(levari PRIVATE $<TARGET_OBJECTS:dec_${tier}>)
        # the dispatch.c TU (in levari_core) is the ONLY reader of these
        # defines (grep: LEVAR_HAVE_ / LEVAR_DECODE_HAVE_X86 appear nowhere
        # else in src/) — they tell the dispatcher which suffixed tier
        # externs exist, so they must match the tier objects attached above
        string(TOUPPER ${tier} tier_upper)
        target_compile_definitions(levari_core PRIVATE LEVAR_HAVE_${tier_upper})
    endforeach()
    target_compile_definitions(levari_core PRIVATE LEVAR_DECODE_HAVE_X86)
endif()

# libav* backend for aac/wma/URLs: compiled once per supported libav major
# against the vendored public headers (vendor/ffmpeg/, see its README) —
# NEVER against the system's libav: the backend reads libav struct FIELDS
# across the dlopen boundary, so each variant is paired with the runtime
# majors it was compiled for and decode.c dispatches by avutil_version().
# The libraries are still dlopen'd at runtime, so a machine without any
# libav simply loses the ffmpeg formats instead of failing to start.
# LEVAR_FF_ABIS must stay in sync with LV_FFAPI_LIST in src/decode/ffabi.h.
if(NOT WIN32)
    set(LEVAR_FF_ABIS 5 6 7 8 9)
    # Slim checkouts (the GitHub package) ship the generator instead of the
    # header sets — fail HERE with the fix, not at the first #include.
    foreach(abi IN LISTS LEVAR_FF_ABIS)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg/ff${abi}/include/libavformat/avformat.h)
            message(FATAL_ERROR
                "vendor/ffmpeg/ff${abi}/include is missing: the libav* backend "
                "compiles once per major against the pinned vendored headers. "
                "Run 'tools/gen_vendor_ffmpeg.sh' at the repo root first "
                "(one-time, needs network, verifies pinned sha256s) — see docs/build.md.")
        endif()
    endforeach()
    foreach(abi IN LISTS LEVAR_FF_ABIS)
        add_library(dec_ffab${abi} OBJECT src/decode/ffab.c)
        # OBJECT libs do NOT inherit the main target's position-independence:
        # without this the Linux PIE link dies on R_X86_64_32 (same trap as
        # the modplug static archive)
        set_target_properties(dec_ffab${abi} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_compile_definitions(dec_ffab${abi} PRIVATE LV_FFSUF=${abi})
        target_include_directories(dec_ffab${abi} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg/ff${abi}/include)
        target_sources(levari PRIVATE $<TARGET_OBJECTS:dec_ffab${abi}>)
    endforeach()
    # HAVE_FFMPEG's only reader is decode.c (grep src/): in a tier-less
    # build that TU is in levari_core, in a tiered build it is each
    # dec_<tier> OBJECT lib — both get it; mcp inherits it through the
    # core archive member it links.
    target_compile_definitions(levari_core PRIVATE HAVE_FFMPEG)
    foreach(tier IN LISTS LEVAR_TIERS)
        target_compile_definitions(dec_${tier} PRIVATE HAVE_FFMPEG)
    endforeach()
endif()

# optional MPRIS (D-Bus) media-player integration, Linux only:
# without dbus-devel the player builds and runs, just without MPRIS
if(UNIX AND NOT APPLE)
    pkg_check_modules(DBUS dbus-1)
    if(DBUS_FOUND)
        # link the absolute path, not -ldbus-1: the zig-legacy cross link
        # (tools/zig-legacy-toolchain.cmake) keeps the host's /usr/lib64 off
        # the search path on purpose, so a bare -l would not resolve there.
        # Harmless for native builds (same library); the soname recorded in
        # the binary is still libdbus-1.so.3, whose ABI is stable since 2007,
        # so old targets satisfy it with their own dbus.
        find_library(DBUS_LINK NAMES dbus-1 HINTS "${DBUS_LIBRARY_DIRS}")
        if(NOT DBUS_LINK)
            set(DBUS_LINK "${DBUS_LIBRARIES}")
        endif()
        target_sources(levari_engine PRIVATE src/mpris/mpris.c)
        # PUBLIC: mpris.h itself branches on HAVE_MPRIS, and main.c (exe)
        # includes mpris.h to wire the raise hook — exe and mpris.c MUST
        # compile the same branch of that header.
        target_compile_definitions(levari_engine PUBLIC HAVE_MPRIS)
        target_include_directories(levari_engine PUBLIC ${DBUS_INCLUDE_DIRS})
        target_link_directories(levari_engine PUBLIC ${DBUS_LIBRARY_DIRS})
        target_link_libraries(levari_engine PUBLIC "${DBUS_LINK}")
    endif()
endif()

if(APPLE)
    install(TARGETS levari BUNDLE DESTINATION .)
else()
    install(TARGETS levari RUNTIME DESTINATION bin)
endif()
if(UNIX AND NOT APPLE)
    install(FILES assets/levari.desktop DESTINATION share/applications)
    install(FILES assets/levari.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()

# optional MCP (Model Context Protocol) server, stdio transport
option(ENABLE_MCP "Build levari-mcp, a stdio MCP server controlling the player" OFF)
if(ENABLE_MCP)
    add_executable(levari-mcp
        src/mcp/mcp_main.c
        src/mcp/mcp_server.c
        src/mcp/mcp_tools.c
        third_party/cJSON.c
    )
    # refactor item 3: the old copy-pasted source list (daemon_client,
    # library, library_cli, config, tag, decode, stb_vorbis) is just the
    # core archive now — one compile shared with levari.
    target_link_libraries(levari-mcp levari_core)
    if(LEVAR_TIERS)
        # core carries dispatch.c, whose suffixed tier externs must join
        # the link — attach the once-compiled tier objects (same as levari)
        foreach(tier IN LISTS LEVAR_TIERS)
            target_sources(levari-mcp PRIVATE $<TARGET_OBJECTS:dec_${tier}>)
        endforeach()
    endif()
    target_include_directories(levari-mcp PRIVATE src third_party)
    target_compile_definitions(levari-mcp PRIVATE LV_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_link_libraries(levari-mcp modplug snes_spc winpthread m advapi32)
    elseif(APPLE)
        target_link_libraries(levari-mcp modplug snes_spc pthread m)
    else()
        target_link_libraries(levari-mcp modplug snes_spc dl pthread m)
    endif()
    lv_harden(levari-mcp)
    set_source_files_properties(third_party/cJSON.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-Wno-unknown-pragmas;-fstack-protector-strong;-D_FORTIFY_SOURCE=2")
    if(NOT WIN32)
        # ffmpeg backend: same variant objects as the levari target (the
        # ffab externs are referenced by the core's decode.c member)
        foreach(abi IN LISTS LEVAR_FF_ABIS)
            target_sources(levari-mcp PRIVATE $<TARGET_OBJECTS:dec_ffab${abi}>)
        endforeach()
    endif()
    install(TARGETS levari-mcp RUNTIME DESTINATION bin)
endif()

# ---- CTest battery (refactor item 3) --------------------------------------
# Every C harness from the scratch-script era is a real target now: the
# find-glob link recipes (and with them the stale-object hunt and the
# stale-BINARY trap — a test that no longer compiles can no longer "pass"
# as last session's binary) are replaced by targets + filtered archives;
# every case runs through tools/ctest_home.sh (fresh scratch HOME whose
# name contains 'scratch', XDG layout, audio.backend=null, daemon.socket,
# /tmp/scratch-p2, cwd inside the scratch, SDL_VIDEODRIVER=dummy).
# Skipped when cross-compiling (win/pi/legacy trees never execute what
# they build; native linux/mac hosts get the full battery).
option(LV_TESTS "build the CTest battery (native builds only)" ON)
if(LV_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    find_program(LV_PYTHON3 NAMES python3)

    # lv_filtered_lib(TGT base member...) — include-the-.c harnesses embed
    # their module in the test TU; the same-named archive member would
    # collide, so TGT is an ar-COPY of base minus those members. This is
    # the end of the object-hunt genre: link the archive, drop the module.
    function(lv_filtered_lib tgt base)
        set(_p "${CMAKE_CURRENT_BINARY_DIR}/filtered/${tgt}.a")
        add_custom_command(OUTPUT "${_p}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/filtered"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${base}>" "${_p}"
            COMMAND ${CMAKE_AR} -d "${_p}" ${ARGN}
            DEPENDS "${base}"
            COMMENT "filtered archive: ${base} minus [${ARGN}]")
        add_library(${tgt} STATIC IMPORTED GLOBAL)
        set_target_properties(${tgt} PROPERTIES IMPORTED_LOCATION "${_p}")
        add_custom_target(${tgt}_make DEPENDS "${_p}")
    endfunction()

    # shared link closure: vendored libs + SDL + system libs
    add_library(lv_test_deps INTERFACE)
    target_link_directories(lv_test_deps INTERFACE ${SDL2_LIBRARY_DIRS})
    if(APPLE)
        target_link_libraries(lv_test_deps INTERFACE modplug snes_spc lzma ${SDL2_LIBRARIES} m pthread)
    else()
        target_link_libraries(lv_test_deps INTERFACE modplug snes_spc lzma ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS} m pthread)
    endif()

    # the tier objects dispatch.c (in core) externs to — same once-compiled
    # OBJECT libs the executables attach — plus the ffab variants that
    # decode.c's ff_pick references (the old find-glob picked both up via
    # the dec_*.dir pattern; the target world names them explicitly)
    set(LV_TIER_OBJS "")
    if(LEVAR_TIERS)
        foreach(tier IN LISTS LEVAR_TIERS)
            list(APPEND LV_TIER_OBJS $<TARGET_OBJECTS:dec_${tier}>)
        endforeach()
    endif()
    if(NOT WIN32)
        foreach(abi IN LISTS LEVAR_FF_ABIS)
            list(APPEND LV_TIER_OBJS $<TARGET_OBJECTS:dec_ffab${abi}>)
        endforeach()
    endif()

    set(LV_CTHOME "${CMAKE_SOURCE_DIR}/tools/ctest_home.sh")

    # lv_test(NAME test.c FILTER ... [NO_TEST]) — NO_TEST suppresses the
    # add_test registration (for cases whose argv the wrapper must extend)
    function(lv_test name src)
        cmake_parse_arguments(T "NO_TEST" "" "FILTER" ${ARGN})
        add_executable(${name} ${src})
        target_sources(${name} PRIVATE ${LV_TIER_OBJS})
        target_include_directories(${name} PRIVATE
            src src/ui src/tag src/library src/config src/decode
            third_party ${SDL2_INCLUDE_DIRS})
        if(T_FILTER)
            target_link_libraries(${name} ${T_FILTER} levari_engine lv_test_deps)
            foreach(_f ${T_FILTER})
                add_dependencies(${name} ${_f}_make)
            endforeach()
        else()
            target_link_libraries(${name} levari_ui levari_engine lv_test_deps)
        endif()
        if(NOT T_NO_TEST)
            add_test(NAME ${name} COMMAND sh ${LV_CTHOME} $<TARGET_FILE:${name}>)
        endif()
    endfunction()

    lv_filtered_lib(ui_x_opt   levari_ui ui_opt.c.o)
    lv_filtered_lib(ui_x_eq    levari_ui ui_eq.c.o)
    lv_filtered_lib(ui_x_core  levari_ui ui.c.o)
    lv_filtered_lib(ui_x_pl    levari_ui ui_pl.c.o)
    lv_filtered_lib(ui_x_plf   levari_ui ui.c.o ui_pl.c.o ui_file.c.o)
    lv_filtered_lib(ui_x_plc   levari_ui ui.c.o ui_pl.c.o)
    lv_filtered_lib(ui_x_urld  levari_ui ui.c.o ui_url.c.o)
    lv_filtered_lib(ui_x_info  levari_ui ui_info.c.o ui_pl.c.o)
    lv_filtered_lib(ui_x_urlf  levari_ui ui_url.c.o ui_file.c.o)
    lv_filtered_lib(ui_x_srv   levari_ui search.c.o)
    lv_filtered_lib(core_x_pl  levari_core playlist.c.o)

    lv_test(testabout     test/about/testabout.c             FILTER ui_x_opt)
    lv_test(testeq        test/eq/testeq.c                   FILTER ui_x_eq)
    lv_test(testoptenc    test/optenc/testoptenc.c           FILTER ui_x_opt)
    lv_test(testoptvhz    test/optvhz/testoptvhz.c           FILTER ui_x_opt)
    lv_test(testpldock    test/pldock/testpldock.c           FILTER ui_x_core)
    lv_test(testpldrop    test/pldrop/testpldrop.c           FILTER ui_x_core NO_TEST)
    # pldrop takes the repo test/ dir (its fixtures) as argv[1]
    add_test(NAME testpldrop COMMAND sh ${LV_CTHOME} --test-arg $<TARGET_FILE:testpldrop>)
    lv_test(testplmultisel test/plmultisel/testplmultisel.c FILTER ui_x_plf)
    lv_test(testplscan    test/plscan/testplscan.c           FILTER ui_x_plc)
    lv_test(testpltot     test/pltot/testpltot.c             FILTER ui_x_pl)
    lv_test(testsonginfo  test/songinfo/testsonginfo.c       FILTER ui_x_info)
    lv_test(testuidirty   test/uidirty/testuidirty.c         FILTER ui_x_core)
    lv_test(testsearchsb  test/searchsb/testsearchsb.c       FILTER ui_x_srv)
    lv_test(testnorm      test/searchnorm/testnorm.c         FILTER ui_x_srv)
    # durclamp carries playlist.c in its TU and wants its scratch as argv[1]
    add_executable(testdurclamp test/durclamp/testdurclamp.c)
    target_sources(testdurclamp PRIVATE ${LV_TIER_OBJS})
    target_include_directories(testdurclamp PRIVATE
        src src/ui src/tag src/library src/config src/decode third_party ${SDL2_INCLUDE_DIRS})
    target_link_libraries(testdurclamp core_x_pl levari_engine lv_test_deps)
    add_dependencies(testdurclamp core_x_pl_make)
    add_test(NAME testdurclamp COMMAND sh ${LV_CTHOME} --scratch-arg $<TARGET_FILE:testdurclamp>)
    # testspcseek: the TU carries decode.c (fuzz-harness shape), so the
    # tier OBJECT libs stay out AND dispatch.c.o is filtered from core —
    # dispatch externs the tier-renamed dec_* symbols that would collide
    # with (or steal the plain names from) the test TU. No HAVE_FFMPEG:
    # the ffmpeg branch is not the subject; Windows ships that shape.
    # TIER-LESS TRAP (0.3.8 mac cut): with LEVAR_TIERS off, core carries
    # decode.c.o + stb_vorbis.c.o INSTEAD of dispatch.c.o — and macOS' ar
    # FAILS (GNU ar tolerates) deleting a member that does not exist, so
    # the filter set must follow the same if as the target_sources block.
    if(LEVAR_TIERS)
        lv_filtered_lib(core_x_dec levari_core dispatch.c.o)
    else()
        lv_filtered_lib(core_x_dec levari_core decode.c.o stb_vorbis.c.o)
    endif()
    add_executable(testspcseek test/spcseek/testspcseek.c third_party/stb_vorbis.c)
    target_include_directories(testspcseek PRIVATE
        src src/ui src/tag src/library src/config src/decode third_party ${SDL2_INCLUDE_DIRS})
    target_link_libraries(testspcseek core_x_dec levari_engine lv_test_deps)
    add_dependencies(testspcseek core_x_dec_make)
    add_test(NAME testspcseek COMMAND sh ${LV_CTHOME} --scratch-arg $<TARGET_FILE:testspcseek>)

    lv_test(testrescan    test/rescan/testrescan.c)
    lv_test(testurlidx    test/urlidx/testurlidx.c)
    if(LV_STREAMING)
        # url/snap harnesses pin url-window statics: with streaming off
        # ui_url.c compiles empty and their pins have no subject
        lv_test(testsnapwin   test/snapwin/testsnapwin.c     FILTER ui_x_urld)
        lv_test(testurlwin    test/urlwin/testurlwin.c       FILTER ui_x_urlf)
    endif()

    # standalone harnesses (no library link at all — the old shape)
    add_executable(testurlgate test/urlgate/testurlgate.c)
    target_include_directories(testurlgate PRIVATE src)
    add_test(NAME testurlgate COMMAND sh ${LV_CTHOME} $<TARGET_FILE:testurlgate>)

    add_executable(testplaylistedit test/playlistedit/testplaylistedit.c
        src/playlist/playlist.c src/tag/tag.c)
    target_include_directories(testplaylistedit PRIVATE src src/tag ${SDL2_INCLUDE_DIRS})
    target_link_libraries(testplaylistedit m pthread)
    add_test(NAME testplaylistedit COMMAND sh ${LV_CTHOME} $<TARGET_FILE:testplaylistedit>)

    add_executable(testdsp test/dsp/testdsp.c src/dsp/dsp.c)
    target_include_directories(testdsp PRIVATE src/dsp)
    # the bit-identical-chain pins compare a hand-written float biquad (test
    # TU) against eq_process (dsp.c): same FP rules or the comparison is
    # meaningless. dsp.c carries the -O3 source prop (FMA contraction on by
    # default); a test TU at the build type's -O/-O0 contracts differently.
    # Pin the contract off for BOTH TUs — the tool this tree already uses
    # for bit-identical tier output — so the pin tests math, not flags.
    target_compile_options(testdsp PRIVATE -ffp-contract=off)
    target_link_libraries(testdsp m pthread)
    add_test(NAME testdsp COMMAND sh ${LV_CTHOME} $<TARGET_FILE:testdsp>)

    add_executable(testx8bit test/x8bit/testx8bit.c)
    target_include_directories(testx8bit PRIVATE src third_party ${SDL2_INCLUDE_DIRS})
    target_link_libraries(testx8bit ${SDL2_LIBRARIES})
    target_link_directories(testx8bit PRIVATE ${SDL2_LIBRARY_DIRS})
    add_test(NAME testx8bit COMMAND sh ${LV_CTHOME} $<TARGET_FILE:testx8bit>)

    # stb_vorbis seek-window guard (vendored decoder unit, no engine link):
    # the TU includes stb_vorbis.c HEADER_ONLY, the implementation is its
    # own TU here — never both (duplicate definitions).
    add_executable(teststbseek test/stbseek/teststbseek.c third_party/stb_vorbis.c)
    target_include_directories(teststbseek PRIVATE third_party)
    target_link_libraries(teststbseek m)
    add_test(NAME teststbseek COMMAND sh ${LV_CTHOME} --test-arg $<TARGET_FILE:teststbseek>)

    # self-contained python drivers (ffmatrix.py deliberately NOT wired:
    # it needs the macOS dev host, see the ffabi dossier)
    if(LV_PYTHON3)
        foreach(drv probe/probecheck tagwords/tagcheck verify/testverify
                    deadskip/testdeadskip dribble/daemon_dribble)
            string(REGEX REPLACE ".*/" "" _dn ${drv})
            if(_dn STREQUAL "probecheck")
                # probecheck takes the build dir as argv[1] (cwd is the
                # wrapper's scratch, so the ./build default would miss)
                add_test(NAME py_${_dn} COMMAND sh ${LV_CTHOME}
                    ${LV_PYTHON3} "${CMAKE_SOURCE_DIR}/test/${drv}.py" "${CMAKE_BINARY_DIR}")
            else()
                add_test(NAME py_${_dn} COMMAND sh ${LV_CTHOME}
                    ${LV_PYTHON3} "${CMAKE_SOURCE_DIR}/test/${drv}.py")
            endif()
        endforeach()
    endif()
endif()
