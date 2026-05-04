cmake_minimum_required(VERSION 3.10)
project(JellySync CXX C)

set(TOOLCHAIN_PATH "$ENV{PBSDK}" CACHE PATH "PocketBook SDK root (SDK-iMX6/usr)")
if(NOT TOOLCHAIN_PATH OR NOT EXISTS "${TOOLCHAIN_PATH}")
    message(FATAL_ERROR "SDK introuvable. export PBSDK=~/pocketbook/sdk/SDK-iMX6/usr")
endif()

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CROSS_BIN "${TOOLCHAIN_PATH}/bin/arm-obreey-linux-gnueabi-")
set(CMAKE_C_COMPILER   "${CROSS_BIN}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_BIN}g++")
set(CMAKE_AR           "${CROSS_BIN}ar")
set(CMAKE_STRIP        "${CROSS_BIN}strip")
set(CMAKE_RANLIB       "${CROSS_BIN}ranlib")

set(SYSROOT "${TOOLCHAIN_PATH}/arm-obreey-linux-gnueabi/sysroot")
set(CMAKE_SYSROOT "${SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ARM_FLAGS "-march=armv7-a -mfpu=neon -mfloat-abi=softfp")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   ${ARM_FLAGS} -O2 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARM_FLAGS} -O2 -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function")

include_directories(
    src/
    "${SYSROOT}/usr/local/include"
    "${SYSROOT}/usr/include"
    "${SYSROOT}/usr/include/freetype2"
)

link_directories(
    "${SYSROOT}/usr/local/lib"
    "${SYSROOT}/usr/lib"
    "${SYSROOT}/lib"
)

add_compile_definitions(
    FLASHDIR="/mnt/ext1"
    USERDATA="/mnt/ext1/system"
    CONFIG_FILE="/mnt/ext1/system/config/jellysync.cfg"
    BOOKS_DIR="/mnt/ext1/books/Jellyfin"
    APP_VERSION="1.0.0"
    JELLYFIN_API_IMPL
)

add_executable(jellysync.app src/main.cpp src/cJSON.cpp)

target_link_libraries(jellysync.app
    inkview pthread curl ssl crypto z m stdc++
)

add_custom_command(TARGET jellysync.app POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:jellysync.app>
    COMMENT "Strip..."
)

if(DEFINED DEVICE)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEVICE}/applications/jellysync.app"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:jellysync.app>
                "${DEVICE}/applications/jellysync.app/jellysync.app"
        DEPENDS jellysync.app
    )
endif()
