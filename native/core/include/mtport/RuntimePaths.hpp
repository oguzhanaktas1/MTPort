// Well-known per-user locations. MTPORT_HOME relocates everything (used by tests).
#pragma once

#include <filesystem>
#include <string>

namespace mtport {

std::filesystem::path appSupportDir();  // ~/Library/Application Support/MTPort
std::filesystem::path runtimeDir();     // .../runtime   (agent socket, mount state, device locks)
std::filesystem::path locksDir();       // .../runtime/locks
std::filesystem::path mountsDir();      // .../runtime/mounts
std::filesystem::path cacheDir();       // ~/Library/Caches/MTPort
std::filesystem::path stagingDir();     // .../Staging

// Creates the directory (and parents) with mode 0700 and verifies it is owned by the current
// user and not accessible to group/other. Throws MtpError{IoError} otherwise.
void ensurePrivateDir(const std::filesystem::path& dir);

// Turns an arbitrary device/model name into [A-Za-z0-9._-]+ (never empty, never "."/"..").
std::string safeName(const std::string& raw);

}  // namespace mtport
