# Build PC de JellySync avec l'émulateur InkView (SDL2)
#
#   cmake -S emulator -B build-emu
#   cmake --build build-emu
#   ./build-emu/jellysync-emu
#
# Le build liseuse (CMakeLists.txt à la racine) n'est pas modifié.

cmake_minimum_required(VERSION 3.10)
project(JellySyncEmulator CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(APP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Dossier qui joue le rôle de /mnt/ext1 (mémoire de la liseuse)
set(EMU_FLASHDIR "${CMAKE_BINARY_DIR}/ext1" CACHE PATH "Dossier simulant /mnt/ext1")
file(MAKE_DIRECTORY "${EMU_FLASHDIR}/system/config" "${EMU_FLASHDIR}/books/Jellyfin")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf)
find_package(CURL REQUIRED)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)

add_executable(jellysync-emu
    "${APP_SRC}/main.cpp"
    "${APP_SRC}/cJSON.cpp"
    inkview_sdl.cpp
)

# emulator/ passe avant src/ : <inkview.h> désigne l'émulateur
target_include_directories(jellysync-emu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${APP_SRC}"
)

target_compile_definitions(jellysync-emu PRIVATE
    FLASHDIR="${EMU_FLASHDIR}"
    USERDATA="${EMU_FLASHDIR}/system"
    CONFIG_FILE="${EMU_FLASHDIR}/system/config/jellysync.cfg"
    BOOKS_DIR="${EMU_FLASHDIR}/books/Jellyfin"
    APP_VERSION="1.0.0-emu"
    JELLYFIN_API_IMPL
    EMU_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
)

target_compile_options(jellysync-emu PRIVATE
    -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)

target_link_libraries(jellysync-emu PRIVATE
    PkgConfig::SDL2 CURL::libcurl PNG::PNG Threads::Threads)
