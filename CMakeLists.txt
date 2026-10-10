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
    ${CMAKE_SOURCE_DIR}/third_party/Containa
    ${CMAKE_SOURCE_DIR}/third_party/Arena/include
)
target_compile_definitions(wfs_third_party INTERFACE FMT_HEADER_ONLY=1)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libstdc++)
endif()

enable_testing()
option(WFS_BUILD_LEGACY "Build the existing world CLI and core" ON)
if(WFS_BUILD_LEGACY)
    add_subdirectory(core)
    add_subdirectory(cli)
    if(APPLE AND WFS_FSKIT)
        add_subdirectory(macos/fskit)
    endif()
endif()
add_subdirectory(daemon)

option(WFS_COW_FSKIT "Build experimental read-only CoW FSKit adapter" OFF)
if(APPLE AND WFS_COW_FSKIT)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_library(forkfs_revision_fskit STATIC macos/cow-fskit/RevisionVolume.mm)
    target_compile_options(forkfs_revision_fskit PRIVATE -fobjc-arc -Wall -Wextra -Wno-error=deprecated-declarations)
    target_link_libraries(forkfs_revision_fskit PUBLIC forkfs_namespace "-framework FSKit" "-framework Foundation")
    add_executable(ForkRevisionExtension macos/cow-fskit/RevisionFileSystem.mm)
    target_compile_options(ForkRevisionExtension PRIVATE -fobjc-arc -Wno-error=deprecated-declarations)
    target_link_libraries(ForkRevisionExtension PRIVATE forkfs_revision_fskit "-framework ExtensionFoundation")
    target_link_options(ForkRevisionExtension PRIVATE "-Wl,-e,_NSExtensionMain")
    set(revision_appex "${CMAKE_BINARY_DIR}/ForkRevisionExtension.appex/Contents")
    add_custom_command(TARGET ForkRevisionExtension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${revision_appex}/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ForkRevisionExtension>" "${revision_appex}/MacOS/ForkRevisionExtension"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/macos/cow-fskit/Info.plist" "${revision_appex}/Info.plist")
    add_executable(ForkfsRevision MACOSX_BUNDLE macos/cow-fskit/HostApp.mm macos/cow-fskit/RevisionFileSystem.mm)
    set_target_properties(ForkfsRevision PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/cow-fskit/Host-Info.plist")
    target_compile_options(ForkfsRevision PRIVATE -fobjc-arc -Wno-error=deprecated-declarations)
    target_link_libraries(ForkfsRevision PRIVATE forkfs_revision_fskit "-framework Cocoa")
    add_custom_target(forkfs_revision_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:ForkfsRevision>/Contents/PlugIns"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/ForkRevisionExtension.appex" "$<TARGET_BUNDLE_DIR:ForkfsRevision>/Contents/PlugIns/ForkRevisionExtension.appex"
        DEPENDS ForkfsRevision ForkRevisionExtension)
    add_test(NAME forkfsd_fskit_bundle COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/macos/cow-fskit/check_bundle.py"
        "$<TARGET_BUNDLE_DIR:ForkfsRevision>" "${CMAKE_SOURCE_DIR}/macos/cow-fskit")
    add_executable(forkfsd_fskit_callback_test macos/cow-fskit/RevisionVolumeTest.mm macos/cow-fskit/RevisionFileSystem.mm)
    target_compile_options(forkfsd_fskit_callback_test PRIVATE -fobjc-arc -Wno-error=deprecated-declarations)
    target_link_libraries(forkfsd_fskit_callback_test PRIVATE forkfs_revision_fskit)
    add_test(NAME forkfsd_fskit_callbacks COMMAND forkfsd_fskit_callback_test)
    set_tests_properties(forkfsd_fskit_callbacks PROPERTIES TIMEOUT 60)
endif()
