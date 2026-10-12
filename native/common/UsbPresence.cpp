#include "UsbPresence.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <initializer_list>

namespace mtport {

namespace {
bool readNumber(io_registry_entry_t e, CFStringRef key, int32_t& out) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    if (!ref) return false;
    bool ok = false;
    if (CFGetTypeID(ref) == CFNumberGetTypeID())
        ok = CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &out);
    CFRelease(ref);
    return ok;
}
}  // namespace

namespace {
std::string readString(io_registry_entry_t e, CFStringRef key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    std::string out;
    if (ref && CFGetTypeID(ref) == CFStringGetTypeID()) {
        char buf[256];
        if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof buf, kCFStringEncodingUTF8)) out = buf;
    }
    if (ref) CFRelease(ref);
    return out;
}
}  // namespace

std::vector<UsbDeviceInfo> listUsbDevices() {
    std::vector<UsbDeviceInfo> out;
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
        return out;
    for (io_registry_entry_t e; (e = IOIteratorNext(it)) != IO_OBJECT_NULL; IOObjectRelease(e)) {
        int32_t v = -1, p = -1;
        if (!readNumber(e, CFSTR("idVendor"), v) || !readNumber(e, CFSTR("idProduct"), p)) continue;
        UsbDeviceInfo d;
        d.vendorId = static_cast<uint16_t>(v);
        d.productId = static_cast<uint16_t>(p);
        d.vendorName = readString(e, CFSTR("USB Vendor Name"));
        d.productName = readString(e, CFSTR("USB Product Name"));
        out.push_back(std::move(d));
    }
    IOObjectRelease(it);
    return out;
}

int usbDeviceCount(uint16_t vendorId, uint16_t productId) {
    int count = 0;
    for (const char* cls : {"IOUSBHostDevice", "IOUSBDevice"}) {
        io_iterator_t it = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(cls), &it) != KERN_SUCCESS) continue;
        for (io_registry_entry_t e; (e = IOIteratorNext(it)) != IO_OBJECT_NULL; IOObjectRelease(e)) {
            int32_t v = -1, p = -1;
            if (readNumber(e, CFSTR("idVendor"), v) && readNumber(e, CFSTR("idProduct"), p) && v == vendorId &&
                p == productId)
                ++count;
        }
        IOObjectRelease(it);
        if (count > 0) break;  // IOUSBDevice is the legacy alias of IOUSBHostDevice; do not double count
    }
    return count;
}

}  // namespace mtport
