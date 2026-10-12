// Mapping between MTP objects and the POSIX names Finder sees: path splitting/validation,
// Unicode-normalised comparison, reversible display names, storage naming and inode numbers.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mtport/DeviceTypes.hpp"

namespace mtport {

// Splits an absolute virtual path. "/" -> {}. Rejects relative paths, "." / ".." components,
// embedded NULs and over-long components with FsError(EINVAL / ENAMETOOLONG). Repeated slashes
// are collapsed.
std::vector<std::string> splitVirtualPath(std::string_view path);

// NFC form used for *comparison only* (macOS tools may send NFD for names stored as NFC).
std::string normalizeNfc(std::string_view utf8);

// One displayed directory entry. `displayName` is what Finder sees; `node.name` is the original.
struct DirEntryView {
    std::string displayName;
    std::string key;  // NFC(displayName), used for lookups
    FileNode node;
};

struct DirListing {
    std::vector<DirEntryView> entries;  // sorted by displayName bytes, then objectId
    std::unordered_map<std::string, size_t> byKey;
    const DirEntryView* find(std::string_view name) const;  // NFC-insensitive lookup
};

// Builds the deterministic, collision-free view of a raw MTP folder listing:
//  * '/' becomes U+2215, NUL is dropped, empty/"."/".." names get a placeholder,
//  * invalid UTF-8 bytes are %XX escaped,
//  * duplicate names (MTP allows them) get " (2)", " (3)" ... before the extension, assigned in
//    objectId order so the result is stable for a given listing.
DirListing buildDirListing(std::vector<FileNode> nodes);

struct StorageEntry {
    std::string displayName;
    std::string key;
    StorageDescriptor storage;
};
// Storage labels as top-level folder names; duplicate/empty labels get stable suffixes.
std::vector<StorageEntry> buildStorageView(const std::vector<StorageDescriptor>& storages);

// True for names macOS/Finder create automatically: ._*, .DS_Store, .Spotlight-V100, ...
bool isMacMetadataName(std::string_view name);
bool isMacMetadataDirName(std::string_view name);

// Stable inode numbers inside one mount generation. Root is 1.
class InodeTable {
public:
    static constexpr uint64_t kRoot = 1;
    uint64_t forStorage(StorageId storage);
    uint64_t forObject(StorageId storage, ObjectId object);
    void reset();  // new generation: nothing carries over

private:
    uint64_t allocate(uint64_t key);
    std::mutex mutex_;
    std::unordered_map<uint64_t, uint64_t> map_;
    uint64_t next_{16};
};

}  // namespace mtport
