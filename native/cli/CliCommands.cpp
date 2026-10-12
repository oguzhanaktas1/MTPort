#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "Cli.hpp"
#include "Doctor.hpp"
#include "MountState.hpp"
#include "Process.hpp"
#include "mtport/DeviceLock.hpp"
#include "mtport/NameRules.hpp"
#include "mtport/PosixErrorMapper.hpp"
#include "mtport/RuntimePaths.hpp"
#include "mtport/Sha256.hpp"
#include "mtport/VirtualPathMapper.hpp"

namespace fs = std::filesystem;

namespace mtport::cli {

namespace {

constexpr const char* kVersion = "0.1.0";
std::atomic<bool> g_cancel{false};

void onSigint(int) {
    g_cancel = true;
}

const std::set<std::string> kValueOptions = {"--device",    "--storage",   "--path",     "--src",       "--dst",
                                             "--file",      "--direction", "--backend",  "--fake-root", "--runs",
                                             "--chunk-kib", "--mode",      "--new-name", "--max-bytes", "--transport"};
const std::set<std::string> kBoolFlags = {"--json",
                                          "--foreground",
                                          "--read-only",
                                          "--read-write",
                                          "--force",
                                          "--dir",
                                          "--parents",
                                          "--verify",
                                          "--help",
                                          "--version",
                                          "--fake-no-partial-read",
                                          "--metadata-passthrough",
                                          "--accept-metadata-noops"};
const std::set<std::string> kCommands = {"doctor", "devices", "info",  "ls",      "pull",   "push", "mkdir",
                                         "rm",     "mv",      "mount", "unmount", "status", "bench"};

std::string isoTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string humanBytes(uint64_t b) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    std::ostringstream os;
    if (u == 0)
        os << b << " B";
    else
        os << std::fixed << std::setprecision(v < 10 ? 2 : 1) << v << " " << units[u];
    return os.str();
}

std::string errorMessageFor(MtpErrc c, const std::string& detail) {
    std::string hint;
    switch (c) {
        case MtpErrc::DeviceBusy:
            hint =
                ". Close Image Capture / Android File Transfer / OpenMTP / other MTP apps, and make sure no "
                "MTPort mount owns this phone (mtport status).";
            break;
        case MtpErrc::MtpNotReady:
            hint = ". Unlock the phone and choose USB > File transfer (MTP).";
            break;
        case MtpErrc::DeviceDisconnected:
            hint = ". Check the cable (it must carry data, not only power) and reconnect the phone.";
            break;
        case MtpErrc::MountBackendUnavailable:
            hint =
                ". Run 'mtport doctor' and follow docs/SETUP_MACOS.md (macFUSE >= 5.4.0 with the FSKit extension "
                "enabled).";
            break;
        default:
            break;
    }
    return std::string(toString(c)) + ": " + detail + hint;
}

struct Context {
    const Args& args;
    std::ostream& out;
    std::ostream& err;
    bool json() const { return args.has("--json"); }
};

[[noreturn]] void usageError(const std::string& m) {
    throw std::invalid_argument(m);
}

BackendSpec specOf(const Args& a) {
    return backendFromArgs(a);
}

DeviceId pickDevice(Context& c, IMtpDeviceFactory& factory) {
    if (auto d = c.args.opt("--device"); !d.empty()) return d;
    auto devices = factory.enumerate();
    if (devices.empty()) throw MtpError(MtpErrc::DeviceDisconnected, "no MTP-capable USB device detected");
    if (devices.size() > 1) {
        std::string ids;
        for (auto& d : devices) ids += " " + d.id;
        usageError("several devices found, pass --device <id>; candidates:" + ids);
    }
    return devices.front().id;
}

std::unique_ptr<IMtpDevice> openDevice(Context& c, IMtpDeviceFactory& factory, DeviceId* idOut = nullptr) {
    const DeviceId id = pickDevice(c, factory);
    if (idOut) *idOut = id;
    try {
        return factory.open(id);
    } catch (const MtpError& e) {
        if (e.code() == MtpErrc::DeviceBusy) {
            if (auto st = readMountState(id); st && processAlive(st->pid))
                throw MtpError(MtpErrc::DeviceBusy, "the phone is mounted by MTPort (pid " +
                                                        std::to_string(st->pid) + ", " + st->mountPoint +
                                                        "); use the mounted volume or run 'mtport unmount'");
        }
        throw;
    }
}

StorageId parseStorage(Context& c, IMtpDevice& dev) {
    const std::string s = c.args.opt("--storage");
    if (s.empty()) {
        auto st = dev.storages();
        if (st.size() == 1) return st.front().id;
        std::string list;
        for (auto& x : st) list += " " + std::to_string(x.id) + "(" + x.name + ")";
        usageError("pass --storage <id>; available:" + list);
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0' || v > UINT32_MAX) usageError("bad --storage value '" + s + "'");
    return static_cast<StorageId>(v);
}

// Walks `comps` from the storage root listing only the folders on the way (never recursive).
// Returns nullopt for the storage root itself.
std::optional<FileNode> resolveNode(IMtpDevice& dev, StorageId storage, const std::vector<std::string>& comps) {
    ObjectId cur = kRootObject;
    std::optional<FileNode> node;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (node && !node->isDirectory) throw MtpError(MtpErrc::NotADirectory, "'" + node->name + "' is not a folder");
        const std::string want = normalizeNfc(comps[i]);
        std::vector<FileNode> hits;
        for (auto& n : dev.list(storage, cur))
            if (normalizeNfc(n.name) == want) hits.push_back(std::move(n));
        if (hits.empty()) throw MtpError(MtpErrc::ObjectNotFound, "no such file or folder: " + comps[i]);
        if (hits.size() > 1)
            throw MtpError(MtpErrc::InvalidVirtualPath, std::to_string(hits.size()) + " objects are named '" +
                                                            comps[i] + "'; the name is ambiguous on the phone");
        node = std::move(hits.front());
        cur = node->objectId;
    }
    return node;
}

std::vector<std::string> splitPathOrThrow(const std::string& p) {
    try {
        return splitVirtualPath(p);
    } catch (const FsError& e) {
        throw MtpError(MtpErrc::InvalidVirtualPath, std::string("bad path '") + p + "': " + e.what());
    }
}

Json nodeJson(const FileNode& n) {
    Json j = Json::object();
    j["objectId"] = n.objectId;
    j["parentObjectId"] = n.parentObjectId;
    j["storageId"] = n.storageId;
    j["name"] = n.name;
    j["type"] = n.isDirectory ? "folder" : "file";
    j["sizeBytes"] = n.sizeBytes;
    if (n.modifiedAt) j["modified"] = isoTime(*n.modifiedAt);
    return j;
}

struct ProgressPrinter {
    bool enabled;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point lastPrint{};
    bool operator()(const TransferProgress& p) {
        if (g_cancel) return false;
        if (!enabled) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPrint < std::chrono::milliseconds(250) && p.bytesDone != p.bytesTotal) return true;
        lastPrint = now;
        const double secs = std::chrono::duration<double>(now - start).count();
        std::cerr << "\r" << humanBytes(p.bytesDone) << " / " << humanBytes(p.bytesTotal);
        if (secs > 0.2)
            std::cerr << "  " << std::fixed << std::setprecision(1)
                      << (static_cast<double>(p.bytesDone) / 1048576.0 / secs) << " MiB/s";
        std::cerr << "   " << std::flush;
        return true;
    }
};

ProgressCallback makeProgress(Context& c, ProgressPrinter& pp) {
    pp.enabled = !c.json() && ::isatty(STDERR_FILENO);
    return [&pp](const TransferProgress& p) { return pp(p); };
}

// ---- commands ----------------------------------------------------------------------------------

int cmdDevices(Context& c) {
    auto factory = makeFactory(specOf(c.args));
    auto devices = factory->enumerate();
    Json arr = Json::array();
    for (auto& d : devices) {
        Json j = Json::object();
        j["id"] = d.id;
        char vp[16];
        std::snprintf(vp, sizeof vp, "%04x:%04x", d.vendorId, d.productId);
        j["usbId"] = vp;
        j["manufacturer"] = d.manufacturer;
        j["model"] = d.model;
        j["connected"] = d.connected;
        j["mtpReady"] = Json();  // not probed: opening a session would claim the phone
        auto st = readMountState(d.id);
        const bool mounted = st && processAlive(st->pid);
        j["mountedByMTPort"] = mounted;
        j["ownedByAnotherProcess"] = DeviceLock::isHeld(d.id);
        if (mounted) j["mountPoint"] = st->mountPoint;
        arr.push(std::move(j));
    }
    if (c.json()) {
        Json root = Json::object();
        root["backend"] = specOf(c.args).kind;
        root["devices"] = std::move(arr);
        c.out << root.dump() << "\n";
        return kOk;
    }
    if (devices.empty()) {
        c.out << "No MTP-capable USB device detected.\n"
                 "Plug the phone in with a data cable, unlock it and choose USB > File transfer (MTP).\n";
        return kOk;
    }
    c.out << "ID                  USB        NAME                          STATE\n";
    for (auto& d : arr.items()) {
        const std::string name = d.find("manufacturer")->asString() + " " + d.find("model")->asString();
        std::string state = "detected (MTP session not probed)";
        if (d.find("mountedByMTPort")->asBool())
            state = "mounted at " + d.find("mountPoint")->asString();
        else if (d.find("ownedByAnotherProcess")->asBool())
            state = "in use by another MTPort process";
        c.out << std::left << std::setw(20) << d.find("id")->asString() << std::setw(11) << d.find("usbId")->asString()
              << std::setw(30) << name.substr(0, 28) << state << "\n";
    }
    return kOk;
}

int cmdInfo(Context& c) {
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const auto d = dev->descriptor();
    const auto caps = dev->capabilities();
    const auto storages = dev->storages();
    if (c.json()) {
        Json j = Json::object();
        j["id"] = d.id;
        j["manufacturer"] = d.manufacturer;
        j["model"] = d.model;
        j["mtpReady"] = d.mtpReady;
        Json cj = Json::object();
        cj["getPartialObject"] = caps.getPartialObject;
        cj["sendPartialObject"] = caps.sendPartialObject;
        cj["editObjects"] = caps.editObjects;
        cj["moveObject"] = caps.moveObject;
        cj["copyObject"] = caps.copyObject;
        cj["renameObject"] = caps.renameObject;
        cj["deleteObject"] = caps.deleteObject;
        j["capabilities"] = std::move(cj);
        Json sa = Json::array();
        for (auto& s : storages) {
            Json sj = Json::object();
            sj["id"] = s.id;
            sj["name"] = s.name;
            sj["capacityBytes"] = s.capacityBytes;
            sj["freeBytes"] = s.freeBytes;
            sj["readOnly"] = s.readOnly;
            sa.push(std::move(sj));
        }
        j["storages"] = std::move(sa);
        c.out << j.dump() << "\n";
        return kOk;
    }
    c.out << "Device:       " << d.manufacturer << " " << d.model << "\nId:           " << d.id
          << "\nMTP session:  open\nCapabilities: partialRead=" << caps.getPartialObject
          << " partialWrite=" << caps.sendPartialObject << " edit=" << caps.editObjects << " move=" << caps.moveObject
          << " copy=" << caps.copyObject << " rename=" << caps.renameObject << " delete=" << caps.deleteObject << "\n";
    if (!caps.getPartialObject)
        c.out << "              (no partial reads: mounts fall back to a staged full download per file)\n";
    c.out << "Storages:\n";
    for (auto& s : storages)
        c.out << "  " << s.id << "  " << s.name << "  " << humanBytes(s.capacityBytes) << " total, "
              << humanBytes(s.freeBytes) << " free" << (s.readOnly ? "  [read-only]" : "") << "\n";
    return kOk;
}

int cmdLs(Context& c) {
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    const auto comps = splitPathOrThrow(c.args.opt("--path", "/"));
    ObjectId dir = kRootObject;
    if (auto n = resolveNode(*dev, storage, comps)) {
        if (!n->isDirectory) {
            if (c.json())
                c.out << Json(Json::Array{nodeJson(*n)}).dump() << "\n";
            else
                c.out << "file  " << humanBytes(n->sizeBytes) << "  " << n->name << "\n";
            return kOk;
        }
        dir = n->objectId;
    }
    auto nodes = dev->list(storage, dir);
    std::sort(nodes.begin(), nodes.end(), [](const FileNode& a, const FileNode& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return a.name < b.name;
    });
    if (c.json()) {
        Json arr = Json::array();
        for (auto& n : nodes) arr.push(nodeJson(n));
        c.out << arr.dump() << "\n";
        return kOk;
    }
    for (auto& n : nodes) {
        c.out << (n.isDirectory ? "dir   " : "file  ") << std::right << std::setw(10)
              << (n.isDirectory ? "-" : humanBytes(n.sizeBytes)) << "  "
              << (n.modifiedAt ? isoTime(*n.modifiedAt) : std::string("-                   ")) << "  " << n.name
              << (n.isDirectory ? "/" : "") << "\n";
    }
    c.err << nodes.size() << " entries\n";
    return kOk;
}

int cmdPull(Context& c) {
    const std::string src = c.args.opt("--src"), dst = c.args.opt("--dst");
    if (src.empty() || dst.empty()) usageError("pull needs --src <path on phone> and --dst <local path>");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    auto node = resolveNode(*dev, storage, splitPathOrThrow(src));
    if (!node) throw MtpError(MtpErrc::IsADirectory, "the storage root is not a file");
    if (node->isDirectory)
        throw MtpError(MtpErrc::IsADirectory, "pull copies single files; '" + node->name + "' is a folder");

    fs::path target = dst;
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        if (!isValidObjectName(node->name))
            throw MtpError(MtpErrc::InvalidVirtualPath,
                           "the phone's file name is not safe to use locally; give --dst a file path");
        target /= node->name;
    }
    if (!fs::exists(target.parent_path().empty() ? fs::path(".") : target.parent_path(), ec))
        throw MtpError(MtpErrc::IoError, "destination folder does not exist");

    ProgressPrinter pp{};
    auto progress = makeProgress(c, pp);
    const auto t0 = std::chrono::steady_clock::now();
    dev->download(node->objectId, target, progress);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (pp.enabled) c.err << "\n";
    const std::string sha = sha256File(target);
    const uint64_t size = fs::file_size(target);
    if (size != node->sizeBytes)
        throw MtpError(MtpErrc::IoError, "local size differs from the size the phone reported");
    if (c.json()) {
        Json j = Json::object();
        j["destination"] = target.string();
        j["sizeBytes"] = size;
        j["seconds"] = secs;
        j["mibPerSecond"] = secs > 0 ? static_cast<double>(size) / 1048576.0 / secs : 0.0;
        j["sha256"] = sha;
        c.out << j.dump() << "\n";
    } else {
        c.out << "Pulled " << humanBytes(size) << " to " << target.string() << " in " << std::fixed
              << std::setprecision(2) << secs << " s\nsha256 " << sha << "\n";
    }
    return kOk;
}

int cmdPush(Context& c) {
    const std::string src = c.args.opt("--src"), dst = c.args.opt("--dst");
    if (src.empty() || dst.empty()) usageError("push needs --src <local file> and --dst <path on phone>");
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) throw MtpError(MtpErrc::InvalidArgument, "--src must be a regular file");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);

    auto comps = splitPathOrThrow(dst);
    std::string finalName;
    if (!dst.empty() && dst.back() == '/')
        finalName = fs::path(src).filename().string();
    else if (!comps.empty()) {
        finalName = comps.back();
        comps.pop_back();
    }
    if (!isValidObjectName(finalName)) throw MtpError(MtpErrc::InvalidVirtualPath, "invalid destination file name");
    ObjectId parent = kRootObject;
    if (auto p = resolveNode(*dev, storage, comps)) {
        if (!p->isDirectory) throw MtpError(MtpErrc::NotADirectory, "destination parent is not a folder");
        parent = p->objectId;
    }

    ProgressPrinter pp{};
    auto progress = makeProgress(c, pp);
    const uint64_t size = fs::file_size(src);
    const auto t0 = std::chrono::steady_clock::now();
    // upload() refuses to replace an existing object (FileAlreadyExists): there is no overwrite path.
    const ObjectId id = dev->upload(storage, parent, src, finalName, progress);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (pp.enabled) c.err << "\n";
    const FileNode created = dev->stat(id);
    if (created.sizeBytes != size)
        throw MtpError(MtpErrc::IoError, "the phone reports a different size after upload (" +
                                             std::to_string(created.sizeBytes) + " vs " + std::to_string(size) + ")");

    std::string verified = "not requested";
    if (c.args.has("--verify")) {
        const fs::path tmpDir = fs::temp_directory_path() / ("mtport-verify-" + std::to_string(::getpid()));
        fs::create_directories(tmpDir);
        const fs::path back = tmpDir / "roundtrip.bin";
        dev->download(id, back, nullptr);
        verified = sha256File(back) == sha256File(src) ? "PASS (sha256 equal)" : "FAIL (sha256 differs)";
        fs::remove_all(tmpDir, ec);  // our own temporary directory
        if (verified[0] == 'F')
            throw MtpError(MtpErrc::IoError, "round-trip verification failed: the uploaded bytes differ");
    }
    if (c.json()) {
        Json j = Json::object();
        j["objectId"] = id;
        j["sizeBytes"] = size;
        j["seconds"] = secs;
        j["roundTrip"] = verified;
        c.out << j.dump() << "\n";
    } else {
        c.out << "Pushed " << humanBytes(size) << " as object " << id << " in " << std::fixed << std::setprecision(2)
              << secs << " s; round-trip: " << verified << "\n";
    }
    return kOk;
}

int cmdMkdir(Context& c) {
    const std::string path = c.args.opt("--path");
    if (path.empty()) usageError("mkdir needs --path");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    auto comps = splitPathOrThrow(path);
    if (comps.empty()) usageError("cannot create the storage root");
    ObjectId cur = kRootObject;
    ObjectId last = 0;
    for (size_t i = 0; i < comps.size(); ++i) {
        const bool final = i + 1 == comps.size();
        std::optional<FileNode> existing;
        const std::string want = normalizeNfc(comps[i]);
        for (auto& n : dev->list(storage, cur))
            if (normalizeNfc(n.name) == want) {
                existing = n;
                break;
            }
        if (existing) {
            if (final) throw MtpError(MtpErrc::FileAlreadyExists, "'" + comps[i] + "' already exists");
            if (!existing->isDirectory) throw MtpError(MtpErrc::NotADirectory, "'" + comps[i] + "' is a file");
            cur = existing->objectId;
            continue;
        }
        if (!final && !c.args.has("--parents"))
            throw MtpError(MtpErrc::ObjectNotFound, "parent '" + comps[i] + "' does not exist (use --parents)");
        cur = last = dev->mkdir(storage, cur, comps[i]);
    }
    if (c.json())
        c.out << Json(Json::Object{{"objectId", Json(last)}}).dump() << "\n";
    else
        c.out << "Created folder (object " << last << ")\n";
    return kOk;
}

int cmdRm(Context& c) {
    const std::string path = c.args.opt("--path");
    if (path.empty()) usageError("rm needs --path");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    auto node = resolveNode(*dev, storage, splitPathOrThrow(path));
    if (!node) throw MtpError(MtpErrc::InvalidArgument, "cannot remove a storage root");
    if (node->isDirectory && !c.args.has("--dir"))
        throw MtpError(MtpErrc::IsADirectory, "'" + node->name + "' is a folder; pass --dir to remove an EMPTY folder");
    dev->remove(node->objectId, node->isDirectory);  // exactly this object; folders must be empty
    c.out << "Removed " << (node->isDirectory ? "folder" : "file") << " (object " << node->objectId << ")\n";
    return kOk;
}

int cmdMv(Context& c) {
    const std::string path = c.args.opt("--path"), name = c.args.opt("--new-name");
    if (path.empty() || name.empty()) usageError("mv needs --path and --new-name (rename within the same folder)");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    auto node = resolveNode(*dev, storage, splitPathOrThrow(path));
    if (!node) throw MtpError(MtpErrc::InvalidArgument, "cannot rename a storage root");
    dev->rename(node->objectId, name);
    c.out << "Renamed (object " << node->objectId << ")\n";
    return kOk;
}

int cmdMount(Context& c) {
    auto factory = makeFactory(specOf(c.args));
    const DeviceId id = pickDevice(c, *factory);
    if (DeviceLock::isHeld(id)) {
        if (auto st = readMountState(id); st && processAlive(st->pid)) {
            c.out << "Already mounted at " << st->mountPoint << " (pid " << st->pid << ")\n";
            return kOk;
        }
        throw MtpError(MtpErrc::DeviceBusy, "another process currently owns this phone's MTP session");
    }
    const fs::path worker = findSibling("mtport-mount");
    if (worker.empty())
        throw MtpError(MtpErrc::MountBackendUnavailable,
                       "mtport-mount was not built (macFUSE headers/libfuse not found at configure time); see "
                       "docs/SETUP_MACOS.md");

    std::vector<std::string> argv = {worker.string(), "--device", id, "--backend", c.args.opt("--backend", "real")};
    if (auto r = c.args.opt("--fake-root"); !r.empty()) {
        argv.push_back("--fake-root");
        argv.push_back(r);
    }
    if (c.args.has("--fake-no-partial-read")) argv.push_back("--fake-no-partial-read");
    if (c.args.has("--read-write") && c.args.has("--read-only")) usageError("choose one of --read-only / --read-write");
    argv.push_back(c.args.has("--read-write") ? "--read-write" : "--read-only");
    if (auto t = c.args.opt("--transport"); !t.empty()) {
        argv.push_back("--transport");
        argv.push_back(t);
    }
    if (c.args.has("--metadata-passthrough")) argv.push_back("--metadata-passthrough");
    if (c.args.has("--accept-metadata-noops")) argv.push_back("--accept-metadata-noops");

    if (c.args.has("--foreground")) {
        argv.push_back("--foreground");
        const auto r = runProcess(argv, 24 * 3600);  // blocks until unmounted
        c.out << r.output;
        return r.exitCode == 0 ? kOk : kGeneric;
    }

    removeMountState(id);
    ensurePrivateDir(runtimeDir());
    const fs::path log = runtimeDir() / ("mount-" + safeName(id) + ".log");
    const pid_t pid = spawnDetached(argv, log);
    if (pid < 0) throw MtpError(MtpErrc::IoError, "cannot start mtport-mount");
    for (int i = 0; i < 300; ++i) {  // up to ~30 s: macOS may show the FSKit approval flow
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto st = readMountState(id)) {
            if (st->phase == "mounted") {
                if (c.json())
                    c.out << st->toJson().dump() << "\n";
                else
                    c.out << "Mounted " << st->model << " at " << st->mountPoint
                          << (st->readOnly ? " (read-only)" : " (read-write)") << "\n";
                return kOk;
            }
            if (st->phase == "error")
                throw MtpError(st->error.rfind("MountBackendUnavailable", 0) == 0 ? MtpErrc::MountBackendUnavailable
                                                                                    : MtpErrc::MtpNotReady,
                               "mount failed: " + st->error);
        }
        if (!processAlive(pid) && !readMountState(id)) break;
    }
    throw MtpError(MtpErrc::MtpNotReady, "the mount worker did not report a mounted volume; see " + log.string());
}

int cmdUnmount(Context& c) {
    auto spec = specOf(c.args);
    DeviceId id = c.args.opt("--device");
    if (id.empty()) {
        auto live = listMountStates(true);
        if (live.size() != 1)
            usageError("pass --device <id> (" + std::to_string(live.size()) + " MTPort mounts are active)");
        id = live.front().deviceId;
    }
    auto st = readMountState(id);
    if (!st || !processAlive(st->pid))
        throw MtpError(MtpErrc::ObjectNotFound, "this device is not mounted by MTPort");
    // diskutil is the supported way to unmount a FUSE/FSKit volume without elevated privileges.
    std::vector<std::string> argv = {"/usr/sbin/diskutil", "unmount"};
    if (c.args.has("--force")) argv.push_back("force");
    argv.push_back(st->mountPoint);
    const auto r = runProcess(argv, 60);
    if (r.exitCode != 0)
        throw MtpError(MtpErrc::DeviceBusy, "unmount failed: " + r.output +
                                                " (close Finder windows/files using the volume, or pass --force)");
    for (int i = 0; i < 100 && processAlive(st->pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (processAlive(st->pid))
        throw MtpError(MtpErrc::DeviceBusy,
                       "volume unmounted but the worker (pid " + std::to_string(st->pid) + ") is still shutting down");
    c.out << "Unmounted " << st->mountPoint << " - safe to disconnect.\n";
    return kOk;
}

int cmdStatus(Context& c) {
    const auto states = listMountStates(true);
    if (c.json()) {
        Json arr = Json::array();
        for (auto& s : states) arr.push(s.toJson());
        Json root = Json::object();
        root["mounts"] = std::move(arr);
        c.out << root.dump() << "\n";
        return kOk;
    }
    if (states.empty()) {
        c.out << "No MTPort mounts are active.\n";
        return kOk;
    }
    for (auto& s : states) {
        c.out << s.model << "  [" << s.phase << "]  " << s.mountPoint << (s.readOnly ? "  read-only" : "  read-write")
              << "  pid " << s.pid << "\n"
              << "  read " << humanBytes(s.bytesRead) << ", staged " << humanBytes(s.bytesStaged) << ", uploaded "
              << humanBytes(s.bytesUploaded) << ", uploads ok/failed/active " << s.uploadsOk << "/" << s.uploadsFailed
              << "/" << s.uploadsActive << ", pending " << s.pendingUploads << ", recoverable " << s.recoverableUploads
              << "\n";
        if (!s.error.empty()) c.out << "  error: " << s.error << "\n";
    }
    return kOk;
}

int cmdBench(Context& c) {
    if (c.args.opt("--direction", "pull") != "pull")
        usageError("bench only supports --direction pull (it never writes to the phone)");
    const std::string file = c.args.opt("--file");
    if (file.empty()) usageError("bench needs --file <path on phone>");
    const int runs = std::max(1, std::atoi(c.args.opt("--runs", "1").c_str()));
    const std::string mode = c.args.opt("--mode", "download");
    auto factory = makeFactory(specOf(c.args));
    auto dev = openDevice(c, *factory);
    const StorageId storage = parseStorage(c, *dev);
    auto node = resolveNode(*dev, storage, splitPathOrThrow(file));
    if (!node || node->isDirectory) throw MtpError(MtpErrc::IsADirectory, "--file must name a file");

    Json results = Json::array();
    const fs::path tmpDir = fs::temp_directory_path() / ("mtport-bench-" + std::to_string(::getpid()));
    fs::create_directories(tmpDir);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{tmpDir};  // our own temp dir only

    for (int r = 0; r < runs; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t bytes = 0;
        std::string sha;
        if (mode == "download") {
            const fs::path out = tmpDir / ("run" + std::to_string(r) + ".bin");
            ProgressPrinter pp{};
            auto progress = makeProgress(c, pp);
            dev->download(node->objectId, out, progress);
            bytes = fs::file_size(out);
            sha = sha256File(out);
            std::error_code ec;
            fs::remove(out, ec);
        } else if (mode == "partial") {
            if (!dev->capabilities().getPartialObject)
                throw MtpError(MtpErrc::OperationUnsupported, "device has no partial reads");
            const size_t chunk =
                static_cast<size_t>(std::max(1, std::atoi(c.args.opt("--chunk-kib", "1024").c_str()))) * 1024;
            const uint64_t limit = c.args.opt("--max-bytes").empty()
                                       ? node->sizeBytes
                                       : std::strtoull(c.args.opt("--max-bytes").c_str(), nullptr, 10);
            std::vector<std::byte> buf(chunk);
            Sha256 h;
            while (bytes < std::min<uint64_t>(limit, node->sizeBytes)) {
                if (g_cancel) throw MtpError(MtpErrc::Cancelled, "interrupted");
                const size_t n = dev->read(node->objectId, bytes,
                                           std::span<std::byte>(buf).first(static_cast<size_t>(
                                               std::min<uint64_t>(chunk, node->sizeBytes - bytes))));
                if (n == 0) throw MtpError(MtpErrc::IoError, "premature EOF");
                h.update(buf.data(), n);
                bytes += n;
            }
            sha = h.hexDigest();
        } else {
            usageError("--mode must be download or partial");
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Json j = Json::object();
        j["run"] = r + 1;
        j["bytes"] = bytes;
        j["seconds"] = secs;
        j["mibPerSecond"] = secs > 0 ? static_cast<double>(bytes) / 1048576.0 / secs : 0.0;
        j["sha256"] = sha;
        results.push(std::move(j));
    }
    Json root = Json::object();
    root["mode"] = mode;
    root["backend"] = specOf(c.args).kind;
    root["runs"] = std::move(results);
    if (c.json()) {
        c.out << root.dump() << "\n";
        return kOk;
    }
    for (auto& j : root.find("runs")->items())
        c.out << "run " << j.find("run")->asInt() << ": " << humanBytes(static_cast<uint64_t>(j.find("bytes")->asInt()))
              << " in " << std::fixed << std::setprecision(2) << j.find("seconds")->asDouble() << " s ("
              << j.find("mibPerSecond")->asDouble() << " MiB/s) sha256 " << j.find("sha256")->asString() << "\n";
    return kOk;
}

int cmdDoctor(Context& c) {
    const auto checks = runDoctor(specOf(c.args));
    bool anyFail = false;
    for (auto& k : checks) anyFail |= k.status == CheckStatus::Fail;
    if (c.json()) {
        c.out << doctorToJson(checks).dump() << "\n";
    } else {
        for (auto& k : checks) {
            c.out << "[" << toString(k.status) << "] " << k.id << ": " << k.summary << "\n";
            if (!k.remedy.empty()) c.out << "       -> " << k.remedy << "\n";
        }
    }
    return anyFail ? kGeneric : kOk;
}

void printHelp(std::ostream& o) {
    o << "mtport " << kVersion
      << " - Android MTP phones in Finder (developer preview)\n\n"
         "  mtport doctor [--json]\n"
         "  mtport devices [--json]\n"
         "  mtport info  [--device ID] [--json]\n"
         "  mtport ls    [--device ID] [--storage N] --path /DCIM [--json]\n"
         "  mtport pull  [--device ID] [--storage N] --src /DCIM/a.jpg --dst ~/Desktop/a.jpg\n"
         "  mtport push  [--device ID] [--storage N] --src file --dst /Download/file [--verify]\n"
         "  mtport mkdir [--device ID] [--storage N] --path /Download/New [--parents]\n"
         "  mtport rm    [--device ID] [--storage N] --path /Download/file [--dir]\n"
         "  mtport mv    [--device ID] [--storage N] --path /Download/a --new-name b\n"
         "  mtport mount [--device ID] [--foreground] [--read-only | --read-write] [--transport webdav|fskit]\n"
         "  mtport unmount [--device ID] [--force]\n"
         "  mtport status [--json]\n"
         "  mtport bench --device ID --direction pull --file /Download/benchmark.bin [--mode download|partial] "
         "[--runs N]\n\n"
         "Paths are inside the selected MTP storage, not on this Mac. push/mkdir/mv never replace existing files.\n"
         "Test backend: --backend fake --fake-root DIR (mock phone backed by a host folder; not for real use).\n";
}

}  // namespace

// ---- public API ------------------------------------------------------------------------------------

Args parseArgs(int argc, const char* const* argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            if (kBoolFlags.count(s)) {
                a.flags.insert(s);
                continue;
            }
            if (kValueOptions.count(s)) {
                if (i + 1 >= argc) throw std::invalid_argument("option " + s + " needs a value");
                a.options[s] = argv[++i];
                continue;
            }
            throw std::invalid_argument("unknown option " + s);
        }
        if (a.command.empty() && kCommands.count(s)) {
            a.command = s;
            continue;
        }
        a.positional.push_back(s);
    }
    if (!a.positional.empty() && a.command.empty())
        throw std::invalid_argument("unknown command '" + a.positional.front() + "'");
    if (!a.positional.empty()) throw std::invalid_argument("unexpected argument '" + a.positional.front() + "'");
    return a;
}

BackendSpec backendFromArgs(const Args& args) {
    BackendSpec s;
    s.kind = args.opt("--backend", "real");
    if (auto r = args.opt("--fake-root"); !r.empty())
        s.fakeRoot = r;
    else if (const char* e = std::getenv("MTPORT_FAKE_ROOT"))
        s.fakeRoot = e;
    s.fakePartialRead = !args.has("--fake-no-partial-read");
    return s;
}

int exitCodeFor(MtpErrc c) {
    switch (c) {
        case MtpErrc::DeviceBusy:
            return kBusy;
        case MtpErrc::DeviceDisconnected:
            return kNoDevice;
        case MtpErrc::MtpNotReady:
            return kNotReady;
        case MtpErrc::ObjectNotFound:
            return kNotFound;
        case MtpErrc::FileAlreadyExists:
            return kExists;
        case MtpErrc::MountBackendUnavailable:
            return kBackendUnavailable;
        case MtpErrc::InvalidArgument:
        case MtpErrc::InvalidVirtualPath:
            return kUsage;
        default:
            return kGeneric;
    }
}

int run(const Args& args, std::ostream& out, std::ostream& err) {
    if (args.has("--version")) {
        out << "mtport " << kVersion;
        if (auto v = libmtpVersionString(); !v.empty()) out << " (libmtp " << v << ")";
        out << "\n";
        return kOk;
    }
    if (args.has("--help") || args.command.empty()) {
        printHelp(args.command.empty() && !args.has("--help") ? err : out);
        return args.has("--help") ? kOk : kUsage;
    }
    struct sigaction sa{};
    sa.sa_handler = onSigint;
    ::sigaction(SIGINT, &sa, nullptr);

    Context c{args, out, err};
    try {
        const std::string& cmd = args.command;
        if (cmd == "doctor") return cmdDoctor(c);
        if (cmd == "devices") return cmdDevices(c);
        if (cmd == "info") return cmdInfo(c);
        if (cmd == "ls") return cmdLs(c);
        if (cmd == "pull") return cmdPull(c);
        if (cmd == "push") return cmdPush(c);
        if (cmd == "mkdir") return cmdMkdir(c);
        if (cmd == "rm") return cmdRm(c);
        if (cmd == "mv") return cmdMv(c);
        if (cmd == "mount") return cmdMount(c);
        if (cmd == "unmount") return cmdUnmount(c);
        if (cmd == "status") return cmdStatus(c);
        if (cmd == "bench") return cmdBench(c);
        return kUsage;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const MtpError& e) {
        if (args.has("--json")) {
            Json j = Json::object();
            Json ej = Json::object();
            ej["code"] = std::string(toString(e.code()));
            ej["message"] = e.detail();
            j["error"] = std::move(ej);
            out << j.dump() << "\n";
        } else {
            err << "mtport: " << errorMessageFor(e.code(), e.detail()) << "\n";
        }
        return exitCodeFor(e.code());
    } catch (const std::exception& e) {
        err << "mtport: " << e.what() << "\n";
        return kGeneric;
    }
}

}  // namespace mtport::cli
