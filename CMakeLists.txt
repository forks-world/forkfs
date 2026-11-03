cmake_minimum_required(VERSION 3.20)
# Retired switches fail explicitly instead of silently restoring native-tree semantics.
if(WFS_BUILD_LEGACY)
    message(FATAL_ERROR "The native-directory world/core/FSKit backend is retired. Only the revision-backed runtime is supported.")
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/cli/world" OR EXISTS "${CMAKE_BINARY_DIR}/core/libworldfs_core.a")
    message(FATAL_ERROR "This build directory contains retired world artifacts. Use a fresh build directory; existing files are preserved.")
endif()
project(forkfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE)
    # Command Line Tools ship an incomplete usr/include/c++/v1 that shadows the SDK's libc++
    # headers. Point the compiler at the SDK copy explicitly.
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE WFS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(WFS_TOOLCHAIN_INC "${CMAKE_CXX_COMPILER}/../../include/c++/v1" ABSOLUTE)
    if(NOT EXISTS "${WFS_TOOLCHAIN_INC}/string" AND EXISTS "${WFS_SDK}/usr/include/c++/v1/string")
        add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-nostdinc++>
                            "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-isystem ${WFS_SDK}/usr/include/c++/v1>")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libstdc++)
endif()

enable_testing()
add_subdirectory(daemon)

option(WFS_FSKIT "Build the revision-backed macOS FSKit frontend" OFF)
if(WFS_FSKIT)
    if(NOT APPLE)
        message(FATAL_ERROR "The revision FSKit frontend requires macOS")
    endif()
    enable_language(OBJCXX)
    add_subdirectory(macos/forkfs)
endif()
