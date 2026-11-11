#pragma once
#include <cstdint>

namespace forkfs {
// Shared by the namespace and the FSKit RPC frontend. This is a logical size
// bound, not an allocation reservation or a repository quota.
inline constexpr uint64_t maximum_file_bytes = uint64_t{1} << 40; // 1 TiB
inline constexpr uint64_t legacy_file_bytes = uint64_t{64} << 20;
inline constexpr uint64_t file_chunk_bytes = uint64_t{64} << 10;
}
