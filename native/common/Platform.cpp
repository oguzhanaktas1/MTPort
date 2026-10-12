#include "Platform.hpp"

#include <sys/sysctl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include "Process.hpp"
#include "mtport/RuntimePaths.hpp"

namespace mtport {

std::string macOSVersion() {
    char buf[64] = {};
    size_t len = sizeof buf;
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) != 0) return {};
    return buf;
}

std::string macfuseVersion() {
    const std::filesystem::path info = "/Library/Filesystems/macfuse.fs/Contents/Info";
    std::error_code ec;
    if (!std::filesystem::exists(info.string() + ".plist", ec)) return {};
    auto r = runProcess({"/usr/bin/defaults", "read", info.string(), "CFBundleShortVersionString"}, 10);
    if (r.exitCode != 0) return {};
    while (!r.output.empty() && (r.output.back() == '\n' || r.output.back() == ' ')) r.output.pop_back();
    return r.output;
}

bool versionAtLeast(const std::string& have, const std::string& need) {
    auto parse = [](const std::string& v) {
        std::vector<long> out;
        size_t i = 0;
        while (i < v.size()) {
            size_t j = i;
            while (j < v.size() && std::isdigit(static_cast<unsigned char>(v[j]))) ++j;
            if (j == i) break;
            out.push_back(std::stol(v.substr(i, j - i)));
            if (j < v.size() && v[j] == '.')
                ++j;
            else
                break;
            i = j;
        }
        return out;
    };
    auto a = parse(have), b = parse(need);
    if (a.empty()) return false;
    const size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a >= b;
}

MountPrereq checkMountPrerequisites() {
    MountPrereq r;
    const std::string os = macOSVersion();
    if (os.empty() || !versionAtLeast(os, kMinMacOS)) {
        r.problem = "macOS " + std::string(kMinMacOS) + " or newer is required for the macFUSE FSKit backend (found " +
                    (os.empty() ? "unknown" : os) +
                    "). MTPort does not fall back to the kernel-extension backend.";
        return r;
    }
    const std::string fuse = macfuseVersion();
    if (fuse.empty()) {
        r.problem = "macFUSE is not installed. Install macFUSE " + std::string(kMinMacfuse) +
                    " or newer from https://macfuse.github.io and approve its extension in System Settings.";
        return r;
    }
    if (!versionAtLeast(fuse, kMinMacfuse)) {
        r.problem = "macFUSE " + fuse + " is too old; " + kMinMacfuse +
                    " or newer is required (5.3.3 corrupts 1-14 byte writes).";
        return r;
    }
    r.ok = true;
    return r;
}

std::string defaultMountPoint(const std::string& deviceModel) {
    return "/Volumes/MTPort-" + safeName(deviceModel);
}

bool isUnderVolumes(const std::string& path) {
    const std::string prefix = "/Volumes/";
    if (path.rfind(prefix, 0) != 0) return false;
    const std::string rest = path.substr(prefix.size());
    if (rest.empty() || rest == "." || rest == ".." || rest.find('/') != std::string::npos) return false;
    return rest.find('\0') == std::string::npos;
}

}  // namespace mtport
