// Selects which IMtpDeviceFactory the front ends use: the real libmtp backend (default) or the
// host-directory test double (`--backend fake`, tests/dev only; always announced as MOCK).
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "mtport/IMtpDeviceFactory.hpp"

namespace mtport {

struct BackendSpec {
    std::string kind{"real"};  // "real" | "fake"
    std::filesystem::path fakeRoot;
    bool fakePartialRead{true};
};

bool realBackendAvailable();
bool mockBackendAvailable();
std::string libmtpVersionString();  // "" when libmtp is not linked

// Throws MtpError{OperationUnsupported} when the requested backend was not compiled in.
std::unique_ptr<IMtpDeviceFactory> makeFactory(const BackendSpec& spec);

}  // namespace mtport
