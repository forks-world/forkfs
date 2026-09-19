find_package(SQLite3 REQUIRED)

# The M0 FSKit frontend (view/namespace layer + the `world fs mount` commands) is frozen as a
# fallback (docs/TASKS.md). It is not part of the M1 clonefile World model, so it is off by
# default and its sources are simply not compiled.
option(WFS_FSKIT "Build the FSKit view/namespace layer and frontend" OFF)

set(WFS_CORE_SOURCES
    src/store.cpp
    src/world.cpp
    src/platform_posix.cpp
)
if(APPLE)
    list(APPEND WFS_CORE_SOURCES src/platform_darwin.cpp)
endif()
if(WFS_FSKIT)
    list(APPEND WFS_CORE_SOURCES src/view.cpp)
endif()

add_library(worldfs_core STATIC ${WFS_CORE_SOURCES})
target_include_directories(worldfs_core PUBLIC include PRIVATE src)
target_link_libraries(worldfs_core PUBLIC SQLite3::SQLite3 PRIVATE wfs_third_party)
target_compile_features(worldfs_core PUBLIC cxx_std_23)
target_compile_options(worldfs_core PRIVATE -Wall -Wextra)
# Darwin hides ENOATTR & friends under strict -std=; ask for the full BSD namespace.
target_compile_definitions(worldfs_core PRIVATE _DARWIN_C_SOURCE=1)
if(APPLE)
    # FSEventsGetCurrentEventId(): the fork cursor for the O(changes) diff (T1.3).
    target_link_libraries(worldfs_core PUBLIC "-framework CoreServices")
endif()
if(WFS_FSKIT)
    target_compile_definitions(worldfs_core PUBLIC WFS_FSKIT=1)
endif()

add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE worldfs_core)
add_test(NAME core_test COMMAND core_test)

if(WFS_FSKIT)
    add_executable(fskit_test tests/fskit_test.cpp)
    target_include_directories(fskit_test PRIVATE src)
    target_link_libraries(fskit_test PRIVATE worldfs_core wfs_third_party)
    add_test(NAME fskit_test COMMAND fskit_test)
endif()
