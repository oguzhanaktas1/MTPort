#include "mtport/Sha256.hpp"

#include <fcntl.h>

#include <array>
#include <cerrno>

#include "mtport/MtpError.hpp"
#include "mtport/UniqueFd.hpp"

namespace mtport {

void Sha256::update(const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (len > 0) {  // CC_LONG is 32-bit
        const CC_LONG n = static_cast<CC_LONG>(len > (1u << 30) ? (1u << 30) : len);
        CC_SHA256_Update(&ctx_, p, n);
        p += n;
        len -= n;
    }
}

std::string Sha256::hexDigest() {
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx_);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : md) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xf]);
    }
    return out;
}

std::string sha256File(const std::filesystem::path& file) {
    UniqueFd fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot open " + file.string());
    Sha256 h;
    std::array<char, 1 << 20> buf;
    for (;;) {
        ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            throw MtpError(MtpErrc::IoError, "read failed on " + file.string());
        }
        if (n == 0) break;
        h.update(buf.data(), static_cast<size_t>(n));
    }
    return h.hexDigest();
}

}  // namespace mtport
