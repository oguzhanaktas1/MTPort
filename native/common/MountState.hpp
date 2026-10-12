// Per-mount state file written by mtport-mount and read by CLI/agent/app.
// Lives in ~/Library/Application Support/MTPort/runtime/mounts/<safe-device-id>.json (0600).
#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

#include "Json.hpp"
#include "mtport/DeviceTypes.hpp"

namespace mtport {

struct MountState {
    DeviceId deviceId;
    std::string model;
    std::string mountPoint;
    pid_t pid{};
    bool readOnly{true};
    std::string phase;  // "starting" | "mounted" | "draining" | "error"
    std::string error;  // human-readable, no file names
    // transfer counters (bytes), written about once a second
    uint64_t bytesRead{}, bytesStaged{}, bytesUploaded{};
    uint64_t uploadsOk{}, uploadsFailed{}, uploadsActive{}, uploadBytesDone{}, uploadBytesTotal{};
    uint64_t pendingUploads{}, recoverableUploads{};
    uint64_t capacityBytes{}, freeBytes{};  // 0 when the phone does not report them
    int64_t updatedAtEpoch{};

    // "vvvv:pppp" part of the device id (the bus address after '@' changes whenever the phone re-enumerates).
    std::string usbKey() const { return deviceId.substr(0, deviceId.find('@')); }

    Json toJson() const;
    static MountState fromJson(const Json&);
};

void writeMountState(const MountState& st);  // atomic replace, mode 0600
std::optional<MountState> readMountState(const DeviceId& id);
std::vector<MountState> listMountStates(bool onlyLive);  // onlyLive: pid still running
void removeMountState(const DeviceId& id);
// Markers left by workers that ended through a deliberate unmount (phase "ejected").
std::vector<MountState> ejectionMarkers();

}  // namespace mtport
