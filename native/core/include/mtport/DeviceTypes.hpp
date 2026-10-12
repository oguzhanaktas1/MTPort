// Domain model shared by the MTP core, the filesystem layer and the front ends.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace mtport {

using DeviceId = std::string;  // opaque; "vvvv:pppp@bus.dev" for libmtp devices, never a raw serial
using StorageId = uint32_t;
using ObjectId = uint32_t;  // valid inside one MTP session only; 0 means "storage root"

inline constexpr ObjectId kRootObject = 0;

struct DeviceDescriptor {
    DeviceId id;
    std::string manufacturer, model;
    std::string friendlyName;  // the name the owner gave the phone (MTP DeviceFriendlyName); may be empty
    uint16_t vendorId{}, productId{};
    bool connected{};  // visible on the USB bus
    bool mtpReady{};   // an MTP session was actually opened (never inferred from USB presence)
};

struct StorageDescriptor {
    StorageId id{};
    std::string name;
    uint64_t capacityBytes{}, freeBytes{};
    bool readOnly{};
};

struct FileNode {
    StorageId storageId{};
    ObjectId objectId{}, parentObjectId{};
    std::string name;
    bool isDirectory{};
    uint64_t sizeBytes{};
    std::optional<std::chrono::system_clock::time_point> modifiedAt;
};

struct DeviceCapabilities {
    bool getPartialObject{}, sendPartialObject{}, editObjects{};
    bool moveObject{}, copyObject{}, renameObject{}, deleteObject{};
};

}  // namespace mtport
