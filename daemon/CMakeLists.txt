if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "forkfs metadata storage currently supports macOS and Linux")
endif()
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/leveldb ${CMAKE_BINARY_DIR}/third_party/leveldb)
set_target_properties(leveldb PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
# Upstream disables RTTI, including the Env base used by our profiling wrapper.
set_source_files_properties(level_store.cpp PROPERTIES COMPILE_OPTIONS "-fno-rtti")
add_library(forkfs_metadata STATIC journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_include_directories(forkfs_metadata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(forkfs_metadata PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(forkfs_metadata PUBLIC leveldb Threads::Threads)
if(NOT APPLE)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(forkfs_metadata PUBLIC OpenSSL::Crypto)
endif()
add_executable(forkfsd_object_cache_test tests/object_cache.cpp)
target_link_libraries(forkfsd_object_cache_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_object_cache COMMAND forkfsd_object_cache_test)
set_tests_properties(forkfsd_object_cache PROPERTIES TIMEOUT 60)

add_executable(forkfsd_metadata_tree_test tests/metadata_tree.cpp)
target_link_libraries(forkfsd_metadata_tree_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_metadata_tree COMMAND forkfsd_metadata_tree_test)
set_tests_properties(forkfsd_metadata_tree PROPERTIES TIMEOUT 60)

# Separate build keeps deterministic barriers completely out of production.
add_executable(forkfsd_lock_granularity_test tests/lock_granularity.cpp journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_lock_granularity_test PRIVATE FORKFS_TEST_LOCKS=1)
target_include_directories(forkfsd_lock_granularity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_lock_granularity_test PRIVATE leveldb Threads::Threads)
if(NOT APPLE)
    target_link_libraries(forkfsd_lock_granularity_test PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME forkfsd_lock_granularity COMMAND forkfsd_lock_granularity_test)
set_tests_properties(forkfsd_lock_granularity PROPERTIES TIMEOUT 60)

add_executable(forkfsd_metadata_view_test tests/metadata_view.cpp)
target_link_libraries(forkfsd_metadata_view_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_metadata_view COMMAND forkfsd_metadata_view_test)
set_tests_properties(forkfsd_metadata_view PROPERTIES TIMEOUT 60)

add_library(forkfs_namespace STATIC container.cpp namespace.cpp compaction.cpp maintenance.cpp)
target_link_libraries(forkfs_namespace PUBLIC forkfs_metadata)
target_compile_options(forkfs_namespace PRIVATE -Wall -Wextra -Wpedantic)
foreach(suite file_io handles inode_handles inode_entries inode_contracts inode_create inode_size inode_xattrs inode_corruption parent_index transaction)
    add_executable(forkfsd_${suite}_test tests/${suite}.cpp)
    target_link_libraries(forkfsd_${suite}_test PRIVATE forkfs_namespace)
    add_test(NAME forkfsd_${suite} COMMAND forkfsd_${suite}_test)
    set_tests_properties(forkfsd_${suite} PROPERTIES TIMEOUT 120)
endforeach()

add_executable(forkfsd main.cpp rpc.cpp)
target_link_libraries(forkfsd PRIVATE forkfs_namespace)
target_compile_options(forkfsd PRIVATE -Wall -Wextra -Wpedantic)
# Fault injection is absent from the production libraries and executable.
add_executable(forkfsd_leveldb_fault_test main.cpp rpc.cpp container.cpp namespace.cpp
    compaction.cpp maintenance.cpp journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_leveldb_fault_test PRIVATE FORKFS_TEST_FAULTS=1)
target_include_directories(forkfsd_leveldb_fault_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_leveldb_fault_test PRIVATE leveldb Threads::Threads)
add_executable(forkfsd_inode_recovery_test tests/inode_recovery.cpp container.cpp namespace.cpp
    compaction.cpp maintenance.cpp journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_inode_recovery_test PRIVATE FORKFS_TEST_FAULTS=1)
target_include_directories(forkfsd_inode_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_inode_recovery_test PRIVATE leveldb Threads::Threads)
add_test(NAME forkfsd_inode_recovery COMMAND forkfsd_inode_recovery_test)
set_tests_properties(forkfsd_inode_recovery PROPERTIES TIMEOUT 120 LABELS metadata-regression)
if(NOT APPLE)
    target_link_libraries(forkfsd_leveldb_fault_test PRIVATE OpenSSL::Crypto)
    target_link_libraries(forkfsd_inode_recovery_test PRIVATE OpenSSL::Crypto)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(suite leveldb file_io_crash handles_crash symlinks attributes save)
    add_test(NAME forkfsd_${suite} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
    set_tests_properties(forkfsd_${suite} PROPERTIES TIMEOUT 180)
endforeach()
foreach(suite rpc sessions directories rpc_metadata)
    add_test(NAME forkfsd_${suite} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.py $<TARGET_FILE:forkfsd>)
    set_tests_properties(forkfsd_${suite} PROPERTIES TIMEOUT 180)
endforeach()

set_tests_properties(forkfsd_rpc_metadata PROPERTIES LABELS metadata-regression)

add_executable(forkfsd_inode_frontend_test tests/inode_frontend.cpp)
target_link_libraries(forkfsd_inode_frontend_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_inode_frontend COMMAND forkfsd_inode_frontend_test)
set_tests_properties(forkfsd_inode_frontend PROPERTIES TIMEOUT 60)

add_executable(forkfsd_revision_preview_test tests/revision_preview.cpp)
target_link_libraries(forkfsd_revision_preview_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_revision_preview COMMAND forkfsd_revision_preview_test)
set_tests_properties(forkfsd_revision_preview PROPERTIES TIMEOUT 60)

add_executable(forkfsd_metadata_manager_test tests/metadata_manager.cpp)
target_link_libraries(forkfsd_metadata_manager_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_metadata_manager COMMAND forkfsd_metadata_manager_test)
set_tests_properties(forkfsd_metadata_manager PROPERTIES TIMEOUT 120)

foreach(seed 1 173 4294967295)
    add_test(NAME forkfsd_metadata_manager_seed_${seed} COMMAND forkfsd_metadata_manager_test ${seed})
    set_tests_properties(forkfsd_metadata_manager_seed_${seed} PROPERTIES TIMEOUT 120 LABELS metadata-regression)
endforeach()
set_tests_properties(forkfsd_metadata_manager forkfsd_metadata_view forkfsd_metadata_tree
    forkfsd_revision_preview forkfsd_inode_handles forkfsd_inode_entries forkfsd_inode_contracts forkfsd_inode_create forkfsd_inode_size forkfsd_inode_xattrs forkfsd_inode_corruption forkfsd_parent_index forkfsd_transaction forkfsd_lock_granularity
    forkfsd_file_io_crash forkfsd_handles_crash PROPERTIES LABELS metadata-regression)

add_test(NAME forkfsd_basic_harness COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/harness/regression.py --binary $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_basic_harness PROPERTIES TIMEOUT 120 LABELS metadata-regression)
add_test(NAME forkfsd_harness_runner COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/harness/test_runner.py)
set_tests_properties(forkfsd_harness_runner PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_executable(forkfsd_restore_test tests/restore.cpp)
target_link_libraries(forkfsd_restore_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_restore COMMAND forkfsd_restore_test)
set_tests_properties(forkfsd_restore PROPERTIES TIMEOUT 60 LABELS metadata-regression)
add_test(NAME forkfsd_restore_crash COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/restore_crash.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_restore_crash PROPERTIES TIMEOUT 60 LABELS metadata-regression)

add_test(NAME forkfsd_script_control COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/tests/forkfs_control.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_script_control PROPERTIES TIMEOUT 90 LABELS metadata-regression)
add_test(NAME forkfsd_script_control_unit COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/tests/test_forkfs_control.py)
set_tests_properties(forkfsd_script_control_unit PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_replay COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_replay PROPERTIES TIMEOUT 180 LABELS metadata-regression)
add_executable(forkfsd_replay_core_test tests/replay.cpp)
target_link_libraries(forkfsd_replay_core_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_replay_core COMMAND forkfsd_replay_core_test)
set_tests_properties(forkfsd_replay_core PROPERTIES TIMEOUT 90 LABELS metadata-regression)

add_test(NAME forkfsd_replay_corruption COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_corruption.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_replay_corruption PROPERTIES TIMEOUT 180 LABELS metadata-regression)

foreach(seed 1 173 4294967295 random)
    set(seed_args --seed ${seed})
    if(seed STREQUAL "random")
        set(seed_args)
    endif()
    add_test(NAME forkfsd_replay_fuzz_${seed} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_fuzz.py --binary $<TARGET_FILE:forkfsd>
        ${seed_args} --steps 32 --mutations 24 --artifacts ${CMAKE_BINARY_DIR}/fuzz-artifacts)
    set_tests_properties(forkfsd_replay_fuzz_${seed} PROPERTIES TIMEOUT 180 LABELS metadata-regression)
endforeach()

option(WFS_BUILD_FUZZERS "Build replay parser fuzz target with sanitizers" OFF)
option(WFS_FUZZ_STANDALONE "Run saved corpus without libFuzzer (toolchains lacking its runtime)" OFF)
if(WFS_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Replay fuzzing requires Clang")
    endif()
    # Instrument LevelDB too: mixing instrumented/uninstrumented libc++ vector
    # operations can produce false container-overflow reports on macOS.
    target_compile_options(leveldb PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    target_link_options(leveldb INTERFACE -fsanitize=address,undefined)
    target_compile_options(forkfs_metadata PUBLIC -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    target_link_options(forkfs_metadata PUBLIC -fsanitize=address,undefined)
    add_executable(forkfs_replay_fuzzer tests/replay_fuzzer.cpp)
    target_link_libraries(forkfs_replay_fuzzer PRIVATE forkfs_metadata)
    if(WFS_FUZZ_STANDALONE)
        target_compile_definitions(forkfs_replay_fuzzer PRIVATE FORKFS_FUZZ_STANDALONE=1)
    else()
        target_compile_options(forkfs_metadata PUBLIC -fsanitize=fuzzer-no-link)
        target_link_options(forkfs_replay_fuzzer PRIVATE -fsanitize=fuzzer)
    endif()
endif()

add_test(NAME forkfsd_online_backup COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_backup.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_online_backup PROPERTIES TIMEOUT 180 LABELS metadata-regression)

# Deterministic barrier exists only in this test executable.
add_executable(forkfsd_online_backup_core_test tests/online_backup.cpp container.cpp namespace.cpp
    compaction.cpp maintenance.cpp journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_online_backup_core_test PRIVATE FORKFS_TEST_BACKUP=1)
target_include_directories(forkfsd_online_backup_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_online_backup_core_test PRIVATE leveldb Threads::Threads)
if(NOT APPLE)
    target_link_libraries(forkfsd_online_backup_core_test PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME forkfsd_online_backup_core COMMAND forkfsd_online_backup_core_test)
set_tests_properties(forkfsd_online_backup_core PROPERTIES TIMEOUT 60 LABELS metadata-regression)

add_test(NAME forkfsd_backup_tasks COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/backup_tasks.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_backup_tasks PROPERTIES TIMEOUT 120 LABELS metadata-regression)

add_test(NAME forkfsd_revision_point COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/revision_point.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_revision_point PROPERTIES TIMEOUT 90 LABELS metadata-regression)

add_test(NAME forkfsd_replay_until COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_until.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_replay_until PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_test(NAME forkfsd_restore_plan COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/restore_plan.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_restore_plan PROPERTIES TIMEOUT 180 LABELS metadata-regression)
