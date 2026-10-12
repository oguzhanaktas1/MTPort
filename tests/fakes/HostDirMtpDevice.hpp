// A "phone" whose single storage is a directory on the Mac. TEST SUPPORT ONLY: lets the CLI
// (`--backend fake`) and the mock FSKit mount exercise the real code paths with state that
// survives between processes. Never used as the product backend.
#pragma once

#include <map>
#include <mutex>

#include "mtport/IMtpDevice.hpp"

namespace mtport::fake {

class HostDirMtpDevice final : public IMtpDevice {
public:
    struct Options {
        std::string id = "fake:0002@1.2";
        std::string model = "Mock Phone (host directory)";
        std::string storageName = "Internal Storage";
        bool partialRead = true;
    };
    static constexpr StorageId kStorageId = 0x00010001;

    HostDirMtpDevice(std::filesystem::path root, Options opt);

    DeviceDescriptor descriptor() const override;
    DeviceCapabilities capabilities() const override;
    std::vector<StorageDescriptor> storages() override;
    std::vector<FileNode> list(StorageId, ObjectId parent) override;
    FileNode stat(ObjectId) override;
    size_t read(ObjectId, uint64_t offset, std::span<std::byte> dst) override;
    void download(ObjectId, const std::filesystem::path& target, ProgressCallback progress) override;
    ObjectId upload(StorageId, ObjectId parent, const std::filesystem::path& src, std::string_view finalName,
                    ProgressCallback progress) override;
    ObjectId mkdir(StorageId, ObjectId parent, std::string_view name) override;
    void remove(ObjectId, bool isDirectory) override;
    void rename(ObjectId, std::string_view newName) override;

private:
    std::filesystem::path pathOf(ObjectId) const;  // caller holds mutex_
    ObjectId idFor(const std::filesystem::path& rel);
    FileNode nodeFor(ObjectId id, const std::filesystem::path& rel, bool isDir, uint64_t size,
                     std::chrono::system_clock::time_point mtime, ObjectId parent) const;
    void checkStorage(StorageId) const;

    std::filesystem::path root_;
    Options opt_;
    mutable std::mutex mutex_;
    std::map<ObjectId, std::filesystem::path> idToRel_;
    std::map<std::filesystem::path, ObjectId> relToId_;
    ObjectId next_{1};
};

}  // namespace mtport::fake
