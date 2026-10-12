#include "DeviceEnumerator.hpp"

#include <cstdio>
#include <cstdlib>

#include "MtpSessionGuard.hpp"
#include "mtport/MtpError.hpp"

namespace mtport::detail {

std::vector<RawDevice> detectRawDevices() {
    ensureLibmtpInit();
    LIBMTP_raw_device_t* raw = nullptr;
    int count = 0;
    LIBMTP_error_number_t err;
    {
        StdoutToStderr quiet;
        err = LIBMTP_Detect_Raw_Devices(&raw, &count);
    }
    std::unique_ptr<LIBMTP_raw_device_t, decltype(&std::free)> guard(raw, &std::free);

    switch (err) {
        case LIBMTP_ERROR_NONE:
            break;
        case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
            return {};
        case LIBMTP_ERROR_CONNECTING:
            throw MtpError(MtpErrc::DeviceBusy, "cannot connect to the USB subsystem");
        case LIBMTP_ERROR_MEMORY_ALLOCATION:
            throw MtpError(MtpErrc::IoError, "libmtp out of memory");
        default:
            throw MtpError(MtpErrc::IoError, "libmtp device detection failed");
    }

    std::vector<RawDevice> out;
    for (int i = 0; i < count; ++i) {
        RawDevice d{};
        d.raw =
            raw[i];  // value copy; vendor/product point into libmtp's static tables, which outlive the array we free
        char id[64];
        std::snprintf(id, sizeof id, "%04x:%04x@%u.%u", raw[i].device_entry.vendor_id, raw[i].device_entry.product_id,
                      raw[i].bus_location, static_cast<unsigned>(raw[i].devnum));
        d.descriptor.id = id;
        d.descriptor.vendorId = raw[i].device_entry.vendor_id;
        d.descriptor.productId = raw[i].device_entry.product_id;
        if (raw[i].device_entry.vendor) d.descriptor.manufacturer = raw[i].device_entry.vendor;
        if (raw[i].device_entry.product) d.descriptor.model = raw[i].device_entry.product;
        d.descriptor.connected = true;
        d.descriptor.mtpReady = false;  // unknown until a session is actually opened
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace mtport::detail
