# This target never links worldfs_core or opens native World file data.
find_library(FSKIT_FRAMEWORK FSKit REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(ForkFSExtension RevisionFileSystem.mm RevisionVolume.mm RevisionConfig.mm
    ${CMAKE_SOURCE_DIR}/daemon/rpc_client.cpp)
target_include_directories(ForkFSExtension PRIVATE ${CMAKE_SOURCE_DIR}/daemon)
target_compile_options(ForkFSExtension PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(ForkFSExtension PRIVATE ${FSKIT_FRAMEWORK} "-framework Foundation" "-framework ExtensionFoundation")
target_link_options(ForkFSExtension PRIVATE "-Wl,-e,_NSExtensionMain")
add_executable(ForkFSHost Host.mm)
target_compile_options(ForkFSHost PRIVATE -fobjc-arc)
target_link_libraries(ForkFSHost PRIVATE "-framework AppKit")

add_executable(forkfs_bridge_test tests/BridgeTest.mm RevisionFileSystem.mm RevisionVolume.mm RevisionConfig.mm
    ${CMAKE_SOURCE_DIR}/daemon/rpc_client.cpp)
target_include_directories(forkfs_bridge_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/daemon)
target_compile_options(forkfs_bridge_test PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(forkfs_bridge_test PRIVATE ${FSKIT_FRAMEWORK} "-framework Foundation")
add_test(NAME forkfs_bridge COMMAND forkfs_bridge_test $<TARGET_FILE:forkfsd>)
set_tests_properties(forkfs_bridge PROPERTIES TIMEOUT 120)
add_test(NAME forkfs_loopback_bridge COMMAND forkfs_bridge_test $<TARGET_FILE:forkfsd>
    ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fskit-bridge.py)
set_tests_properties(forkfs_loopback_bridge PROPERTIES TIMEOUT 120)

foreach(target ForkFSExtension ForkFSHost forkfs_bridge_test)
    target_compile_options(${target} PRIVATE -mmacosx-version-min=26.0)
    target_link_options(${target} PRIVATE -mmacosx-version-min=26.0)
endforeach()
