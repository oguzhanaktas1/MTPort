// Request dispatcher for the local control protocol (schema version 1). Transport-agnostic so it
// can be tested without sockets.
//
//   request : {"protocolVersion":1,"requestId":"<str>","method":"<name>","parameters":{...}}
//   response: {"protocolVersion":1,"requestId":"<str>","result":{...}}  |  {...,"error":{"code","message"}}
//
// Methods: devices.list, device.info, mount.start, mount.stop, mount.status, transfer.list,
//          transfer.cancel, app.doctor, settings.get, settings.set, agent.ping
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <chrono>
#include <map>
#include <set>
#include <string>

#include "Backend.hpp"
#include "DeviceCoordinator.hpp"
#include "MountSupervisor.hpp"

namespace mtport {

inline constexpr int kProtocolVersion = 1;

struct AgentSettings {
    bool autoMount{true};          // plug the phone in and it appears in Finder (user's explicit wish)
    bool autoMountReadOnly{false}; // read/write: staged writes were verified on real hardware; true = browse only
};

class AgentCore {
public:
    AgentCore(std::shared_ptr<DeviceCoordinator> coordinator, std::shared_ptr<IMountController> controller,
              BackendSpec backend, std::filesystem::path settingsFile);

    std::string handleLine(const std::string& line);  // never throws

    AgentSettings settings() const;
    // One auto-mount pass: mounts newly attached, ready phones if the setting is on. A failed attempt (e.g.
    // phone still locked) is retried every 10 s, at most 6 times per attach. A phone that was mounted and then
    // ejected is NOT remounted until it is unplugged and plugged in again.
    void autoMountTick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
                       int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());

private:
    Json dispatch(const std::string& method, const Json& params);
    void loadSettings();
    void saveSettings() const;

    std::shared_ptr<DeviceCoordinator> coordinator_;
    std::shared_ptr<IMountController> controller_;
    BackendSpec backend_;
    std::filesystem::path settingsFile_;
    mutable std::mutex mutex_;
    AgentSettings settings_;
    struct Attempts {
        int count{0};
        std::chrono::steady_clock::time_point last{};
    };
    std::map<std::string, Attempts> autoAttempts_;
    std::set<std::string> everMounted_;  // mounted at some point during this attach
};

}  // namespace mtport
