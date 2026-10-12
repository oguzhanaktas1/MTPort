// FUSE file-handle bookkeeping. Every handle records the mount generation it was opened in;
// after a reconnect (generation bump) old handles answer ESTALE instead of serving stale bytes.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mtport/DeviceTypes.hpp"

namespace mtport {

class StagedFile;
struct OverlayFile;

struct OpenFile {
    uint64_t generation{};
    FileNode node;  // snapshot taken at open time (existing remote files); size is authoritative for reads
    int flags{};
    bool writeRequested{false};            // opened with O_RDWR: reads allowed, writes refused
    std::atomic<bool> stageMode{false};    // reads are served from the ReadStageCache
    std::shared_ptr<StagedFile> staged;    // a new file being written on the Mac
    std::shared_ptr<OverlayFile> overlay;  // in-memory Finder metadata file
    std::string pendingKey;                // key in the filesystem's pending-create table
    std::mutex mutex;                      // serialises writes on this handle
};

class FileHandleTable {
public:
    uint64_t insert(std::shared_ptr<OpenFile> file);
    // Throws FsError(EBADF) for unknown handles and FsError(ESTALE) for handles opened in an
    // older generation.
    std::shared_ptr<OpenFile> get(uint64_t handle, uint64_t currentGeneration) const;
    std::shared_ptr<OpenFile> remove(uint64_t handle);  // nullptr if unknown
    std::vector<std::shared_ptr<OpenFile>> snapshot() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<OpenFile>> files_;
    uint64_t next_{1};
};

}  // namespace mtport
