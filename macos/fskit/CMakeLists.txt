# FSKit appex executable. Entry point is Foundation's NSExtensionMain, like Apple's
# own FSKit modules; principal class comes from Info.plist (EXExtensionPrincipalClass).
add_executable(WorldFSExtension
    WorldItem.mm
    WorldVolume.mm
    WorldVolumeHandler.mm
    WorldFileSystem.mm
)
target_compile_options(WorldFSExtension PRIVATE -fobjc-arc -Wall)
target_link_libraries(WorldFSExtension PRIVATE worldfs_core
    "-framework FSKit" "-framework Foundation" "-framework ExtensionFoundation")
target_link_options(WorldFSExtension PRIVATE "-Wl,-e,_NSExtensionMain")
