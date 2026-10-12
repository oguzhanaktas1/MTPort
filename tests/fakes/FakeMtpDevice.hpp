// In-memory phone with fault injection. TEST SUPPORT ONLY: it never talks to USB and is never
// the product backend. Behaviour mirrors LibmtpDevice (same checks, same typed errors) so the
// filesystem layer can be exercised without hardware.
#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "mtport/DeviceLock.hpp"
#include "mtport/IMtpDevice.hpp"
#include "mtport/IMtpDeviceFactory.hpp"

namespace mtport::fake {

enum class Op { Storages, List, Stat, Read, Download, Upload, Mkdir, Remove, Rename };

class FakeMtpDevice final : public IMtpDevice {
public:
    struct Options {
        std::string id = "fake:0001@1.1";
        std::string manufacturer = "Fake Inc.";
        std::string model = "Fake Phone";
        bool partialRead = true;  // false => read() throws OperationUnsupported (staged fallback path)
        bool renameSupported = true;
    };

    FakeMtpDevice() : FakeMtpDevice(Options{}) {}
    explicit FakeMtpDevice(Options opt);

    // ---- fixture construction ----------------------------------------------------------
    StorageId addStorage(std::string name, uint64_t capacity, uint64_t free, bool readOnly = false);
    ObjectId addDir(StorageId, ObjectId parent, std::string name);
    ObjectId addFile(StorageId, ObjectId parent, std::string name, std::vector<std::byte> bytes);
    ObjectId addFile(StorageId, ObjectId parent, std::string name, std::string_view text);
    // File whose content is generated (patternByte), so multi-GiB sizes cost no memory.
    ObjectId addSyntheticFile(StorageId, ObjectId parent, std::string name, uint64_t size, uint8_t seed);
    static uint8_t patternByte(uint8_t seed, uint64_t offset);

    // ---- fault injection ---------------------------------------------------------------
    // The next `count` calls of `op` throw `code` (count < 0: forever).
    void failNext(Op op, MtpErrc code, int count = 1);
    // The phone is "unplugged" right after the next call of `op` starts: that call and every
    // later call throws DeviceDisconnected.
    void unplugDuring(Op op);
    void setUnplugged(bool);
    void setStorageFree(StorageId, uint64_t free);

    // ---- introspection -------------------------------------------------------------------
    size_t calls(Op op) const;
    std::vector<ObjectId> listedParents() const;  // every parent id passed to list(), in order
    bool exists(ObjectId) const;
    std::vector<std::byte> bytesOf(ObjectId) const;  // materialises; for small files only
    size_t objectCount() const;

    // ---- IMtpDevice ------------------------------------------------------------------------
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
    struct Obj {
        FileNode node;
        std::vector<std::byte> bytes;
        bool synthetic{false};
        uint8_t seed{0};
    };
    struct Storage {
        StorageDescriptor d;
    };
    void enter(Op op);  // counts the call and applies fault injection; caller holds mutex_
    Obj& obj(ObjectId id);
    const Obj& obj(ObjectId id) const;
    void checkParent(StorageId, ObjectId parent);
    uint64_t contentSize(const Obj&) const;

    Options opt_;
    mutable std::mutex mutex_;
    std::map<StorageId, Storage> storages_;
    std::map<ObjectId, Obj> objects_;
    ObjectId nextId_{1};
    StorageId nextStorage_{0x00010001};
    bool unplugged_{false};
    std::map<Op, std::pair<MtpErrc, int>> failures_;
    std::set<Op> unplugOn_;
    std::map<Op, size_t> callCounts_;
    std::vector<ObjectId> listed_;
};

// Factory handing out single-owner sessions to one FakeMtpDevice (the phone).
class FakeMtpDeviceFactory final : public IMtpDeviceFactory {
public:
    explicit FakeMtpDeviceFactory(std::shared_ptr<IMtpDevice> phone);
    std::vector<DeviceDescriptor> enumerate() override;
    std::unique_ptr<IMtpDevice> open(const DeviceId& id) override;
    void setPresent(bool present) { present_ = present; }

private:
    std::shared_ptr<IMtpDevice> phone_;
    bool present_{true};
};

}  // namespace mtport::fake
