// Validation of untrusted names (from the phone, from Finder, from the command line).
#pragma once

#include <cstdint>
#include <string_view>

namespace mtport {

inline constexpr size_t kMaxNameBytes = 255;

inline bool isValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n;
        uint32_t cp;
        if (c < 0x80) {
            n = 1;
            cp = c;
        } else if ((c >> 5) == 0x6) {
            n = 2;
            cp = c & 0x1f;
        } else if ((c >> 4) == 0xe) {
            n = 3;
            cp = c & 0x0f;
        } else if ((c >> 3) == 0x1e) {
            n = 4;
            cp = c & 0x07;
        } else
            return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return false;  // overlong
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += n;
    }
    return true;
}

// A single path component that is safe to create on the phone and to present to Finder.
inline bool isValidObjectName(std::string_view n) {
    if (n.empty() || n.size() > kMaxNameBytes) return false;
    if (n == "." || n == "..") return false;
    for (char c : n)
        if (c == '/' || c == '\0') return false;
    return isValidUtf8(n);
}

}  // namespace mtport
