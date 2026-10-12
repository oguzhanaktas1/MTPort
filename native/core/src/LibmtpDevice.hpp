#pragma once

#include <atomic>
#include <condition_variable>
#include <set>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "MtpSessionGuard.hpp"
#include "mtport/DeviceLock.hpp"
#include "mtport/IMtpDevice.hpp"

namespace mtport {

// Single owner of one libmtp session. Every public call takes `mutex_`: the MTP wire protocol
// is strictly serial and libmtp handles are not thread safe.
class LibmtpDevice final : public IMtpDevice {
public:
    LibmtpDevice(detail::SessionPtr session, DeviceDescriptor desc, DeviceLock lock);
    ~LibmtpDevice() override;

    DeviceDescriptor descriptor() const override { return desc_; }
    DeviceCapabilities capabilities() const override { return caps_; }
    std::vector<StorageDescriptor> storages() override;
    std::vector<FileNode> list(StorageId, ObjectId parent) override;
    FileNode stat(ObjectId) override;
    size_t read(ObjectId, uint64_t offset, std::span<std::byte> dst) override;
    void download(ObjectId, const std::filesystem::path& target, ProgressCallback progress) override;
    ObjectId upload(StorageId, ObjectId parent, const std::filesystem::path& stagedSource, std::string_view finalName,
                    ProgressCallback progress) override;
    ObjectId mkdir(StorageId, ObjectId parent, std::string_view name) override;
    void remove(ObjectId, bool isDirectory) override;
    void rename(ObjectId, std::string_view newName) override;
    void cancelListings() override { cancelListings_ = true; }

private:
    [[noreturn]] void fail(const char* op, MtpErrc fallback);
    void ensureUsable() const;
    FileNode statLocked(ObjectId);
    // `mutex_` is held by the caller of listLocked (mutating operations); list() instead hands its lock to listImpl,
    // which lets other callers in between the per-object queries so one huge folder never freezes the whole phone.
    std::vector<FileNode> listLocked(StorageId, ObjectId parent);
    std::vector<FileNode> listImpl(StorageId, ObjectId parent, std::chrono::milliseconds maxAge,
                                   std::unique_lock<std::mutex>* yieldLock);
    std::vector<uint32_t> childHandles(StorageId, uint32_t parentHandle);
    std::unique_lock<std::mutex> lockDevice();
    void remember(const FileNode&);
    std::vector<StorageDescriptor> storagesLocked();

    // Metadata of objects seen in this session. The phone answers one query per object (~17 ms on a Galaxy), so a
    // 1,800-photo folder costs half a minute to list from scratch; the (cheap) handle list is always re-read, and only
    // objects that are new, older than the allowed age, or moved are queried again. Direct stat() always asks the phone.
    struct CachedNode {
        FileNode node;
        std::chrono::steady_clock::time_point fetched;
    };
    std::unordered_map<ObjectId, CachedNode> objects_;  // guarded by mutex_
    // One listing per folder at a time: a second caller (Finder asking while the background warm-up is still reading
    // the same folder) waits and then finds everything remembered, instead of asking the phone for each object twice.
    std::mutex listingMutex_;
    std::condition_variable listingDone_;
    std::set<std::pair<StorageId, ObjectId>> listingsRunning_;
    std::atomic<bool> cancelListings_{false};
    std::atomic<int> waiting_{0};                       // callers blocked on mutex_ (lets a long listing step aside)

    mutable std::mutex mutex_;
    detail::SessionPtr session_;
    DeviceDescriptor desc_;
    DeviceCapabilities caps_{};
    DeviceLock lock_;  // keeps the cross-process ownership lock for the session's lifetime
    bool broken_{false};
};

}  // namespace mtport
