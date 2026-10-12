// Typed errors. The whole code base uses exceptions derived from MtpError for device-level
// failures; the filesystem layer converts them into errno values at its boundary.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace mtport {

enum class MtpErrc {
    DeviceDisconnected,
    DeviceBusy,
    MtpNotReady,  // locked / charging only / access denied
    ObjectNotFound,
    InvalidVirtualPath,
    OperationUnsupported,
    RemoteReadOnly,
    StagingDiskFull,
    DeviceStorageFull,
    FileAlreadyExists,
    DirectoryNotEmpty,
    NotADirectory,
    IsADirectory,
    FileTooLarge,
    Timeout,
    ProtocolError,
    StaleHandle,
    Cancelled,
    IoError,
    InvalidArgument,
    MountBackendUnavailable,  // macFUSE/FSKit missing, too old, or not approved
};

constexpr std::string_view toString(MtpErrc c) {
    switch (c) {
        case MtpErrc::DeviceDisconnected:
            return "DeviceDisconnected";
        case MtpErrc::DeviceBusy:
            return "DeviceBusy";
        case MtpErrc::MtpNotReady:
            return "MtpNotReady";
        case MtpErrc::ObjectNotFound:
            return "ObjectNotFound";
        case MtpErrc::InvalidVirtualPath:
            return "InvalidVirtualPath";
        case MtpErrc::OperationUnsupported:
            return "OperationUnsupported";
        case MtpErrc::RemoteReadOnly:
            return "RemoteReadOnly";
        case MtpErrc::StagingDiskFull:
            return "StagingDiskFull";
        case MtpErrc::DeviceStorageFull:
            return "DeviceStorageFull";
        case MtpErrc::FileAlreadyExists:
            return "FileAlreadyExists";
        case MtpErrc::DirectoryNotEmpty:
            return "DirectoryNotEmpty";
        case MtpErrc::NotADirectory:
            return "NotADirectory";
        case MtpErrc::IsADirectory:
            return "IsADirectory";
        case MtpErrc::FileTooLarge:
            return "FileTooLarge";
        case MtpErrc::Timeout:
            return "Timeout";
        case MtpErrc::ProtocolError:
            return "ProtocolError";
        case MtpErrc::StaleHandle:
            return "StaleHandle";
        case MtpErrc::Cancelled:
            return "Cancelled";
        case MtpErrc::IoError:
            return "IoError";
        case MtpErrc::InvalidArgument:
            return "InvalidArgument";
        case MtpErrc::MountBackendUnavailable:
            return "MountBackendUnavailable";
    }
    return "Unknown";
}

class MtpError : public std::runtime_error {
public:
    MtpError(MtpErrc code, std::string message)
        : std::runtime_error(std::string(toString(code)) + ": " + message), code_(code), detail_(std::move(message)) {}

    MtpErrc code() const noexcept { return code_; }
    const std::string& detail() const noexcept { return detail_; }

private:
    MtpErrc code_;
    std::string detail_;
};

}  // namespace mtport
