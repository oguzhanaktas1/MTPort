#include "mtport/LibmtpDeviceFactory.hpp"

#include "DeviceEnumerator.hpp"
#include "LibmtpDevice.hpp"
#include "MtpSessionGuard.hpp"

namespace mtport {

LibmtpDeviceFactory::LibmtpDeviceFactory() {
    detail::ensureLibmtpInit();
}

const char* LibmtpDeviceFactory::libmtpVersion() {
    return LIBMTP_VERSION_STRING;
}

std::vector<DeviceDescriptor> LibmtpDeviceFactory::enumerate() {
    std::vector<DeviceDescriptor> out;
    for (auto& d : detail::detectRawDevices()) out.push_back(std::move(d.descriptor));
    return out;
}

std::unique_ptr<IMtpDevice> LibmtpDeviceFactory::open(const DeviceId& id) {
    auto devices = detail::detectRawDevices();
    for (auto& d : devices) {
        if (d.descriptor.id != id) continue;
        // Take the cross-process ownership lock *before* touching USB so that two MTPort
        // processes never fight over one phone.
        DeviceLock lock = DeviceLock::acquire(id);
        detail::SessionPtr session;
        // By default libmtp describes each object with MTP GetObjPropList. A Galaxy needs ~20 ms per object for it but
        // ~5 ms for the mandatory GetObjectInfo, with identical results (compared on all 1,856 objects of DCIM/Camera:
        // 0 differences; sizes still come from the 64-bit ObjectSize property, so files over 4 GiB stay exact). It is
        // the very path libmtp uses for every device it flags as an Android MTP implementation.
        constexpr uint32_t kFlagBrokenGetObjPropList = 0x00000004;  // DEVICE_FLAG_BROKEN_MTPGETOBJPROPLIST (libmtp)
        d.raw.device_entry.device_flags |= kFlagBrokenGetObjPropList;
        {
            detail::StdoutToStderr quiet;
            session.reset(LIBMTP_Open_Raw_Device_Uncached(&d.raw));
        }
        if (!session) {
            throw MtpError(MtpErrc::DeviceBusy,
                           "could not open an MTP session. Unlock the phone and choose USB > File transfer (MTP); "
                           "close Image Capture / Android File Transfer / other MTP apps; on macOS the system "
                           "'ptpcamerad' helper can also hold the interface");
        }
        return std::make_unique<LibmtpDevice>(std::move(session), d.descriptor, std::move(lock));
    }
    throw MtpError(MtpErrc::DeviceDisconnected, "no such USB device: " + id);
}

}  // namespace mtport
