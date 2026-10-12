// Host facts needed before attempting a mount (shared by doctor, the mount worker and the agent).
#pragma once

#include <string>

namespace mtport {

std::string macOSVersion();    // "15.4.1", "" if unreadable
std::string macfuseVersion();  // CFBundleShortVersionString of /Library/Filesystems/macfuse.fs, "" if absent
bool versionAtLeast(const std::string& have, const std::string& need);

inline constexpr const char* kMinMacOS = "15.4";
inline constexpr const char* kMinMacfuse = "5.4.0";

struct MountPrereq {
    bool ok{false};
    std::string problem;  // actionable message when !ok (MountBackendUnavailable)
};
// macOS >= 15.4 and macFUSE >= 5.4.0. Never suggests or performs a kernel-extension fallback.
MountPrereq checkMountPrerequisites();

// "/Volumes/MTPort-<safeName>" - the only place MTPort ever mounts.
std::string defaultMountPoint(const std::string& deviceModel);
bool isUnderVolumes(const std::string& path);  // exactly one component below /Volumes, no traversal

}  // namespace mtport
