#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "mtport/DeviceTypes.hpp"
#include "mtport/MtpError.hpp"
#include "mtport/TransferProgress.hpp"

namespace mtport {

// One IMtpDevice is the single owner of one MTP session. Implementations serialise their own
// calls (a mutex or a task executor); callers may use one instance from several threads but
// calls never run concurrently on the wire.
//
// Failures are reported by throwing MtpError. Nothing ever returns "success" when the device
// did not confirm the operation.
class IMtpDevice {
public:
    virtual ~IMtpDevice() = default;

    virtual DeviceDescriptor descriptor() const = 0;
    virtual DeviceCapabilities capabilities() const = 0;
    virtual std::vector<StorageDescriptor> storages() = 0;

    // Non-recursive listing of one folder. parent == kRootObject lists the storage root.
    virtual std::vector<FileNode> list(StorageId, ObjectId parent) = 0;
    virtual FileNode stat(ObjectId) = 0;

    // Reads up to dst.size() bytes at `offset`. Returns the bytes read (0 only at/after EOF
    // or when dst is empty). Throws OperationUnsupported when the device cannot do partial
    // reads, so the caller can fall back to download().
    virtual size_t read(ObjectId, uint64_t offset, std::span<std::byte> dst) = 0;

    // Streams the object into `target`. `target` must not exist; the data is written to a
    // private temporary file in the same directory and atomically moved into place only after
    // the complete object arrived.
    virtual void download(ObjectId, const std::filesystem::path& target, ProgressCallback progress) = 0;

    // Sends a *new* object. Throws FileAlreadyExists if `finalName` exists in the folder.
    virtual ObjectId upload(StorageId, ObjectId parent, const std::filesystem::path& stagedSource,
                            std::string_view finalName, ProgressCallback progress) = 0;

    virtual ObjectId mkdir(StorageId, ObjectId parent, std::string_view name) = 0;

    // Deletes exactly one object. A directory must be empty (DirectoryNotEmpty otherwise);
    // this layer never deletes recursively.
    virtual void remove(ObjectId, bool isDirectory) = 0;
    virtual void rename(ObjectId, std::string_view newName) = 0;

    // Asks long-running listings to stop at their next step (they then fail with MtpErrc::Cancelled). Used at shutdown
    // so that an eject never waits for a half-read photo folder. Devices without such work ignore it.
    virtual void cancelListings() {}
};

}  // namespace mtport
