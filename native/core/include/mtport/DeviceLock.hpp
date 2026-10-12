// Cross-process "one MTP session owner per physical phone" lock (flock on a private file).
#pragma once

#include <string>

#include "mtport/DeviceTypes.hpp"
#include "mtport/UniqueFd.hpp"

namespace mtport {

class DeviceLock {
public:
    // Throws MtpError{DeviceBusy} if another process (or another DeviceLock in this process)
    // already holds the lock for `id`.
    static DeviceLock acquire(const DeviceId& id);

    // Non-throwing probe used by `status`: true when someone currently owns the device.
    static bool isHeld(const DeviceId& id);

    DeviceLock() = default;
    DeviceLock(DeviceLock&&) noexcept = default;
    DeviceLock& operator=(DeviceLock&&) noexcept = default;

private:
    explicit DeviceLock(UniqueFd fd) : fd_(std::move(fd)) {}
    UniqueFd fd_;  // closing the descriptor releases the flock
};

}  // namespace mtport
