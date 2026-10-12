#include "Process.hpp"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

extern char** environ;

namespace mtport {

namespace {
std::vector<char*> toArgv(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}
}  // namespace

ProcessResult runProcess(const std::vector<std::string>& argv, int timeoutSeconds) {
    ProcessResult res;
    if (argv.empty()) return res;
    int fds[2];
    if (::pipe(fds) != 0) return res;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    auto args = toArgv(argv);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        res.output = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc);
        return res;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    char buf[4096];
    int status = 0;
    bool exited = false;
    while (true) {
        ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) {
            if (res.output.size() < (1u << 20)) res.output.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
        }
        if (exited) {
            while ((n = ::read(fds[0], buf, sizeof buf)) > 0)
                if (res.output.size() < (1u << 20)) res.output.append(buf, static_cast<size_t>(n));
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            res.output += "\n[timed out]";
            ::close(fds[0]);
            return res;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fds[0]);
    if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
    return res;
}

pid_t spawnDetached(const std::vector<std::string>& argv, const std::filesystem::path& logFile) {
    if (argv.empty()) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (logFile.empty()) {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // A new process group (not a new session): the child survives the terminal's Ctrl-C but stays in the user's
    // login/audit session, which the macOS network-mount machinery (NetAuth agent) requires. POSIX_SPAWN_SETSID made
    // NetFSMountURLSync block forever.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    auto args = toArgv(argv);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, argv[0].c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

std::filesystem::path executableDir() {
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    auto p = std::filesystem::canonical(buf, ec);
    return ec ? std::filesystem::path{} : p.parent_path();
}

std::filesystem::path findSibling(const std::string& name) {
    std::error_code ec;
    // Installed layout: all tools side by side. Build tree: native/{cli,agent,mount}/ are siblings.
    for (const char* rel : {".", "../mount", "../agent", "../cli"}) {
        auto candidate = (executableDir() / rel / name).lexically_normal();
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    if (const char* path = std::getenv("PATH")) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == std::string::npos) end = p.size();
            auto c = std::filesystem::path(p.substr(start, end - start)) / name;
            if (!p.substr(start, end - start).empty() && std::filesystem::exists(c, ec)) return c;
            start = end + 1;
        }
    }
    return {};
}

bool processAlive(pid_t pid) {
    if (pid <= 0) return false;
    // kill(pid, 0) also succeeds for a zombie (exited, not yet reaped); that is not a live worker.
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc info {};
    size_t len = sizeof info;
    if (::sysctl(mib, 4, &info, &len, nullptr, 0) == 0 && len > 0) return info.kp_proc.p_stat != SZOMB;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

}  // namespace mtport
