#include "HostDirMtpDevice.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

#include "mtport/NameRules.hpp"
#include "mtport/UniqueFd.hpp"

namespace fs = std::filesystem;

namespace mtport::fake {

namespace {
std::string suffix() {
    static std::mt19937_64 rng{std::random_device{}()};
    return std::to_string(rng());
}

// Copies src to dst (dst must not exist) via a temp file + exclusive rename.
void copyExclusive(const fs::path& src, const fs::path& dst, const ProgressCallback& progress) {
    UniqueFd in(::open(src.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in.valid()) throw MtpError(MtpErrc::IoError, "cannot open source");
    struct stat st{};
    ::fstat(in.get(), &st);
    const fs::path tmp = dst.parent_path() / ("." + dst.filename().string() + ".afpart-" + suffix());
    UniqueFd out(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!out.valid()) throw MtpError(MtpErrc::IoError, "cannot create temp file");
    std::vector<char> buf(256 * 1024);
    uint64_t done = 0;
    for (;;) {
        const ssize_t n = ::read(in.get(), buf.data(), buf.size());
        if (n < 0) {
            ::unlink(tmp.c_str());
            throw MtpError(MtpErrc::IoError, "read failed");
        }
        if (n == 0) break;
        if (::write(out.get(), buf.data(), static_cast<size_t>(n)) != n) {
            ::unlink(tmp.c_str());
            throw MtpError(errno == ENOSPC ? MtpErrc::DeviceStorageFull : MtpErrc::IoError, "write failed");
        }
        done += static_cast<uint64_t>(n);
        if (progress && !progress({done, static_cast<uint64_t>(st.st_size)})) {
            ::unlink(tmp.c_str());
            throw MtpError(MtpErrc::Cancelled, "cancelled");
        }
    }
    out.reset();
    if (::renamex_np(tmp.c_str(), dst.c_str(), RENAME_EXCL) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        throw MtpError(e == EEXIST ? MtpErrc::FileAlreadyExists : MtpErrc::IoError, "cannot place file");
    }
}
}  // namespace

HostDirMtpDevice::HostDirMtpDevice(fs::path root, Options opt) : root_(std::move(root)), opt_(std::move(opt)) {
    std::error_code ec;
    root_ = fs::canonical(root_, ec);
    if (ec || !fs::is_directory(root_)) throw MtpError(MtpErrc::InvalidArgument, "fake root is not a directory");
}

DeviceDescriptor HostDirMtpDevice::descriptor() const {
    DeviceDescriptor d;
    d.id = opt_.id;
    d.manufacturer = "MTPort test";
    d.model = opt_.model;
    d.vendorId = 0xFFFE;
    d.productId = 0x0002;
    d.connected = d.mtpReady = true;
    return d;
}

DeviceCapabilities HostDirMtpDevice::capabilities() const {
    DeviceCapabilities c;
    c.getPartialObject = opt_.partialRead;
    c.renameObject = c.deleteObject = true;
    return c;
}

void HostDirMtpDevice::checkStorage(StorageId s) const {
    if (s != kStorageId) throw MtpError(MtpErrc::ObjectNotFound, "no such storage");
}

std::vector<StorageDescriptor> HostDirMtpDevice::storages() {
    struct statvfs sv{};
    ::statvfs(root_.c_str(), &sv);
    StorageDescriptor d;
    d.id = kStorageId;
    d.name = opt_.storageName;
    d.capacityBytes = static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize;
    d.freeBytes = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    return {d};
}

fs::path HostDirMtpDevice::pathOf(ObjectId id) const {
    if (id == kRootObject) return root_;
    auto it = idToRel_.find(id);
    if (it == idToRel_.end()) throw MtpError(MtpErrc::ObjectNotFound, "no such object");
    return root_ / it->second;
}

ObjectId HostDirMtpDevice::idFor(const fs::path& rel) {
    auto [it, inserted] = relToId_.try_emplace(rel, next_);
    if (inserted) {
        idToRel_[next_] = rel;
        ++next_;
    }
    return it->second;
}

FileNode HostDirMtpDevice::nodeFor(ObjectId id, const fs::path& rel, bool isDir, uint64_t size,
                                   std::chrono::system_clock::time_point mtime, ObjectId parent) const {
    return FileNode{kStorageId, id, parent, rel.filename().string(), isDir, isDir ? 0 : size, mtime};
}

std::vector<FileNode> HostDirMtpDevice::list(StorageId s, ObjectId parent) {
    std::lock_guard lk(mutex_);
    checkStorage(s);
    const fs::path dir = pathOf(parent);
    const fs::path relDir = parent == kRootObject ? fs::path{} : idToRel_.at(parent);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw MtpError(MtpErrc::NotADirectory, "not a folder");
    std::vector<FileNode> out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.find(".afpart-") != std::string::npos) continue;
        struct stat st{};
        if (::lstat(e.path().c_str(), &st) != 0) continue;
        const bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && !S_ISREG(st.st_mode)) continue;  // symlinks, devices: not MTP objects
        const fs::path rel = relDir / name;
        out.push_back(nodeFor(idFor(rel), rel, isDir, static_cast<uint64_t>(st.st_size),
                              std::chrono::system_clock::from_time_t(st.st_mtime), parent));
    }
    return out;
}

FileNode HostDirMtpDevice::stat(ObjectId id) {
    std::lock_guard lk(mutex_);
    const fs::path p = pathOf(id);
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) throw MtpError(MtpErrc::ObjectNotFound, "gone");
    const fs::path rel = idToRel_.at(id);
    const fs::path prel = rel.parent_path();
    const ObjectId parent = prel.empty() ? kRootObject : relToId_.at(prel);
    return nodeFor(id, rel, S_ISDIR(st.st_mode), static_cast<uint64_t>(st.st_size),
                   std::chrono::system_clock::from_time_t(st.st_mtime), parent);
}

size_t HostDirMtpDevice::read(ObjectId id, uint64_t offset, std::span<std::byte> dst) {
    std::lock_guard lk(mutex_);
    if (!opt_.partialRead) throw MtpError(MtpErrc::OperationUnsupported, "partial reads disabled");
    UniqueFd fd(::open(pathOf(id).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid()) throw MtpError(MtpErrc::ObjectNotFound, "cannot open");
    size_t total = 0;
    while (total < dst.size()) {
        const ssize_t n = ::pread(fd.get(), dst.data() + total, dst.size() - total, static_cast<off_t>(offset + total));
        if (n < 0) throw MtpError(MtpErrc::IoError, "pread failed");
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return total;
}

void HostDirMtpDevice::download(ObjectId id, const fs::path& target, ProgressCallback progress) {
    fs::path src;
    {
        std::lock_guard lk(mutex_);
        src = pathOf(id);
    }
    copyExclusive(src, target, progress);
}

ObjectId HostDirMtpDevice::upload(StorageId s, ObjectId parent, const fs::path& src, std::string_view finalName,
                                  ProgressCallback progress) {
    std::lock_guard lk(mutex_);
    checkStorage(s);
    if (!isValidObjectName(finalName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    const fs::path dir = pathOf(parent);
    const fs::path dst = dir / std::string(finalName);
    struct stat st{};
    if (::lstat(dst.c_str(), &st) == 0) throw MtpError(MtpErrc::FileAlreadyExists, "exists");
    copyExclusive(src, dst, progress);
    const fs::path rel = (parent == kRootObject ? fs::path{} : idToRel_.at(parent)) / std::string(finalName);
    return idFor(rel);
}

ObjectId HostDirMtpDevice::mkdir(StorageId s, ObjectId parent, std::string_view name) {
    std::lock_guard lk(mutex_);
    checkStorage(s);
    if (!isValidObjectName(name)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    const fs::path dst = pathOf(parent) / std::string(name);
    if (::mkdir(dst.c_str(), 0755) != 0)
        throw MtpError(errno == EEXIST ? MtpErrc::FileAlreadyExists : MtpErrc::IoError, "mkdir failed");
    return idFor((parent == kRootObject ? fs::path{} : idToRel_.at(parent)) / std::string(name));
}

void HostDirMtpDevice::remove(ObjectId id, bool isDirectory) {
    std::lock_guard lk(mutex_);
    const fs::path p = pathOf(id);
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) throw MtpError(MtpErrc::ObjectNotFound, "gone");
    if (S_ISDIR(st.st_mode) != isDirectory)
        throw MtpError(isDirectory ? MtpErrc::NotADirectory : MtpErrc::IsADirectory, "type mismatch");
    const int rc = isDirectory ? ::rmdir(p.c_str()) : ::unlink(p.c_str());
    if (rc != 0) throw MtpError(errno == ENOTEMPTY ? MtpErrc::DirectoryNotEmpty : MtpErrc::IoError, "delete failed");
    relToId_.erase(idToRel_.at(id));
    idToRel_.erase(id);
}

void HostDirMtpDevice::rename(ObjectId id, std::string_view newName) {
    std::lock_guard lk(mutex_);
    if (!isValidObjectName(newName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    const fs::path src = pathOf(id);
    const fs::path dst = src.parent_path() / std::string(newName);
    if (::renamex_np(src.c_str(), dst.c_str(), RENAME_EXCL) != 0)
        throw MtpError(errno == EEXIST ? MtpErrc::FileAlreadyExists : MtpErrc::IoError, "rename failed");
    const fs::path oldRel = idToRel_.at(id);
    const fs::path newRel = oldRel.parent_path() / std::string(newName);
    relToId_.erase(oldRel);
    relToId_[newRel] = id;
    idToRel_[id] = newRel;
}

}  // namespace mtport::fake
