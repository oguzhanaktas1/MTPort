#include "mtport/RuntimePaths.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "mtport/MtpError.hpp"

namespace fs = std::filesystem;

namespace mtport {

namespace {
fs::path homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return "/tmp";
}
}  // namespace

// The app used to be called AndroidFinder. Its per-user data (settings, locks and above all the staging area with
// uploads that might still need recovering) lives under the old name: move it once, atomically, instead of
// silently starting from scratch. A second process racing us simply finds the new directory already in place.
static void migrateLegacyDir(const fs::path& legacy, const fs::path& current) {
    std::error_code ec;
    if (!fs::exists(legacy, ec) || fs::exists(current, ec)) return;
    fs::create_directories(current.parent_path(), ec);
    fs::rename(legacy, current, ec);
}

fs::path appSupportDir() {
    if (const char* o = std::getenv("MTPORT_HOME"); o && *o) return fs::path(o) / "Support";
    static const fs::path dir = [] {
        const fs::path base = homeDir() / "Library" / "Application Support";
        migrateLegacyDir(base / "AndroidFinder", base / "MTPort");
        return base / "MTPort";
    }();
    return dir;
}
fs::path runtimeDir() {
    return appSupportDir() / "runtime";
}
fs::path locksDir() {
    return runtimeDir() / "locks";
}
fs::path mountsDir() {
    return runtimeDir() / "mounts";
}
fs::path cacheDir() {
    if (const char* o = std::getenv("MTPORT_HOME"); o && *o) return fs::path(o) / "Cache";
    static const fs::path dir = [] {
        const fs::path base = homeDir() / "Library" / "Caches";
        migrateLegacyDir(base / "AndroidFinder", base / "MTPort");
        return base / "MTPort";
    }();
    return dir;
}
fs::path stagingDir() {
    return cacheDir() / "Staging";
}

void ensurePrivateDir(const fs::path& dir) {
    // Create every missing component with 0700 (umask can only remove bits).
    fs::path built;
    for (const auto& part : dir) {
        built /= part;
        if (built == "/" || built.empty()) continue;
        struct stat st{};
        if (::lstat(built.c_str(), &st) == 0) continue;  // existing parents (e.g. $HOME) are not ours to chmod
        if (::mkdir(built.c_str(), 0700) != 0 && errno != EEXIST)
            throw MtpError(MtpErrc::IoError, "cannot create " + built.string() + ": " + std::strerror(errno));
    }
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        throw MtpError(MtpErrc::IoError, dir.string() + " is not a directory");
    if (st.st_uid != ::getuid()) throw MtpError(MtpErrc::IoError, dir.string() + " is not owned by the current user");
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        if (::chmod(dir.c_str(), 0700) != 0)
            throw MtpError(MtpErrc::IoError, "cannot restrict permissions of " + dir.string());
    }
}

std::string safeName(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    return out.empty() ? "Device" : out;
}

}  // namespace mtport
