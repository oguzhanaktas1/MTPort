// Mounts the loopback WebDAV server with macOS' own network-volume machinery (NetFS, the API behind Finder's
// "Connect to Server"). Needs no root, no kernel extension and no macFUSE; the result is a real network volume
// that Finder lists in the sidebar with an eject button.
#pragma once

#include <cstdint>
#include <string>

namespace mtport::webdav {

struct MountResult {
    bool ok{false};
    std::string mountPoint;  // e.g. /Volumes/MTPort-SM-G990E
    std::string error;
};

// Finder lists a network volume in its sidebar under the *host name* of the URL it was mounted from. The server is
// loopback-only, and every `<label>.localhost` name resolves to 127.0.0.1 without root or any network traffic, so the
// phone's own name is used as that label (e.g. "Galaxy S21 FE" -> "galaxy-s21-fe.localhost").
std::string hostLabel(const std::string& deviceName);  // DNS-safe label ("android" when nothing usable is left)
std::string loopbackHost(const std::string& deviceName);

MountResult mountVolume(uint16_t port, const std::string& user, const std::string& password, const std::string& volumeName,
                        bool readOnly, const std::string& host = "127.0.0.1");

// True while `mountPoint` is still a mounted webdav volume.
bool isMounted(const std::string& mountPoint);

}  // namespace mtport::webdav
