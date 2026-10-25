#pragma once
#include "journal.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace forkfs {
// Stable FSKit volume identity for one daemon mount session. The hash input is
// the exact UTF-8/ASCII byte concatenation of this domain and the two fixed
// lowercase-hex credentials; no separators or terminating NUL are included.
inline std::array<unsigned char,16> session_volume_uuid(const std::string& generation,
                                                       const std::string& session) {
    std::string payload="forkfs.fskit.volume.v1";
    payload+=generation;
    payload+=session;
    std::vector<unsigned char> bytes(payload.begin(),payload.end());
    auto digest=Journal::object_id(bytes);
    std::array<unsigned char,16> uuid{};
    std::copy_n(digest.begin(),uuid.size(),uuid.begin());
    uuid[6]=static_cast<unsigned char>((uuid[6]&0x0f)|0x80); // UUID version 8
    uuid[8]=static_cast<unsigned char>((uuid[8]&0x3f)|0x80); // RFC 4122 variant
    return uuid;
}
}
