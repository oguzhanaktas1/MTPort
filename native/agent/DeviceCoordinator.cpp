#include "DeviceCoordinator.hpp"

#include <algorithm>
#include <condition_variable>
#include <map>

namespace mtport {

const char* toString(DeviceState s) {
    switch (s) {
        case DeviceState::Detected:
            return "detected";
        case DeviceState::AwaitingMtp:
            return "awaitingMtp";
        case DeviceState::Connecting:
            return "connecting";
        case DeviceState::Mounted:
            return "mounted";
        case DeviceState::Draining:
            return "draining";
        case DeviceState::Error:
            return "error";
    }
    return "?";
}

Json DeviceView::toJson() const {
    Json j = Json::object();
    j["key"] = key;
    j["id"] = id;
    j["name"] = name;
    j["usbId"] = usbId;
    j["state"] = toString(state);
    j["detail"] = detail;
    j["mountPoint"] = mountPoint;
    j["readOnly"] = readOnly;
    j["capacityBytes"] = capacityBytes;
    j["freeBytes"] = freeBytes;
    return j;
}

bool DeviceCoordinator::isAndroidVendor(uint16_t vid) {
    static const uint16_t kVendors[] = {0x18d1, 0x04e8, 0x2717, 0x12d1, 0x2a70, 0x22b8, 0x0fce, 0x1004, 0x0bb4, 0x22d9,
                                        0x0b05, 0x2d95, 0x19d2, 0x0e8d, 0x05c6, 0x1bbb, 0x2b0e, 0x1782, 0x2916, 0x0489};
    return std::find(std::begin(kVendors), std::end(kVendors), vid) != std::end(kVendors);
}

DeviceCoordinator::DeviceCoordinator(std::shared_ptr<IMtpDeviceFactory> factory, UsbLister usb, MountLister mounts,
                                     int debounceCycles)
    : factory_(std::move(factory)),
      usb_(std::move(usb)),
      mounts_(std::move(mounts)),
      debounce_(std::max(1, debounceCycles)) {}

DeviceCoordinator::~DeviceCoordinator() {
    stopPolling();
}

namespace {
std::string usbIdOf(uint16_t v, uint16_t p) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04x:%04x", v, p);
    return buf;
}
}  // namespace

void DeviceCoordinator::refresh() {
    std::map<std::string, DeviceView> fresh;
    std::vector<DeviceDescriptor> mtp;
    try {
        mtp = factory_->enumerate();
    } catch (const std::exception&) {
        // detection hiccup: keep the previous picture for this cycle (counts as a miss below)
        std::lock_guard lk(mutex_);
        for (auto& [k, c] : candidates_) ++c.missed;
        return;
    }
    const auto usb = usb_ ? usb_() : std::vector<UsbDeviceInfo>{};
    const auto mounts = mounts_ ? mounts_() : std::vector<MountState>{};

    for (const auto& d : mtp) {
        DeviceView v;
        v.key = d.id;
        v.id = d.id;
        v.name = (d.manufacturer.empty() ? "" : d.manufacturer + " ") + (d.model.empty() ? "Android device" : d.model);
        v.usbId = usbIdOf(d.vendorId, d.productId);
        v.state = DeviceState::Detected;
        v.detail = "MTP interface present. Ready to mount.";
        fresh[v.key] = v;
    }
    // Phones on the bus that expose no MTP interface: charging-only / locked / MTP not selected.
    for (const auto& u : usb) {
        if (!isAndroidVendor(u.vendorId)) continue;
        const std::string uid = usbIdOf(u.vendorId, u.productId);
        const bool listed = std::any_of(mtp.begin(), mtp.end(), [&](const DeviceDescriptor& d) {
            return d.vendorId == u.vendorId && d.productId == u.productId;
        });
        if (listed) continue;
        DeviceView v;
        v.key = "usb:" + uid;
        v.name = u.productName.empty() ? "Android phone" : u.productName;
        v.usbId = uid;
        v.state = DeviceState::AwaitingMtp;
        v.detail = "Phone detected but it exposes no MTP interface. Unlock it and choose USB > File transfer.";
        fresh[v.key] = v;
    }
    // Overlay what the mount workers report; these are authoritative and applied immediately.
    std::map<std::string, bool> immediate;
    for (const auto& m : mounts) {
        DeviceView& v = fresh[m.deviceId];
        if (v.key.empty()) {
            v.key = m.deviceId;
            v.id = m.deviceId;
            v.name = m.model.empty() ? "Android device" : m.model;
        }
        v.mountPoint = m.mountPoint;
        v.readOnly = m.readOnly;
        v.capacityBytes = m.capacityBytes;
        v.freeBytes = m.freeBytes;
        if (m.phase == "mounted") {
            v.state = DeviceState::Mounted;
            v.detail = "Mounted in Finder.";
        } else if (m.phase == "draining") {
            v.state = DeviceState::Draining;
            v.detail = m.error.empty() ? "Ejecting..." : m.error;
        } else if (m.phase == "starting") {
            v.state = DeviceState::Connecting;
            v.detail = "Opening the MTP session...";
        } else if (m.phase == "error") {
            v.state = DeviceState::Error;
            v.detail = m.error;
        }
        immediate[v.key] = true;
    }

    std::lock_guard lk(mutex_);
    for (auto& [key, view] : fresh) {
        Slot& s = candidates_[key];
        if (s.view.state == view.state && s.seen > 0)
            ++s.seen;
        else
            s.seen = 1;
        s.view = view;
        s.missed = 0;
        if (immediate.count(key)) s.seen = std::max(s.seen, debounce_);
    }
    for (auto it = candidates_.begin(); it != candidates_.end();) {
        if (!fresh.count(it->first)) {
            ++it->second.missed;
            if (it->second.missed >= debounce_) {
                it = candidates_.erase(it);
                continue;
            }
        }
        ++it;
    }
    published_.clear();
    for (auto& [key, s] : candidates_)
        if (s.seen >= debounce_) published_.push_back(s.view);
}

void DeviceCoordinator::startPolling(std::chrono::milliseconds interval) {
    if (polling_.exchange(true)) return;
    thread_ = std::thread([this, interval] {
        while (polling_) {
            refresh();
            std::unique_lock lk(wakeMutex_);
            wake_.wait_for(lk, interval, [this] { return !polling_.load(); });
        }
    });
}

void DeviceCoordinator::stopPolling() {
    if (!polling_.exchange(false)) return;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::vector<DeviceView> DeviceCoordinator::snapshot() const {
    std::lock_guard lk(mutex_);
    return published_;
}

std::optional<DeviceView> DeviceCoordinator::find(const DeviceId& id) const {
    std::lock_guard lk(mutex_);
    for (auto& v : published_)
        if (!v.id.empty() && v.id == id) return v;
    return std::nullopt;
}

bool DeviceCoordinator::known(const DeviceId& id) const {
    return find(id).has_value();
}

}  // namespace mtport
