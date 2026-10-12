#pragma once

#include <memory>
#include <vector>

#include "mtport/IMtpDevice.hpp"

namespace mtport {

class IMtpDeviceFactory {
public:
    virtual ~IMtpDeviceFactory() = default;

    // USB-level enumeration. Does not open MTP sessions, so it never disturbs other owners.
    virtual std::vector<DeviceDescriptor> enumerate() = 0;

    // Opens the (single) MTP session for a device. Throws DeviceBusy if another owner (in this
    // process, another MTPort process, or another application) already holds it.
    virtual std::unique_ptr<IMtpDevice> open(const DeviceId&) = 0;
};

}  // namespace mtport
