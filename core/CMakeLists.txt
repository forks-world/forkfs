find_package(SQLite3 REQUIRED)

add_library(worldfs_core STATIC
    src/store.cpp
    src/view.cpp
    src/platform_posix.cpp
)
target_include_directories(worldfs_core PUBLIC include PRIVATE src)
target_link_libraries(worldfs_core PUBLIC SQLite3::SQLite3 PRIVATE wfs_third_party)
target_compile_features(worldfs_core PUBLIC cxx_std_23)
target_compile_options(worldfs_core PRIVATE -Wall -Wextra)
# Darwin hides ENOATTR & friends under strict -std=; ask for the full BSD namespace.
target_compile_definitions(worldfs_core PRIVATE _DARWIN_C_SOURCE=1)

add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE worldfs_core)
add_test(NAME core_test COMMAND core_test)
