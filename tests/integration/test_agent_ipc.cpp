#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include "AgentCore.hpp"
#include "IpcServer.hpp"
#include "Json.hpp"
#include "TestHarness.hpp"
#include "TestSupport.hpp"

using namespace mtport;
using namespace mtport::fake;

namespace {

struct RecordingController final : IMountController {
    std::vector<std::string> started, stopped;
    std::vector<bool> startedReadOnly, stopForce;
    bool failStart{false};
    void start(const DeviceId& id, bool ro) override {
        if (failStart) throw MtpError(MtpErrc::MtpNotReady, "no worker");
        started.push_back(id);
        startedReadOnly.push_back(ro);
    }
    void stop(const DeviceId& id, bool force) override {
        stopped.push_back(id);
        stopForce.push_back(force);
    }
};

struct Rig {
    aftest::TempDir tmp;
    std::shared_ptr<FakeMtpDevice> phone = std::make_shared<FakeMtpDevice>();
    std::shared_ptr<FakeMtpDeviceFactory> factory = std::make_shared<FakeMtpDeviceFactory>(phone);
    std::vector<UsbDeviceInfo> usb;
    std::vector<MountState> mounts;
    std::shared_ptr<DeviceCoordinator> coord;
    std::shared_ptr<RecordingController> ctl = std::make_shared<RecordingController>();
    std::unique_ptr<AgentCore> core;

    explicit Rig(int debounce = 2) {
        ::setenv("MTPORT_HOME", tmp.path.c_str(), 1);
        coord =
            std::make_shared<DeviceCoordinator>(factory, [this] { return usb; }, [this] { return mounts; }, debounce);
        core = std::make_unique<AgentCore>(coord, ctl, BackendSpec{"fake", {}, true}, tmp.path / "settings.json");
    }
    ~Rig() { ::unsetenv("MTPORT_HOME"); }

    Json call(const std::string& method, Json params = Json::object()) {
        Json req = Json::object();
        req["protocolVersion"] = 1;
        req["requestId"] = "t1";
        req["method"] = method;
        req["parameters"] = std::move(params);
        return Json::parse(core->handleLine(req.dump()));
    }
    static std::string errCode(const Json& r) {
        return r.find("error") ? r.find("error")->find("code")->asString() : "";
    }
    void settle() {
        coord->refresh();
        coord->refresh();
    }
};

}  // namespace

AF_TEST(json, round_trip_and_limits) {
    Json j = Json::object();
    j["s"] = "a\"b\\c\n\t\x01 \xc3\xbc";
    j["n"] = int64_t{-5};
    j["f"] = 1.5;
    j["b"] = true;
    j["z"] = Json();
    j["a"] = Json(Json::Array{Json(1), Json("x"), Json::object()});
    const Json back = Json::parse(j.dump());
    CHECK_EQ(back.dump(), j.dump());
    CHECK_EQ(back.find("s")->asString(), j.find("s")->asString());
    CHECK_EQ(back.find("n")->asInt(), int64_t{-5});
    CHECK(Json::parse("{\"u\":\"\\u00e9\"}").find("u")->asString() == "\xc3\xa9");
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]x", "nul", "{\"a\":1} trailing", "\"unterminated"})
        CHECK_THROWS_BAD(bad);
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK_THROWS_BAD(deep.c_str());                       // nesting limit
    CHECK_THROWS_BAD(std::string(2000000, ' ').c_str());  // size limit (default 1 MiB)
}

AF_TEST(agent_core, protocol_validation) {
    Rig r;
    auto parse = [&](const std::string& line) { return Json::parse(r.core->handleLine(line)); };
    CHECK_EQ(Rig::errCode(parse("not json")), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(parse("[]")), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(parse("{\"method\":\"devices.list\"}")), std::string("UnsupportedProtocol"));
    CHECK_EQ(Rig::errCode(parse("{\"protocolVersion\":2,\"method\":\"devices.list\"}")),
             std::string("UnsupportedProtocol"));
    CHECK_EQ(Rig::errCode(parse("{\"protocolVersion\":1}")), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(parse("{\"protocolVersion\":1,\"method\":\"nope\"}")), std::string("UnknownMethod"));
    CHECK_EQ(Rig::errCode(parse("{\"protocolVersion\":1,\"method\":\"devices.list\",\"parameters\":3}")),
             std::string("BadRequest"));
    auto ok = parse("{\"protocolVersion\":1,\"requestId\":\"abc\",\"method\":\"agent.ping\"}");
    CHECK_EQ(ok.find("requestId")->asString(), std::string("abc"));
    CHECK(ok.find("result")->find("pong")->asBool());
    // requestId is echoed but bounded
    auto long_id =
        parse("{\"protocolVersion\":1,\"requestId\":\"" + std::string(500, 'x') + "\",\"method\":\"agent.ping\"}");
    CHECK(long_id.find("requestId")->asString().size() <= 64);
}

AF_TEST(agent_core, device_states_from_observations) {
    Rig r;
    r.factory->setPresent(false);  // nothing speaks MTP yet
    r.settle();
    r.usb.push_back({0x18d1, 0x4ee2, "Google", "Pixel 8 (charging)"});  // Android vendor, no MTP interface
    r.coord->refresh();
    CHECK(r.coord->snapshot().empty());  // debounced: one sighting is not enough
    r.coord->refresh();
    auto snap = r.coord->snapshot();
    CHECK_EQ(snap.size(), size_t{1});
    CHECK_EQ(snap[0].state, DeviceState::AwaitingMtp);
    CHECK(snap[0].id.empty());  // nothing to mount yet
    CHECK(snap[0].detail.find("File transfer") != std::string::npos);

    // Non-Android USB gear (a mouse) is ignored.
    r.usb.push_back({0x046d, 0xc077, "Logitech", "Mouse"});
    r.settle();
    CHECK_EQ(r.coord->snapshot().size(), size_t{1});

    // The user selects File transfer: the phone now shows up in libmtp's list.
    r.usb.clear();
    r.factory->setPresent(true);
    r.coord->refresh();
    for (const auto& v : r.coord->snapshot()) CHECK(v.state != DeviceState::Detected);  // not yet debounced
    r.coord->refresh();
    auto snap2 = r.coord->snapshot();
    CHECK_EQ(snap2.size(), size_t{1});
    CHECK_EQ(snap2[0].state, DeviceState::Detected);
    CHECK_EQ(snap2[0].id, r.phone->descriptor().id);
    CHECK(r.coord->known(r.phone->descriptor().id));

    // Unplug: removed after the debounce window, not instantly (cable jiggle tolerance).
    r.factory->setPresent(false);
    r.coord->refresh();
    CHECK_EQ(r.coord->snapshot().size(), size_t{1});
    r.coord->refresh();
    CHECK(r.coord->snapshot().empty());
    CHECK(!r.coord->known(r.phone->descriptor().id));
}

AF_TEST(agent_core, mount_worker_state_overlays_are_immediate) {
    Rig r;
    r.settle();
    MountState m;
    m.deviceId = r.phone->descriptor().id;
    m.model = "Fake Phone";
    m.mountPoint = "/Volumes/MTPort-Fake-Phone";
    m.phase = "mounted";
    m.readOnly = true;
    m.capacityBytes = 100;
    m.freeBytes = 40;
    r.mounts.push_back(m);
    r.coord->refresh();  // authoritative worker state needs no debounce
    auto v = r.coord->find(m.deviceId);
    CHECK(v.has_value());
    CHECK_EQ(v->state, DeviceState::Mounted);
    CHECK_EQ(v->mountPoint, m.mountPoint);
    CHECK_EQ(v->capacityBytes, uint64_t{100});
    r.mounts[0].phase = "draining";
    r.coord->refresh();
    CHECK_EQ(r.coord->find(m.deviceId)->state, DeviceState::Draining);
    r.mounts[0].phase = "error";
    r.mounts[0].error = "DeviceBusy: another app owns it";
    r.coord->refresh();
    CHECK_EQ(r.coord->find(m.deviceId)->state, DeviceState::Error);
    CHECK_EQ(r.coord->find(m.deviceId)->detail, std::string("DeviceBusy: another app owns it"));
}

AF_TEST(agent_core, mount_commands_are_allowlisted_and_state_checked) {
    Rig r;
    r.settle();
    const std::string id = r.phone->descriptor().id;
    // Unknown id (not enumerated) is rejected before any process is started.
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json("0000:0000@9.9")}}))),
             std::string("UnknownDevice"));
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json("../../etc/passwd")}}))),
             std::string("UnknownDevice"));
    CHECK_EQ(Rig::errCode(r.call("mount.start")), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json(5)}}))), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json(std::string(500, 'a'))}}))),
             std::string("BadRequest"));
    CHECK(r.ctl->started.empty());

    auto ok = r.call("mount.start", Json(Json::Object{{"deviceId", Json(id)}}));
    CHECK(ok.find("result") != nullptr);
    CHECK_EQ(r.ctl->started.size(), size_t{1});
    CHECK(r.ctl->startedReadOnly[0]);  // read-only unless explicitly asked otherwise
    r.call("mount.start", Json(Json::Object{{"deviceId", Json(id)}, {"readOnly", Json(false)}}));
    CHECK(!r.ctl->startedReadOnly[1]);

    // A device that is already mounted cannot be mounted again.
    MountState m;
    m.deviceId = id;
    m.phase = "mounted";
    m.mountPoint = "/Volumes/x";
    r.mounts.push_back(m);
    r.coord->refresh();
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json(id)}}))), std::string("BadState"));

    r.call("mount.stop", Json(Json::Object{{"deviceId", Json(id)}, {"force", Json(true)}}));
    CHECK_EQ(r.ctl->stopped.size(), size_t{1});
    CHECK(r.ctl->stopForce[0]);
    r.ctl->failStart = true;
    r.mounts.clear();
    r.settle();
    CHECK_EQ(Rig::errCode(r.call("mount.start", Json(Json::Object{{"deviceId", Json(id)}}))),
             std::string("MtpNotReady"));
}

AF_TEST(agent_core, transfer_and_misc_methods) {
    Rig r;
    CHECK_EQ(Rig::errCode(r.call("transfer.cancel")), std::string("Unsupported"));  // honest: no safe cancel in v0.1.0
    MountState m;
    m.deviceId = "fake:0001@1.1";
    m.pid = ::getpid();  // "live" worker
    m.phase = "mounted";
    m.uploadsOk = 3;
    m.bytesUploaded = 4242;
    writeMountState(m);
    auto t = r.call("transfer.list");
    CHECK_EQ(t.find("result")->find("transfers")->items().size(), size_t{1});
    CHECK_EQ(t.find("result")->find("transfers")->items()[0].find("bytesUploaded")->asInt(), int64_t{4242});
    auto s = r.call("mount.status", Json(Json::Object{{"deviceId", Json("nope")}}));
    CHECK(s.find("result")->find("mounts")->items().empty());
    auto d = r.call("app.doctor");
    CHECK(d.find("result")->find("checks") != nullptr);
}

AF_TEST(agent_core, plugging_in_mounts_the_phone_by_default) {
    Rig r;
    CHECK(r.call("settings.get").find("result")->find("autoMount")->asBool());   // user's wish: plug in -> Finder
    r.settle();
    const auto t0 = std::chrono::steady_clock::now();
    r.core->autoMountTick(t0);
    CHECK_EQ(r.ctl->started.size(), size_t{1});
    CHECK(!r.ctl->startedReadOnly[0]);                      // read/write (verified on real hardware)
    r.core->autoMountTick(t0 + std::chrono::seconds(1));    // worker is starting: no second request inside the back-off
    CHECK_EQ(r.ctl->started.size(), size_t{1});
}

AF_TEST(agent_core, failed_mount_is_retried_with_backoff_and_a_limit) {
    // e.g. the phone is still locked: the worker fails, the state becomes "error", the agent tries again later.
    Rig r;
    r.settle();
    MountState m;
    m.deviceId = r.phone->descriptor().id;
    m.phase = "error";
    m.error = "MtpNotReady: unlock the phone";
    r.mounts.push_back(m);
    r.coord->refresh();
    auto t = std::chrono::steady_clock::now();
    r.core->autoMountTick(t);
    CHECK_EQ(r.ctl->started.size(), size_t{1});
    r.core->autoMountTick(t + std::chrono::seconds(5));
    CHECK_EQ(r.ctl->started.size(), size_t{1});             // too soon
    r.core->autoMountTick(t + std::chrono::seconds(11));
    CHECK_EQ(r.ctl->started.size(), size_t{2});
    for (int i = 2; i < 20; ++i) r.core->autoMountTick(t + std::chrono::seconds(11 * i));
    CHECK_EQ(r.ctl->started.size(), size_t{6});             // gives up after 6 attempts per attach
}

AF_TEST(agent_core, ejected_phone_is_not_remounted_until_replugged) {
    Rig r;
    r.settle();
    MountState m;
    m.deviceId = r.phone->descriptor().id;
    m.phase = "mounted";
    m.mountPoint = "/Volumes/x";
    r.mounts.push_back(m);
    r.coord->refresh();
    auto t = std::chrono::steady_clock::now();
    r.core->autoMountTick(t);                                // it is mounted
    r.mounts.clear();                                        // user pressed Eject, the cable stays in
    r.settle();
    r.core->autoMountTick(t + std::chrono::seconds(60));
    CHECK(r.ctl->started.empty());
    r.factory->setPresent(false);                            // unplugged...
    r.settle();
    r.core->autoMountTick(t + std::chrono::seconds(70));
    r.factory->setPresent(true);                             // ...and plugged in again
    r.settle();
    r.core->autoMountTick(t + std::chrono::seconds(80));
    CHECK_EQ(r.ctl->started.size(), size_t{1});
}

AF_TEST(agent_core, eject_marker_holds_until_the_phone_is_really_unplugged) {
    Rig r;
    r.settle();
    // A deliberate unmount leaves an "ejected" marker. The phone re-enumerates (new bus address "@1.1" -> "@1.7")
    // and stays plugged in: no remount, however long that takes.
    MountState e;
    e.deviceId = "fake:0001@9.9";
    e.phase = "ejected";
    writeMountState(e);
    const auto t = std::chrono::steady_clock::now();
    r.core->autoMountTick(t, 0);
    CHECK(r.ctl->started.empty());
    r.core->autoMountTick(t + std::chrono::seconds(3600), INT64_MAX / 2);   // an hour later, still plugged in
    CHECK(r.ctl->started.empty());
    CHECK_EQ(ejectionMarkers().size(), size_t{1});

    // Unplugged for real (absent, and past the re-enumeration window): the marker is dropped...
    r.factory->setPresent(false);
    r.settle();
    r.core->autoMountTick(t + std::chrono::seconds(3601), INT64_MAX / 2);
    CHECK_EQ(ejectionMarkers().size(), size_t{0});
    // ...so the next plug-in mounts.
    r.factory->setPresent(true);
    r.settle();
    r.core->autoMountTick(t + std::chrono::seconds(3700), INT64_MAX / 2);
    CHECK_EQ(r.ctl->started.size(), size_t{1});
}

AF_TEST(agent_core, absent_right_after_an_eject_is_just_re_enumeration) {
    Rig r;
    r.settle();
    MountState e;
    e.deviceId = "fake:0001@9.9";
    e.phase = "ejected";
    writeMountState(e);
    r.factory->setPresent(false);   // gone for a moment, as happens while the phone re-enumerates
    r.settle();
    const auto t = std::chrono::steady_clock::now();
    const int64_t written = ejectionMarkers().at(0).updatedAtEpoch;
    r.core->autoMountTick(t, written + 2);
    CHECK_EQ(ejectionMarkers().size(), size_t{1});   // 2 s after the eject: not yet considered unplugged
}

AF_TEST(agent_core, auto_mount_can_be_switched_off_or_made_read_only_and_persists) {
    Rig r;
    r.call("settings.set", Json(Json::Object{{"autoMount", Json(false)}}));
    r.settle();
    r.core->autoMountTick();
    CHECK(r.ctl->started.empty());
    CHECK_EQ(Rig::errCode(r.call("settings.set", Json(Json::Object{{"autoMount", Json("yes")}}))), std::string("BadRequest"));
    CHECK_EQ(Rig::errCode(r.call("settings.set", Json(Json::Object{{"autoMountReadOnly", Json(1)}}))), std::string("BadRequest"));

    r.call("settings.set", Json(Json::Object{{"autoMount", Json(true)}, {"autoMountReadOnly", Json(true)}}));
    r.core->autoMountTick();
    CHECK_EQ(r.ctl->started.size(), size_t{1});
    CHECK(r.ctl->startedReadOnly[0]);

    // Persisted with owner-only permissions and reloaded by a fresh core.
    struct stat st{};
    CHECK_EQ(::stat((r.tmp.path / "settings.json").c_str(), &st), 0);
    CHECK_EQ(st.st_mode & 0777, mode_t{0600});
    AgentCore again(r.coord, r.ctl, BackendSpec{"fake", {}, true}, r.tmp.path / "settings.json");
    CHECK(again.settings().autoMount);
    CHECK(again.settings().autoMountReadOnly);
}

AF_TEST(agent_ipc, socket_permissions_roundtrip_and_limits) {
    // Unix socket paths are limited to ~104 bytes: use a short private directory.
    char tmpl[] = "/tmp/afipc.XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const std::filesystem::path sock = std::filesystem::path(dir) / "run" / "agent.sock";
    {
        IpcServer server(sock,
                         [](const std::string& line) { return "{\"echo\":" + std::to_string(line.size()) + "}"; });
        server.start();
        struct stat st{};
        CHECK_EQ(::stat(sock.c_str(), &st), 0);
        CHECK(S_ISSOCK(st.st_mode));
        CHECK_EQ(st.st_mode & 0777, mode_t{0600});
        CHECK_EQ(::stat(sock.parent_path().c_str(), &st), 0);
        CHECK_EQ(st.st_mode & 0777, mode_t{0700});

        CHECK_EQ(ipcRoundTrip(sock, "hello"), std::string("{\"echo\":5}"));
        CHECK_EQ(ipcRoundTrip(sock, "x"), std::string("{\"echo\":1}"));  // sequential clients
        // Oversized request: rejected by the server, handler never sees it.
        const auto big = ipcRoundTrip(sock, std::string(kMaxIpcMessageBytes + 100, 'a'));
        CHECK(big.find("BadRequest") != std::string::npos);
        // A second agent on the same path is refused instead of stealing the socket.
        IpcServer second(sock, [](const std::string&) { return std::string("{}"); });
        CHECK_MTP(second.start(), MtpErrc::DeviceBusy);
    }
    CHECK(!std::filesystem::exists(sock));  // stop() unlinks
    CHECK_MTP(ipcRoundTrip(sock, "x"), MtpErrc::DeviceDisconnected);
    std::filesystem::remove_all(dir);
}

AF_TEST(agent_ipc, stale_socket_file_is_replaced) {
    char tmpl[] = "/tmp/afipc.XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const std::filesystem::path sock = std::filesystem::path(dir) / "agent.sock";
    {  // a crashed agent leaves a socket node behind with nobody listening on it
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
        CHECK_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
        ::close(fd);
    }
    CHECK(std::filesystem::exists(sock));
    CHECK_MTP(ipcRoundTrip(sock, "x"), MtpErrc::DeviceDisconnected);  // dead node: connect fails
    {
        IpcServer s(sock, [](const std::string&) { return std::string("{\"v\":1}"); });
        s.start();  // replaces the stale node
        CHECK_EQ(ipcRoundTrip(sock, "ping"), std::string("{\"v\":1}"));
    }
    // A regular file squatting on the socket path is never deleted.
    {
        std::ofstream(sock) << "not a socket";
    }
    IpcServer t(sock, [](const std::string&) { return std::string("{}"); });
    CHECK_MTP(t.start(), MtpErrc::IoError);
    CHECK(std::filesystem::exists(sock));
    std::filesystem::remove_all(dir);
}

AF_TEST(agent_ipc, end_to_end_through_the_dispatcher) {
    Rig r;
    char tmpl[] = "/tmp/afipc.XXXXXX";
    const std::string dir = ::mkdtemp(tmpl);
    const std::filesystem::path sock = std::filesystem::path(dir) / "agent.sock";
    r.settle();
    IpcServer server(sock, [&](const std::string& line) { return r.core->handleLine(line); });
    server.start();
    const auto resp =
        Json::parse(ipcRoundTrip(sock, "{\"protocolVersion\":1,\"requestId\":\"7\",\"method\":\"devices.list\"}"));
    CHECK_EQ(resp.find("requestId")->asString(), std::string("7"));
    const auto& devs = resp.find("result")->find("devices")->items();
    CHECK_EQ(devs.size(), size_t{1});
    CHECK_EQ(devs[0].find("state")->asString(), std::string("detected"));
    CHECK(devs[0].find("id")->asString() == r.phone->descriptor().id);
    server.stop();
    std::filesystem::remove_all(dir);
}
