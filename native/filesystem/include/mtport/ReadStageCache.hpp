// Disk-backed fallback for devices without GetPartialObject: the whole object is downloaded
// once into a private cache file and Finder reads are served from it.
//
// A cache file becomes visible only after the download finished completely (the device layer
// downloads into a temporary name and renames atomically), so an incomplete file can never be
// mistaken for a complete one. The cache is disposable: it is emptied at start-up because MTP
// object ids are not stable across sessions.
#pragma once

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

#include "mtport/IMtpDevice.hpp"
#include "mtport/UniqueFd.hpp"

namespace mtport {

class ReadStageCache {
public:
    struct Config {
        std::filesystem::path directory;
        uint64_t quotaBytes{0};  // 0 = min(8 GiB, 10% of free disk)
        uint64_t minFreeDiskBytes{256u << 20};
    };

    struct Entry;

    class Lease {
    public:
        Lease() = default;
        uint64_t size() const;
        // pread() on the completed local copy (short only at EOF).
        size_t read(uint64_t offset, std::span<std::byte> dst) const;
        explicit operator bool() const { return static_cast<bool>(entry_); }

    private:
        friend class ReadStageCache;
        explicit Lease(std::shared_ptr<Entry> e) : entry_(std::move(e)) {}
        std::shared_ptr<Entry> entry_;
    };

    explicit ReadStageCache(Config cfg);

    // Returns a lease on a complete local copy, downloading it first if needed. Throws
    // FileTooLarge when the object cannot fit the budget and StagingDiskFull when the Mac lacks space.
    Lease acquire(uint64_t generation, const FileNode& node, IMtpDevice& device, const ProgressCallback& progress = {});

    uint64_t usedBytes() const;
    uint64_t quotaBytes() const { return quota_; }
    size_t entryCount() const;

private:
    void evictFor(uint64_t needed);  // caller holds mutex_
    std::filesystem::path dir_;
    uint64_t quota_;
    uint64_t minFree_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
    std::list<std::string> lru_;  // front = most recent
    uint64_t used_{0};
    uint64_t counter_{0};
};

}  // namespace mtport
