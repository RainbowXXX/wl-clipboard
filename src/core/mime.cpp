#include "core/mime.h"

#include <algorithm>
#include <cstring>

namespace wlclip::core {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

static bool looks_like_text(const std::vector<std::byte>& data) {
    std::size_t n = std::min<std::size_t>(data.size(), 1024);
    for (std::size_t i = 0; i < n; ++i) {
        auto c = static_cast<unsigned char>(data[i]);
        if (c == 0) return false;
        if (c < 0x09) return false;
        if (c > 0x0D && c < 0x20) return false;
    }
    return true;
}

std::string guess_mime(const std::vector<std::byte>& data,
                       std::string_view explicit_type) {
    if (!explicit_type.empty()) return std::string(explicit_type);
    if (data.size() >= 8) {
        const auto* b = reinterpret_cast<const unsigned char*>(data.data());
        if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "image/png";
        if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)              return "image/jpeg";
        if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8')  return "image/gif";
    }
    if (looks_like_text(data)) return "text/plain;charset=utf-8";
    return "application/octet-stream";
}

std::string choose_mime(const std::vector<std::string>& av,
                        const std::string& prefer) {
    if (av.empty()) return {};
    if (!prefer.empty()) {
        for (const auto& m : av) if (m == prefer) return m;
        for (const auto& m : av) {
            if (starts_with(m, prefer) || starts_with(prefer, m)) return m;
        }
    }
    for (const auto& m : av) if (m == "text/plain;charset=utf-8") return m;
    for (const auto& m : av) if (m == "UTF8_STRING")              return m;
    for (const auto& m : av) if (starts_with(m, "text/"))         return m;
    return av.front();
}

bool is_textual(std::string_view mime) {
    return starts_with(mime, "text/") ||
           mime == "UTF8_STRING" ||
           mime == "TEXT" ||
           mime == "STRING";
}

}
