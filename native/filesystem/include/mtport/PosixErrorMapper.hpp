// MtpErrc -> errno, plus the exception type the filesystem layer throws at its API boundary.
#pragma once

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <string>

#include "mtport/MtpError.hpp"

namespace mtport {

// errno values. On macOS EOPNOTSUPP (102) is what Finder/POSIX tools expect for
// "this filesystem cannot do that", ENOTSUP (45) is reserved for xattr-style probes.
int toErrno(MtpErrc code) noexcept;

class FsError : public std::runtime_error {
public:
    explicit FsError(int err, std::string what = {})
        : std::runtime_error(what.empty() ? std::string("errno ") + std::to_string(err) : std::move(what)),
          errno_(err) {}
    int err() const noexcept { return errno_; }

private:
    int errno_;
};

// Converts any exception to a positive errno (MtpError -> mapped, FsError -> its value,
// bad_alloc -> ENOMEM, everything else -> EIO).
int errnoFromException(const std::exception& e) noexcept;

}  // namespace mtport
