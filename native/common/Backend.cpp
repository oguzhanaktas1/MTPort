#include "Backend.hpp"

#ifdef MTPORT_HAVE_LIBMTP
#include "mtport/LibmtpDeviceFactory.hpp"
#endif
#ifdef MTPORT_HAVE_MOCK
#include "FakeMtpDevice.hpp"
#include "HostDirMtpDevice.hpp"
#endif

namespace mtport {

bool realBackendAvailable() {
#ifdef MTPORT_HAVE_LIBMTP
    return true;
#else
    return false;
#endif
}

bool mockBackendAvailable() {
#ifdef MTPORT_HAVE_MOCK
    return true;
#else
    return false;
#endif
}

std::string libmtpVersionString() {
#ifdef MTPORT_HAVE_LIBMTP
    return LibmtpDeviceFactory::libmtpVersion();
#else
    return {};
#endif
}

std::unique_ptr<IMtpDeviceFactory> makeFactory(const BackendSpec& spec) {
    if (spec.kind == "fake") {
#ifdef MTPORT_HAVE_MOCK
        if (spec.fakeRoot.empty()) throw MtpError(MtpErrc::InvalidArgument, "--backend fake needs --fake-root DIR");
        fake::HostDirMtpDevice::Options o;
        o.partialRead = spec.fakePartialRead;
        auto phone = std::make_shared<fake::HostDirMtpDevice>(spec.fakeRoot, o);
        return std::make_unique<fake::FakeMtpDeviceFactory>(phone);
#else
        throw MtpError(MtpErrc::OperationUnsupported, "this build has no mock backend");
#endif
    }
    if (spec.kind != "real") throw MtpError(MtpErrc::InvalidArgument, "unknown backend '" + spec.kind + "'");
#ifdef MTPORT_HAVE_LIBMTP
    return std::make_unique<LibmtpDeviceFactory>();
#else
    throw MtpError(MtpErrc::OperationUnsupported,
                   "this build has no libmtp backend; install libmtp (brew install libmtp libusb pkgconf) and rebuild");
#endif
}

}  // namespace mtport
