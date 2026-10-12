#include "mtport/MtpFilesystem.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "mtport/NameRules.hpp"
#include "mtport/RuntimePaths.hpp"

namespace mtport {

namespace {
constexpr size_t kOverlayMaxFile = 16u << 20;  // Finder metadata files are tiny; bound them anyway
constexpr ObjectId kPendingInoBase = 0xF0000000u;
}  // namespace

// ---- private helper types ------------------------------------------------------------------

struct OverlayFile {
    std::mutex mutex;
    std::vector<std::byte> data;
    std::chrono::system_clock::time_point mtime{std::chrono::system_clock::now()};
    uint64_t ino{};
};

struct MtpFilesystem::Overlay {
    struct Item {
        StorageId storage{};
        ObjectId parent{};
        std::string name;
        std::shared_ptr<OverlayFile> file;
    };
    std::unordered_map<std::string, Item> items;
    uint64_t nextIno{kPendingInoBase + 0x08000000u};
};

struct MtpFilesystem::Pending {
    std::shared_ptr<StagedFile> staged;
    uint64_t ino{};
    StorageId storage{};
    ObjectId parent{};
    std::string name;
    std::string key;
    std::chrono::system_clock::time_point created{std::chrono::system_clock::now()};
    // An empty file whose last handle was released. FSKit creates a file, releases that handle at once and
    // then opens it again to write, so empty files wait here (see housekeeping()) instead of being uploaded.
    bool lingering{false};
    std::chrono::steady_clock::time_point lingerSince{};
};

struct MtpFilesystem::Resolved {
    enum class Kind { Root, Storage, Object, Pending, Overlay } kind{Kind::Root};
    StorageEntry storage;
    FileNode node;
    ObjectId parent{kRootObject};
    std::string displayName;
    std::shared_ptr<Pending> pending;
    std::shared_ptr<OverlayFile> overlay;

    bool isDir() const {
        return kind == Kind::Root || kind == Kind::Storage || (kind == Kind::Object && node.isDirectory);
    }
    // Object id to list when this node is a directory.
    ObjectId dirId() const { return kind == Kind::Object ? node.objectId : kRootObject; }
};

// ---- construction ----------------------------------------------------------------------------

MtpFilesystem::MtpFilesystem(std::shared_ptr<IMtpDevice> device, FsConfig cfg)
    : device_(std::move(device)),
      cfg_(std::move(cfg)),
      mountTime_(std::chrono::system_clock::now()),
      cache_(cfg_.cache),
      staging_(WriteStaging::Config{cfg_.stagingDir.empty() ? stagingDir() : cfg_.stagingDir, cfg_.stagingQuotaBytes}),
      readStage_(ReadStageCache::Config{cfg_.readStageDir.empty() ? cacheDir() / "ReadCache" : cfg_.readStageDir,
                                        cfg_.readStageQuotaBytes}),
      overlay_(std::make_unique<Overlay>()) {
    if (!device_) throw MtpError(MtpErrc::InvalidArgument, "no device");
}

MtpFilesystem::~MtpFilesystem() = default;

std::shared_ptr<IMtpDevice> MtpFilesystem::dev() const {
    std::lock_guard lk(deviceMutex_);
    return device_;
}

DeviceCapabilities MtpFilesystem::deviceCapabilities() const {
    return dev()->capabilities();
}

void MtpFilesystem::checkAlive() const {
    if (disconnected_.load()) throw FsError(ENODEV, "the phone is no longer connected");
}

void MtpFilesystem::markDisconnected() {
    disconnected_ = true;
    if (!disconnectNotified_.exchange(true) && onDisconnect_) onDisconnect_();
}

template <class F>
auto MtpFilesystem::guarded(F&& f) {
    try {
        checkAlive();
        return f();
    } catch (const FsError&) {
        throw;
    } catch (const MtpError& e) {
        if (e.code() == MtpErrc::DeviceDisconnected) markDisconnected();
        throw FsError(toErrno(e.code()), e.what());
    } catch (const std::exception& e) {
        throw FsError(errnoFromException(e), e.what());
    }
}

namespace {
// Stands in for a session that was handed back; every operation reports a disconnected phone.
class DetachedDevice final : public IMtpDevice {
public:
    explicit DetachedDevice(DeviceDescriptor d) : d_(std::move(d)) {}
    DeviceDescriptor descriptor() const override { return d_; }
    DeviceCapabilities capabilities() const override { return {}; }
    std::vector<StorageDescriptor> storages() override { gone(); }
    std::vector<FileNode> list(StorageId, ObjectId) override { gone(); }
    FileNode stat(ObjectId) override { gone(); }
    size_t read(ObjectId, uint64_t, std::span<std::byte>) override { gone(); }
    void download(ObjectId, const std::filesystem::path&, ProgressCallback) override { gone(); }
    ObjectId upload(StorageId, ObjectId, const std::filesystem::path&, std::string_view, ProgressCallback) override {
        gone();
    }
    ObjectId mkdir(StorageId, ObjectId, std::string_view) override { gone(); }
    void remove(ObjectId, bool) override { gone(); }
    void rename(ObjectId, std::string_view) override { gone(); }

private:
    [[noreturn]] static void gone() { throw MtpError(MtpErrc::DeviceDisconnected, "the MTP session was released"); }
    DeviceDescriptor d_;
};
}  // namespace

void MtpFilesystem::detachDevice() {
    disconnectNotified_ = true;  // an intentional hand-back is not an unplug
    disconnected_ = true;
    std::lock_guard lk(deviceMutex_);
    device_ = std::make_shared<DetachedDevice>(device_->descriptor());
}

std::string MtpFilesystem::pendingKeyFor(StorageId s, ObjectId p, const std::string& nameKey) {
    return std::to_string(s) + ':' + std::to_string(p) + ':' + nameKey;
}

// ---- resolution ------------------------------------------------------------------------------

std::vector<StorageEntry> MtpFilesystem::storageView() {
    {
        std::lock_guard lk(stateMutex_);
        if (!storageCache_.empty() && std::chrono::steady_clock::now() - storageCacheTime_ < cfg_.cache.dirTtl)
            return storageCache_;
    }
    auto view = buildStorageView(dev()->storages());
    std::lock_guard lk(stateMutex_);
    storageCache_ = view;
    storageCacheTime_ = std::chrono::steady_clock::now();
    return view;
}

std::shared_ptr<const DirListing> MtpFilesystem::listing(StorageId storage, ObjectId parent) {
    const uint64_t gen = generation_.load();
    if (auto cached = cache_.getDir(gen, storage, parent)) return cached;
    auto fresh = std::make_shared<const DirListing>(buildDirListing(dev()->list(storage, parent)));
    cache_.putDir(gen, storage, parent, fresh);
    return fresh;
}

void MtpFilesystem::invalidateParent(StorageId storage, ObjectId parent) {
    cache_.invalidateDir(generation_.load(), storage, parent);
}

MtpFilesystem::Resolved MtpFilesystem::resolve(const std::string& path) {
    const auto comps = splitVirtualPath(path);
    Resolved r;
    if (comps.empty()) return r;

    const std::string storageKey = normalizeNfc(comps[0]);
    bool found = false;
    for (auto& s : storageView()) {
        if (s.key == storageKey) {
            r.storage = s;
            found = true;
            break;
        }
    }
    if (!found) throw FsError(ENOENT, "no such storage");
    r.kind = Resolved::Kind::Storage;
    r.displayName = r.storage.displayName;
    r.node = FileNode{};
    r.node.storageId = r.storage.storage.id;
    r.node.isDirectory = true;
    ObjectId parent = kRootObject;

    for (size_t i = 1; i < comps.size(); ++i) {
        const bool last = i + 1 == comps.size();
        const std::string key = normalizeNfc(comps[i]);
        const StorageId sid = r.storage.storage.id;
        auto lst = listing(sid, parent);
        if (const DirEntryView* e = lst->find(comps[i])) {
            if (!last && !e->node.isDirectory) throw FsError(ENOTDIR, "not a directory");
            r.kind = Resolved::Kind::Object;
            r.node = e->node;
            r.node.storageId = sid;
            r.parent = parent;
            r.displayName = e->displayName;
            parent = e->node.objectId;
            continue;
        }
        if (last) {
            std::lock_guard lk(stateMutex_);
            const std::string pk = pendingKeyFor(sid, parent, key);
            if (auto it = pending_.find(pk); it != pending_.end()) {
                r.kind = Resolved::Kind::Pending;
                r.pending = it->second;
                r.parent = parent;
                r.displayName = comps[i];
                r.node = FileNode{sid, 0, parent, it->second->name, false, 0, std::nullopt};
                return r;
            }
            if (auto it = overlay_->items.find(pk); it != overlay_->items.end()) {
                r.kind = Resolved::Kind::Overlay;
                r.overlay = it->second.file;
                r.parent = parent;
                r.displayName = comps[i];
                r.node = FileNode{sid, 0, parent, it->second.name, false, 0, std::nullopt};
                return r;
            }
        }
        throw FsError(ENOENT, "no such file or directory");
    }
    return r;
}

MtpFilesystem::Resolved MtpFilesystem::resolveParent(const std::string& path, std::string& leaf) {
    auto comps = splitVirtualPath(path);
    if (comps.empty()) throw FsError(EINVAL, "the volume root has no parent");
    leaf = comps.back();
    comps.pop_back();
    std::string parentPath = "/";
    for (size_t i = 0; i < comps.size(); ++i) parentPath += (i ? "/" : "") + comps[i];
    Resolved p = resolve(parentPath);
    if (!p.isDir()) throw FsError(ENOTDIR, "parent is not a directory");
    return p;
}

uint64_t MtpFilesystem::ino(const Resolved& r) {
    switch (r.kind) {
        case Resolved::Kind::Root:
            return InodeTable::kRoot;
        case Resolved::Kind::Storage:
            return inodes_.forStorage(r.storage.storage.id);
        case Resolved::Kind::Object:
            return inodes_.forObject(r.node.storageId, r.node.objectId);
        case Resolved::Kind::Pending:
            return r.pending->ino;
        case Resolved::Kind::Overlay:
            return r.overlay->ino;
    }
    return 0;
}

FsStat MtpFilesystem::statOf(const Resolved& r) {
    FsStat st;
    st.ino = ino(r);
    st.uid = cfg_.uid;
    st.gid = cfg_.gid;
    st.isDir = r.isDir();
    const bool ro = cfg_.readOnly || (r.kind != Resolved::Kind::Root && r.storage.storage.readOnly);
    if (st.isDir) {
        st.mode = ro ? 0555 : 0755;
        st.nlink = 2;
        st.mtime = r.kind == Resolved::Kind::Object && r.node.modifiedAt ? *r.node.modifiedAt : mountTime_;
        return st;
    }
    st.mode = ro ? 0444 : 0644;
    switch (r.kind) {
        case Resolved::Kind::Object:
            st.size = r.node.sizeBytes;
            st.mtime = r.node.modifiedAt ? *r.node.modifiedAt : mountTime_;
            break;
        case Resolved::Kind::Pending:
            st.pending = true;
            st.size = r.pending->staged->size();
            st.mtime = r.pending->created;
            break;
        case Resolved::Kind::Overlay: {
            std::lock_guard lk(r.overlay->mutex);
            st.size = r.overlay->data.size();
            st.mtime = r.overlay->mtime;
            break;
        }
        default:
            break;
    }
    return st;
}

// ---- read-side operations ----------------------------------------------------------------------

void MtpFilesystem::refreshNode(Resolved& r) {
    if (r.kind != Resolved::Kind::Object || r.node.isDirectory) return;
    FileNode fresh = dev()->stat(r.node.objectId);
    if (fresh.isDirectory) throw FsError(ESTALE, "the object changed type on the phone");
    fresh.storageId = r.node.storageId;
    if (fresh.parentObjectId == 0) fresh.parentObjectId = r.node.parentObjectId;
    r.node = std::move(fresh);
}

FsStat MtpFilesystem::getattr(const std::string& path, bool fresh) {
    return guarded([&] {
        Resolved r = resolve(path);
        if (fresh) refreshNode(r);
        return statOf(r);
    });
}

void MtpFilesystem::cancelBackgroundWork() {
    try {
        dev()->cancelListings();
    } catch (...) {
    }
}

void MtpFilesystem::prefetch(const std::vector<std::string>& storageRelativePaths, const std::function<bool()>& stop) {
    std::vector<std::string> storages;
    try {
        for (const auto& s : storageView()) storages.push_back(s.displayName);
    } catch (...) {
        return;
    }
    for (const auto& rel : storageRelativePaths) {
        for (const auto& st : storages) {
            if (stop && stop()) return;
            try {
                // resolve() lists every ancestor, so reading "<storage>/DCIM/Camera" warms DCIM as well
                Resolved dir = resolve("/" + st + "/" + rel);
                if (dir.isDir() && dir.kind == Resolved::Kind::Object) (void)listing(dir.storage.storage.id, dir.dirId());
            } catch (...) {
                // missing folder, unplugged phone, ...: prefetching is best effort
            }
        }
    }
}

std::vector<FsDirEntry> MtpFilesystem::readdir(const std::string& path) {
    return guarded([&] {
        Resolved dir = resolve(path);
        if (!dir.isDir()) throw FsError(ENOTDIR, "not a directory");
        std::vector<FsDirEntry> out;
        out.push_back({".", ino(dir), true});
        if (dir.kind == Resolved::Kind::Root) {
            out.push_back({"..", InodeTable::kRoot, true});
            for (const auto& s : storageView()) out.push_back({s.displayName, inodes_.forStorage(s.storage.id), true});
            return out;
        }
        const StorageId sid = dir.storage.storage.id;
        const ObjectId did = dir.dirId();
        uint64_t parentIno = InodeTable::kRoot;
        if (dir.kind == Resolved::Kind::Object)
            parentIno = dir.parent == kRootObject ? inodes_.forStorage(sid) : inodes_.forObject(sid, dir.parent);
        out.push_back({"..", parentIno, true});

        auto lst = listing(sid, did);
        out.reserve(out.size() + lst->entries.size());
        for (const auto& e : lst->entries)
            out.push_back({e.displayName, inodes_.forObject(sid, e.node.objectId), e.node.isDirectory});

        std::lock_guard lk(stateMutex_);
        for (const auto& [k, p] : pending_)
            if (p->storage == sid && p->parent == did && !lst->find(p->name)) out.push_back({p->name, p->ino, false});
        for (const auto& [k, o] : overlay_->items)
            if (o.storage == sid && o.parent == did && !lst->find(o.name)) out.push_back({o.name, o.file->ino, false});
        return out;
    });
}

uint64_t MtpFilesystem::open(const std::string& path, int flags) {
    return guarded([&]() -> uint64_t {
        Resolved r = resolve(path);
        if (r.isDir()) throw FsError(EISDIR, "is a directory");
        const int acc = flags & O_ACCMODE;
        auto f = std::make_shared<OpenFile>();
        f->generation = generation_.load();
        f->flags = flags;

        switch (r.kind) {
            case Resolved::Kind::Overlay:
                if (cfg_.readOnly && acc != O_RDONLY) throw FsError(EROFS, "read-only volume");
                f->overlay = r.overlay;
                if ((flags & O_TRUNC) && acc != O_RDONLY) {
                    std::lock_guard lk(r.overlay->mutex);
                    r.overlay->data.clear();
                }
                break;
            case Resolved::Kind::Pending: {
                f->staged = r.pending->staged;
                f->pendingKey = r.pending->key;
                f->node = r.node;
                // O_TRUNC on a file that has not been uploaded yet simply empties the local staging copy.
                if ((flags & O_TRUNC) && acc != O_RDONLY && !cfg_.readOnly) f->staged->truncate(0);
                std::lock_guard lk(stateMutex_);
                r.pending->lingering = false;  // being written again: not a lingering empty file any more
                break;
            }
            case Resolved::Kind::Object:
                if (acc == O_WRONLY)
                    throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP, "files on the phone cannot be modified in place");
                if ((flags & O_TRUNC) && acc != O_RDONLY)
                    throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP,
                                  "replacing an existing phone file is not supported");
                refreshNode(r);  // the listing may be minutes old: reads are bounded by the phone's real size
                f->node = r.node;
                f->writeRequested = acc == O_RDWR;  // FSKit may open read/write for read-only use: reads still work
                break;
            default:
                throw FsError(EISDIR, "is a directory");
        }
        return handles_.insert(std::move(f));
    });
}

size_t MtpFilesystem::read(uint64_t fh, uint64_t offset, std::span<std::byte> dst) {
    return guarded([&]() -> size_t {
        auto f = handles_.get(fh, generation_.load());
        if (f->overlay) {
            std::lock_guard lk(f->overlay->mutex);
            const auto& d = f->overlay->data;
            if (offset >= d.size() || dst.empty()) return 0;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(dst.size(), d.size() - offset));
            std::memcpy(dst.data(), d.data() + offset, n);
            return n;
        }
        if (f->staged) {
            const size_t n = f->staged->read(offset, dst);
            counters_.bytesRead += n;
            return n;
        }
        if (offset >= f->node.sizeBytes || dst.empty()) return 0;  // POSIX EOF: exactly 0 bytes, never padding
        const size_t len = static_cast<size_t>(std::min<uint64_t>(dst.size(), f->node.sizeBytes - offset));
        std::span<std::byte> out = dst.first(len);
        auto d = dev();

        if (!f->stageMode.load() && d->capabilities().getPartialObject) {
            try {
                // The kernel treats a short read as EOF, so loop until the request is fully satisfied.
                size_t total = 0;
                while (total < len) {
                    const size_t chunk = std::min(len - total, std::max<size_t>(cfg_.maxReadBytes, 1));
                    const size_t n = d->read(f->node.objectId, offset + total, out.subspan(total, chunk));
                    ++counters_.deviceReads;
                    if (n == 0)
                        throw MtpError(MtpErrc::IoError, "the phone returned no data before the end of the file");
                    total += n;
                }
                counters_.bytesRead += total;
                return total;
            } catch (const MtpError& e) {
                if (e.code() != MtpErrc::OperationUnsupported) throw;
                f->stageMode = true;  // device refused partial reads: fall back to a complete local copy
            }
        }
        auto lease = readStage_.acquire(f->generation, f->node, *d);
        const size_t n = lease.read(offset, out);
        if (n != len) throw MtpError(MtpErrc::IoError, "short read from the staged copy");
        counters_.bytesRead += n;
        return n;
    });
}

// ---- write-side operations -----------------------------------------------------------------------

uint64_t MtpFilesystem::create(const std::string& path, uint32_t /*mode*/) {
    return guarded([&]() -> uint64_t {
        if (cfg_.readOnly) throw FsError(EROFS, "read-only volume");
        std::string leaf;
        Resolved parent = resolveParent(path, leaf);
        if (parent.kind == Resolved::Kind::Root) throw FsError(EPERM, "files cannot be created next to storages");
        if (parent.storage.storage.readOnly) throw FsError(EROFS, "storage is read-only");
        if (!isValidObjectName(leaf)) throw FsError(EINVAL, "invalid file name");
        const StorageId sid = parent.storage.storage.id;
        const ObjectId pid = parent.dirId();
        const std::string key = normalizeNfc(leaf);

        if (listing(sid, pid)->find(leaf)) throw FsError(EEXIST, "file exists");

        auto f = std::make_shared<OpenFile>();
        f->generation = generation_.load();
        f->flags = O_RDWR | O_CREAT;
        f->node = FileNode{sid, 0, pid, leaf, false, 0, std::nullopt};
        const std::string pk = pendingKeyFor(sid, pid, key);

        std::lock_guard lk(stateMutex_);
        if (pending_.count(pk) || overlay_->items.count(pk)) throw FsError(EEXIST, "file exists");
        if (cfg_.metadataOverlay && isMacMetadataName(leaf)) {
            auto of = std::make_shared<OverlayFile>();
            of->ino = overlay_->nextIno++;
            overlay_->items.emplace(pk, Overlay::Item{sid, pid, leaf, of});
            f->overlay = of;
            f->pendingKey = pk;
        } else {
            auto p = std::make_shared<Pending>();
            p->staged = staging_.create(leaf, sid, pid);
            p->ino = inodes_.forObject(sid, kPendingInoBase + pendingSeq_++);
            p->storage = sid;
            p->parent = pid;
            p->name = leaf;
            p->key = pk;
            pending_.emplace(pk, p);
            f->staged = p->staged;
            f->pendingKey = pk;
        }
        return handles_.insert(std::move(f));
    });
}

size_t MtpFilesystem::write(uint64_t fh, uint64_t offset, std::span<const std::byte> data) {
    return guarded([&]() -> size_t {
        auto f = handles_.get(fh, generation_.load());
        std::lock_guard lk(f->mutex);
        if (f->overlay) {
            std::lock_guard ol(f->overlay->mutex);
            if (data.empty()) return 0;
            if (offset > kOverlayMaxFile || data.size() > kOverlayMaxFile - offset)
                throw FsError(ENOSPC, "Finder metadata file too large");
            auto& d = f->overlay->data;
            if (d.size() < offset + data.size()) d.resize(offset + data.size());
            std::memcpy(d.data() + offset, data.data(), data.size());
            f->overlay->mtime = std::chrono::system_clock::now();
            return data.size();
        }
        if (!f->staged)
            throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP,
                          "existing files on the phone cannot be modified in place; copy a new file instead");
        const size_t n = f->staged->write(offset, data);
        counters_.bytesStaged += n;
        return n;
    });
}

void MtpFilesystem::commit(OpenFile& f) {
    if (!f.staged) return;
    if (f.generation != generation_.load())
        throw FsError(ESTALE, "device session changed; staged bytes were preserved");
    auto staged = f.staged;
    ++counters_.uploadsActive;
    counters_.uploadBytesDone = 0;
    counters_.uploadBytesTotal = staged->size();
    bool attempted = false;
    ObjectId newId = 0;
    try {
        newId = staged->commit([&](const std::filesystem::path& p) {
            attempted = true;
            return dev()->upload(staged->storageId(), staged->parentId(), p, staged->finalName(),
                                 [&](const TransferProgress& pr) {
                                     counters_.uploadBytesDone = pr.bytesDone;
                                     return !disconnected_.load();
                                 });
        });
    } catch (...) {
        --counters_.uploadsActive;
        if (attempted) ++counters_.uploadsFailed;  // a repeated flush of an already-failed file is not a new attempt
        throw;
    }
    --counters_.uploadsActive;
    ++counters_.uploadsOk;
    counters_.bytesUploaded += staged->size();
    // Remote success confirmed: only now does the view change.
    markFresh(staged->storageId(), newId);
    invalidateParent(staged->storageId(), staged->parentId());
    std::lock_guard lk(stateMutex_);
    pending_.erase(f.pendingKey);
}

void MtpFilesystem::flush(uint64_t fh) {
    guarded([&] {
        auto f = handles_.get(fh, generation_.load());
        // An empty file is not uploaded on close: FSKit is about to re-open it for writing (see Pending::lingering).
        if (f->staged && f->staged->size() != 0) commit(*f);
    });
}

void MtpFilesystem::fsync(uint64_t fh) {
    guarded([&] {
        auto f = handles_.get(fh, generation_.load());
        if (f->staged) commit(*f);  // an explicit fsync is a request to make the file durable now
    });
}

void MtpFilesystem::release(uint64_t fh) noexcept {
    auto f = handles_.remove(fh);
    if (!f) return;
    try {
        if (f->staged && !disconnected_.load() && f->staged->state() == StageState::Dirty) {
            if (f->staged->size() == 0) {
                std::lock_guard lk(stateMutex_);
                if (auto it = pending_.find(f->pendingKey); it != pending_.end() && it->second->staged == f->staged) {
                    it->second->lingering = true;
                    it->second->lingerSince = std::chrono::steady_clock::now();
                }
            } else {
                commit(*f);
            }
        }
    } catch (...) {
        // Errors were recorded by the staged file (state FAILED + manifest) and counted; release
        // cannot report them to the application, so flush/fsync are where callers see them.
    }
    if (f->staged || f->overlay) {
        std::lock_guard lk(stateMutex_);
        if (f->staged) {
            auto it = pending_.find(f->pendingKey);
            // A file that is still not on the phone must not stay visible as if it were.
            if (it != pending_.end() && it->second->staged == f->staged && f->staged->state() != StageState::Dirty)
                pending_.erase(it);
        }
    }
}

bool MtpFilesystem::hasOpenHandle(const std::shared_ptr<StagedFile>& staged) const {
    for (const auto& h : handles_.snapshot())
        if (h->staged == staged) return true;
    return false;
}

// Uploads one lingering empty file. Caller must not hold stateMutex_.
void MtpFilesystem::commitLingering(const std::shared_ptr<Pending>& p) {
    OpenFile tmp;
    tmp.generation = generation_.load();
    tmp.staged = p->staged;
    tmp.pendingKey = p->key;
    try {
        commit(tmp);
    } catch (...) {
        std::lock_guard lk(stateMutex_);  // the failure is recorded (manifest, counters); an empty file is not worth showing
        pending_.erase(p->key);
        throw;
    }
}

void MtpFilesystem::housekeeping(bool force) noexcept {
    try {
        if (disconnected_.load()) return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<Pending>> due;
        {
            std::lock_guard lk(stateMutex_);
            for (auto& [k, p] : pending_)
                if (p->lingering && (force || now - p->lingerSince >= cfg_.emptyFileGrace)) due.push_back(p);
        }
        for (auto& p : due) {
            if (hasOpenHandle(p->staged)) continue;
            try {
                commitLingering(p);
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

void MtpFilesystem::ftruncate(uint64_t fh, uint64_t size) {
    guarded([&] {
        auto f = handles_.get(fh, generation_.load());
        std::lock_guard lk(f->mutex);
        if (f->overlay) {
            if (size > kOverlayMaxFile) throw FsError(ENOSPC, "too large");
            std::lock_guard ol(f->overlay->mutex);
            f->overlay->data.resize(size);
            return;
        }
        if (f->staged) {
            f->staged->truncate(size);
            return;
        }
        if (size == f->node.sizeBytes) return;
        throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP, "existing phone files cannot be resized");
    });
}

void MtpFilesystem::truncate(const std::string& path, uint64_t size) {
    guarded([&] {
        Resolved r = resolve(path);
        if (r.isDir()) throw FsError(EISDIR, "is a directory");
        switch (r.kind) {
            case Resolved::Kind::Pending:
                r.pending->staged->truncate(size);
                return;
            case Resolved::Kind::Overlay: {
                if (size > kOverlayMaxFile) throw FsError(ENOSPC, "too large");
                std::lock_guard ol(r.overlay->mutex);
                r.overlay->data.resize(size);
                return;
            }
            default:
                if (size == r.node.sizeBytes) return;
                throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP, "existing phone files cannot be resized");
        }
    });
}

void MtpFilesystem::mkdir(const std::string& path) {
    guarded([&] {
        if (cfg_.readOnly) throw FsError(EROFS, "read-only volume");
        std::string leaf;
        Resolved parent = resolveParent(path, leaf);
        if (parent.kind == Resolved::Kind::Root) throw FsError(EPERM, "storages are defined by the phone");
        if (parent.storage.storage.readOnly) throw FsError(EROFS, "storage is read-only");
        if (!isValidObjectName(leaf)) throw FsError(EINVAL, "invalid folder name");
        if (cfg_.metadataOverlay && isMacMetadataDirName(leaf))
            throw FsError(EPERM, "macOS index/trash folders cannot be created on an MTP volume");
        const StorageId sid = parent.storage.storage.id;
        const ObjectId pid = parent.dirId();
        if (listing(sid, pid)->find(leaf)) throw FsError(EEXIST, "file exists");
        const ObjectId created = dev()->mkdir(sid, pid, leaf);
        markFresh(sid, created);
        invalidateParent(sid, pid);
    });
}

void MtpFilesystem::unlink(const std::string& path) {
    guarded([&] {
        if (cfg_.readOnly) throw FsError(EROFS, "read-only volume");
        Resolved r = resolve(path);
        switch (r.kind) {
            case Resolved::Kind::Root:
            case Resolved::Kind::Storage:
                throw FsError(EPERM, "cannot remove a storage");
            case Resolved::Kind::Pending: {
                if (!r.pending->lingering || hasOpenHandle(r.pending->staged))
                    throw FsError(EBUSY, "file is still being written");
                r.pending->staged->discard();  // an empty file nobody has open: only the local staging file goes away
                std::lock_guard lk(stateMutex_);
                pending_.erase(r.pending->key);
                return;
            }
            case Resolved::Kind::Overlay: {
                std::lock_guard lk(stateMutex_);
                overlay_->items.erase(pendingKeyFor(r.node.storageId, r.parent, normalizeNfc(r.displayName)));
                return;
            }
            case Resolved::Kind::Object:
                if (r.node.isDirectory) throw FsError(EISDIR, "is a directory");
                if (r.storage.storage.readOnly) throw FsError(EROFS, "storage is read-only");
                dev()->remove(r.node.objectId, false);
                invalidateParent(r.node.storageId, r.parent);
                return;
        }
    });
}

void MtpFilesystem::rmdir(const std::string& path) {
    guarded([&] {
        if (cfg_.readOnly) throw FsError(EROFS, "read-only volume");
        Resolved r = resolve(path);
        if (r.kind == Resolved::Kind::Root || r.kind == Resolved::Kind::Storage)
            throw FsError(EPERM, "cannot remove a storage");
        if (r.kind != Resolved::Kind::Object || !r.node.isDirectory) throw FsError(ENOTDIR, "not a directory");
        if (r.storage.storage.readOnly) throw FsError(EROFS, "storage is read-only");
        // Never recursive: the device layer refuses non-empty folders with ENOTEMPTY.
        dev()->remove(r.node.objectId, true);
        invalidateParent(r.node.storageId, r.parent);
    });
}

void MtpFilesystem::rename(const std::string& from, const std::string& to) {
    guarded([&] {
        if (cfg_.readOnly) throw FsError(EROFS, "read-only volume");
        Resolved src = resolve(from);
        if (src.kind == Resolved::Kind::Root || src.kind == Resolved::Kind::Storage)
            throw FsError(EPERM, "storages cannot be renamed");
        if (src.kind == Resolved::Kind::Pending) {
            if (!src.pending->lingering || hasOpenHandle(src.pending->staged))
                throw FsError(EBUSY, "file is still being written");
            commitLingering(src.pending);  // a rename needs the object to exist on the phone first
            src = resolve(from);
        }

        std::string leaf;
        Resolved dstParent = resolveParent(to, leaf);
        if (dstParent.kind == Resolved::Kind::Root) throw FsError(EPERM, "cannot place files next to storages");
        if (!isValidObjectName(leaf)) throw FsError(EINVAL, "invalid name");
        const StorageId dsid = dstParent.storage.storage.id;
        const ObjectId dpid = dstParent.dirId();
        if (dsid != src.node.storageId || dpid != src.parent)
            throw FsError(EXDEV, "moving between folders is not supported; copy and delete explicitly");
        if (normalizeNfc(leaf) == normalizeNfc(src.displayName)) return;

        const std::string dkey = pendingKeyFor(dsid, dpid, normalizeNfc(leaf));
        {
            std::lock_guard lk(stateMutex_);
            if (pending_.count(dkey) || overlay_->items.count(dkey)) throw FsError(EEXIST, "target exists");
        }
        if (listing(dsid, dpid)->find(leaf))
            throw FsError(EOPNOTSUPP, "replacing an existing file is not supported (no safe atomic overwrite on MTP)");

        if (src.kind == Resolved::Kind::Overlay) {
            if (!(cfg_.metadataOverlay && isMacMetadataName(leaf)))
                throw FsError(EOPNOTSUPP, "metadata files cannot be renamed into real files");
            std::lock_guard lk(stateMutex_);
            const std::string skey = pendingKeyFor(dsid, dpid, normalizeNfc(src.displayName));
            auto it = overlay_->items.find(skey);
            if (it == overlay_->items.end()) throw FsError(ENOENT, "gone");
            auto item = std::move(it->second);
            overlay_->items.erase(it);
            item.name = leaf;
            overlay_->items.emplace(dkey, std::move(item));
            return;
        }
        if (src.storage.storage.readOnly) throw FsError(EROFS, "storage is read-only");
        dev()->rename(src.node.objectId, leaf);
        invalidateParent(dsid, dpid);
    });
}

void MtpFilesystem::markFresh(StorageId s, ObjectId id) {
    std::lock_guard lk(stateMutex_);
    if (freshObjects_.size() >= 4096) freshObjects_.clear();
    freshObjects_.emplace(s, id);
}

void MtpFilesystem::utimens(const std::string& path) {
    guarded([&] {
        Resolved r = resolve(path);
        if (r.kind == Resolved::Kind::Pending || r.kind == Resolved::Kind::Overlay || cfg_.acceptMetadataNoops) return;
        if (r.kind == Resolved::Kind::Object) {
            std::lock_guard lk(stateMutex_);
            if (freshObjects_.count({r.node.storageId, r.node.objectId})) return;
        }
        throw FsError(cfg_.readOnly ? EROFS : EOPNOTSUPP,
                      "MTP does not allow setting timestamps; the phone assigns them");
    });
}

void MtpFilesystem::chmod(const std::string& path, uint32_t mode) {
    guarded([&] {
        Resolved r = resolve(path);
        const FsStat st = statOf(r);
        if ((mode & 07777) == st.mode || cfg_.acceptMetadataNoops) return;
        throw FsError(EPERM, "MTP objects have no POSIX permissions");
    });
}

void MtpFilesystem::chown(const std::string& path, int64_t uid, int64_t gid) {
    guarded([&] {
        resolve(path);
        const bool uidOk = uid < 0 || static_cast<uint32_t>(uid) == cfg_.uid;
        const bool gidOk = gid < 0 || static_cast<uint32_t>(gid) == cfg_.gid;
        if ((uidOk && gidOk) || cfg_.acceptMetadataNoops) return;
        throw FsError(EPERM, "MTP objects have no owners");
    });
}

FsStatvfs MtpFilesystem::statfs(const std::string& path) {
    return guarded([&]() -> FsStatvfs {
        Resolved r = resolve(path);
        FsStatvfs out;
        uint64_t cap = 0, free = 0;
        if (r.kind == Resolved::Kind::Root) {
            for (const auto& s : storageView()) {
                cap += s.storage.capacityBytes;
                free += s.storage.freeBytes;
            }
        } else {
            cap = r.storage.storage.capacityBytes;
            free = r.storage.storage.freeBytes;
        }
        if (cap == 0) throw FsError(EOPNOTSUPP, "the phone does not report storage capacity");
        out.blocks = cap / out.blockSize;
        out.blocksFree = free / out.blockSize;
        out.blocksAvail = cfg_.readOnly ? 0 : out.blocksFree;
        return out;
    });
}

// ---- lifecycle -------------------------------------------------------------------------------------

void MtpFilesystem::reconnect(std::shared_ptr<IMtpDevice> device) {
    if (!device) throw MtpError(MtpErrc::InvalidArgument, "no device");
    {
        std::lock_guard lk(deviceMutex_);
        device_ = std::move(device);
    }
    ++generation_;  // every OpenFile from the previous session now answers ESTALE
    cache_.clear();
    inodes_.reset();
    {
        std::lock_guard lk(stateMutex_);
        pending_.clear();  // staged bytes stay on disk; they are not valid in the new session
        storageCache_.clear();
        freshObjects_.clear();
    }
    disconnectNotified_ = false;
    disconnected_ = false;
}

size_t MtpFilesystem::pendingUploadCount() const {
    std::lock_guard lk(stateMutex_);
    return pending_.size();
}

}  // namespace mtport
