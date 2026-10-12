// Cheap "is the phone still on the USB bus?" probe via the IOKit registry. It reads registry
// properties only and never opens the device, so it cannot disturb an active MTP session.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mtport {

struct UsbDeviceInfo {
    uint16_t vendorId{}, productId{};
    std::string vendorName, productName;  // as reported by the device's USB string descriptors (may be empty)
};
// All USB devices currently in the IOKit registry (registry read only; no device is opened).
std::vector<UsbDeviceInfo> listUsbDevices();

// Number of USB devices currently attached with this vendor/product id.
int usbDeviceCount(uint16_t vendorId, uint16_t productId);

}  // namespace mtport
