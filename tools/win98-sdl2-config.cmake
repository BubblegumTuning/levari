# Era SDL floor config (SDL 2.0.x static prefix for the Win98 legacy
# build; installed as <prefix>/cmake/sdl2-config.cmake by
# tools/build_sdl2_win98.sh). Mirrors the variables the consumer reads
# (SDL2_INCLUDE_DIRS, SDL2_LIBDIR, SDL2_LIBRARIES).
set(SDL2_FOUND TRUE)
get_filename_component(_lv_era_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# the codebase uses BOTH include styles: <SDL2/SDL.h> and <SDL.h> —
# expose the parent AND the SDL2 dir, same as the official mingw config
set(SDL2_INCLUDE_DIRS "${_lv_era_root}/include" "${_lv_era_root}/include/SDL2")
set(SDL2_LIBDIR "${_lv_era_root}/lib")
set(SDL2_LIBRARIES "${_lv_era_root}/lib/libSDL2.a")
