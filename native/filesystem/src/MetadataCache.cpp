#include "mtport/MetadataCache.hpp"

namespace mtport {

MetadataCache::MetadataCache(Config cfg, Clock clock) : cfg_(cfg), clock_(std::move(clock)) {}

std::chrono::steady_clock::time_point MetadataCache::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

std::shared_ptr<const DirListing> MetadataCache::getDir(uint64_t gen, StorageId s, ObjectId p) {
    std::lock_guard lk(mutex_);
    auto it = dirs_.find(DirKey{gen, s, p});
    if (it == dirs_.end()) return nullptr;
    if (now() - it->second.stored >= cfg_.dirTtl) {
        nodes_ -= it->second.listing->entries.size();
        lru_.erase(it->second.lru);
        dirs_.erase(it);
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.listing;
}

void MetadataCache::putDir(uint64_t gen, StorageId s, ObjectId p, std::shared_ptr<const DirListing> listing) {
    if (!listing) return;
    std::lock_guard lk(mutex_);
    const DirKey key{gen, s, p};
    if (auto it = dirs_.find(key); it != dirs_.end()) {
        nodes_ -= it->second.listing->entries.size();
        lru_.erase(it->second.lru);
        dirs_.erase(it);
    }
    if (listing->entries.size() > cfg_.maxTotalNodes) return;  // too big to cache at all
    lru_.push_front(key);
    nodes_ += listing->entries.size();
    dirs_[key] = DirSlot{std::move(listing), now(), lru_.begin()};
    evictLocked();
}

void MetadataCache::evictLocked() {
    while (!lru_.empty() && (dirs_.size() > cfg_.maxDirectories || nodes_ > cfg_.maxTotalNodes)) {
        const DirKey victim = lru_.back();
        auto it = dirs_.find(victim);
        nodes_ -= it->second.listing->entries.size();
        dirs_.erase(it);
        lru_.pop_back();
    }
}

void MetadataCache::invalidateDir(uint64_t gen, StorageId s, ObjectId p) {
    std::lock_guard lk(mutex_);
    if (auto it = dirs_.find(DirKey{gen, s, p}); it != dirs_.end()) {
        nodes_ -= it->second.listing->entries.size();
        lru_.erase(it->second.lru);
        dirs_.erase(it);
    }
}

void MetadataCache::clear() {
    std::lock_guard lk(mutex_);
    dirs_.clear();
    lru_.clear();
    nodes_ = 0;
}

size_t MetadataCache::directoryCount() const {
    std::lock_guard lk(mutex_);
    return dirs_.size();
}
size_t MetadataCache::nodeCount() const {
    std::lock_guard lk(mutex_);
    return nodes_;
}

}  // namespace mtport
