#include <new>

#include "TestHarness.hpp"
#include "mtport/PosixErrorMapper.hpp"

using namespace mtport;

AF_TEST(error_mapping, adr_table) {
    CHECK_EQ(toErrno(MtpErrc::DeviceDisconnected), ENODEV);
    CHECK_EQ(toErrno(MtpErrc::DeviceBusy), EBUSY);
    CHECK_EQ(toErrno(MtpErrc::MtpNotReady), EACCES);
    CHECK_EQ(toErrno(MtpErrc::ObjectNotFound), ENOENT);
    CHECK_EQ(toErrno(MtpErrc::InvalidVirtualPath), EINVAL);
    CHECK_EQ(toErrno(MtpErrc::OperationUnsupported), EOPNOTSUPP);
    CHECK_EQ(toErrno(MtpErrc::RemoteReadOnly), EROFS);
    CHECK_EQ(toErrno(MtpErrc::StagingDiskFull), ENOSPC);
    CHECK_EQ(toErrno(MtpErrc::DeviceStorageFull), ENOSPC);
    CHECK_EQ(toErrno(MtpErrc::FileAlreadyExists), EEXIST);
    CHECK_EQ(toErrno(MtpErrc::DirectoryNotEmpty), ENOTEMPTY);
    CHECK_EQ(toErrno(MtpErrc::Timeout), ETIMEDOUT);
    CHECK_EQ(toErrno(MtpErrc::StaleHandle), ESTALE);
}

AF_TEST(error_mapping, every_code_maps_to_a_real_errno) {
    for (int i = 0; i <= static_cast<int>(MtpErrc::MountBackendUnavailable); ++i) {
        const int e = toErrno(static_cast<MtpErrc>(i));
        CHECK(e > 0);
        CHECK(!toString(static_cast<MtpErrc>(i)).empty());
        CHECK(toString(static_cast<MtpErrc>(i)) != "Unknown");
    }
}

AF_TEST(error_mapping, exception_translation) {
    CHECK_EQ(errnoFromException(MtpError(MtpErrc::ObjectNotFound, "x")), ENOENT);
    CHECK_EQ(errnoFromException(FsError(EPERM)), EPERM);
    CHECK_EQ(errnoFromException(std::bad_alloc()), ENOMEM);
    CHECK_EQ(errnoFromException(std::runtime_error("boom")), EIO);
}
