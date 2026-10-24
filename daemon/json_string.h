#pragma once

#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>

namespace forkfs {
namespace json_string {

inline bool valid_utf8(const std::string& value) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        unsigned char c = p[i++];
        if (c <= 0x7f) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= value.size() || (p[i++] & 0xc0) != 0x80) return false;
        } else if (c >= 0xe0 && c <= 0xef) {
            if (i + 1 >= value.size()) return false;
            unsigned char a = p[i++], b = p[i++];
            if ((a & 0xc0) != 0x80 || (b & 0xc0) != 0x80) return false;
            if ((c == 0xe0 && a < 0xa0) || (c == 0xed && a >= 0xa0)) return false;
        } else if (c >= 0xf0 && c <= 0xf4) {
            if (i + 2 >= value.size()) return false;
            unsigned char a = p[i++], b = p[i++], d = p[i++];
            if ((a & 0xc0) != 0x80 || (b & 0xc0) != 0x80 || (d & 0xc0) != 0x80) return false;
            if ((c == 0xf0 && a < 0x90) || (c == 0xf4 && a >= 0x90)) return false;
        } else return false;
    }
    return true;
}

inline std::string quote(const std::string& value) {
    if (!valid_utf8(value)) throw std::system_error(EILSEQ, std::generic_category(), "invalid UTF-8 in JSON string");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(value.size() + 2); out += '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

} // namespace json_string
} // namespace forkfs
