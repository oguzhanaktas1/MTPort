#include "FakeMtpDevice.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>

#include "mtport/NameRules.hpp"

namespace fs = std::filesystem;

namespace mtport::fake {

namespace {
constexpr size_t kChunk = 64 * 1024;

std::string tmpSuffix() {
    static std::mt19937_64 rng{std::random_device{}()};
    return std::to_string(rng());
}
}  // namespace

FakeMtpDevice::FakeMtpDevice(Options opt) : opt_(std::move(opt)) {}

uint8_t FakeMtpDevice::patternByte(uint8_t seed, uint64_t off) {
    return static_cast<uint8_t>(((off * 0x9E3779B97F4A7C15ull) >> 56) ^ seed);
}

StorageId FakeMtpDevice::addStorage(std::string name, uint64_t capacity, uint64_t free, bool readOnly) {
    std::lock_guard lk(mutex_);
    const StorageId id = nextStorage_++;
    StorageDescriptor d;
    d.id = id;
    d.name = std::move(name);
    d.capacityBytes = capacity;
    d.freeBytes = free;
    d.readOnly = readOnly;
    storages_[id] = Storage{d};
    return id;
}

ObjectId FakeMtpDevice::addDir(StorageId s, ObjectId parent, std::string name) {
    std::lock_guard lk(mutex_);
    const ObjectId id = nextId_++;
    Obj o;
    o.node = FileNode{s, id, parent, std::move(name), true, 0, std::chrono::system_clock::now()};
    objects_[id] = std::move(o);
    return id;
}

ObjectId FakeMtpDevice::addFile(StorageId s, ObjectId parent, std::string name, std::vector<std::byte> bytes) {
    std::lock_guard lk(mutex_);
    const ObjectId id = nextId_++;
    Obj o;
    o.node = FileNode{s, id, parent, std::move(name), false, bytes.size(), std::chrono::system_clock::now()};
    o.bytes = std::move(bytes);
    objects_[id] = std::move(o);
    return id;
}

ObjectId FakeMtpDevice::addFile(StorageId s, ObjectId parent, std::string name, std::string_view text) {
    std::vector<std::byte> b(text.size());
    std::memcpy(b.data(), text.data(), text.size());
    return addFile(s, parent, std::move(name), std::move(b));
}

ObjectId FakeMtpDevice::addSyntheticFile(StorageId s, ObjectId parent, std::string name, uint64_t size, uint8_t seed) {
    std::lock_guard lk(mutex_);
    const ObjectId id = nextId_++;
    Obj o;
    o.node = FileNode{s, id, parent, std::move(name), false, size, std::chrono::system_clock::now()};
    o.synthetic = true;
    o.seed = seed;
    objects_[id] = std::move(o);
    return id;
}

void FakeMtpDevice::failNext(Op op, MtpErrc code, int count) {
    std::lock_guard lk(mutex_);
    failures_[op] = {code, count};
}
void FakeMtpDevice::unplugDuring(Op op) {
    std::lock_guard lk(mutex_);
    unplugOn_.insert(op);
}
void FakeMtpDevice::setUnplugged(bool v) {
    std::lock_guard lk(mutex_);
    unplugged_ = v;
}
void FakeMtpDevice::setStorageFree(StorageId s, uint64_t free) {
    std::lock_guard lk(mutex_);
    storages_.at(s).d.freeBytes = free;
}

size_t FakeMtpDevice::calls(Op op) const {
    std::lock_guard lk(mutex_);
    auto it = callCounts_.find(op);
    return it == callCounts_.end() ? 0 : it->second;
}
std::vector<ObjectId> FakeMtpDevice::listedParents() const {
    std::lock_guard lk(mutex_);
    return listed_;
}
bool FakeMtpDevice::exists(ObjectId id) const {
    std::lock_guard lk(mutex_);
    return objects_.count(id) != 0;
}
size_t FakeMtpDevice::objectCount() const {
    std::lock_guard lk(mutex_);
    return objects_.size();
}
std::vector<std::byte> FakeMtpDevice::bytesOf(ObjectId id) const {
    std::lock_guard lk(mutex_);
    const Obj& o = obj(id);
    if (!o.synthetic) return o.bytes;
    std::vector<std::byte> out(o.node.sizeBytes);
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = std::byte{patternByte(o.seed, i)};
    return out;
}

void FakeMtpDevice::enter(Op op) {
    ++callCounts_[op];
    if (unplugOn_.erase(op)) unplugged_ = true;
    if (unplugged_) throw MtpError(MtpErrc::DeviceDisconnected, "fake phone unplugged");
    auto it = failures_.find(op);
    if (it != failures_.end() && it->second.second != 0) {
        const MtpErrc c = it->second.first;
        if (it->second.second > 0) --it->second.second;
        throw MtpError(c, "injected fault");
    }
}

FakeMtpDevice::Obj& FakeMtpDevice::obj(ObjectId id) {
    auto it = objects_.find(id);
    if (it == objects_.end()) throw MtpError(MtpErrc::ObjectNotFound, "no such object");
    return it->second;
}
const FakeMtpDevice::Obj& FakeMtpDevice::obj(ObjectId id) const {
    auto it = objects_.find(id);
    if (it == objects_.end()) throw MtpError(MtpErrc::ObjectNotFound, "no such object");
    return it->second;
}
uint64_t FakeMtpDevice::contentSize(const Obj& o) const {
    return o.node.sizeBytes;
}

void FakeMtpDevice::checkParent(StorageId s, ObjectId parent) {
    if (!storages_.count(s)) throw MtpError(MtpErrc::ObjectNotFound, "no such storage");
    if (parent == kRootObject) return;
    const Obj& p = obj(parent);
    if (!p.node.isDirectory) throw MtpError(MtpErrc::NotADirectory, "parent is not a folder");
    if (p.node.storageId != s) throw MtpError(MtpErrc::InvalidArgument, "parent belongs to another storage");
}

DeviceDescriptor FakeMtpDevice::descriptor() const {
    DeviceDescriptor d;
    d.id = opt_.id;
    d.manufacturer = opt_.manufacturer;
    d.model = opt_.model;
    d.vendorId = 0xFFFE;
    d.productId = 0x0001;
    d.connected = true;
    d.mtpReady = true;
    return d;
}

DeviceCapabilities FakeMtpDevice::capabilities() const {
    DeviceCapabilities c;
    c.getPartialObject = opt_.partialRead;
    c.renameObject = opt_.renameSupported;
    c.deleteObject = true;
    return c;
}

std::vector<StorageDescriptor> FakeMtpDevice::storages() {
    std::lock_guard lk(mutex_);
    enter(Op::Storages);
    std::vector<StorageDescriptor> out;
    for (auto& [id, s] : storages_) out.push_back(s.d);
    return out;
}

std::vector<FileNode> FakeMtpDevice::list(StorageId s, ObjectId parent) {
    std::lock_guard lk(mutex_);
    enter(Op::List);
    listed_.push_back(parent);
    checkParent(s, parent);
    std::vector<FileNode> out;
    for (auto& [id, o] : objects_)
        if (o.node.storageId == s && o.node.parentObjectId == parent) out.push_back(o.node);
    return out;
}

FileNode FakeMtpDevice::stat(ObjectId id) {
    std::lock_guard lk(mutex_);
    enter(Op::Stat);
    return obj(id).node;
}

size_t FakeMtpDevice::read(ObjectId id, uint64_t offset, std::span<std::byte> dst) {
    std::lock_guard lk(mutex_);
    enter(Op::Read);
    if (!opt_.partialRead) throw MtpError(MtpErrc::OperationUnsupported, "fake device without GetPartialObject");
    const Obj& o = obj(id);
    if (o.node.isDirectory) throw MtpError(MtpErrc::IsADirectory, "folder");
    if (offset > UINT64_MAX - dst.size()) throw MtpError(MtpErrc::InvalidArgument, "range overflow");
    const uint64_t size = o.node.sizeBytes;
    if (offset >= size || dst.empty()) return 0;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(dst.size(), size - offset));
    if (o.synthetic) {
        for (size_t i = 0; i < n; ++i) dst[i] = std::byte{patternByte(o.seed, offset + i)};
    } else {
        std::memcpy(dst.data(), o.bytes.data() + offset, n);
    }
    return n;
}

void FakeMtpDevice::download(ObjectId id, const fs::path& target, ProgressCallback progress) {
    std::lock_guard lk(mutex_);
    enter(Op::Download);
    const Obj& o = obj(id);
    if (o.node.isDirectory) throw MtpError(MtpErrc::IsADirectory, "folder");
    struct stat st{};
    if (::lstat(target.c_str(), &st) == 0) throw MtpError(MtpErrc::FileAlreadyExists, "target exists");
    const fs::path tmp = target.parent_path() / ("." + target.filename().string() + ".afpart-" + tmpSuffix());
    UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot create temp file");
    const uint64_t size = o.node.sizeBytes;
    std::vector<std::byte> buf(kChunk);
    uint64_t done = 0;
    while (done < size) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - done));
        for (size_t i = 0; i < n; ++i)
            buf[i] = o.synthetic ? std::byte{patternByte(o.seed, done + i)} : o.bytes[done + i];
        if (::write(fd.get(), buf.data(), n) != static_cast<ssize_t>(n)) {
            fd.reset();
            ::unlink(tmp.c_str());
            throw MtpError(MtpErrc::IoError, "write failed");
        }
        done += n;
        if (progress && !progress({done, size})) {
            fd.reset();
            ::unlink(tmp.c_str());
            throw MtpError(MtpErrc::Cancelled, "download cancelled");
        }
    }
    fd.reset();
    if (::renamex_np(tmp.c_str(), target.c_str(), RENAME_EXCL) != 0) {
        ::unlink(tmp.c_str());
        throw MtpError(MtpErrc::FileAlreadyExists, "target appeared");
    }
}

ObjectId FakeMtpDevice::upload(StorageId s, ObjectId parent, const fs::path& src, std::string_view finalName,
                               ProgressCallback progress) {
    std::lock_guard lk(mutex_);
    enter(Op::Upload);
    if (!isValidObjectName(finalName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    checkParent(s, parent);
    std::ifstream in(src, std::ios::binary);
    if (!in) throw MtpError(MtpErrc::IoError, "cannot read source");
    std::error_code ec;
    const uint64_t size = fs::file_size(src, ec);
    if (ec) throw MtpError(MtpErrc::IoError, "cannot stat source");
    const Storage& st = storages_.at(s);
    if (st.d.readOnly) throw MtpError(MtpErrc::RemoteReadOnly, "read-only storage");
    if (size > st.d.freeBytes) throw MtpError(MtpErrc::DeviceStorageFull, "phone full");
    for (auto& [id, o] : objects_)
        if (o.node.storageId == s && o.node.parentObjectId == parent && o.node.name == finalName)
            throw MtpError(MtpErrc::FileAlreadyExists, "exists");
    std::vector<std::byte> bytes(size);
    uint64_t done = 0;
    while (done < size) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, size - done));
        in.read(reinterpret_cast<char*>(bytes.data() + done), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(in.gcount()) != n) throw MtpError(MtpErrc::IoError, "short read of source");
        done += n;
        if (progress && !progress({done, size})) throw MtpError(MtpErrc::Cancelled, "upload cancelled");
    }
    const ObjectId id = nextId_++;
    Obj o;
    o.node = FileNode{s, id, parent, std::string(finalName), false, size, std::chrono::system_clock::now()};
    o.bytes = std::move(bytes);
    objects_[id] = std::move(o);
    storages_.at(s).d.freeBytes -= size;
    return id;
}

ObjectId FakeMtpDevice::mkdir(StorageId s, ObjectId parent, std::string_view name) {
    std::lock_guard lk(mutex_);
    enter(Op::Mkdir);
    if (!isValidObjectName(name)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    checkParent(s, parent);
    if (storages_.at(s).d.readOnly) throw MtpError(MtpErrc::RemoteReadOnly, "read-only storage");
    for (auto& [id, o] : objects_)
        if (o.node.storageId == s && o.node.parentObjectId == parent && o.node.name == name)
            throw MtpError(MtpErrc::FileAlreadyExists, "exists");
    const ObjectId id = nextId_++;
    Obj o;
    o.node = FileNode{s, id, parent, std::string(name), true, 0, std::chrono::system_clock::now()};
    objects_[id] = std::move(o);
    return id;
}

void FakeMtpDevice::remove(ObjectId id, bool isDirectory) {
    std::lock_guard lk(mutex_);
    enter(Op::Remove);
    const Obj& o = obj(id);
    if (o.node.isDirectory != isDirectory)
        throw MtpError(isDirectory ? MtpErrc::NotADirectory : MtpErrc::IsADirectory, "type mismatch");
    if (o.node.isDirectory)
        for (auto& [oid, c] : objects_)
            if (c.node.parentObjectId == id && c.node.storageId == o.node.storageId)
                throw MtpError(MtpErrc::DirectoryNotEmpty, "not empty");
    if (storages_.at(o.node.storageId).d.readOnly) throw MtpError(MtpErrc::RemoteReadOnly, "read-only storage");
    if (!o.node.isDirectory) storages_.at(o.node.storageId).d.freeBytes += o.node.sizeBytes;
    objects_.erase(id);
}

void FakeMtpDevice::rename(ObjectId id, std::string_view newName) {
    std::lock_guard lk(mutex_);
    enter(Op::Rename);
    if (!isValidObjectName(newName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    if (!opt_.renameSupported) throw MtpError(MtpErrc::OperationUnsupported, "rename unsupported");
    Obj& o = obj(id);
    for (auto& [oid, c] : objects_)
        if (oid != id && c.node.storageId == o.node.storageId && c.node.parentObjectId == o.node.parentObjectId &&
            c.node.name == newName)
            throw MtpError(MtpErrc::FileAlreadyExists, "exists");
    o.node.name = std::string(newName);
}

// ---- factory + single-owner session -----------------------------------------------------------

namespace {
// Forwards to the shared phone and holds the cross-process ownership lock.
class FakeSession final : public IMtpDevice {
public:
    FakeSession(std::shared_ptr<IMtpDevice> phone, DeviceLock lock)
        : phone_(std::move(phone)), lock_(std::move(lock)) {}
    DeviceDescriptor descriptor() const override { return phone_->descriptor(); }
    DeviceCapabilities capabilities() const override { return phone_->capabilities(); }
    std::vector<StorageDescriptor> storages() override { return phone_->storages(); }
    std::vector<FileNode> list(StorageId s, ObjectId p) override { return phone_->list(s, p); }
    FileNode stat(ObjectId id) override { return phone_->stat(id); }
    size_t read(ObjectId id, uint64_t off, std::span<std::byte> dst) override { return phone_->read(id, off, dst); }
    void download(ObjectId id, const fs::path& t, ProgressCallback p) override {
        phone_->download(id, t, std::move(p));
    }
    ObjectId upload(StorageId s, ObjectId p, const fs::path& src, std::string_view n, ProgressCallback pr) override {
        return phone_->upload(s, p, src, n, std::move(pr));
    }
    ObjectId mkdir(StorageId s, ObjectId p, std::string_view n) override { return phone_->mkdir(s, p, n); }
    void remove(ObjectId id, bool d) override { phone_->remove(id, d); }
    void rename(ObjectId id, std::string_view n) override { phone_->rename(id, n); }

private:
    std::shared_ptr<IMtpDevice> phone_;
    DeviceLock lock_;
};
}  // namespace

FakeMtpDeviceFactory::FakeMtpDeviceFactory(std::shared_ptr<IMtpDevice> phone) : phone_(std::move(phone)) {}

std::vector<DeviceDescriptor> FakeMtpDeviceFactory::enumerate() {
    if (!present_) return {};
    DeviceDescriptor d = phone_->descriptor();
    d.mtpReady = false;  // like the real factory: unknown until a session is opened
    return {d};
}

std::unique_ptr<IMtpDevice> FakeMtpDeviceFactory::open(const DeviceId& id) {
    if (!present_ || phone_->descriptor().id != id) throw MtpError(MtpErrc::DeviceDisconnected, "no such fake device");
    return std::make_unique<FakeSession>(phone_, DeviceLock::acquire(id));
}

}  // namespace mtport::fake
