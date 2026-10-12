#include "mtport/ReadStageCache.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "mtport/RuntimePaths.hpp"

namespace fs = std::filesystem;

namespace mtport {

struct ReadStageCache::Entry {
    std::mutex mutex;  // held while the object downloads; later readers wait here
    bool complete{false};
    fs::path path;
    UniqueFd fd;
    uint64_t size{0};
    std::string key;
};

uint64_t ReadStageCache::Lease::size() const {
    return entry_ ? entry_->size : 0;
}

size_t ReadStageCache::Lease::read(uint64_t offset, std::span<std::byte> dst) const {
    if (!entry_ || !entry_->complete) throw MtpError(MtpErrc::StaleHandle, "no completed local copy");
    if (offset >= entry_->size || dst.empty()) return 0;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(dst.size(), entry_->size - offset));
    size_t done = 0;
    while (done < want) {
        const ssize_t n = ::pread(entry_->fd.get(), dst.data() + done, want - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw MtpError(MtpErrc::IoError, "read-cache read failed");
        }
        if (n == 0) throw MtpError(MtpErrc::IoError, "read-cache file is shorter than the remote size");
        done += static_cast<size_t>(n);
    }
    return want;
}

namespace {
uint64_t availableBytes(const fs::path& dir) {
    struct statvfs sv{};
    if (::statvfs(dir.c_str(), &sv) != 0) return 0;
    return static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
}
}  // namespace

ReadStageCache::ReadStageCache(Config cfg) : dir_(std::move(cfg.directory)), minFree_(cfg.minFreeDiskBytes) {
    ensurePrivateDir(dir_);
    // Our own disposable cache: object ids from a previous session are meaningless now.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("rs-", 0) == 0 || name.find(".afpart-") != std::string::npos) fs::remove(e.path(), ec);
    }
    constexpr uint64_t kCap = 8ull << 30;
    quota_ = cfg.quotaBytes ? cfg.quotaBytes : std::min<uint64_t>(kCap, availableBytes(dir_) / 10);
}

uint64_t ReadStageCache::usedBytes() const {
    std::lock_guard lk(mutex_);
    return used_;
}
size_t ReadStageCache::entryCount() const {
    std::lock_guard lk(mutex_);
    return entries_.size();
}

void ReadStageCache::evictFor(uint64_t needed) {
    for (auto it = lru_.rbegin(); it != lru_.rend() && used_ + needed > quota_;) {
        auto e = entries_.find(*it);
        // use_count()==1: only the map references it, so no reader holds a lease.
        if (e != entries_.end() && e->second.use_count() == 1 && e->second->complete) {
            ::unlink(e->second->path.c_str());
            used_ -= e->second->size;
            entries_.erase(e);
            it = decltype(it)(lru_.erase(std::next(it).base()));
        } else {
            ++it;
        }
    }
}

ReadStageCache::Lease ReadStageCache::acquire(uint64_t generation, const FileNode& node, IMtpDevice& device,
                                              const ProgressCallback& progress) {
    const std::string key = std::to_string(generation) + ":" + std::to_string(node.objectId) + ":" +
                            std::to_string(node.sizeBytes) + ":" +
                            std::to_string(node.modifiedAt ? node.modifiedAt->time_since_epoch().count() : 0);
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard lk(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            entry = it->second;
            lru_.remove(key);
            lru_.push_front(key);
        } else {
            if (node.sizeBytes > quota_)
                throw MtpError(MtpErrc::FileTooLarge,
                               "file exceeds the read-cache budget and the phone does not support random-access reads");
            const uint64_t avail = availableBytes(dir_);
            if (avail < minFree_ || node.sizeBytes > avail - minFree_)
                throw MtpError(MtpErrc::StagingDiskFull,
                               "not enough free space on the Mac to stage this file for reading");
            evictFor(node.sizeBytes);
            if (used_ + node.sizeBytes > quota_)
                throw MtpError(MtpErrc::StagingDiskFull, "read-cache budget is fully pinned by files that are in use");
            entry = std::make_shared<Entry>();
            entry->key = key;
            entry->size = node.sizeBytes;
            entry->path = dir_ / ("rs-" + std::to_string(++counter_) + ".data");
            entries_.emplace(key, entry);
            lru_.push_front(key);
            used_ += node.sizeBytes;  // reserve now so concurrent acquires respect the budget
        }
    }

    std::lock_guard elk(entry->mutex);
    if (!entry->complete) {
        try {
            device.download(node.objectId, entry->path, progress);  // atomic: final name appears only when complete
            UniqueFd fd(::open(entry->path.c_str(), O_RDONLY | O_CLOEXEC));
            struct stat st{};
            if (!fd.valid() || ::fstat(fd.get(), &st) != 0 || static_cast<uint64_t>(st.st_size) != node.sizeBytes)
                throw MtpError(MtpErrc::IoError, "staged copy has the wrong size");
            entry->fd = std::move(fd);
            entry->complete = true;
        } catch (...) {
            std::lock_guard lk(mutex_);
            used_ -= entry->size;
            entries_.erase(key);
            lru_.remove(key);
            ::unlink(entry->path.c_str());  // our own cache file
            throw;
        }
    }
    return Lease(entry);
}

}  // namespace mtport
