cmake_minimum_required(VERSION 3.16)
project(levari VERSION 0.2.4 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(WIN32)
    # MinGW cross builds: point SDL2_DIR at the SDL2-mingw cmake dir, e.g.
    #   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=tools/mingw-w64-toolchain.cmake \
    #         -DSDL2_DIR=/path/to/SDL2-2.32.10/cmake
    find_package(SDL2 CONFIG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()
find_package(ZLIB REQUIRED)
if(WIN32)
    # prefer the static zlib so the exe doesn't need zlib1.dll
    get_filename_component(_zlib_dir "${ZLIB_LIBRARY_RELEASE}" DIRECTORY)
    find_library(ZLIB_STATIC NAMES libz.a HINTS "${_zlib_dir}")
    if(ZLIB_STATIC)
        set(ZLIB_LINK "${ZLIB_STATIC}")
    else()
        set(ZLIB_LINK ZLIB::ZLIB)
    endif()
else()
    set(ZLIB_LINK ZLIB::ZLIB)
endif()

# vendored libmodplug (public domain): mod/s3m/xm/it -> 44.1kHz pcm
# MODPLUG_BASIC_SUPPORT drops the exotic loaders (669/abc/amf/... kept
# out of the build); exotic variants fall back to the libav* backend
enable_language(CXX)
set(CMAKE_CXX_STANDARD 11)
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
    target_compile_options(modplug PRIVATE -Os -w)
else()
    target_compile_options(modplug PRIVATE -Os -flto -w)
endif()

add_executable(levari
    src/main.c
    src/ui/ui.c
    src/ui/search.c
    src/skin/skin.c
    src/skin/cjk_font.c
    src/decode/decode.c
    src/tag/tag.c
    src/audio/audio.c
    src/playlist/playlist.c
    src/player/player.c
    src/dsp/dsp.c
    src/viz/viz.c
    src/daemon/daemon.c
    src/daemon/daemon_client.c
    src/library/library.c
    src/config/config.c
    third_party/stb_vorbis.c
)

if(WIN32)
    # link the SDL2 import lib directly rather than the SDL2::SDL2 target
    # (which injects -mwindows via CMake interface; we set it explicitly
    # below so the intent is visible: GUI subsystem, no console window on
    # double-click; CLI verbs re-attach to the parent console in main.c).
    target_compile_definitions(levari PRIVATE SDL_MAIN_HANDLED MODPLUG_BUILD)
    target_include_directories(levari PRIVATE src third_party ${SDL2_INCLUDE_DIRS})
    # static SDL2: single self-contained exe, no SDL2.dll needed
    target_link_libraries(levari "${SDL2_LIBDIR}/libSDL2.a"
        ${ZLIB_LINK} modplug winpthread m advapi32
        user32 gdi32 winmm imm32 ole32 oleaut32 setupapi version uuid
        dinput8 dxguid)
else()
    target_include_directories(levari PRIVATE src third_party ${SDL2_INCLUDE_DIRS})
    # Homebrew's libdir is not on the default linker search path
    target_link_directories(levari PRIVATE ${SDL2_LIBRARY_DIRS})
    if(APPLE)
        # no libdl on macOS (dlopen is in libSystem); ld64 has no --gc-sections/-s
        target_link_libraries(levari ${SDL2_LIBRARIES} ZLIB::ZLIB modplug pthread m)
    else()
        target_link_libraries(levari ${SDL2_LIBRARIES} ZLIB::ZLIB modplug dl pthread m)
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
if(WIN32)
    target_compile_options(levari PRIVATE -Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
    # -mwindows: GUI subsystem (no console on double-click). No -Wl,-s:
    # stripped exes trip heuristic AV scans (Behavior:Win32/DefenseEvasion)
    # and the symbol table costs little here.
    target_link_options(levari PRIVATE -Wl,--gc-sections -mwindows)
elseif(APPLE)
    target_compile_options(levari PRIVATE -Os -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
    target_link_options(levari PRIVATE -flto -Wl,-dead_strip)
else()
    target_compile_options(levari PRIVATE -Os -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
    target_link_options(levari PRIVATE -flto -Wl,--gc-sections -Wl,-s)
endif()

# optional libav* backend for aac/wma and other exotic formats:
# dev headers only at build time; the libraries are dlopen'd at runtime,
# so a missing libav* just disables those formats instead of breaking startup
if(NOT WIN32)
    pkg_check_modules(AV libavformat libavcodec libavutil)
    if(AV_FOUND)
        target_compile_definitions(levari PRIVATE HAVE_FFMPEG)
        target_include_directories(levari PRIVATE ${AV_INCLUDE_DIRS})
    endif()
endif()

# optional MPRIS (D-Bus) media-player integration, Linux only:
# without dbus-devel the player builds and runs, just without MPRIS
if(UNIX AND NOT APPLE)
    pkg_check_modules(DBUS dbus-1)
    if(DBUS_FOUND)
        target_sources(levari PRIVATE src/mpris/mpris.c)
        target_compile_definitions(levari PRIVATE HAVE_MPRIS)
        target_include_directories(levari PRIVATE ${DBUS_INCLUDE_DIRS})
        target_link_directories(levari PRIVATE ${DBUS_LIBRARY_DIRS})
        target_link_libraries(levari ${DBUS_LIBRARIES})
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
        src/daemon/daemon_client.c
        src/library/library.c
        src/config/config.c
        src/tag/tag.c
        src/decode/decode.c
        third_party/cJSON.c
        third_party/stb_vorbis.c
    )
    target_include_directories(levari-mcp PRIVATE src third_party)
    target_compile_definitions(levari-mcp PRIVATE LV_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_compile_definitions(levari-mcp PRIVATE MODPLUG_BUILD)
        target_link_libraries(levari-mcp modplug winpthread m advapi32)
    elseif(APPLE)
        target_link_libraries(levari-mcp modplug pthread m)
    else()
        target_link_libraries(levari-mcp modplug dl pthread m)
    endif()
    if(WIN32)
        target_compile_options(levari-mcp PRIVATE -Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
        target_link_options(levari-mcp PRIVATE -Wl,--gc-sections)
    elseif(APPLE)
        target_compile_options(levari-mcp PRIVATE -Os -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
        target_link_options(levari-mcp PRIVATE -flto -Wl,-dead_strip)
    else()
        target_compile_options(levari-mcp PRIVATE -Os -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter)
        target_link_options(levari-mcp PRIVATE -flto -Wl,--gc-sections -Wl,-s)
    endif()
    set_source_files_properties(third_party/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")
    if(AV_FOUND)
        target_compile_definitions(levari-mcp PRIVATE HAVE_FFMPEG)
        target_include_directories(levari-mcp PRIVATE ${AV_INCLUDE_DIRS})
    endif()
    install(TARGETS levari-mcp RUNTIME DESTINATION bin)
endif()
