#include "WebDavMount.hpp"

#include <NetFS/NetFS.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <cctype>
#include <cstring>
#include <utility>

namespace mtport::webdav {

namespace {
struct CF {  // tiny RAII for CoreFoundation objects
    CFTypeRef ref;
    explicit CF(CFTypeRef r) : ref(r) {}
    ~CF() {
        if (ref) CFRelease(ref);
    }
    CF(const CF&) = delete;
    CF& operator=(const CF&) = delete;
};

std::string toStd(CFStringRef s) {
    char buf[1024];
    return s && CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? buf : "";
}
}  // namespace

std::string hostLabel(const std::string& deviceName) {
    // Latin letters outside ASCII are folded to their base letter (a Turkish phone name keeps reading naturally);
    // everything else that is not a letter or digit becomes a separator.
    static const std::pair<uint32_t, char> kFold[] = {
        {0xE7, 'c'}, {0xC7, 'c'}, {0x11F, 'g'}, {0x11E, 'g'}, {0x131, 'i'}, {0x130, 'i'}, {0xF6, 'o'}, {0xD6, 'o'},
        {0x15F, 's'}, {0x15E, 's'}, {0xFC, 'u'}, {0xDC, 'u'}, {0xE0, 'a'}, {0xE1, 'a'}, {0xE2, 'a'}, {0xE4, 'a'},
        {0xC4, 'a'}, {0xE8, 'e'}, {0xE9, 'e'}, {0xEA, 'e'}, {0xEB, 'e'}, {0xEC, 'i'}, {0xED, 'i'}, {0xEE, 'i'},
        {0xF2, 'o'}, {0xF3, 'o'}, {0xF4, 'o'}, {0xF9, 'u'}, {0xFA, 'u'}, {0xFB, 'u'}, {0xF1, 'n'}, {0xDF, 's'}};
    std::string out;
    auto put = [&](char c) {
        if (c == '-') {
            if (!out.empty() && out.back() != '-') out.push_back('-');
        } else {
            out.push_back(c);
        }
    };
    for (size_t i = 0; i < deviceName.size();) {
        const unsigned char b = static_cast<unsigned char>(deviceName[i]);
        uint32_t cp = b;
        size_t len = 1;
        if (b >= 0xF0) { cp = b & 0x07; len = 4; }
        else if (b >= 0xE0) { cp = b & 0x0F; len = 3; }
        else if (b >= 0xC0) { cp = b & 0x1F; len = 2; }
        if (len > 1) {
            if (i + len > deviceName.size()) break;
            for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(deviceName[i + k]) & 0x3F);
        }
        i += len;
        if (cp < 0x80) {
            const char c = static_cast<char>(cp);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) put(c);
            else if (c >= 'A' && c <= 'Z') put(static_cast<char>(c - 'A' + 'a'));
            else put('-');
            continue;
        }
        char folded = '-';
        for (const auto& [from, to] : kFold)
            if (from == cp) folded = to;
        put(folded);
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 63) {
        out.resize(63);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out.empty() ? "android" : out;
}

std::string loopbackHost(const std::string& deviceName) { return hostLabel(deviceName) + ".localhost"; }

MountResult mountVolume(uint16_t port, const std::string& user, const std::string& password, const std::string& volumeName,
                        bool readOnly, const std::string& host) {
    MountResult r;
    std::string encodedHost;  // the phone's name may hold spaces and accents: percent-encode everything but unreserved
    for (unsigned char c : host) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            encodedHost.push_back(static_cast<char>(c));
        } else {
            static const char* hex = "0123456789ABCDEF";
            encodedHost += {'%', hex[c >> 4], hex[c & 15]};
        }
    }
    const std::string urlText = "http://" + encodedHost + ":" + std::to_string(port) + "/" + volumeName + "/";
    CF url(CFURLCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(urlText.data()), static_cast<CFIndex>(urlText.size()),
                                kCFStringEncodingUTF8, nullptr));
    CF cfUser(CFStringCreateWithCString(nullptr, user.c_str(), kCFStringEncodingUTF8));
    CF cfPass(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
    CFMutableDictionaryRef open = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef mount = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CF openGuard(open), mountGuard(mount);
    // Never show an authentication or error dialog from a background worker.
    CFDictionarySetValue(open, kNAUIOptionKey, kNAUIOptionNoUI);
    // Read-only is not requested through MNT_RDONLY (that made NetFSMountURLSync hang): the server simply does not
    // advertise WebDAV class 2 (locking) when the volume is read-only, and macOS then mounts it read-only by itself.
    (void)readOnly;
    CFArrayRef points = nullptr;
    const int rc = NetFSMountURLSync(static_cast<CFURLRef>(url.ref), nullptr, static_cast<CFStringRef>(cfUser.ref),
                                     static_cast<CFStringRef>(cfPass.ref), open, mount, &points);
    CF pointsGuard(points);
    if (rc != 0 && rc != EEXIST) {
        r.error = "NetFS mount failed (" + std::to_string(rc) + ": " + std::strerror(rc > 0 ? rc : -rc) + ")";
        return r;
    }
    if (!points || CFArrayGetCount(points) < 1) {
        r.error = "NetFS reported success but returned no mount point";
        return r;
    }
    r.mountPoint = toStd(static_cast<CFStringRef>(CFArrayGetValueAtIndex(points, 0)));
    r.ok = !r.mountPoint.empty();
    if (!r.ok) r.error = "NetFS returned an empty mount point";
    return r;
}

bool isMounted(const std::string& mountPoint) {
    struct statfs* list = nullptr;
    const int n = ::getmntinfo(&list, MNT_NOWAIT);
    for (int i = 0; i < n; ++i)
        if (mountPoint == list[i].f_mntonname && std::strcmp(list[i].f_fstypename, "webdav") == 0) return true;
    return false;
}

}  // namespace mtport::webdav
