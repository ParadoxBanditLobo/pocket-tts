cmake_minimum_required(VERSION 3.25)
project(pocket_tts_desktop LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
find_package(raylib 5.5 QUIET)

if(NOT TARGET raylib)
    # Devuan may not ship raylib 5.5 development packages. Build pinned
    # source instead of silently requiring a system upgrade.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

if(NOT TARGET raylib)
    message(FATAL_ERROR "Raylib 5.5 could not be built; check network and X11 headers.")
endif()

add_executable(pocket-tts-desktop main.cpp)
target_link_libraries(pocket-tts-desktop PRIVATE
    raylib pthread dl m GL X11 Xrandr Xi Xcursor Xinerama rt)
target_compile_options(pocket-tts-desktop PRIVATE -Wall -Wextra -Wpedantic)

message(STATUS "Desktop frontend: local pipe worker, no HTTP server")
