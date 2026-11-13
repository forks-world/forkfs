#pragma once
#include <cstdint>

namespace forkfs {
// Shared by the namespace and the FSKit RPC frontend. This is a logical size
// bound, not an allocation reservation or a repository quota.
// Matches the advertised FSKit pathconf link limit for files and directories.
inline constexpr uint64_t maximum_link_count=32767;
inline constexpr uint64_t maximum_file_bytes = uint64_t{1} << 40; // 1 TiB
inline constexpr uint64_t legacy_file_bytes = uint64_t{64} << 20;
// The native FSKit path accepts values below 256 KiB. Keep all layers
// within that measured transport boundary, including the endpoint.
inline constexpr uint64_t maximum_xattr_bytes = (uint64_t{256} << 10)-1;
// BSD metadata flags. User immutable constrains mutation/admission paths.
inline constexpr uint32_t file_flag_nodump=0x00000001;
inline constexpr uint32_t file_flag_immutable=0x00000002;
inline constexpr uint32_t file_flag_append=0x00000004;
inline constexpr uint32_t file_flag_hidden=0x00008000;
inline constexpr uint32_t supported_file_flags=file_flag_nodump|file_flag_hidden|file_flag_immutable|file_flag_append;
inline constexpr uint64_t file_chunk_bytes = uint64_t{64} << 10;
}
