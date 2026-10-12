#include "mtport/PosixErrorMapper.hpp"

#include <new>

namespace mtport {

int toErrno(MtpErrc code) noexcept {
    switch (code) {
        case MtpErrc::DeviceDisconnected:
            return ENODEV;
        case MtpErrc::DeviceBusy:
            return EBUSY;
        case MtpErrc::MtpNotReady:
            return EACCES;
        case MtpErrc::ObjectNotFound:
            return ENOENT;
        case MtpErrc::InvalidVirtualPath:
            return EINVAL;
        case MtpErrc::OperationUnsupported:
            return EOPNOTSUPP;
        case MtpErrc::RemoteReadOnly:
            return EROFS;
        case MtpErrc::StagingDiskFull:
            return ENOSPC;
        case MtpErrc::DeviceStorageFull:
            return ENOSPC;
        case MtpErrc::FileAlreadyExists:
            return EEXIST;
        case MtpErrc::DirectoryNotEmpty:
            return ENOTEMPTY;
        case MtpErrc::NotADirectory:
            return ENOTDIR;
        case MtpErrc::IsADirectory:
            return EISDIR;
        case MtpErrc::FileTooLarge:
            return EFBIG;
        case MtpErrc::Timeout:
            return ETIMEDOUT;
        case MtpErrc::ProtocolError:
            return EIO;
        case MtpErrc::StaleHandle:
            return ESTALE;
        case MtpErrc::Cancelled:
            return ECANCELED;
        case MtpErrc::IoError:
            return EIO;
        case MtpErrc::InvalidArgument:
            return EINVAL;
        case MtpErrc::MountBackendUnavailable:
            return ENXIO;
    }
    return EIO;
}

int errnoFromException(const std::exception& e) noexcept {
    if (auto* f = dynamic_cast<const FsError*>(&e)) return f->err();
    if (auto* m = dynamic_cast<const MtpError*>(&e)) return toErrno(m->code());
    if (dynamic_cast<const std::bad_alloc*>(&e)) return ENOMEM;
    return EIO;
}

}  // namespace mtport
