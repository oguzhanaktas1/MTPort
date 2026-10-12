#include "mtport/WriteStaging.hpp"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "mtport/RuntimePaths.hpp"

namespace fs = std::filesystem;

namespace mtport {

namespace {

std::string randomId() {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard lk(m);
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(rng()),
                  static_cast<unsigned long long>(rng()));
    return buf;
}

uint64_t availableBytes(const fs::path& dir) {
    struct statvfs sv{};
    if (::statvfs(dir.c_str(), &sv) != 0) return 0;
    return static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%')
            out += "%25";
        else if (c == '\n')
            out += "%0A";
        else if (c == '\r')
            out += "%0D";
        else
            out.push_back(c);
    }
    return out;
}
std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string h = s.substr(i + 1, 2);
            if (h == "25") {
                out.push_back('%');
                i += 2;
                continue;
            }
            if (h == "0A") {
                out.push_back('\n');
                i += 2;
                continue;
            }
            if (h == "0D") {
                out.push_back('\r');
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

}  // namespace

const char* toString(StageState s) {
    switch (s) {
        case StageState::Open:
            return "OPEN";
        case StageState::Dirty:
            return "DIRTY";
        case StageState::Committing:
            return "COMMITTING";
        case StageState::Committed:
            return "COMMITTED";
        case StageState::Failed:
            return "FAILED";
    }
    return "?";
}

// ---- WriteStaging --------------------------------------------------------------------------

uint64_t WriteStaging::defaultQuota(const fs::path& dir) {
    constexpr uint64_t kCap = 20ull << 30;
    return std::min<uint64_t>(kCap, availableBytes(dir) / 5);
}

WriteStaging::WriteStaging(Config cfg) : dir_(std::move(cfg.directory)), minFree_(cfg.minFreeDiskBytes) {
    ensurePrivateDir(dir_);
    quota_ = cfg.quotaBytes ? cfg.quotaBytes : defaultQuota(dir_);
    // Bytes preserved from earlier (failed/abandoned) sessions still occupy the budget.
    std::error_code ec;
    uint64_t existing = 0;
    for (const auto& e : fs::directory_iterator(dir_, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".stage") existing += e.file_size(ec);
    used_ = existing;
}

void WriteStaging::reserve(uint64_t bytes) {
    std::lock_guard lk(reserveMutex_);
    if (bytes > quota_ || used_.load() > quota_ - bytes)
        throw MtpError(MtpErrc::StagingDiskFull, "MTPort staging quota exhausted");
    const uint64_t avail = availableBytes(dir_);
    if (avail < minFree_ || bytes > avail - minFree_)
        throw MtpError(MtpErrc::StagingDiskFull, "not enough free space on the Mac for staging");
    used_ += bytes;
}

void WriteStaging::release(uint64_t bytes) {
    uint64_t cur = used_.load();
    while (!used_.compare_exchange_weak(cur, cur >= bytes ? cur - bytes : 0)) {
    }
}

std::shared_ptr<StagedFile> WriteStaging::create(std::string finalName, StorageId storage, ObjectId parent) {
    const std::string id = randomId();
    const fs::path path = dir_ / (id + ".stage");
    UniqueFd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!fd.valid()) {
        if (errno == ENOSPC) throw MtpError(MtpErrc::StagingDiskFull, "cannot create staging file: disk full");
        throw MtpError(MtpErrc::IoError, std::string("cannot create staging file: ") + std::strerror(errno));
    }
    auto sf = std::shared_ptr<StagedFile>(
        new StagedFile(this, id, path, std::move(fd), std::move(finalName), storage, parent));
    sf->state_ = StageState::Dirty;  // a created file must exist on the phone after close, even if empty
    return sf;
}

std::vector<WriteStaging::Recoverable> WriteStaging::listRecoverable() const {
    std::vector<Recoverable> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (e.path().extension() != ".manifest") continue;
        std::ifstream in(e.path());
        Recoverable r;
        r.manifest = e.path();
        r.stagedFile = e.path();
        r.stagedFile.replace_extension(".stage");
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = unescape(line.substr(eq + 1));
            if (k == "name")
                r.finalName = v;
            else if (k == "size")
                r.sizeBytes = std::strtoull(v.c_str(), nullptr, 10);
            else if (k == "error")
                r.error = v;
        }
        out.push_back(std::move(r));
    }
    return out;
}

// ---- StagedFile ----------------------------------------------------------------------------

StagedFile::StagedFile(WriteStaging* owner, std::string id, fs::path path, UniqueFd fd, std::string finalName,
                       StorageId storage, ObjectId parent)
    : owner_(owner),
      id_(std::move(id)),
      path_(std::move(path)),
      fd_(std::move(fd)),
      finalName_(std::move(finalName)),
      storage_(storage),
      parent_(parent) {}

StagedFile::~StagedFile() {
    std::lock_guard lk(mutex_);
    if (state_ == StageState::Committed || discarded_) return;  // file already gone / released
    // Abandoned with content: keep the user's bytes and say why. Empty files carry nothing.
    if (size_ == 0) {
        fd_.reset();
        ::unlink(path_.c_str());
        owner_->release(reserved_);
        return;
    }
    if (state_ != StageState::Failed) {
        failure_ = MtpErrc::Cancelled;
        failureText_ = "mount ended before this file was uploaded";
    }
    writeManifestLocked(failureText_);
}

void StagedFile::writeManifestLocked(const std::string& error) {
    fs::path manifest = path_;
    manifest.replace_extension(".manifest");
    UniqueFd fd(::open(manifest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!fd.valid()) return;
    std::ostringstream os;
    os << "name=" << escape(finalName_) << "\nstorage=" << storage_ << "\nparent=" << parent_ << "\nsize=" << size_
       << "\nerror=" << escape(error) << "\n";
    const std::string text = os.str();
    [[maybe_unused]] ssize_t n = ::write(fd.get(), text.data(), text.size());
}

StageState StagedFile::state() const {
    std::lock_guard lk(mutex_);
    return state_;
}
uint64_t StagedFile::size() const {
    std::lock_guard lk(mutex_);
    return size_;
}
std::optional<MtpErrc> StagedFile::failure() const {
    std::lock_guard lk(mutex_);
    return failure_;
}

size_t StagedFile::write(uint64_t offset, std::span<const std::byte> data) {
    std::lock_guard lk(mutex_);
    if (discarded_) throw MtpError(MtpErrc::StaleHandle, "staged file was discarded");
    if (state_ == StageState::Committed)
        throw MtpError(MtpErrc::OperationUnsupported,
                       "file was already uploaded to the phone; in-place modification is not supported");
    if (state_ == StageState::Failed) throw MtpError(failure_.value_or(MtpErrc::IoError), failureText_);
    if (data.empty()) return 0;
    if (offset > UINT64_MAX - data.size()) throw MtpError(MtpErrc::FileTooLarge, "write offset overflows 64 bits");
    const uint64_t end = offset + data.size();
    if (end > reserved_) {
        owner_->reserve(end - reserved_);
        reserved_ = end;
    }
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n =
            ::pwrite(fd_.get(), data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSPC) throw MtpError(MtpErrc::StagingDiskFull, "Mac disk full while staging");
            throw MtpError(MtpErrc::IoError, std::string("staging write failed: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    size_ = std::max(size_, end);
    state_ = StageState::Dirty;
    return data.size();
}

size_t StagedFile::read(uint64_t offset, std::span<std::byte> dst) {
    std::lock_guard lk(mutex_);
    if (discarded_) throw MtpError(MtpErrc::StaleHandle, "staged file was discarded");
    if (state_ == StageState::Committed)
        throw MtpError(MtpErrc::StaleHandle, "staged bytes were released after upload");
    if (offset >= size_ || dst.empty()) return 0;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(dst.size(), size_ - offset));
    size_t done = 0;
    while (done < want) {
        const ssize_t n = ::pread(fd_.get(), dst.data() + done, want - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw MtpError(MtpErrc::IoError, "staging read failed");
        }
        if (n == 0) {  // sparse tail not yet materialised: reads as zeros, exactly like POSIX
            std::memset(dst.data() + done, 0, want - done);
            done = want;
            break;
        }
        done += static_cast<size_t>(n);
    }
    return want;
}

void StagedFile::truncate(uint64_t newSize) {
    std::lock_guard lk(mutex_);
    if (discarded_) throw MtpError(MtpErrc::StaleHandle, "staged file was discarded");
    if (state_ == StageState::Committed)
        throw MtpError(MtpErrc::OperationUnsupported, "file was already uploaded; truncate is not supported");
    if (state_ == StageState::Failed) throw MtpError(failure_.value_or(MtpErrc::IoError), failureText_);
    if (newSize > reserved_) {
        owner_->reserve(newSize - reserved_);
        reserved_ = newSize;
    }
    if (::ftruncate(fd_.get(), static_cast<off_t>(newSize)) != 0)
        throw MtpError(MtpErrc::IoError, std::string("staging truncate failed: ") + std::strerror(errno));
    if (newSize < reserved_) {
        owner_->release(reserved_ - newSize);
        reserved_ = newSize;
    }
    size_ = newSize;
    state_ = StageState::Dirty;
}

ObjectId StagedFile::commit(const SendFn& send) {
    std::lock_guard lk(mutex_);  // held across the upload: concurrent flush/release wait, then see COMMITTED
    if (discarded_) throw MtpError(MtpErrc::StaleHandle, "staged file was discarded");
    if (state_ == StageState::Committed) return committedId_;
    if (state_ == StageState::Failed) throw MtpError(failure_.value_or(MtpErrc::IoError), failureText_);

    state_ = StageState::Committing;
    try {
        ::fsync(fd_.get());
        const ObjectId id = send(path_);
        committedId_ = id;
        state_ = StageState::Committed;
    } catch (const MtpError& e) {
        state_ = StageState::Failed;
        failure_ = e.code();
        failureText_ = e.detail();
        writeManifestLocked(failureText_);
        throw;
    } catch (const std::exception& e) {
        state_ = StageState::Failed;
        failure_ = MtpErrc::IoError;
        failureText_ = e.what();
        writeManifestLocked(failureText_);
        throw MtpError(MtpErrc::IoError, failureText_);
    }
    // Remote confirmation received: only now is the local copy expendable.
    fd_.reset();
    ::unlink(path_.c_str());
    owner_->release(reserved_);
    reserved_ = 0;
    return committedId_;
}

void StagedFile::discard() {
    std::lock_guard lk(mutex_);
    if (discarded_ || state_ == StageState::Committed) return;
    discarded_ = true;
    fd_.reset();
    ::unlink(path_.c_str());
    fs::path manifest = path_;
    manifest.replace_extension(".manifest");
    ::unlink(manifest.c_str());
    owner_->release(reserved_);
    reserved_ = 0;
}

}  // namespace mtport
