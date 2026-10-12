#include "LibmtpDevice.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "mtport/NameRules.hpp"

namespace fs = std::filesystem;

namespace mtport {

namespace {

// Per GetPartialObject round trip. The phone has a fixed cost per request, so bigger requests stream faster
// (Galaxy S21 FE, USB High Speed: 256 KiB 15.6, 1 MiB 24.3, 16 MiB 33.6 MB/s). Small reads stay small: only as much as
// the caller asked for is requested.
constexpr uint32_t kMaxPartialChunk = 16u << 20;

// How old a remembered object may be when a folder is listed. Listings are for display; anything that reads or
// changes an object asks the phone directly. Mutating operations check names against a much fresher view.
constexpr std::chrono::milliseconds kListMaxAge{5 * 60 * 1000};
constexpr std::chrono::milliseconds kMutationMaxAge{30 * 1000};
constexpr size_t kMaxRememberedObjects = 200000;

// PTP response codes that matter for error mapping (ISO 15740).
constexpr unsigned kPtpSessionNotOpen = 0x2003, kPtpOpNotSupported = 0x2005, kPtpInvalidObject = 0x2009,
                   kPtpStoreFull = 0x200C, kPtpStoreReadOnly = 0x200E, kPtpAccessDenied = 0x200F,
                   kPtpDeviceBusy = 0x2019;

// libmtp reports PTP failures as text such as "PTP Layer error 200f: ...". Extract the code.
int extractPtpCode(const std::string& text) {
    auto pos = text.find("error ");
    if (pos == std::string::npos) return -1;
    pos += 6;
    if (text.compare(pos, 2, "0x") == 0) pos += 2;
    unsigned v = 0;
    int digits = 0;
    while (pos < text.size() && digits < 4 && std::isxdigit(static_cast<unsigned char>(text[pos]))) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos])));
        v = v * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        ++pos;
        ++digits;
    }
    return digits == 4 ? static_cast<int>(v) : -1;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

LIBMTP_filetype_t guessFileType(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return LIBMTP_FILETYPE_UNKNOWN;
    std::string ext(name.substr(dot + 1));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "jpg" || ext == "jpeg") return LIBMTP_FILETYPE_JPEG;
    if (ext == "png") return LIBMTP_FILETYPE_PNG;
    if (ext == "gif") return LIBMTP_FILETYPE_GIF;
    if (ext == "mp3") return LIBMTP_FILETYPE_MP3;
    if (ext == "mp4" || ext == "m4v") return LIBMTP_FILETYPE_MP4;
    if (ext == "txt") return LIBMTP_FILETYPE_TEXT;
    if (ext == "html" || ext == "htm") return LIBMTP_FILETYPE_HTML;
    return LIBMTP_FILETYPE_UNKNOWN;
}

struct ProgressCtx {
    const ProgressCallback* cb;
    bool cancelled{false};
};

int progressTrampoline(uint64_t const sent, uint64_t const total, void const* const data) {
    auto* ctx = static_cast<ProgressCtx*>(const_cast<void*>(data));
    if (!ctx || !ctx->cb || !*ctx->cb) return 0;
    try {
        if (!(*ctx->cb)(TransferProgress{sent, total})) {
            ctx->cancelled = true;
            return 1;
        }
    } catch (...) {
        ctx->cancelled = true;
        return 1;
    }
    return 0;
}

std::string randomSuffix() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}

FileNode toNode(const LIBMTP_file_t& f) {
    FileNode n;
    n.storageId = f.storage_id;
    n.objectId = f.item_id;
    n.parentObjectId = f.parent_id == LIBMTP_FILES_AND_FOLDERS_ROOT ? kRootObject : f.parent_id;
    n.name = f.filename ? f.filename : "";
    n.isDirectory = f.filetype == LIBMTP_FILETYPE_FOLDER;
    n.sizeBytes = n.isDirectory ? 0 : f.filesize;
    if (f.modificationdate > 0) n.modifiedAt = std::chrono::system_clock::from_time_t(f.modificationdate);
    return n;
}

}  // namespace

LibmtpDevice::LibmtpDevice(detail::SessionPtr session, DeviceDescriptor desc, DeviceLock lock)
    : session_(std::move(session)), desc_(std::move(desc)), lock_(std::move(lock)) {
    auto* d = session_.get();
    caps_.getPartialObject = LIBMTP_Check_Capability(d, LIBMTP_DEVICECAP_GetPartialObject) != 0;
    caps_.sendPartialObject = LIBMTP_Check_Capability(d, LIBMTP_DEVICECAP_SendPartialObject) != 0;
    caps_.editObjects = LIBMTP_Check_Capability(d, LIBMTP_DEVICECAP_EditObjects) != 0;
    caps_.moveObject = LIBMTP_Check_Capability(d, LIBMTP_DEVICECAP_MoveObject) != 0;
    caps_.copyObject = LIBMTP_Check_Capability(d, LIBMTP_DEVICECAP_CopyObject) != 0;
    // libmtp has no capability bit for these; rename depends on the ObjectFileName property and
    // deletion is a mandatory-ish operation. Both are verified by the device's actual answer.
    caps_.renameObject = LIBMTP_Is_Property_Supported(d, LIBMTP_PROPERTY_ObjectFileName, LIBMTP_FILETYPE_UNKNOWN) != 0;
    caps_.deleteObject = true;
    desc_.mtpReady = true;
    desc_.manufacturer = detail::takeString(LIBMTP_Get_Manufacturername(d));
    desc_.model = detail::takeString(LIBMTP_Get_Modelname(d));
    desc_.friendlyName = detail::takeString(LIBMTP_Get_Friendlyname(d));
    LIBMTP_Clear_Errorstack(d);
}

LibmtpDevice::~LibmtpDevice() = default;  // SessionPtr releases the libmtp handle, DeviceLock the flock

void LibmtpDevice::ensureUsable() const {
    if (broken_) throw MtpError(MtpErrc::DeviceDisconnected, "the MTP session failed earlier; reconnect the phone");
}

void LibmtpDevice::fail(const char* op, MtpErrc fallback) {
    std::string text;
    LIBMTP_error_number_t num = LIBMTP_ERROR_NONE;
    for (auto* e = LIBMTP_Get_Errorstack(session_.get()); e; e = e->next) {
        num = e->errornumber;
        if (e->error_text) {
            if (!text.empty()) text += "; ";
            text += e->error_text;
        }
    }
    LIBMTP_Clear_Errorstack(session_.get());

    MtpErrc code = fallback;
    const int ptp = extractPtpCode(text);
    switch (num) {
        case LIBMTP_ERROR_STORAGE_FULL:
            code = MtpErrc::DeviceStorageFull;
            break;
        case LIBMTP_ERROR_CANCELLED:
            code = MtpErrc::Cancelled;
            break;
        case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
            code = MtpErrc::DeviceDisconnected;
            broken_ = true;
            break;
        case LIBMTP_ERROR_CONNECTING:
            code = MtpErrc::DeviceBusy;
            break;
        case LIBMTP_ERROR_USB_LAYER:
            // libusb failures after a successful open almost always mean unplug or a stalled bus.
            if (contains(text, "TIMEOUT") || contains(text, "timed out")) {
                code = MtpErrc::Timeout;
            } else {
                code = MtpErrc::DeviceDisconnected;
                broken_ = true;
            }
            break;
        default:
            break;
    }
    if (ptp >= 0) {
        switch (static_cast<unsigned>(ptp)) {
            case kPtpInvalidObject:
                code = MtpErrc::ObjectNotFound;
                break;
            case kPtpStoreFull:
                code = MtpErrc::DeviceStorageFull;
                break;
            case kPtpStoreReadOnly:
                code = MtpErrc::RemoteReadOnly;
                break;
            case kPtpAccessDenied:
                code = MtpErrc::RemoteReadOnly;
                break;
            case kPtpOpNotSupported:
                code = MtpErrc::OperationUnsupported;
                break;
            case kPtpDeviceBusy:
                code = MtpErrc::DeviceBusy;
                break;
            case kPtpSessionNotOpen:
                code = MtpErrc::DeviceDisconnected;
                broken_ = true;
                break;
            default:
                break;
        }
    }
    throw MtpError(code, std::string(op) + " failed" + (text.empty() ? "" : " (" + text + ")"));
}

std::vector<StorageDescriptor> LibmtpDevice::storages() {
    auto lk = lockDevice();
    ensureUsable();
    return storagesLocked();
}

std::vector<StorageDescriptor> LibmtpDevice::storagesLocked() {
    auto* d = session_.get();
    const int rc = LIBMTP_Get_Storage(d, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (rc < 0) fail("storage query", MtpErrc::MtpNotReady);
    std::vector<StorageDescriptor> out;
    for (auto* s = d->storage; s; s = s->next) {
        StorageDescriptor sd;
        sd.id = s->id;
        if (s->StorageDescription && *s->StorageDescription)
            sd.name = s->StorageDescription;
        else if (s->VolumeIdentifier && *s->VolumeIdentifier)
            sd.name = s->VolumeIdentifier;
        else
            sd.name = "Storage " + std::to_string(s->id);
        sd.capacityBytes = s->MaxCapacity;
        sd.freeBytes = s->FreeSpaceInBytes;
        sd.readOnly = s->AccessCapability == 1 || s->AccessCapability == 2;
        out.push_back(std::move(sd));
    }
    return out;
}

std::unique_lock<std::mutex> LibmtpDevice::lockDevice() {
    ++waiting_;
    std::unique_lock<std::mutex> lk(mutex_);
    --waiting_;
    return lk;
}

void LibmtpDevice::remember(const FileNode& n) {
    if (objects_.size() >= kMaxRememberedObjects) objects_.clear();
    objects_[n.objectId] = CachedNode{n, std::chrono::steady_clock::now()};
}

std::vector<FileNode> LibmtpDevice::list(StorageId storage, ObjectId parent) {
    const auto key = std::make_pair(storage, parent);
    {
        std::unique_lock ll(listingMutex_);
        listingDone_.wait(ll, [&] { return !listingsRunning_.count(key) || cancelListings_.load(); });
        listingsRunning_.insert(key);
    }
    struct Done {
        LibmtpDevice& d;
        std::pair<StorageId, ObjectId> key;
        ~Done() {
            {
                std::lock_guard ll(d.listingMutex_);
                d.listingsRunning_.erase(key);
            }
            d.listingDone_.notify_all();
        }
    } done{*this, key};
    auto lk = lockDevice();
    ensureUsable();
    return listImpl(storage, parent, kListMaxAge, &lk);
}

std::vector<FileNode> LibmtpDevice::listLocked(StorageId storage, ObjectId parent) {
    return listImpl(storage, parent, kMutationMaxAge, nullptr);
}

std::vector<uint32_t> LibmtpDevice::childHandles(StorageId storage, uint32_t parentHandle) {
    LIBMTP_Clear_Errorstack(session_.get());  // a failure is only an error if *this* call pushed one
    uint32_t* kids = nullptr;
    const int n = LIBMTP_Get_Children(session_.get(), storage, parentHandle, &kids);
    detail::MemPtr<uint32_t> guard(kids);
    if (n < 0) fail("folder listing", MtpErrc::IoError);
    return n > 0 && kids ? std::vector<uint32_t>(kids, kids + n) : std::vector<uint32_t>{};
}

std::vector<FileNode> LibmtpDevice::listImpl(StorageId storage, ObjectId parent, std::chrono::milliseconds maxAge,
                                             std::unique_lock<std::mutex>* yieldLock) {
    const uint32_t p = parent == kRootObject ? LIBMTP_FILES_AND_FOLDERS_ROOT : parent;
    const std::vector<uint32_t> handles = childHandles(storage, p);
    if (handles.empty()) {
        // A flaky USB link was seen to yield an empty result without any error. An empty folder must never be
        // *shown* when the phone actually has children, so ask a second time before believing it.
        const auto again = childHandles(storage, p);
        if (!again.empty())
            throw MtpError(MtpErrc::IoError, "the phone returned an empty listing for a folder with " +
                                                 std::to_string(again.size()) +
                                                 " entries; refusing to present it as empty");
        return {};
    }
    std::vector<FileNode> out;
    out.reserve(handles.size());
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < handles.size(); ++i) {
        if (yieldLock && i > 0 && waiting_.load() > 0) {
            // Someone else (a read, the Finder asking about another folder) is waiting: let them go first.
            yieldLock->unlock();
            for (int spin = 0; spin < 500 && waiting_.load() > 0; ++spin) std::this_thread::sleep_for(std::chrono::microseconds(100));
            yieldLock->lock();
        }
        ensureUsable();
        if (yieldLock && cancelListings_.load())  // only browsing is cancellable: uploads must finish
            throw MtpError(MtpErrc::Cancelled, "folder listing cancelled");
        FileNode n;
        const auto it = objects_.find(handles[i]);
        const bool sameParent = it != objects_.end() && (it->second.node.parentObjectId == parent ||
                                                         (parent == kRootObject && it->second.node.parentObjectId == 0));
        if (sameParent && started - it->second.fetched <= maxAge) {
            n = it->second.node;
        } else {
            try {
                n = statLocked(handles[i]);
            } catch (const MtpError& e) {
                if (e.code() == MtpErrc::ObjectNotFound) continue;  // deleted on the phone while we were listing
                throw;
            }
        }
        if (n.storageId == 0) n.storageId = storage;
        if (n.parentObjectId == 0 && parent != kRootObject) n.parentObjectId = parent;
        out.push_back(std::move(n));
    }
    return out;
}

FileNode LibmtpDevice::stat(ObjectId id) {
    auto lk = lockDevice();
    ensureUsable();
    return statLocked(id);
}

FileNode LibmtpDevice::statLocked(ObjectId id) {
    if (id == kRootObject) throw MtpError(MtpErrc::InvalidArgument, "the storage root has no object metadata");
    LIBMTP_Clear_Errorstack(session_.get());
    detail::FilePtr f(LIBMTP_Get_Filemetadata(session_.get(), id));
    if (!f) {
        if (LIBMTP_Get_Errorstack(session_.get()) != nullptr) fail("metadata query", MtpErrc::ObjectNotFound);
        throw MtpError(MtpErrc::ObjectNotFound, "no such MTP object");
    }
    FileNode node = toNode(*f);
    remember(node);
    return node;
}

size_t LibmtpDevice::read(ObjectId id, uint64_t offset, std::span<std::byte> dst) {
    auto lk = lockDevice();
    ensureUsable();
    if (dst.empty()) return 0;
    if (!caps_.getPartialObject)
        throw MtpError(MtpErrc::OperationUnsupported, "device does not support GetPartialObject");
    if (offset > UINT64_MAX - dst.size()) throw MtpError(MtpErrc::InvalidArgument, "read range overflows 64 bits");
    size_t total = 0;
    while (total < dst.size()) {
        const uint32_t want = static_cast<uint32_t>(std::min<size_t>(dst.size() - total, kMaxPartialChunk));
        unsigned char* data = nullptr;
        unsigned int got = 0;
        const int rc = LIBMTP_GetPartialObject(session_.get(), id, offset + total, want, &data, &got);
        detail::MemPtr<unsigned char> guard(data);
        if (rc != 0) fail("partial read", MtpErrc::IoError);
        if (got == 0) break;  // EOF
        if (got > want || !data) throw MtpError(MtpErrc::ProtocolError, "device returned more data than requested");
        std::memcpy(dst.data() + total, data, got);
        total += got;
    }
    return total;
}

void LibmtpDevice::download(ObjectId id, const fs::path& target, ProgressCallback progress) {
    auto lk = lockDevice();
    ensureUsable();
    struct stat st{};
    if (::lstat(target.c_str(), &st) == 0)
        throw MtpError(MtpErrc::FileAlreadyExists, target.string() + " already exists");
    const FileNode node = statLocked(id);
    if (node.isDirectory) throw MtpError(MtpErrc::IsADirectory, "object is a folder");

    const fs::path tmp = target.parent_path() / ("." + target.filename().string() + ".afpart-" + randomSuffix());
    UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot create " + tmp.string() + ": " + std::strerror(errno));

    auto discardTemp = [&] {
        fd.reset();
        ::unlink(tmp.c_str());  // our own private temporary file, never the user's data
    };

    ProgressCtx ctx{&progress};
    const int rc = LIBMTP_Get_File_To_File_Descriptor(session_.get(), id, fd.get(), progressTrampoline, &ctx);
    if (rc != 0) {
        discardTemp();
        if (ctx.cancelled) {
            LIBMTP_Clear_Errorstack(session_.get());
            throw MtpError(MtpErrc::Cancelled, "download cancelled");
        }
        fail("download", MtpErrc::IoError);
    }
    struct stat out{};
    if (::fstat(fd.get(), &out) != 0 || static_cast<uint64_t>(out.st_size) != node.sizeBytes) {
        discardTemp();
        throw MtpError(MtpErrc::IoError, "downloaded size does not match the size the phone reported");
    }
    if (::fcntl(fd.get(), F_FULLFSYNC) != 0) ::fsync(fd.get());
    ::fchmod(fd.get(), 0644);
    fd.reset();
    if (::renamex_np(tmp.c_str(), target.c_str(), RENAME_EXCL) != 0) {
        const int e = errno;
        ::unlink(tmp.c_str());
        if (e == EEXIST) throw MtpError(MtpErrc::FileAlreadyExists, target.string() + " appeared during download");
        throw MtpError(MtpErrc::IoError, "cannot move download into place: " + std::string(std::strerror(e)));
    }
}

ObjectId LibmtpDevice::upload(StorageId storage, ObjectId parent, const fs::path& src, std::string_view finalName,
                              ProgressCallback progress) {
    auto lk = lockDevice();
    ensureUsable();
    if (!isValidObjectName(finalName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid file name");

    UniqueFd fd(::open(src.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot open " + src.string() + ": " + std::strerror(errno));
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode))
        throw MtpError(MtpErrc::InvalidArgument, src.string() + " is not a regular file");
    const auto size = static_cast<uint64_t>(st.st_size);

    for (const auto& s : storagesLocked()) {
        if (s.id != storage) continue;
        if (s.readOnly) throw MtpError(MtpErrc::RemoteReadOnly, "storage is read-only");
        if (s.freeBytes != 0 && size > s.freeBytes)
            throw MtpError(MtpErrc::DeviceStorageFull, "not enough free space on the phone");
    }
    for (const auto& n : listLocked(storage, parent))
        if (n.name == finalName)
            throw MtpError(MtpErrc::FileAlreadyExists, "a file with this name already exists on the phone");

    detail::FilePtr meta(LIBMTP_new_file_t());
    if (!meta) throw MtpError(MtpErrc::IoError, "out of memory");
    meta->filename = ::strdup(std::string(finalName).c_str());
    meta->filesize = size;
    meta->parent_id = parent;  // 0 == storage root for sends
    meta->storage_id = storage;
    meta->filetype = guessFileType(finalName);
    meta->modificationdate = st.st_mtime;

    ProgressCtx ctx{&progress};
    const int rc =
        LIBMTP_Send_File_From_File_Descriptor(session_.get(), fd.get(), meta.get(), progressTrampoline, &ctx);
    if (rc != 0) {
        // The phone may now hold an incomplete object. We deliberately do not try to delete
        // something we cannot positively identify; the caller keeps its staged bytes.
        if (ctx.cancelled) {
            LIBMTP_Clear_Errorstack(session_.get());
            throw MtpError(MtpErrc::Cancelled, "upload cancelled; the phone may hold an incomplete object");
        }
        try {
            fail("upload", MtpErrc::IoError);
        } catch (const MtpError& e) {
            throw MtpError(e.code(), e.detail() + "; the phone may hold an incomplete object that was not removed");
        }
    }
    if (meta->item_id == 0) throw MtpError(MtpErrc::ProtocolError, "device did not return an object id for the upload");
    return meta->item_id;
}

ObjectId LibmtpDevice::mkdir(StorageId storage, ObjectId parent, std::string_view name) {
    auto lk = lockDevice();
    ensureUsable();
    if (!isValidObjectName(name)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid folder name");
    for (const auto& s : storagesLocked())
        if (s.id == storage && s.readOnly) throw MtpError(MtpErrc::RemoteReadOnly, "storage is read-only");
    for (const auto& n : listLocked(storage, parent))
        if (n.name == name) throw MtpError(MtpErrc::FileAlreadyExists, "name already exists on the phone");
    std::string mutableName(name);  // libmtp takes a non-const char*
    const uint32_t id = LIBMTP_Create_Folder(session_.get(), mutableName.data(), parent, storage);
    if (id == 0) fail("create folder", MtpErrc::IoError);
    return id;
}

void LibmtpDevice::remove(ObjectId id, bool isDirectory) {
    auto lk = lockDevice();
    ensureUsable();
    const FileNode node = statLocked(id);
    if (node.isDirectory != isDirectory)
        throw MtpError(isDirectory ? MtpErrc::NotADirectory : MtpErrc::IsADirectory, "object type mismatch");
    if (node.isDirectory) {
        // MTP DeleteObject on a folder may remove its whole subtree on the phone. Refuse unless
        // the folder is verifiably empty.
        uint32_t* kids = nullptr;
        const int n = LIBMTP_Get_Children(session_.get(), node.storageId, id, &kids);
        detail::MemPtr<uint32_t> guard(kids);
        if (n < 0) fail("child query", MtpErrc::IoError);
        if (n > 0) throw MtpError(MtpErrc::DirectoryNotEmpty, "folder is not empty");
    }
    objects_.erase(id);
    if (LIBMTP_Delete_Object(session_.get(), id) != 0) fail("delete", MtpErrc::IoError);
}

void LibmtpDevice::rename(ObjectId id, std::string_view newName) {
    auto lk = lockDevice();
    ensureUsable();
    if (!isValidObjectName(newName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid name");
    if (!caps_.renameObject) throw MtpError(MtpErrc::OperationUnsupported, "device does not support renaming objects");
    const FileNode node = statLocked(id);
    for (const auto& n : listLocked(node.storageId, node.parentObjectId))
        if (n.name == newName && n.objectId != id) throw MtpError(MtpErrc::FileAlreadyExists, "name already exists");
    std::string mutableName(newName);
    if (LIBMTP_Set_Object_Filename(session_.get(), id, mutableName.data()) != 0) fail("rename", MtpErrc::IoError);
    // Devices sometimes accept the command and ignore it. Only a re-read proves success.
    if (statLocked(id).name != newName)
        throw MtpError(MtpErrc::OperationUnsupported, "the phone accepted the rename but did not apply it");
}

}  // namespace mtport
