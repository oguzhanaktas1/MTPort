// Bounded TTL cache of directory listings (a miss inside a cached listing is itself a TTL-bounded
// negative answer, so there is no separate negative-lookup table). Metadata only: never file bytes.
#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mtport/VirtualPathMapper.hpp"

namespace mtport {

class MetadataCache {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Duration = std::chrono::milliseconds;

    struct Config {
        Duration dirTtl{2000};
        size_t maxDirectories{256};
        size_t maxTotalNodes{250'000};  // ~ tens of MB worst case; bounds memory for huge folders
    };

    MetadataCache() : MetadataCache(Config{}, nullptr) {}
    explicit MetadataCache(Config cfg, Clock clock = nullptr);

    // Listing for (generation, storage, parent). nullptr when absent or expired.
    std::shared_ptr<const DirListing> getDir(uint64_t generation, StorageId, ObjectId parent);
    void putDir(uint64_t generation, StorageId, ObjectId parent, std::shared_ptr<const DirListing>);
    void invalidateDir(uint64_t generation, StorageId, ObjectId parent);

    void clear();
    size_t directoryCount() const;
    size_t nodeCount() const;

private:
    struct DirKey {
        uint64_t gen;
        StorageId storage;
        ObjectId parent;
        bool operator==(const DirKey&) const = default;
    };
    struct DirKeyHash {
        size_t operator()(const DirKey& k) const noexcept {
            return std::hash<uint64_t>()(k.gen * 1000003u ^ (uint64_t{k.storage} << 32 | k.parent));
        }
    };
    struct DirSlot {
        std::shared_ptr<const DirListing> listing;
        std::chrono::steady_clock::time_point stored;
        std::list<DirKey>::iterator lru;
    };

    std::chrono::steady_clock::time_point now() const;
    void evictLocked();

    Config cfg_;
    Clock clock_;
    mutable std::mutex mutex_;
    std::unordered_map<DirKey, DirSlot, DirKeyHash> dirs_;
    std::list<DirKey> lru_;  // front = most recently used
    size_t nodes_{0};
};

}  // namespace mtport
