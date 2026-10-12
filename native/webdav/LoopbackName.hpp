// Publishes the single-label host name "<phone name>" -> 127.0.0.1 through the system's mDNS responder (dns_sd), for as long as the object
// lives. Finder lists a network volume under the host name of the URL it was mounted from, so this lets the sidebar
// show the phone's own name, spelled exactly as the owner typed it (case, spaces and accents included). Needs no
// root. The record only ever points at the loopback address.
#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace mtport::webdav {

// The phone's name made safe to be one DNS label: dots/control characters become blanks, surrounding blanks are
// trimmed, blanks become no-break spaces (the only kind NetFS accepts in a host) and the UTF-8 is cut at 63 bytes on
// a character boundary. Case and accents are kept.
// Returns "" when nothing usable is left.
std::string displayHostLabel(const std::string& deviceName);

class LoopbackName {
public:
    // Registers `displayHostLabel(deviceName)`; waits up to `timeoutMs` for the system resolver to find it.
    LoopbackName(const std::string& deviceName, int timeoutMs = 4000);
    ~LoopbackName();
    LoopbackName(const LoopbackName&) = delete;
    LoopbackName& operator=(const LoopbackName&) = delete;

    bool ok() const { return ok_; }
    const std::string& host() const { return host_; }  // the bare label when ok()
    const std::string& error() const { return error_; }

    struct Impl;  // implementation detail (dns_sd state)

private:
    Impl* impl_{nullptr};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool ok_{false};
    std::string host_, error_;
};

}  // namespace mtport::webdav
