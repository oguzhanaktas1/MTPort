#include "MountState.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Process.hpp"
#include "mtport/MtpError.hpp"
#include "mtport/RuntimePaths.hpp"
#include "mtport/UniqueFd.hpp"

namespace fs = std::filesystem;

namespace mtport {

Json MountState::toJson() const {
    Json j = Json::object();
    j["deviceId"] = deviceId;
    j["model"] = model;
    j["mountPoint"] = mountPoint;
    j["pid"] = static_cast<int64_t>(pid);
    j["readOnly"] = readOnly;
    j["phase"] = phase;
    j["error"] = error;
    j["bytesRead"] = bytesRead;
    j["bytesStaged"] = bytesStaged;
    j["bytesUploaded"] = bytesUploaded;
    j["uploadsOk"] = uploadsOk;
    j["uploadsFailed"] = uploadsFailed;
    j["uploadsActive"] = uploadsActive;
    j["uploadBytesDone"] = uploadBytesDone;
    j["uploadBytesTotal"] = uploadBytesTotal;
    j["pendingUploads"] = pendingUploads;
    j["recoverableUploads"] = recoverableUploads;
    j["capacityBytes"] = capacityBytes;
    j["freeBytes"] = freeBytes;
    j["updatedAt"] = updatedAtEpoch;
    return j;
}

MountState MountState::fromJson(const Json& j) {
    MountState s;
    auto str = [&](const char* k) {
        auto* v = j.find(k);
        return v ? v->asString() : std::string{};
    };
    auto num = [&](const char* k) {
        auto* v = j.find(k);
        return v ? static_cast<uint64_t>(v->asInt()) : uint64_t{0};
    };
    s.deviceId = str("deviceId");
    s.model = str("model");
    s.mountPoint = str("mountPoint");
    s.pid = static_cast<pid_t>(num("pid"));
    if (auto* v = j.find("readOnly")) s.readOnly = v->asBool(true);
    s.phase = str("phase");
    s.error = str("error");
    s.bytesRead = num("bytesRead");
    s.bytesStaged = num("bytesStaged");
    s.bytesUploaded = num("bytesUploaded");
    s.uploadsOk = num("uploadsOk");
    s.uploadsFailed = num("uploadsFailed");
    s.uploadsActive = num("uploadsActive");
    s.uploadBytesDone = num("uploadBytesDone");
    s.uploadBytesTotal = num("uploadBytesTotal");
    s.pendingUploads = num("pendingUploads");
    s.recoverableUploads = num("recoverableUploads");
    s.capacityBytes = num("capacityBytes");
    s.freeBytes = num("freeBytes");
    s.updatedAtEpoch = static_cast<int64_t>(num("updatedAt"));
    return s;
}

static fs::path statePath(const DeviceId& id) {
    return mountsDir() / (safeName(id) + ".json");
}

void writeMountState(const MountState& in) {
    ensurePrivateDir(mountsDir());
    MountState st = in;
    st.updatedAtEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path path = statePath(st.deviceId);
    const fs::path tmp = path.string() + ".tmp";
    {
        UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (!fd.valid()) throw MtpError(MtpErrc::IoError, "cannot write mount state");
        const std::string text = st.toJson().dump();
        if (::write(fd.get(), text.data(), text.size()) != static_cast<ssize_t>(text.size()))
            throw MtpError(MtpErrc::IoError, "cannot write mount state");
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) throw MtpError(MtpErrc::IoError, "cannot publish mount state");
}

std::optional<MountState> readMountState(const DeviceId& id) {
    std::ifstream in(statePath(id));
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return MountState::fromJson(Json::parse(ss.str(), 64 * 1024));
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<MountState> listMountStates(bool onlyLive) {
    std::vector<MountState> out;
    std::error_code ec;
    if (!fs::exists(mountsDir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(mountsDir(), ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream in(e.path());
        std::stringstream ss;
        ss << in.rdbuf();
        try {
            MountState st = MountState::fromJson(Json::parse(ss.str(), 64 * 1024));
            if (onlyLive && !processAlive(st.pid)) continue;
            out.push_back(std::move(st));
        } catch (...) {
        }
    }
    return out;
}

void removeMountState(const DeviceId& id) {
    std::error_code ec;
    fs::remove(statePath(id), ec);
}

std::vector<MountState> ejectionMarkers() {
    std::vector<MountState> out;
    for (auto& st : listMountStates(false))
        if (st.phase == "ejected") out.push_back(std::move(st));
    return out;
}

}  // namespace mtport
