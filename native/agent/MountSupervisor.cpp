#include "MountSupervisor.hpp"

#include <sys/wait.h>

#include <chrono>
#include <thread>

#include "MountState.hpp"
#include "Process.hpp"
#include "mtport/DeviceLock.hpp"
#include "mtport/MtpError.hpp"
#include "mtport/RuntimePaths.hpp"

namespace mtport {

void MountSupervisor::start(const DeviceId& id, bool readOnly) {
    const auto worker = findSibling("mtport-mount");
    if (worker.empty())
        throw MtpError(MtpErrc::MountBackendUnavailable,
                       "MountBackendUnavailable: mtport-mount is not installed next to the agent (macFUSE not "
                       "available at build time)");
    if (auto st = readMountState(id); st && processAlive(st->pid) && st->phase != "error")
        throw MtpError(MtpErrc::DeviceBusy, "this phone is already mounted or mounting");
    if (DeviceLock::isHeld(id)) throw MtpError(MtpErrc::DeviceBusy, "another process owns this phone's MTP session");

    std::vector<std::string> argv = {worker.string(), "--device", id, "--backend", spec_.kind};
    if (!spec_.fakeRoot.empty()) {
        argv.push_back("--fake-root");
        argv.push_back(spec_.fakeRoot.string());
    }
    if (!spec_.fakePartialRead) argv.push_back("--fake-no-partial-read");
    argv.push_back(readOnly ? "--read-only" : "--read-write");
    removeMountState(id);
    ensurePrivateDir(runtimeDir());
    const pid_t pid = spawnDetached(argv, runtimeDir() / ("mount-" + safeName(id) + ".log"));
    if (pid < 0) throw MtpError(MtpErrc::IoError, "cannot start the mount worker");
    children_.push_back(pid);
}

void MountSupervisor::stop(const DeviceId& id, bool force) {
    auto st = readMountState(id);
    if (!st || !processAlive(st->pid))
        throw MtpError(MtpErrc::ObjectNotFound, "this phone is not mounted by MTPort");
    std::vector<std::string> argv = {"/usr/sbin/diskutil", "unmount"};
    if (force) argv.push_back("force");
    argv.push_back(st->mountPoint);
    const auto r = runProcess(argv, 60);
    if (r.exitCode != 0)
        throw MtpError(MtpErrc::DeviceBusy,
                       "unmount refused (" + r.output + "); close files that use the volume, then retry");
    for (int i = 0; i < 100 && processAlive(st->pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void MountSupervisor::reap() {
    for (auto it = children_.begin(); it != children_.end();) {
        int status = 0;
        // Only our own workers are waited for (a global waitpid(-1) would steal other threads' child results).
        it = ::waitpid(*it, &status, WNOHANG) == *it ? children_.erase(it) : std::next(it);
    }
}

}  // namespace mtport
