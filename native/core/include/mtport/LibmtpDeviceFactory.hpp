// Real backend: libmtp over libusb. Only compiled when libmtp was found at configure time.
#pragma once

#include "mtport/IMtpDeviceFactory.hpp"

namespace mtport {

class LibmtpDeviceFactory final : public IMtpDeviceFactory {
public:
    LibmtpDeviceFactory();
    std::vector<DeviceDescriptor> enumerate() override;
    std::unique_ptr<IMtpDevice> open(const DeviceId& id) override;

    // Linked libmtp version string (LIBMTP_VERSION_STRING at build time).
    static const char* libmtpVersion();
};

}  // namespace mtport
