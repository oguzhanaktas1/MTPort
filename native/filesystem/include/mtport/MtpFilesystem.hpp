// POSIX-flavoured view of one MTP device. FUSE-agnostic: every method throws FsError (errno)
// and takes/returns plain types, so the whole layer is testable against a fake device without macFUSE.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "mtport/FileHandleTable.hpp"
#include "mtport/IMtpDevice.hpp"
#include "mtport/MetadataCache.hpp"
#include "mtport/PosixErrorMapper.hpp"
#include "mtport/ReadStageCache.hpp"
#include "mtport/VirtualPathMapper.hpp"
#include "mtport/WriteStaging.hpp"

namespace mtport {

struct FsConfig {
    bool readOnly{false};
    // Finder-created ._*, .DS_Store, ... are kept in RAM only (never sent to the phone) unless false.
    bool metadataOverlay{true};
    MetadataCache::Config cache{};
    std::filesystem::path stagingDir;    // WriteStaging directory (default: ~/Library/Caches/MTPort/Staging)
    uint64_t stagingQuotaBytes{0};       // 0 = default budget
    std::filesystem::path readStageDir;  // ReadStageCache directory
    uint64_t readStageQuotaBytes{0};
    size_t maxReadBytes{16u << 20};  // device read chunk, clamps a single device call (not a speed cap)
    uint32_t uid{0}, gid{0};        // owner reported for every node
    // Off by default: chmod/chown/utimens on existing phone files fail with EPERM/EOPNOTSUPP because
    // MTP cannot honour them. true turns them into silent no-ops for tools that insist on them.
    bool acceptMetadataNoops{false};
    // How long an empty, closed, newly created file waits for the re-open that FSKit performs before it is
    // uploaded as an empty file (touch).
    std::chrono::milliseconds emptyFileGrace{5000};
};

struct FsStat {
    uint64_t ino{};
    bool isDir{};
    uint32_t mode{};  // permission bits only (no S_IFMT)
    uint32_t nlink{1};
    uint64_t size{};
    bool pending{false};  // a new file that exists only on the Mac so far (not uploaded yet)
    std::chrono::system_clock::time_point mtime{};
    uint32_t uid{}, gid{};
};

struct FsDirEntry {
    std::string name;
    uint64_t ino{};
    bool isDir{};
};

struct FsStatvfs {
    uint64_t blockSize{4096};
    uint64_t blocks{}, blocksFree{}, blocksAvail{};
    uint64_t files{}, filesFree{};
    uint64_t nameMax{255};
};

struct FsCounters {
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> bytesStaged{0};
    std::atomic<uint64_t> bytesUploaded{0};
    std::atomic<uint64_t> uploadsOk{0};
    std::atomic<uint64_t> uploadsFailed{0};
    std::atomic<uint64_t> uploadsActive{0};
    std::atomic<uint64_t> uploadBytesDone{0};  // progress of the upload in flight
    std::atomic<uint64_t> uploadBytesTotal{0};
    std::atomic<uint64_t> deviceReads{0};
};

class MtpFilesystem {
public:
    MtpFilesystem(std::shared_ptr<IMtpDevice> device, FsConfig cfg);
    ~MtpFilesystem();

    // Listings may be minutes old (see LibmtpDevice); `fresh` makes a regular file's size and date come straight from
    // the phone. Anything that promises a length to a reader (HTTP Content-Length, a read) must use it.
    FsStat getattr(const std::string& path, bool fresh = false);

    // Lists the given folders in the background-friendly way so that Finder's first visit finds them already read.
    // Paths are relative to a storage ("DCIM/Camera"); every storage is tried; unknown folders and errors are ignored.
    // Returns when done or when `stop()` returns true.
    void cancelBackgroundWork();  // makes a running prefetch (or any huge listing) return promptly; used at shutdown
    void prefetch(const std::vector<std::string>& storageRelativePaths, const std::function<bool()>& stop);
    std::vector<FsDirEntry> readdir(const std::string& path);  // includes "." and ".."

    uint64_t open(const std::string& path, int flags);
    uint64_t create(const std::string& path, uint32_t mode);
    size_t read(uint64_t fh, uint64_t offset, std::span<std::byte> dst);
    size_t write(uint64_t fh, uint64_t offset, std::span<const std::byte> data);
    void flush(uint64_t fh);
    void fsync(uint64_t fh);
    void release(uint64_t fh) noexcept;
    void ftruncate(uint64_t fh, uint64_t size);

    void mkdir(const std::string& path);
    void unlink(const std::string& path);
    void rmdir(const std::string& path);
    void rename(const std::string& from, const std::string& to);
    void truncate(const std::string& path, uint64_t size);
    // utimens/setcrtime/...: honest EOPNOTSUPP for files that already live on the phone; accepted as a no-op for
    // objects created through this mount (the phone assigns their times).
    void utimens(const std::string& path);
    void chmod(const std::string& path, uint32_t mode);
    void chown(const std::string& path, int64_t uid, int64_t gid);
    FsStatvfs statfs(const std::string& path);

    // ---- lifecycle ---------------------------------------------------------------------
    // Called after the phone was re-enumerated with a fresh MTP session: all handles become
    // ESTALE, caches and inodes are dropped. `device` replaces the old session.
    void reconnect(std::shared_ptr<IMtpDevice> device);
    // The phone vanished: every call now fails with ENODEV; staged bytes are kept.
    void markDisconnected();
    // Drops this filesystem's reference to the MTP session (so the single-owner lock can be taken by a
    // new session) without notifying the disconnect callback. All calls fail with ENODEV until reconnect().
    void detachDevice();
    bool isDisconnected() const { return disconnected_.load(); }
    uint64_t generation() const { return generation_.load(); }

    // Uploads empty files that were created, closed and never re-opened for writing. Call about once a second and
    // (with force=true) before unmount. Never throws; failures are recorded like any failed upload.
    void housekeeping(bool force = false) noexcept;

    const FsCounters& counters() const { return counters_; }
    size_t openHandleCount() const { return handles_.size(); }
    size_t pendingUploadCount() const;
    const FsConfig& config() const { return cfg_; }
    std::vector<WriteStaging::Recoverable> recoverableUploads() const { return staging_.listRecoverable(); }
    DeviceCapabilities deviceCapabilities() const;

    // Invoked once (from the thread that observed it) when an operation proves the phone is gone.
    void setDisconnectCallback(std::function<void()> cb) { onDisconnect_ = std::move(cb); }

private:
    struct Resolved;
    struct Overlay;
    struct Pending;

    std::shared_ptr<IMtpDevice> dev() const;
    void checkAlive() const;
    template <class F>
    auto guarded(F&& f);

    std::vector<StorageEntry> storageView();
    std::shared_ptr<const DirListing> listing(StorageId, ObjectId parent);
    Resolved resolve(const std::string& path);
    void refreshNode(Resolved&);  // re-reads a regular file's node from the phone
    Resolved resolveParent(const std::string& path, std::string& leaf);
    FsStat statOf(const Resolved&);
    uint64_t ino(const Resolved&);
    void invalidateParent(StorageId, ObjectId parent);
    void markFresh(StorageId, ObjectId);
    bool hasOpenHandle(const std::shared_ptr<StagedFile>&) const;
    void commitLingering(const std::shared_ptr<Pending>&);
    void commit(OpenFile& f);
    static std::string pendingKeyFor(StorageId, ObjectId parent, const std::string& nameKey);

    std::shared_ptr<IMtpDevice> device_;
    mutable std::mutex deviceMutex_;  // protects the shared_ptr swap only
    FsConfig cfg_;
    std::atomic<uint64_t> generation_{1};
    std::atomic<bool> disconnected_{false};
    std::atomic<bool> disconnectNotified_{false};
    std::function<void()> onDisconnect_;
    std::chrono::system_clock::time_point mountTime_;

    MetadataCache cache_;
    InodeTable inodes_;
    FileHandleTable handles_;
    WriteStaging staging_;
    ReadStageCache readStage_;
    FsCounters counters_;

    mutable std::mutex stateMutex_;  // pending creates, storage cache, overlay
    std::atomic<ObjectId> pendingSeq_{0};
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
    std::vector<StorageEntry> storageCache_;
    // Objects created through this mount in this session. Finder/FSKit set their timestamps right after
    // creation (utimens/setcrtime...); MTP cannot store them, so for these (and only these) it is a no-op.
    std::set<std::pair<StorageId, ObjectId>> freshObjects_;
    std::chrono::steady_clock::time_point storageCacheTime_{};
    std::unique_ptr<Overlay> overlay_;
};

}  // namespace mtport
