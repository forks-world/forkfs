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

add_library(forkfs_namespace STATIC container.cpp backup_catalog.cpp namespace.cpp compaction.cpp maintenance.cpp)
target_link_libraries(forkfs_namespace PUBLIC forkfs_metadata)
target_compile_options(forkfs_namespace PRIVATE -Wall -Wextra -Wpedantic)
foreach(suite large_files file_io handles inode_handles inode_entries inode_contracts inode_create inode_size inode_xattrs inode_corruption parent_index transaction filenames)
    add_executable(forkfsd_${suite}_test tests/${suite}.cpp)
    target_link_libraries(forkfsd_${suite}_test PRIVATE forkfs_namespace)
    add_test(NAME forkfsd_${suite} COMMAND forkfsd_${suite}_test)
    set_tests_properties(forkfsd_${suite} PROPERTIES TIMEOUT 120)
endforeach()

add_library(forkfs_rpc_client STATIC rpc_client.cpp)
target_include_directories(forkfs_rpc_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(forkfs_rpc_client PRIVATE -Wall -Wextra -Wpedantic)
add_executable(forkfsd main.cpp rpc.cpp)
target_link_libraries(forkfsd PRIVATE forkfs_namespace forkfs_rpc_client)
target_compile_options(forkfsd PRIVATE -Wall -Wextra -Wpedantic)
# Fault injection is absent from the production libraries and executable.
add_executable(forkfsd_leveldb_fault_test main.cpp rpc.cpp container.cpp backup_catalog.cpp namespace.cpp
    compaction.cpp maintenance.cpp journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_leveldb_fault_test PRIVATE FORKFS_TEST_FAULTS=1)
target_include_directories(forkfsd_leveldb_fault_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_leveldb_fault_test PRIVATE leveldb Threads::Threads forkfs_rpc_client)
add_executable(forkfsd_inode_recovery_test tests/inode_recovery.cpp container.cpp backup_catalog.cpp namespace.cpp
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
add_executable(forkfsd_online_backup_core_test tests/online_backup.cpp container.cpp backup_catalog.cpp namespace.cpp
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

add_executable(forkfsd_recovery_namespace_test tests/recovery_namespace.cpp)
target_link_libraries(forkfsd_recovery_namespace_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_recovery_namespace COMMAND forkfsd_recovery_namespace_test)
set_tests_properties(forkfsd_recovery_namespace PROPERTIES TIMEOUT 120 LABELS metadata-regression)

add_test(NAME forkfsd_backup_catalog COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/backup_catalog.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_backup_catalog PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_test(NAME forkfsd_online_catalog COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_catalog.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_online_catalog PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_test(NAME forkfsd_catalog_revision COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_revision.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_catalog_revision PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_executable(forkfsd_revision_dag_test tests/revision_dag.cpp)
target_link_libraries(forkfsd_revision_dag_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_revision_dag_core COMMAND forkfsd_revision_dag_test)
add_test(NAME forkfsd_revision_dag COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/revision_dag.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_revision_dag_core forkfsd_revision_dag PROPERTIES TIMEOUT 120 LABELS metadata-regression)

add_executable(forkfsd_content_segments_test tests/content_segments.cpp)
target_link_libraries(forkfsd_content_segments_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_content_segments COMMAND forkfsd_content_segments_test)
set_tests_properties(forkfsd_content_segments PROPERTIES TIMEOUT 120 LABELS metadata-regression)
add_test(NAME forkfsd_content_segments_crash COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/content_segments.py $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_content_segments_crash PROPERTIES TIMEOUT 180 LABELS metadata-regression)
add_executable(forkfsd_object_store_test tests/object_store.cpp)
target_link_libraries(forkfsd_object_store_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_object_store COMMAND forkfsd_object_store_test)
set_tests_properties(forkfsd_object_store PROPERTIES TIMEOUT 120 LABELS metadata-regression)

add_executable(forkfsd_storage_gc_plan_test tests/storage_gc_plan.cpp)
target_link_libraries(forkfsd_storage_gc_plan_test PRIVATE forkfs_metadata)
add_test(NAME forkfsd_storage_gc_plan COMMAND forkfsd_storage_gc_plan_test)
set_tests_properties(forkfsd_storage_gc_plan PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_executable(forkfsd_object_store_recovery_test tests/object_store_recovery.cpp
    journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_object_store_recovery_test PRIVATE FORKFS_TEST_FAULTS=1)
target_include_directories(forkfsd_object_store_recovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_object_store_recovery_test PRIVATE leveldb Threads::Threads)
if(NOT APPLE)
    target_link_libraries(forkfsd_object_store_recovery_test PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME forkfsd_object_store_recovery COMMAND forkfsd_object_store_recovery_test)
set_tests_properties(forkfsd_object_store_recovery PROPERTIES TIMEOUT 180 LABELS metadata-regression)

# Stateful development workload: independent content model, restart and storage checks.
foreach(seed 42 173)
    add_test(NAME forkfsd_developer_workload_${seed} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/scripts/tests/developer_workload.py $<TARGET_FILE:forkfsd>
        --seed ${seed} --output ${CMAKE_BINARY_DIR}/developer-workload-artifacts
        --files 32 --rounds 3 --workers 2 --steps 7 --max-store-mib 64)
    set_tests_properties(forkfsd_developer_workload_${seed} PROPERTIES
        TIMEOUT 180 LABELS "metadata-regression;developer-workload")
endforeach()
add_test(NAME forkfsd_developer_workload_oracle COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_developer_workload.py)
set_tests_properties(forkfsd_developer_workload_oracle PROPERTIES TIMEOUT 30 LABELS "metadata-regression;developer-workload")

# Test-only HTTP protocol prototype over real forkfs-generated revision objects.
add_test(NAME forkfsd_revision_sync_mock COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/tests/test_revision_sync_mock.py $<TARGET_FILE:forkfsd>
    --output ${CMAKE_BINARY_DIR}/revision-sync-mock-artifacts)
set_tests_properties(forkfsd_revision_sync_mock PROPERTIES TIMEOUT 240 LABELS "metadata-regression;revision-sync")

set_tests_properties(forkfsd_large_files PROPERTIES LABELS metadata-regression)

add_test(NAME forkfsd_large_files_crash COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/content_segments.py $<TARGET_FILE:forkfsd>
    $<TARGET_FILE:forkfsd_leveldb_fault_test> 700000)
set_tests_properties(forkfsd_large_files_crash PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_test(NAME forkfsd_large_file_corruption COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/large_file_corruption.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_large_file_corruption PROPERTIES TIMEOUT 90 LABELS metadata-regression)

add_executable(forkfsd_sparse_content_model_test tests/sparse_content_model.cpp)
target_link_libraries(forkfsd_sparse_content_model_test PRIVATE forkfs_metadata)
foreach(seed 1 173 4294967295)
    add_test(NAME forkfsd_sparse_content_model_${seed} COMMAND forkfsd_sparse_content_model_test ${seed})
    set_tests_properties(forkfsd_sparse_content_model_${seed} PROPERTIES TIMEOUT 120 LABELS metadata-regression)
endforeach()
add_executable(forkfsd_large_file_concurrency_test tests/large_file_concurrency.cpp)
target_link_libraries(forkfsd_large_file_concurrency_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_large_file_concurrency COMMAND forkfsd_large_file_concurrency_test)
set_tests_properties(forkfsd_large_file_concurrency PROPERTIES TIMEOUT 120 LABELS metadata-regression)

add_test(NAME forkfsd_large_file_fault_matrix COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/large_file_fault_matrix.py $<TARGET_FILE:forkfsd>
    $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_large_file_fault_matrix PROPERTIES TIMEOUT 240 LABELS metadata-regression)

add_test(NAME forkfsd_large_file_handles_crash COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/handles_crash.py $<TARGET_FILE:forkfsd>
    $<TARGET_FILE:forkfsd_leveldb_fault_test> 700000)
set_tests_properties(forkfsd_large_file_handles_crash PROPERTIES TIMEOUT 180 LABELS metadata-regression)

foreach(seed 1 173 4294967295)
    add_test(NAME forkfsd_native_file_differential_${seed} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_file_differential.py $<TARGET_FILE:forkfsd>
        --seed ${seed} --steps 300 --output ${CMAKE_BINARY_DIR}/native-differential-artifacts)
    set_tests_properties(forkfsd_native_file_differential_${seed} PROPERTIES TIMEOUT 180 LABELS metadata-regression)
endforeach()
add_test(NAME forkfsd_native_file_differential_random COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_file_differential.py $<TARGET_FILE:forkfsd>
    --steps 300 --output ${CMAKE_BINARY_DIR}/native-differential-artifacts)
set_tests_properties(forkfsd_native_file_differential_random PROPERTIES TIMEOUT 180 LABELS metadata-regression)
add_test(NAME forkfsd_readonly_handle_write_regression COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_file_differential.py $<TARGET_FILE:forkfsd>
    --replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/readonly-handle-write.json
    --output ${CMAKE_BINARY_DIR}/native-differential-artifacts)
set_tests_properties(forkfsd_readonly_handle_write_regression PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_storage_gc COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_gc.py $<TARGET_FILE:forkfsd>
    $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_storage_gc PROPERTIES TIMEOUT 240 LABELS metadata-regression)

foreach(seed 1 173 4294967295)
    add_test(NAME forkfsd_storage_gc_model_${seed} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_gc_model.py $<TARGET_FILE:forkfsd> ${seed})
    set_tests_properties(forkfsd_storage_gc_model_${seed} PROPERTIES TIMEOUT 240 LABELS metadata-regression)
endforeach()

add_test(NAME forkfsd_mount_harness COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_mount_harness.py -v)
set_tests_properties(forkfsd_mount_harness PROPERTIES TIMEOUT 90 LABELS metadata-regression)

add_test(NAME forkfsd_service_plan COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_service_plan.py -v)
set_tests_properties(forkfsd_service_plan PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_uid_boundary_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_uid_boundary.py -v)
set_tests_properties(forkfsd_uid_boundary_unit PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_dedicated_workload_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_dedicated_workload.py -v)
set_tests_properties(forkfsd_dedicated_workload_unit PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_agent_supervisor_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_agent_supervisor.py -v)
set_tests_properties(forkfsd_agent_supervisor_unit PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_mount_session_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_mount_session.py -v)
set_tests_properties(forkfsd_mount_session_unit PROPERTIES TIMEOUT 30 LABELS metadata-regression)

add_test(NAME forkfsd_filename_rpc COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/filename_rpc.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_filename_rpc PROPERTIES TIMEOUT 180 LABELS metadata-regression)

add_test(NAME forkfsd_filename_replay COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/filename_replay.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_filename_replay PROPERTIES TIMEOUT 180 LABELS metadata-regression)
set_tests_properties(forkfsd_filenames PROPERTIES LABELS metadata-regression)

add_test(NAME forkfsd_session_mounts COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/session_mounts.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_session_mounts PROPERTIES TIMEOUT 60 LABELS metadata-regression)
add_test(NAME forkfsd_mount_lifecycle_unit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/scripts/tests -p test_mount_lifecycle.py -v)
set_tests_properties(forkfsd_mount_lifecycle_unit PROPERTIES TIMEOUT 60 LABELS metadata-regression)

add_executable(forkfsd_online_gc_test tests/online_gc.cpp)
target_link_libraries(forkfsd_online_gc_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_online_gc COMMAND forkfsd_online_gc_test)
set_tests_properties(forkfsd_online_gc PROPERTIES TIMEOUT 240 LABELS metadata-regression)
add_executable(forkfsd_gc_memory_budget_test tests/gc_memory_budget.cpp)
target_link_libraries(forkfsd_gc_memory_budget_test PRIVATE forkfs_namespace)
add_test(NAME forkfsd_gc_memory_budget COMMAND forkfsd_gc_memory_budget_test)
set_tests_properties(forkfsd_gc_memory_budget PROPERTIES TIMEOUT 120 LABELS metadata-regression)
add_test(NAME forkfsd_online_gc_crash COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_gc_crash.py $<TARGET_FILE:forkfsd>
    $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_online_gc_crash PROPERTIES TIMEOUT 240 LABELS metadata-regression)

# Backup snapshot barrier is compiled only into this deterministic concurrency test.
add_executable(forkfsd_online_gc_concurrency_test tests/online_gc_concurrency.cpp
    container.cpp backup_catalog.cpp namespace.cpp compaction.cpp maintenance.cpp
    journal.cpp level_store.cpp metadata_tree.cpp metadata_view.cpp)
target_compile_definitions(forkfsd_online_gc_concurrency_test PRIVATE FORKFS_TEST_BACKUP=1 FORKFS_TEST_GC=1)
target_include_directories(forkfsd_online_gc_concurrency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forkfsd_online_gc_concurrency_test PRIVATE leveldb Threads::Threads)
if(NOT APPLE)
    target_link_libraries(forkfsd_online_gc_concurrency_test PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME forkfsd_online_gc_concurrency COMMAND forkfsd_online_gc_concurrency_test)
set_tests_properties(forkfsd_online_gc_concurrency PROPERTIES TIMEOUT 180 LABELS metadata-regression)
add_test(NAME forkfsd_online_gc_rpc COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_gc_rpc.py $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfsd_online_gc_rpc PROPERTIES TIMEOUT 180 LABELS metadata-regression)
add_executable(forkfsd_online_gc_model_test tests/online_gc_model.cpp)
target_link_libraries(forkfsd_online_gc_model_test PRIVATE forkfs_metadata)
foreach(seed 1 173 4294967295)
    add_test(NAME forkfsd_online_gc_model_${seed} COMMAND forkfsd_online_gc_model_test ${seed})
    set_tests_properties(forkfsd_online_gc_model_${seed} PROPERTIES TIMEOUT 120 LABELS metadata-regression)
endforeach()
add_test(NAME forkfsd_rpc_dispatch_deadline COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc_dispatch_deadline.py
    $<TARGET_FILE:forkfsd> $<TARGET_FILE:forkfsd_leveldb_fault_test>)
set_tests_properties(forkfsd_rpc_dispatch_deadline PROPERTIES TIMEOUT 60 LABELS metadata-regression)
