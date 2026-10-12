#pragma once

#include <CommonCrypto/CommonDigest.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace mtport {

class Sha256 {
public:
    Sha256() { CC_SHA256_Init(&ctx_); }
    void update(const void* data, size_t len);
    std::string hexDigest();  // finalises; the object must not be reused afterwards

private:
    CC_SHA256_CTX ctx_;
};

// Streams the file through SHA-256. Throws MtpError{IoError} if it cannot be read.
std::string sha256File(const std::filesystem::path& file);

}  // namespace mtport
