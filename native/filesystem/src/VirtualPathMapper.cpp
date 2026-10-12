#include "mtport/VirtualPathMapper.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "mtport/NameRules.hpp"
#include "mtport/PosixErrorMapper.hpp"

namespace mtport {

std::vector<std::string> splitVirtualPath(std::string_view path) {
    if (path.empty() || path.front() != '/') throw FsError(EINVAL, "virtual path must be absolute");
    if (path.find('\0') != std::string_view::npos) throw FsError(EINVAL, "NUL in path");
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        const size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i == start) break;
        std::string_view comp = path.substr(start, i - start);
        if (comp == "." || comp == "..") throw FsError(EINVAL, "path traversal component");
        if (comp.size() > kMaxNameBytes) throw FsError(ENAMETOOLONG, "path component too long");
        out.emplace_back(comp);
    }
    return out;
}

std::string normalizeNfc(std::string_view s) {
    bool ascii = true;
    for (unsigned char c : s)
        if (c >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii) return std::string(s);

    CFStringRef cf = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                             static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
    if (!cf) return std::string(s);  // not valid UTF-8: compare bytes
    CFMutableStringRef m = CFStringCreateMutableCopy(nullptr, 0, cf);
    CFRelease(cf);
    if (!m) return std::string(s);
    CFStringNormalize(m, kCFStringNormalizationFormC);
    const CFIndex len = CFStringGetLength(m);
    CFIndex needed = 0;
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, nullptr, 0, &needed);
    std::string out(static_cast<size_t>(needed), '\0');
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out.data()),
                     needed, nullptr);
    CFRelease(m);
    return out;
}

const DirEntryView* DirListing::find(std::string_view name) const {
    auto it = byKey.find(normalizeNfc(name));
    return it == byKey.end() ? nullptr : &entries[it->second];
}

namespace {

std::string sanitizeName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    // Replace '/' (U+2215 DIVISION SLASH) and escape bytes that are not valid UTF-8.
    for (size_t i = 0; i < raw.size();) {
        const auto c = static_cast<unsigned char>(raw[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > raw.size() || !isValidUtf8(std::string_view(raw).substr(i, n))) {
            char esc[4];
            std::snprintf(esc, sizeof esc, "%%%02X", c);
            out += esc;
            ++i;
            continue;
        }
        if (c == '/')
            out += "\xE2\x88\x95";
        else if (c != 0)
            out.append(raw, i, n);
        i += n;
    }
    if (out.empty()) out = "(unnamed)";
    if (out == ".") out = "(.)";
    if (out == "..") out = "(..)";
    if (out.size() > kMaxNameBytes) {
        out.resize(kMaxNameBytes);
        while (!out.empty() && !isValidUtf8(out)) out.pop_back();
    }
    return out;
}

std::string withSuffix(const std::string& name, int n) {
    const auto dot = name.rfind('.');
    const std::string tag = " (" + std::to_string(n) + ")";
    if (dot == std::string::npos || dot == 0) return name + tag;
    return name.substr(0, dot) + tag + name.substr(dot);
}

}  // namespace

DirListing buildDirListing(std::vector<FileNode> nodes) {
    std::stable_sort(nodes.begin(), nodes.end(),
                     [](const FileNode& a, const FileNode& b) { return a.objectId < b.objectId; });
    DirListing out;
    std::set<std::string> taken;
    out.entries.reserve(nodes.size());
    for (auto& n : nodes) {
        std::string display = sanitizeName(n.name);
        std::string key = normalizeNfc(display);
        for (int i = 2; taken.count(key); ++i) {
            display = withSuffix(sanitizeName(n.name), i);
            key = normalizeNfc(display);
        }
        taken.insert(key);
        out.entries.push_back(DirEntryView{std::move(display), std::move(key), std::move(n)});
    }
    std::sort(out.entries.begin(), out.entries.end(), [](const DirEntryView& a, const DirEntryView& b) {
        if (a.displayName != b.displayName) return a.displayName < b.displayName;
        return a.node.objectId < b.node.objectId;
    });
    for (size_t i = 0; i < out.entries.size(); ++i) out.byKey.emplace(out.entries[i].key, i);
    return out;
}

std::vector<StorageEntry> buildStorageView(const std::vector<StorageDescriptor>& storages) {
    std::vector<StorageDescriptor> sorted = storages;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::vector<StorageEntry> out;
    std::set<std::string> taken;
    for (auto& s : sorted) {
        std::string base = sanitizeName(s.name.empty() ? "Storage " + std::to_string(s.id) : s.name);
        std::string display = base;
        std::string key = normalizeNfc(display);
        for (int i = 2; taken.count(key); ++i) {
            display = base + " (" + std::to_string(i) + ")";
            key = normalizeNfc(display);
        }
        taken.insert(key);
        out.push_back(StorageEntry{std::move(display), std::move(key), s});
    }
    return out;
}

bool isMacMetadataName(std::string_view n) {
    return n == ".DS_Store" || n == ".localized" || n == ".metadata_never_index" || n == ".hidden" ||
           (n.size() > 2 && n.substr(0, 2) == "._");
}

bool isMacMetadataDirName(std::string_view n) {
    return n == ".Spotlight-V100" || n == ".fseventsd" || n == ".Trashes" || n == ".TemporaryItems" ||
           n == ".DocumentRevisions-V100" || n == ".com.apple.timemachine.donotpresent";
}

uint64_t InodeTable::allocate(uint64_t key) {
    std::lock_guard lk(mutex_);
    auto [it, inserted] = map_.try_emplace(key, next_);
    if (inserted) ++next_;
    return it->second;
}
uint64_t InodeTable::forStorage(StorageId storage) {
    return allocate((uint64_t{1} << 63) | storage);
}
uint64_t InodeTable::forObject(StorageId storage, ObjectId object) {
    return allocate((uint64_t{storage} << 32) | object);
}
void InodeTable::reset() {
    std::lock_guard lk(mutex_);
    map_.clear();
    next_ = 16;
}

}  // namespace mtport
