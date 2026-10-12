#include "AgentCore.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "Doctor.hpp"
#include "IpcServer.hpp"
#include "mtport/RuntimePaths.hpp"
#include "mtport/UniqueFd.hpp"

namespace mtport {

namespace {

struct ProtocolError {
    std::string code, message;
};

std::string requireString(const Json& params, const char* key, size_t maxLen = 128) {
    const Json* v = params.find(key);
    if (!v || !v->isString()) throw ProtocolError{"BadRequest", std::string("missing string parameter '") + key + "'"};
    std::string s = v->asString();
    if (s.empty() || s.size() > maxLen) throw ProtocolError{"BadRequest", std::string("bad length for '") + key + "'"};
    return s;
}

Json errorResponse(const std::string& requestId, const std::string& code, const std::string& message) {
    Json r = Json::object();
    r["protocolVersion"] = kProtocolVersion;
    r["requestId"] = requestId;
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    r["error"] = std::move(e);
    return r;
}

Json transferJson(const MountState& m) {
    Json j = Json::object();
    j["deviceId"] = m.deviceId;
    j["phase"] = m.phase;
    j["bytesRead"] = m.bytesRead;
    j["bytesStaged"] = m.bytesStaged;
    j["bytesUploaded"] = m.bytesUploaded;
    j["uploadsOk"] = m.uploadsOk;
    j["uploadsFailed"] = m.uploadsFailed;
    j["uploadsActive"] = m.uploadsActive;
    j["uploadBytesDone"] = m.uploadBytesDone;
    j["uploadBytesTotal"] = m.uploadBytesTotal;
    j["pendingUploads"] = m.pendingUploads;
    j["recoverableUploads"] = m.recoverableUploads;
    j["updatedAt"] = m.updatedAtEpoch;
    return j;
}

}  // namespace

AgentCore::AgentCore(std::shared_ptr<DeviceCoordinator> coordinator, std::shared_ptr<IMountController> controller,
                     BackendSpec backend, std::filesystem::path settingsFile)
    : coordinator_(std::move(coordinator)),
      controller_(std::move(controller)),
      backend_(std::move(backend)),
      settingsFile_(std::move(settingsFile)) {
    loadSettings();
}

void AgentCore::loadSettings() {
    std::ifstream in(settingsFile_);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        Json j = Json::parse(ss.str(), 4096);
        if (auto* v = j.find("autoMount")) settings_.autoMount = v->asBool(true);
        if (auto* v = j.find("autoMountReadOnly")) settings_.autoMountReadOnly = v->asBool(false);
    } catch (...) {
        settings_ = {};
    }
}

void AgentCore::saveSettings() const {
    if (settingsFile_.empty()) return;
    ensurePrivateDir(settingsFile_.parent_path());
    Json j = Json::object();
    j["autoMount"] = settings_.autoMount;
    j["autoMountReadOnly"] = settings_.autoMountReadOnly;
    const auto tmp = settingsFile_.string() + ".tmp";
    {
        UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (!fd.valid()) return;
        const std::string text = j.dump();
        [[maybe_unused]] ssize_t n = ::write(fd.get(), text.data(), text.size());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, settingsFile_, ec);
}

AgentSettings AgentCore::settings() const {
    std::lock_guard lk(mutex_);
    return settings_;
}

std::string AgentCore::handleLine(const std::string& line) {
    std::string requestId;
    try {
        Json req = Json::parse(line, kMaxIpcMessageBytes);
        if (!req.isObject()) return errorResponse("", "BadRequest", "request must be a JSON object").dump();
        if (auto* id = req.find("requestId"); id && id->isString()) requestId = id->asString().substr(0, 64);
        const Json* ver = req.find("protocolVersion");
        if (!ver || !ver->isNumber() || ver->asInt() != kProtocolVersion)
            return errorResponse(requestId, "UnsupportedProtocol", "this agent speaks protocolVersion 1").dump();
        const Json* method = req.find("method");
        if (!method || !method->isString()) return errorResponse(requestId, "BadRequest", "missing method").dump();
        const Json* params = req.find("parameters");
        Json empty = Json::object();
        if (params && !params->isObject())
            return errorResponse(requestId, "BadRequest", "parameters must be an object").dump();

        Json result = dispatch(method->asString(), params ? *params : empty);
        Json r = Json::object();
        r["protocolVersion"] = kProtocolVersion;
        r["requestId"] = requestId;
        r["result"] = std::move(result);
        return r.dump();
    } catch (const ProtocolError& e) {
        return errorResponse(requestId, e.code, e.message).dump();
    } catch (const MtpError& e) {
        return errorResponse(requestId, std::string(toString(e.code())), e.detail()).dump();
    } catch (const std::runtime_error& e) {
        return errorResponse(requestId, "BadRequest", e.what()).dump();
    } catch (...) {
        return errorResponse(requestId, "InternalError", "unexpected failure").dump();
    }
}

Json AgentCore::dispatch(const std::string& method, const Json& params) {
    if (method == "agent.ping") {
        Json r = Json::object();
        r["pong"] = true;
        r["agentVersion"] = "0.1.0";
        r["backend"] = backend_.kind;
        return r;
    }
    if (method == "devices.list") {
        Json arr = Json::array();
        for (auto& d : coordinator_->snapshot()) arr.push(d.toJson());
        Json r = Json::object();
        r["devices"] = std::move(arr);
        return r;
    }
    if (method == "device.info") {
        const std::string id = requireString(params, "deviceId");
        auto v = coordinator_->find(id);
        if (!v) throw ProtocolError{"UnknownDevice", "no such device is attached"};
        return v->toJson();  // never opens an MTP session: the worker (or the CLI) is the only owner
    }
    if (method == "mount.start") {
        const std::string id = requireString(params, "deviceId");
        const Json* ro = params.find("readOnly");
        const bool readOnly = ro ? ro->asBool(true) : true;
        auto v = coordinator_->find(id);
        if (!v) throw ProtocolError{"UnknownDevice", "no such device is attached"};  // allowlist: only observed devices
        if (v->state != DeviceState::Detected && v->state != DeviceState::Error)
            throw ProtocolError{"BadState", std::string("cannot mount while the device is ") + toString(v->state)};
        controller_->start(id, readOnly);
        Json r = Json::object();
        r["accepted"] = true;
        return r;
    }
    if (method == "mount.stop") {
        const std::string id = requireString(params, "deviceId");
        const Json* f = params.find("force");
        controller_->stop(id, f && f->asBool(false));
        Json r = Json::object();
        r["unmounted"] = true;
        return r;
    }
    if (method == "mount.status" || method == "transfer.list") {
        Json arr = Json::array();
        std::string only;
        if (params.find("deviceId")) only = requireString(params, "deviceId");
        for (auto& m : listMountStates(true)) {
            if (!only.empty() && m.deviceId != only) continue;
            arr.push(method == "mount.status" ? m.toJson() : transferJson(m));
        }
        Json r = Json::object();
        r[method == "mount.status" ? "mounts" : "transfers"] = std::move(arr);
        return r;
    }
    if (method == "transfer.cancel")
        throw ProtocolError{"Unsupported",
                            "cancelling an in-flight MTP transfer is not supported in v0.1.0; unmount instead (staged "
                            "bytes are preserved)"};
    if (method == "app.doctor") return cli::doctorToJson(cli::runDoctor(backend_));
    if (method == "settings.get") {
        Json r = Json::object();
        r["autoMount"] = settings().autoMount;
        r["autoMountReadOnly"] = settings().autoMountReadOnly;
        return r;
    }
    if (method == "settings.set") {
        std::lock_guard lk(mutex_);
        if (auto* v = params.find("autoMount")) {
            if (!v->isBool()) throw ProtocolError{"BadRequest", "autoMount must be a boolean"};
            settings_.autoMount = v->asBool();
        }
        if (auto* v = params.find("autoMountReadOnly")) {
            if (!v->isBool()) throw ProtocolError{"BadRequest", "autoMountReadOnly must be a boolean"};
            settings_.autoMountReadOnly = v->asBool();
        }
        saveSettings();
        Json r = Json::object();
        r["autoMount"] = settings_.autoMount;
        r["autoMountReadOnly"] = settings_.autoMountReadOnly;
        return r;
    }
    throw ProtocolError{"UnknownMethod", "unknown method '" + method.substr(0, 64) + "'"};
}

namespace {
// After an Eject the phone re-enumerates on the bus for a few seconds; only after that window does "absent from the
// USB list" mean "unplugged".
constexpr int64_t kReenumerationSeconds = 15;
}

void AgentCore::autoMountTick(std::chrono::steady_clock::time_point now, int64_t nowEpoch) {
    const AgentSettings cfg = settings();
    std::set<std::string> presentUsb;
    for (auto& d : coordinator_->snapshot())
        if (!d.id.empty()) presentUsb.insert(d.id.substr(0, d.id.find('@')));
    // An ejected phone stays un-mounted for as long as it stays plugged in; the marker disappears once it has
    // really been unplugged (absent after the re-enumeration window), so the next plug-in mounts again.
    std::set<std::string> ejectedUsb;
    for (const auto& e : ejectionMarkers()) {
        if (!presentUsb.count(e.usbKey()) && nowEpoch - e.updatedAtEpoch > kReenumerationSeconds)
            removeMountState(e.deviceId);
        else
            ejectedUsb.insert(e.usbKey());
    }
    std::set<std::string> present;
    for (auto& d : coordinator_->snapshot()) {
        if (d.id.empty()) continue;
        present.insert(d.id);
        if (d.state == DeviceState::Mounted) {
            everMounted_.insert(d.id);
            continue;
        }
        if (!cfg.autoMount) continue;
        if (d.state != DeviceState::Detected && d.state != DeviceState::Error) continue;
        if (everMounted_.count(d.id)) continue;  // the user ejected it: stay out of the way until it is re-plugged
        if (ejectedUsb.count(d.id.substr(0, d.id.find('@')))) continue;  // ...even though it re-enumerated with a new bus address
        Attempts& a = autoAttempts_[d.id];
        constexpr int kMaxAttempts = 6;
        constexpr auto kBackoff = std::chrono::seconds(10);
        if (a.count >= kMaxAttempts || (a.count > 0 && now - a.last < kBackoff)) continue;
        ++a.count;
        a.last = now;
        try {
            controller_->start(d.id, cfg.autoMountReadOnly);
        } catch (...) {
            // surfaced to the user through the worker's error state; retried after the back-off
        }
    }
    for (auto it = autoAttempts_.begin(); it != autoAttempts_.end();)
        it = present.count(it->first) ? std::next(it) : autoAttempts_.erase(it);
    for (auto it = everMounted_.begin(); it != everMounted_.end();)
        it = present.count(*it) ? std::next(it) : everMounted_.erase(it);
}

}  // namespace mtport
