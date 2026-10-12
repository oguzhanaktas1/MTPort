#pragma once

#include <libmtp.h>

#include <string>
#include <vector>

#include "mtport/DeviceTypes.hpp"

namespace mtport::detail {

struct RawDevice {
    DeviceDescriptor descriptor;
    LIBMTP_raw_device_t raw;  // value copy; the vendor/product strings are not used after detection
};

// USB-level detection through libmtp. Opens no MTP session.
std::vector<RawDevice> detectRawDevices();

}  // namespace mtport::detail
