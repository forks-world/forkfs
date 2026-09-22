cmake_minimum_required(VERSION 3.20)
project(worldfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE)
    enable_language(OBJCXX)
    # Command Line Tools ship an incomplete usr/include/c++/v1 that shadows the SDK's libc++
    # headers. Point the compiler at the SDK copy explicitly.
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE WFS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(WFS_TOOLCHAIN_INC "${CMAKE_CXX_COMPILER}/../../include/c++/v1" ABSOLUTE)
    if(NOT EXISTS "${WFS_TOOLCHAIN_INC}/string" AND EXISTS "${WFS_SDK}/usr/include/c++/v1/string")
        add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-nostdinc++>
                            "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-isystem ${WFS_SDK}/usr/include/c++/v1>")
    endif()
endif()

# Header-only third-party libraries (git submodules), see arch.md §39.
add_library(wfs_third_party INTERFACE)
target_include_directories(wfs_third_party SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/fmt/include
    ${CMAKE_SOURCE_DIR}/third_party/smallstring/include
    ${CMAKE_SOURCE_DIR}/third_party/Containa
    ${CMAKE_SOURCE_DIR}/third_party/Arena/include
)
target_compile_definitions(wfs_third_party INTERFACE FMT_HEADER_ONLY=1)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libstdc++)
endif()

enable_testing()
add_subdirectory(core)
add_subdirectory(cli)
if(APPLE AND WFS_FSKIT)
    add_subdirectory(macos/fskit)
endif()
