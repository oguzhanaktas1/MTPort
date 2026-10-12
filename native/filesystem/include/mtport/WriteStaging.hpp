// Host-side staging for Finder writes.
//
// A StagedFile is a private 0600 file under the staging directory. FUSE writes land there with
// pwrite(); nothing is sent to the phone until commit(). State machine:
//
//     OPEN -> DIRTY -> COMMITTING -> COMMITTED
//                          \------> FAILED   (terminal; staged bytes are preserved)
//
// commit() is idempotent: COMMITTED returns the remembered object id, FAILED rethrows the
// remembered error. A failed upload is never retried blindly (the phone may hold a partial
// object, and a retry could duplicate or collide).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mtport/DeviceTypes.hpp"
#include "mtport/MtpError.hpp"
#include "mtport/UniqueFd.hpp"

namespace mtport {

enum class StageState { Open, Dirty, Committing, Committed, Failed };
const char* toString(StageState s);

class WriteStaging;

class StagedFile {
public:
    ~StagedFile();
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;

    const std::string& finalName() const { return finalName_; }
    StorageId storageId() const { return storage_; }
    ObjectId parentId() const { return parent_; }
    const std::string& stageId() const { return id_; }

    StageState state() const;
    uint64_t size() const;
    std::optional<MtpErrc> failure() const;

    // Random-access write. Throws StagingDiskFull when quota or host space is exhausted,
    // OperationUnsupported when the file is already committed, the stored error when failed.
    size_t write(uint64_t offset, std::span<const std::byte> data);
    size_t read(uint64_t offset, std::span<std::byte> dst);
    void truncate(uint64_t newSize);

    using SendFn = std::function<ObjectId(const std::filesystem::path& stagedFile)>;
    // Uploads through `send` exactly once. See class comment for the idempotence rules.
    ObjectId commit(const SendFn& send);

    // Used when the owner abandons a file that never had any content committed *and* the user
    // explicitly unlinked it. Deletes the host staging file (never a remote object).
    void discard();

private:
    friend class WriteStaging;
    StagedFile(WriteStaging* owner, std::string id, std::filesystem::path path, UniqueFd fd, std::string finalName,
               StorageId storage, ObjectId parent);
    void writeManifestLocked(const std::string& error);

    WriteStaging* owner_;
    std::string id_;
    std::filesystem::path path_;
    UniqueFd fd_;
    std::string finalName_;
    StorageId storage_;
    ObjectId parent_;

    mutable std::mutex mutex_;
    StageState state_{StageState::Open};
    uint64_t size_{0};      // logical file size (what Finder sees)
    uint64_t reserved_{0};  // bytes charged against the quota (high-water mark)
    ObjectId committedId_{0};
    bool discarded_{false};
    std::optional<MtpErrc> failure_;
    std::string failureText_;
};

class WriteStaging {
public:
    struct Config {
        std::filesystem::path directory;
        uint64_t quotaBytes{0};                // 0 = default: min(20 GiB, 20% of free disk)
        uint64_t minFreeDiskBytes{64u << 20};  // never fill the host disk below this
    };

    explicit WriteStaging(Config cfg);

    static uint64_t defaultQuota(const std::filesystem::path& dir);

    // New private staging file already in DIRTY state (a created file must exist after close
    // even if it is empty).
    std::shared_ptr<StagedFile> create(std::string finalName, StorageId storage, ObjectId parent);

    uint64_t quotaBytes() const { return quota_; }
    uint64_t usedBytes() const { return used_.load(); }
    const std::filesystem::path& directory() const { return dir_; }

    struct Recoverable {
        std::filesystem::path stagedFile, manifest;
        std::string finalName;
        uint64_t sizeBytes{};
        std::string error;
    };
    // Failed uploads kept on disk for manual recovery (manifest next to the staged bytes).
    std::vector<Recoverable> listRecoverable() const;

private:
    friend class StagedFile;
    // Reserve/release quota; throws MtpError{StagingDiskFull}.
    void reserve(uint64_t bytes);
    void release(uint64_t bytes);

    std::filesystem::path dir_;
    uint64_t quota_;
    uint64_t minFree_;
    std::atomic<uint64_t> used_{0};
    std::mutex reserveMutex_;
};

}  // namespace mtport
