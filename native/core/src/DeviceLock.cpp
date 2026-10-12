#include "mtport/DeviceLock.hpp"

#include <fcntl.h>
#include <sys/file.h>

#include "mtport/MtpError.hpp"
#include "mtport/RuntimePaths.hpp"

namespace mtport {

namespace {
std::filesystem::path lockPath(const DeviceId& id) {
    return locksDir() / (safeName(id) + ".lock");
}

UniqueFd openLockFile(const DeviceId& id) {
    ensurePrivateDir(locksDir());
    UniqueFd fd(::open(lockPath(id).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot open device lock file");
    return fd;
}
}  // namespace

DeviceLock DeviceLock::acquire(const DeviceId& id) {
    UniqueFd fd = openLockFile(id);
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            throw MtpError(MtpErrc::DeviceBusy, "another MTPort process already owns this device's MTP session");
        throw MtpError(MtpErrc::IoError, "flock failed on device lock");
    }
    return DeviceLock(std::move(fd));
}

bool DeviceLock::isHeld(const DeviceId& id) {
    UniqueFd fd = openLockFile(id);
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) return errno == EWOULDBLOCK;
    return false;  // we got it, so nobody held it; closing releases again
}

}  // namespace mtport
