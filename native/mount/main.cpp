// mtport-mount: one process per phone. It owns the MTP session for as long as the volume is mounted and
// serves Finder through one of two transports:
//   webdav (default)  a loopback WebDAV server mounted with macOS' own network-volume support (NetFS). No macFUSE, no
//                     root, no kernel extension; Finder lists the volume in the sidebar with an eject button.
//   fskit             macFUSE's FSKit backend (backend=fskit, never the kernel backend).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <signal.h>
#include <sys/mount.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "Backend.hpp"
#include "MountState.hpp"
#ifdef MTPORT_FUSE_TRANSPORT
#include "MtpFuseAdapter.hpp"
#endif
#include "HttpServer.hpp"
#include "LoopbackName.hpp"
#include "WebDavMount.hpp"
#include "WebDavService.hpp"
#include "Platform.hpp"
#include "Process.hpp"
#include "UsbPresence.hpp"
#include "mtport/RuntimePaths.hpp"

using namespace mtport;

namespace {

constexpr int kExitConfig = 78;       // EX_CONFIG: prerequisites missing
constexpr int kExitTempFail = 75;     // EX_TEMPFAIL: device busy / not ready
constexpr int kExitUnavailable = 69;  // EX_UNAVAILABLE

struct Options {
    DeviceId device;
    BackendSpec backend;
    bool readOnly{true};
    bool foreground{false};
    bool passthrough{false};
    bool noops{false};
    std::string mountpoint;
    std::string transport{"webdav"};  // "webdav" | "fskit"
};

void usage() {
    std::cerr
        << "usage: mtport-mount [--device ID] [--backend real|fake] [--fake-root DIR] [--fake-no-partial-read]\n"
           "                           [--read-only | --read-write] [--foreground] [--mountpoint /Volumes/NAME]\n"
           "                           [--transport webdav|fskit]\n"
           "                           [--metadata-passthrough] [--accept-metadata-noops]\n";
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--device") {
            if (!val(o.device)) return false;
        } else if (a == "--backend") {
            if (!val(o.backend.kind)) return false;
        } else if (a == "--fake-root") {
            if (!val(v)) return false;
            o.backend.fakeRoot = v;
        } else if (a == "--fake-no-partial-read")
            o.backend.fakePartialRead = false;
        else if (a == "--mountpoint") {
            if (!val(o.mountpoint)) return false;
        } else if (a == "--transport") {
            if (!val(o.transport)) return false;
        } else if (a == "--read-only")
            o.readOnly = true;
        else if (a == "--read-write")
            o.readOnly = false;
        else if (a == "--foreground")
            o.foreground = true;
        else if (a == "--metadata-passthrough")
            o.passthrough = true;
        else if (a == "--accept-metadata-noops")
            o.noops = true;
        else
            return false;
    }
    return true;
}

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reconnectRequested{false};  // set by SIGUSR1 (test hook) and by system wake
void onUsr1(int) {
    g_reconnectRequested = true;
}
std::atomic<bool> g_simulateUnplug{false};  // SIGUSR2: developer hook that behaves exactly like a USB loss
void onUsr2(int) {
    g_simulateUnplug = true;
}
std::atomic<bool> g_terminate{false};  // SIGTERM/SIGINT in webdav mode (FUSE installs its own handlers)
void onTerm(int) {
    g_terminate = true;
}
std::mutex g_mutex;
std::condition_variable g_cv;

void sleepFor(std::chrono::milliseconds d) {
    std::unique_lock lk(g_mutex);
    g_cv.wait_for(lk, d, [] { return g_stop.load(); });
}

void log(const char* msg) {
    std::cout << "mtport-mount: " << msg << std::endl;  // never file names or serials
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        usage();
        return 2;
    }
    if (::geteuid() == 0) {
        std::cerr << "mtport-mount refuses to run as root; run it as your normal user.\n";
        return kExitConfig;
    }

    MountState state;
    state.readOnly = opt.readOnly;
    state.pid = ::getpid();
    state.phase = "starting";
    auto publish = [&] {
        try {
            writeMountState(state);
        } catch (...) {
        }
    };

    if (opt.transport != "webdav" && opt.transport != "fskit") {
        std::cerr << "unknown --transport '" << opt.transport << "'\n";
        return 2;
    }
#ifndef MTPORT_FUSE_TRANSPORT
    if (opt.transport == "fskit") {
        std::cerr << "MountBackendUnavailable: this build has no FSKit transport (macFUSE headers/libfuse were not found "
                     "at configure time)\n";
        return kExitConfig;
    }
#endif
    // 1. Platform gate for the FSKit transport: macOS >= 15.4 and macFUSE >= 5.4.0, FSKit only. No kernel-extension
    //    fallback exists. The WebDAV transport needs nothing beyond macOS itself.
    if (const auto pre = checkMountPrerequisites(); opt.transport == "fskit" && !pre.ok) {
        std::cerr << "MountBackendUnavailable: " << pre.problem << "\n";
        state.deviceId = opt.device;
        state.phase = "error";
        state.error = "MountBackendUnavailable: " + pre.problem;
        publish();
        return kExitConfig;
    }

    try {
        auto factory = makeFactory(opt.backend);
        std::shared_ptr<IMtpDeviceFactory> shared(std::move(factory));
        if (opt.device.empty()) {
            auto devs = shared->enumerate();
            if (devs.size() != 1) {
                std::cerr << (devs.empty() ? "no MTP-capable device found" : "several devices found; pass --device")
                          << "\n";
                return kExitUnavailable;
            }
            opt.device = devs.front().id;
        }
        state.deviceId = opt.device;
        publish();

        // 2. Become the single MTP owner (cross-process lock + libmtp session).
        // The worker's only strong reference lives in `fs` after construction, so a reconnect can really release the
        // session.
        std::shared_ptr<IMtpDevice> device(shared->open(opt.device));

        // 2b. Readiness gate. Android only answers storage queries once the phone is unlocked and the user allowed
        // access; right after a re-plug the session can open fine and still be useless. Never mount (and thereby show
        // Finder an unreadable volume) until the storages can really be read. Reopen the session now and then, because
        // some phones only start answering on a fresh session.
        {
            auto ready = [](IMtpDevice& d) {
                try {
                    return !d.storages().empty();
                } catch (const MtpError&) {
                    return false;
                }
            };
            if (!ready(*device)) {
                state.phase = "connecting";
                state.error = "Waiting for the phone: unlock it and allow access to its data";
                state.model = device->descriptor().model;
                publish();
                log("phone storage is not readable yet; waiting for unlock");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
                const std::string usbKey = opt.device.substr(0, opt.device.find('@'));
                for (int tries = 1; !ready(*device); ++tries) {
                    if (std::chrono::steady_clock::now() > deadline)
                        throw MtpError(MtpErrc::MtpNotReady,
                                       "the phone did not allow access to its storage; unlock it and choose "
                                       "File transfer (MTP), then allow access if it asks");
                    sleepFor(std::chrono::seconds(2));
                    if (tries % 3 == 0) {
                        device.reset();  // hand the session back, then open a fresh one
                        sleepFor(std::chrono::seconds(1));
                        try {
                            // The phone may have re-enumerated with a new bus address: find it again by vendor:product.
                            for (auto& d : shared->enumerate()) {
                                if (d.id.substr(0, d.id.find('@')) == usbKey) {
                                    device = std::shared_ptr<IMtpDevice>(shared->open(d.id));
                                    break;
                                }
                            }
                        } catch (const MtpError&) {
                        }
                        if (!device) throw MtpError(MtpErrc::DeviceDisconnected, "the phone went away while waiting for unlock");
                    }
                }
                state.error.clear();
                log("phone storage is readable");
            }
        }
        const DeviceDescriptor desc = device->descriptor();
        state.model = desc.model;
        std::string mountpoint = opt.mountpoint.empty() ? defaultMountPoint(desc.model) : opt.mountpoint;
        if (!isUnderVolumes(mountpoint)) {
            std::cerr << "mount points must be directly under /Volumes\n";
            return kExitConfig;
        }
        state.mountPoint = mountpoint;
        publish();

        FsConfig cfg;
        cfg.readOnly = opt.readOnly;
        cfg.metadataOverlay = !opt.passthrough;
        cfg.acceptMetadataNoops = opt.noops;
        // WebDAV clients create a file with an empty PUT and fill it minutes later (large copies): keep the 0-byte
        // placeholder pending for a long time instead of uploading an empty object that would block the real upload.
        if (opt.transport == "webdav") cfg.emptyFileGrace = std::chrono::minutes(30);
        cfg.uid = ::getuid();
        cfg.gid = ::getgid();
        MtpFilesystem fs(std::move(device), cfg);
#ifdef MTPORT_FUSE_TRANSPORT
        if (opt.transport == "fskit") fuse_adapter::setFilesystem(&fs);
#endif

        const std::string volname = mountpoint.substr(std::strlen("/Volumes/"));
        std::atomic<bool> unmountRequested{false};
        std::mutex mountPointMutex;  // the webdav transport learns the real mount point only after NetFS mounted
        auto currentMountPoint = [&] {
            std::lock_guard lk(mountPointMutex);
            return mountpoint;
        };

        auto requestUnmount = [&](const std::string& reason) {
            if (unmountRequested.exchange(true)) return;
            state.phase = "draining";
            state.error = reason;
            publish();
            log(("unmounting: " + reason).c_str());
            // The phone is gone or the user asked: let the OS tear the volume down (no data on the phone is touched).
            // Never call fuse_exit() from here: once the volume is gone, fuse_main() tears the session down itself
            // and a late fuse_exit() on the freed handle crashed the worker (SIGSEGV, found on a real unplug).
            // Retry, because the first attempt can race with in-flight requests.
            std::thread([mp = currentMountPoint()] {
                for (int attempt = 0; attempt < 5; ++attempt) {
                    if (runProcess({"/usr/sbin/diskutil", "unmount", "force", mp}, 60).exitCode == 0) return;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }).detach();
        };
        fs.setDisconnectCallback([&] { requestUnmount("USB disconnected"); });

#ifdef MTPORT_FUSE_TRANSPORT
        fuse_adapter::Hooks hooks;
        hooks.onInit = [&] {
            state.phase = "mounted";
            state.error.clear();
            publish();
            log("mounted");
        };
        hooks.onDestroy = [&] {
            fs.housekeeping(true);  // upload empty files that were still waiting for a re-open
            log("fuse session ended");
        };
        fuse_adapter::setHooks(hooks);
#endif

        // Sleep/wake (and SIGUSR1 for testing): hand the session back, wait for the phone to re-enumerate and
        // reconnect ONLY if it is still attached. Open handles become ESTALE; staged bytes survive; nothing
        // destructive is replayed.
        auto reconnect = [&] {
            log("reconnecting after wake");
            state.phase = "connecting";
            state.error = "Reconnecting after sleep...";
            publish();
            fs.detachDevice();
            for (int attempt = 0; attempt < 20 && !g_stop.load(); ++attempt) {
                sleepFor(std::chrono::milliseconds(attempt == 0 ? 1500 : 1000));
                try {
                    bool present = false;
                    for (auto& d : shared->enumerate()) present |= d.id == opt.device;
                    if (!present) continue;  // keep waiting: the phone may still be re-enumerating
                    fs.reconnect(shared->open(opt.device));
                    state.phase = "mounted";
                    state.error.clear();
                    publish();
                    log("reconnected");
                    return;
                } catch (const MtpError& e) {
                    if (e.code() != MtpErrc::DeviceBusy && e.code() != MtpErrc::DeviceDisconnected) break;
                }
            }
            fs.markDisconnected();
            requestUnmount("the phone did not come back after sleep");
        };
        std::signal(SIGUSR1, onUsr1);
        std::signal(SIGUSR2, onUsr2);

        // 3. Health monitor: statistics for the UI/CLI, plus a low-frequency USB presence probe.
        std::thread monitor([&] {
            auto lastProbe = std::chrono::steady_clock::now();
            unsigned capTick = 0;
            while (!g_stop.load()) {
                sleepFor(std::chrono::milliseconds(1000));
                fs.housekeeping();
                const auto& c = fs.counters();
                state.bytesRead = c.bytesRead;
                state.bytesStaged = c.bytesStaged;
                state.bytesUploaded = c.bytesUploaded;
                state.uploadsOk = c.uploadsOk;
                state.uploadsFailed = c.uploadsFailed;
                state.uploadsActive = c.uploadsActive;
                state.uploadBytesDone = c.uploadBytesDone;
                state.uploadBytesTotal = c.uploadBytesTotal;
                state.pendingUploads = fs.pendingUploadCount();
                state.recoverableUploads = fs.recoverableUploads().size();
                if (state.capacityBytes == 0 || (c.uploadsActive == 0 && ++capTick % 30 == 0)) {
                    try {
                        const auto sv = fs.statfs("/");
                        state.capacityBytes = sv.blocks * sv.blockSize;
                        state.freeBytes = sv.blocksFree * sv.blockSize;
                    } catch (...) {  // capacity is optional information
                    }
                }
                if (state.phase == "mounted") publish();

                if (g_simulateUnplug.exchange(false) && !fs.isDisconnected()) fs.markDisconnected();
                if (g_reconnectRequested.exchange(false) && !unmountRequested.load()) reconnect();

                if (std::chrono::steady_clock::now() - lastProbe > std::chrono::seconds(2)) {
                    lastProbe = std::chrono::steady_clock::now();
                    bool present = true;
                    if (opt.backend.kind == "real") present = usbDeviceCount(desc.vendorId, desc.productId) > 0;
                    if (!present && !fs.isDisconnected())
                        fs.markDisconnected();  // triggers requestUnmount via the callback
                }
            }
        });

        // System power: acknowledge sleep immediately (staged data is already on disk) and reconnect on wake.
        struct PowerCtx {
            io_connect_t root{0};
        } power;
        static PowerCtx* s_power = nullptr;
        s_power = &power;
        std::thread powerThread([] {
            IONotificationPortRef port = nullptr;
            io_object_t notifier = 0;
            auto cb = [](void*, io_service_t, natural_t msg, void* arg) {
                switch (msg) {
                    case kIOMessageCanSystemSleep:
                    case kIOMessageSystemWillSleep:
                        IOAllowPowerChange(s_power->root, reinterpret_cast<intptr_t>(arg));
                        break;
                    case kIOMessageSystemHasPoweredOn:
                        g_reconnectRequested = true;
                        break;
                    default:
                        break;
                }
            };
            s_power->root = IORegisterForSystemPower(nullptr, &port, cb, &notifier);
            if (!s_power->root) return;
            CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopCommonModes);
            while (!g_stop.load()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
            IODeregisterForSystemPower(&notifier);
            IOServiceClose(s_power->root);
            IONotificationPortDestroy(port);
        });

        // 4. Serve Finder.
        int rc = 0;
        std::string serveError;
        if (opt.transport == "webdav") {
            // A random per-mount password: only this worker and the NetFS call know it; the server listens on 127.0.0.1 only.
            std::string password;
            {
                unsigned char raw[16];
                FILE* f = std::fopen("/dev/urandom", "rb");
                if (!f || std::fread(raw, 1, sizeof raw, f) != sizeof raw) {
                    if (f) std::fclose(f);
                    throw std::runtime_error("no randomness for the mount password");
                }
                std::fclose(f);
                static const char* hex = "0123456789abcdef";
                for (unsigned char c : raw) {
                    password.push_back(hex[c >> 4]);
                    password.push_back(hex[c & 15]);
                }
            }
            webdav::WebDavService service(fs, volname, "/" + volname);
            webdav::HttpServer server("mtport", password,
                                      [&](webdav::HttpRequest& q, webdav::HttpResponse& r) { service.handle(q, r); });
            const uint16_t port = server.start();
            std::signal(SIGTERM, onTerm);
            std::signal(SIGINT, onTerm);
            log(opt.readOnly ? "mounting via WebDAV (read-only)" : "mounting via WebDAV (read-write)");
            if (const char* dev = std::getenv("MTPORT_WEBDAV_SERVE_ONLY"); dev && *dev == '1') {
                // Developer aid: serve on a fixed password and let a human mount it (mount_webdav, curl, ...).
                std::cout << "webdav-serve-only port=" << port << " user=mtport password=" << password << " path=/" << volname << "/"
                          << std::endl;
                while (!g_terminate.load()) sleepFor(std::chrono::milliseconds(500));
                server.stop();
                return 0;
            }
            // Finder's sidebar shows the host name of the mount URL: use the phone's own name (a single label,
            // published through mDNS for 127.0.0.1 only) and fall back to the plain "<name>.localhost" form if that fails.
            std::string phoneName = desc.friendlyName.empty() ? desc.model : desc.friendlyName;
            if (const char* o = std::getenv("MTPORT_SIDEBAR_NAME"); o && *o) phoneName = o;  // developer aid
            webdav::LoopbackName nameRecord(phoneName);
            if (nameRecord.ok()) log(("sidebar name: " + nameRecord.host()).c_str());
            else log(("sidebar name falls back to .localhost: " + nameRecord.error()).c_str());
            // Read the folders people open first while the volume is still being mounted and looked at: a 1,800-photo
            // folder takes the phone ~30 s to describe, after that it is served from memory.
            std::atomic<bool> volumeUp{false};
            std::thread warm([&] {
                fs.prefetch({"DCIM/Camera", "Download", "Pictures"}, [&] {
                    if (g_terminate.load()) return true;
                    if (!volumeUp.load()) return false;
                    std::lock_guard lk(mountPointMutex);
                    return !webdav::isMounted(mountpoint);
                });
                log("prefetch finished");
            });
            auto mounted = nameRecord.ok() ? webdav::mountVolume(port, "mtport", password, volname, opt.readOnly, nameRecord.host())
                                           : webdav::MountResult{};
            if (!mounted.ok) {
                if (nameRecord.ok()) log(("mount by phone name failed (" + mounted.error + "); retrying with .localhost").c_str());
                mounted = webdav::mountVolume(port, "mtport", password, volname, opt.readOnly, webdav::loopbackHost(phoneName));
            }
            if (!mounted.ok) {
                rc = 1;
                serveError = "WebDAV mount failed: " + mounted.error;
            } else {
                {
                    std::lock_guard lk(mountPointMutex);
                    mountpoint = mounted.mountPoint;
                }
                service.enableQuota();  // Finder can now show free/used space
                state.mountPoint = mounted.mountPoint;
                state.phase = "mounted";
                state.error.clear();
                publish();
                log("mounted");
                volumeUp = true;
                // Run until the volume is gone (Finder Eject, `diskutil unmount`, USB loss) or we are told to stop.
                while (!g_terminate.load() && webdav::isMounted(mounted.mountPoint)) sleepFor(std::chrono::milliseconds(500));
                if (g_terminate.load() && webdav::isMounted(mounted.mountPoint)) {
                    for (int attempt = 0; attempt < 5 && webdav::isMounted(mounted.mountPoint); ++attempt) {
                        runProcess({"/usr/sbin/diskutil", "unmount", mounted.mountPoint}, 60);
                        if (webdav::isMounted(mounted.mountPoint)) sleepFor(std::chrono::seconds(2));
                    }
                }
                fs.housekeeping(true);  // upload empty files that were still waiting for a re-open
                log("webdav volume ended");
            }
            fs.cancelBackgroundWork();  // an eject must not wait for a half-read photo folder
            warm.join();
            server.stop();
        } else {
#ifdef MTPORT_FUSE_TRANSPORT
            // -f: never fork after the MTP session/libusb threads exist.
            // Do NOT add "local" here: with FSKit it creates a /dev/diskN device whose volume neither `diskutil unmount`
            // nor `eject` can remove (verified 2026-10-11), leaving a stuck mount until reboot.
            std::string fuseOpts = "backend=fskit,volname=" + volname + ",fsname=mtport";
            if (opt.readOnly) fuseOpts += ",ro";
            // Developer aid for experiments with extra macFUSE mount options; comma-separated, [a-z_=]* only.
            if (const char* extra = std::getenv("MTPORT_FUSE_OPTS"); extra && *extra) {
                const std::string e = extra;
                if (e.find_first_not_of("abcdefghijklmnopqrstuvwxyz_=,") == std::string::npos) fuseOpts += "," + e;
            }
            std::vector<std::string> args = {"mtport-mount", mountpoint, "-f", "-o", fuseOpts};
            // Developer aid: libfuse prints every request and its result to stderr (may contain file names).
            if (const char* dbg = std::getenv("MTPORT_FUSE_DEBUG"); dbg && *dbg == '1')
                args.insert(args.begin() + 3, "-d");
            std::vector<char*> cargs;
            for (auto& a : args) cargs.push_back(a.data());
            log(opt.readOnly ? "starting FSKit mount (read-only)" : "starting FSKit mount (read-write)");
            rc = fuse_main(static_cast<int>(cargs.size()), cargs.data(), &fuse_adapter::operations(), nullptr);
            if (rc != 0)
                serveError = "macFUSE FSKit mount failed (fuse_main returned " + std::to_string(rc) +
                             "). Is the macFUSE file-system extension enabled?";
#endif
        }

        g_stop = true;
        g_cv.notify_all();
        monitor.join();
        powerThread.join();
#ifdef MTPORT_FUSE_TRANSPORT
        fuse_adapter::setFilesystem(nullptr);
#endif
        if (rc != 0) {
            state.phase = "error";
            state.error = serveError;
            publish();
            std::cerr << state.error << "\n";
            return kExitConfig;
        }
        if (fs.isDisconnected()) {
            removeMountState(opt.device);  // USB loss: a re-plug must mount again at once
        } else {
            // Deliberate unmount (Eject): leave a short-lived "ejected" marker so the agent does not remount the
            // phone a second later when it re-enumerates on the bus.
            state.phase = "ejected";
            state.error.clear();
            publish();
        }
        log("unmounted");
        return 0;
    } catch (const MtpError& e) {
        state.phase = "error";
        state.error = std::string(toString(e.code())) + ": " + e.detail();
        publish();
        std::cerr << state.error << "\n";
        return e.code() == MtpErrc::DeviceBusy || e.code() == MtpErrc::MtpNotReady ? kExitTempFail : kExitUnavailable;
    } catch (const std::exception& e) {
        state.phase = "error";
        state.error = e.what();
        publish();
        std::cerr << e.what() << "\n";
        return kExitUnavailable;
    }
}
