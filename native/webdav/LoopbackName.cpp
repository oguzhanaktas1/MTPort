#include "LoopbackName.hpp"

#include <dns_sd.h>
#include <netdb.h>
#include <poll.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstring>

namespace mtport::webdav {

std::string displayHostLabel(const std::string& deviceName) {
    // Dots would split the label and control characters are not allowed; both become blanks.
    std::string s;
    for (unsigned char c : deviceName) s.push_back((c < 0x20 || c == 0x7F || c == '.') ? ' ' : static_cast<char>(c));
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    s = s.substr(b, s.find_last_not_of(' ') - b + 1);
    // NetFS' WebDAV client cannot mount a URL whose host holds a plain space, but accepts a no-break space
    // (U+00A0), which Finder draws exactly like a space.
    std::string out;
    for (char c : s) {
        if (c == ' ') out += "\xC2\xA0";
        else out.push_back(c);
    }
    if (out.size() > 63) {
        size_t n = 63;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;  // do not cut inside a UTF-8 sequence
        out.resize(n);
    }
    while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\xC2\xA0") == 0) out.resize(out.size() - 2);
    return out;
}

struct LoopbackName::Impl {
    DNSServiceRef conn{nullptr};
    DNSRecordRef record{nullptr};
    std::atomic<int> state{0};  // 0 pending, 1 registered, -1 failed
    DNSServiceErrorType err{kDNSServiceErr_NoError};
};

namespace {
void DNSSD_API onRegistered(DNSServiceRef, DNSRecordRef, DNSServiceFlags, DNSServiceErrorType err, void* ctx) {
    auto* impl = static_cast<LoopbackName::Impl*>(ctx);
    impl->err = err;
    impl->state = err == kDNSServiceErr_NoError ? 1 : -1;
}
}  // namespace

LoopbackName::LoopbackName(const std::string& deviceName, int timeoutMs) {
    const std::string label = displayHostLabel(deviceName);
    if (label.empty()) {
        error_ = "the phone has no usable name";
        return;
    }
    const std::string fullName = label;  // a single label: the resolver finds it through mDNS without any domain suffix
    impl_ = new Impl;
    if (DNSServiceCreateConnection(&impl_->conn) != kDNSServiceErr_NoError) {
        error_ = "cannot reach the mDNS responder";
        return;
    }
    const unsigned char loopback[4] = {127, 0, 0, 1};
    const auto rc = DNSServiceRegisterRecord(impl_->conn, &impl_->record, kDNSServiceFlagsUnique, 0, fullName.c_str(),
                                             kDNSServiceType_A, kDNSServiceClass_IN, sizeof loopback, loopback, 120,
                                             onRegistered, impl_);
    if (rc != kDNSServiceErr_NoError) {
        error_ = "mDNS refused the name (" + std::to_string(rc) + ")";
        return;
    }
    thread_ = std::thread([this] {
        const int fd = DNSServiceRefSockFD(impl_->conn);
        while (!stop_.load()) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
                if (DNSServiceProcessResult(impl_->conn) != kDNSServiceErr_NoError) {
                    if (impl_->state == 0) impl_->state = -1;
                    return;
                }
            }
        }
    });
    // The responder does not always send the "registered" callback for a bare label, so success is judged by what
    // matters: the system resolver finds the name and it points at the loopback address.
    const auto resolves = [&] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        bool good = false;
        if (::getaddrinfo(fullName.c_str(), nullptr, &hints, &res) == 0) {
            for (auto* a = res; a; a = a->ai_next)
                if (a->ai_family == AF_INET &&
                    ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr) == INADDR_LOOPBACK)
                    good = true;
            ::freeaddrinfo(res);
        }
        return good;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (impl_->state != -1 && std::chrono::steady_clock::now() < deadline) {
        if (impl_->state == 1 || resolves()) {
            ok_ = true;
            host_ = fullName;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error_ = impl_->state == -1 ? "the name is already in use on this network (" + std::to_string(impl_->err) + ")"
                                : "the system resolver did not find the published name in time";
}

LoopbackName::~LoopbackName() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (impl_) {
        if (impl_->conn) DNSServiceRefDeallocate(impl_->conn);  // withdraws the record
        delete impl_;
    }
}

}  // namespace mtport::webdav
