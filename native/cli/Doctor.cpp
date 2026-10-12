#include "Doctor.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

#include "MountState.hpp"
#include "Platform.hpp"
#include "Process.hpp"
#include "mtport/RuntimePaths.hpp"

namespace fs = std::filesystem;

namespace mtport::cli {

const char* toString(CheckStatus s) {
    switch (s) {
        case CheckStatus::Ok:
            return " OK ";
        case CheckStatus::Warn:
            return "WARN";
        case CheckStatus::Fail:
            return "FAIL";
        case CheckStatus::Info:
            return "INFO";
        case CheckStatus::NotRun:
            return "N/A ";
    }
    return "?";
}

namespace {

bool processRunning(const std::string& name) {
    return runProcess({"/usr/bin/pgrep", "-x", name}, 5).exitCode == 0;
}

}  // namespace

std::vector<Check> runDoctor(const BackendSpec& spec) {
    std::vector<Check> out;
    auto add = [&](std::string id, CheckStatus st, std::string summary, std::string remedy = {}) {
        out.push_back({std::move(id), st, std::move(summary), std::move(remedy)});
    };

    // --- host --------------------------------------------------------------------------------
    const std::string osv = macOSVersion();
    if (osv.empty())
        add("macos", CheckStatus::Warn, "could not read the macOS version");
    else if (versionAtLeast(osv, "15.4"))
        add("macos", CheckStatus::Ok, "macOS " + osv + " (FSKit requires 15.4+)");
    else
        add("macos", CheckStatus::Fail,
            "macOS " + osv + " is older than 15.4: the macFUSE FSKit backend is unavailable",
            "Update macOS to 15.4 or newer. MTPort never falls back to the kernel-extension backend.");

    utsname u{};
    uname(&u);
    const std::string arch = u.machine;
    if (arch == "arm64")
        add("arch", CheckStatus::Ok, "Apple Silicon (arm64)");
    else
        add("arch", CheckStatus::Warn,
            "architecture " + arch + ": Intel is not covered by the v0.1.0 acceptance tests");

    // --- MTP stack ---------------------------------------------------------------------------
    if (realBackendAvailable())
        add("libmtp", CheckStatus::Ok, "libmtp " + libmtpVersionString() + " linked (talks to the phone over libusb)");
    else
        add("libmtp", CheckStatus::Fail, "this build has no libmtp backend",
            "brew install libmtp libusb pkgconf cmake ninja, then rebuild (scripts/build-native.sh)");

    // --- macFUSE / FSKit (optional: the default WebDAV transport does not use it) ------------------
    const fs::path macfuse = "/Library/Filesystems/macfuse.fs";
    std::error_code ec;
    if (!fs::exists(macfuse, ec)) {
        add("macfuse", CheckStatus::Info, "macFUSE is not installed (optional: only the experimental --transport fskit needs it)",
            "The default WebDAV transport mounts the phone with macOS' own network-volume support and needs no macFUSE.");
    } else {
        const std::string ver = macfuseVersion();
        if (ver.empty())
            add("macfuse", CheckStatus::Info, "macFUSE is installed but its version could not be read (optional)");
        else if (versionAtLeast(ver, "5.4.0"))
            add("macfuse", CheckStatus::Ok, "macFUSE " + ver + " (>= 5.4.0; only used by --transport fskit)");
        else
            add("macfuse", CheckStatus::Warn, "macFUSE " + ver + " is older than 5.4.0 (only matters for --transport fskit)",
                "--transport fskit needs >= 5.4.0 (5.3.3 corrupts 1-14 byte writes). Update macFUSE, or keep the default WebDAV transport.");
    }
    const bool libfuse =
        fs::exists("/usr/local/lib/libfuse.dylib", ec) || fs::exists("/usr/local/lib/libfuse.2.dylib", ec);
    add("libfuse", libfuse ? CheckStatus::Ok : CheckStatus::Info,
        libfuse ? "libfuse.dylib found in /usr/local/lib"
                : "libfuse.dylib not found in /usr/local/lib (only installed with macFUSE; optional)");

    {
        const auto r = runProcess({"/usr/bin/pluginkit", "-m", "-A", "-p", "com.apple.fskit.fsmodule"}, 15);
        std::string line;
        std::string lower = r.output;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        const auto pos = lower.find("macfuse");
        if (!fs::exists(macfuse, ec)) {
            add("fskit-extension", CheckStatus::NotRun, "not checked: macFUSE is not installed");
        } else if (r.exitCode != 0 || lower.empty()) {
            add("fskit-extension", CheckStatus::Info, "could not query FSKit extensions (pluginkit)");
        } else if (pos == std::string::npos) {
            add("fskit-extension", CheckStatus::Warn, "no macFUSE FSKit extension is registered",
                "Open System Settings > General > Login Items & Extensions > File System Extensions and enable "
                "macFUSE.");
        } else {
            const size_t ls = lower.rfind('\n', pos);
            const char mark = r.output[ls == std::string::npos ? 0 : ls + 1];
            (void)mark;  // pluginkit does not reliably flag "enabled", so do not guess from it
            add("fskit-extension", CheckStatus::Info,
                "macFUSE FSKit extension is registered (pluginkit cannot tell whether it is enabled)",
                "If 'mtport mount' hangs in 'starting', enable macFUSE under System Settings > General > Login "
                "Items & Extensions > File System Extensions.");
        }
    }

    // --- things that can hold the phone -------------------------------------------------------
    std::string busy;
    for (const char* name : {"Image Capture", "Android File Transfer", "OpenMTP", "MacDroid", "ptpcamerad"})
        if (processRunning(name)) busy += (busy.empty() ? "" : ", ") + std::string(name);
    if (busy.empty())
        add("competing-apps", CheckStatus::Ok, "no known competing MTP/PTP process is running");
    else
        add("competing-apps", CheckStatus::Warn, "running now: " + busy,
            "Another process can own the phone's USB interface and make the MTP open fail with DeviceBusy. Quit these "
            "apps "
            "(macOS may restart 'ptpcamerad' on its own while a camera-class device is attached).");

    // --- devices ------------------------------------------------------------------------------
    try {
        auto factory = makeFactory(spec);
        auto devices = factory->enumerate();
        if (devices.empty())
            add("usb-mtp-devices", CheckStatus::Info, "no MTP-capable USB device detected",
                "Plug in the phone with a data-capable cable, unlock it, and pick USB > File transfer (MTP). "
                "USB visibility alone does not prove MTP readiness (see: system_profiler SPUSBDataType).");
        else {
            std::string ids;
            for (auto& d : devices) ids += (ids.empty() ? "" : ", ") + d.id;
            add("usb-mtp-devices", CheckStatus::Info,
                std::to_string(devices.size()) + " candidate device(s): " + ids +
                    " (MTP session not opened; run 'mtport info')");
        }
    } catch (const std::exception& e) {
        add("usb-mtp-devices", realBackendAvailable() || spec.kind == "fake" ? CheckStatus::Warn : CheckStatus::NotRun,
            std::string("device detection unavailable: ") + e.what());
    }

    // --- local state --------------------------------------------------------------------------
    struct statvfs sv{};
    fs::create_directories(cacheDir(), ec);
    if (::statvfs(cacheDir().c_str(), &sv) == 0) {
        const double freeGiB =
            static_cast<double>(sv.f_bavail) * static_cast<double>(sv.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.1f GiB free for staging (default quota min(20 GiB, 20%% of free))", freeGiB);
        add("staging-space", freeGiB > 1.0 ? CheckStatus::Ok : CheckStatus::Warn, buf);
    }
    const auto mounts = listMountStates(true);
    add("mounts", CheckStatus::Info, std::to_string(mounts.size()) + " active MTPort mount(s)");
    return out;
}

Json doctorToJson(const std::vector<Check>& checks) {
    Json arr = Json::array();
    bool anyFail = false;
    for (auto& c : checks) {
        Json j = Json::object();
        j["id"] = c.id;
        std::string st = toString(c.status);
        st.erase(std::remove(st.begin(), st.end(), ' '), st.end());
        j["status"] = st;
        j["summary"] = c.summary;
        j["remedy"] = c.remedy;
        anyFail |= c.status == CheckStatus::Fail;
        arr.push(std::move(j));
    }
    Json root = Json::object();
    root["ok"] = !anyFail;
    root["checks"] = std::move(arr);
    return root;
}

}  // namespace mtport::cli
