cmake_minimum_required(VERSION 3.25)

set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architecture")

option(EXPORT_COMPILE_COMMANDS "Export compile commands for LSP" ON)
if (EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

include(FetchContent)

message(STATUS "Fetching max-sdk-base")
FetchContent_Declare(
    max-sdk-base
    GIT_REPOSITORY https://github.com/Cycling74/max-sdk-base.git
    GIT_TAG main
)
FetchContent_MakeAvailable(max-sdk-base)

if (NOT DEFINED MAX_SDK_PACKAGE_OUT_OF_TREE)
    set(MAX_SDK_PACKAGE_OUT_OF_TREE ON)
endif ()

project(MaxLilypond)

add_subdirectory(source/lilypond)
