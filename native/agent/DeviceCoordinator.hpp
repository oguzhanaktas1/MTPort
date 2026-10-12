// Turns USB/MTP observations plus mount-worker state files into the device lifecycle:
// Disconnected -> Detected / AwaitingMTP -> Connecting -> Mounted -> Draining, with Error as a side state.
// A USB attach is only a hint: states are derived from what libmtp lists and what workers report,
// and are debounced so a flapping cable does not make the UI (or an auto-mount) thrash.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "MountState.hpp"
#include "UsbPresence.hpp"
#include "mtport/IMtpDeviceFactory.hpp"

namespace mtport {

enum class DeviceState { Detected, AwaitingMtp, Connecting, Mounted, Draining, Error };
const char* toString(DeviceState s);

struct DeviceView {
    std::string key;  // stable within one attach: device id, or "usb:vvvv:pppp" for non-MTP phones
    DeviceId id;      // empty for AwaitingMtp (no MTP interface to address yet)
    std::string name;
    std::string usbId;
    DeviceState state{DeviceState::Detected};
    std::string detail;  // English guidance; the UI localises by state code
    std::string mountPoint;
    bool readOnly{true};
    uint64_t capacityBytes{}, freeBytes{};

    Json toJson() const;
};

class DeviceCoordinator {
public:
    using UsbLister = std::function<std::vector<UsbDeviceInfo>()>;
    using MountLister = std::function<std::vector<MountState>()>;

    DeviceCoordinator(
        std::shared_ptr<IMtpDeviceFactory> factory, UsbLister usb = listUsbDevices,
        MountLister mounts = [] { return listMountStates(true); }, int debounceCycles = 2);
    ~DeviceCoordinator();

    void refresh();  // one observation cycle (the poller calls this; tests call it directly)
    void startPolling(std::chrono::milliseconds interval);
    void stopPolling();

    std::vector<DeviceView> snapshot() const;
    std::optional<DeviceView> find(const DeviceId& id) const;
    bool known(const DeviceId& id) const;  // allowlist for IPC: only devices we actually observed

    // Vendor ids of common Android phones; used only to tell "phone in charging mode" from "nothing plugged in".
    static bool isAndroidVendor(uint16_t vid);

private:
    struct Slot {
        DeviceView view;
        int seen{0};
        int missed{0};
    };
    std::shared_ptr<IMtpDeviceFactory> factory_;
    UsbLister usb_;
    MountLister mounts_;
    int debounce_;
    mutable std::mutex mutex_;
    std::vector<DeviceView> published_;
    std::map<std::string, Slot> candidates_;
    std::atomic<bool> polling_{false};
    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
};

}  // namespace mtport
