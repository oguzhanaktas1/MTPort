#include "IpcServer.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <mutex>
#include <cstring>

#include "mtport/MtpError.hpp"
#include "mtport/RuntimePaths.hpp"

namespace mtport {

namespace {

sockaddr_un makeAddr(const std::filesystem::path& p) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (p.string().size() >= sizeof addr.sun_path)
        throw MtpError(MtpErrc::InvalidArgument, "socket path too long for AF_UNIX: " + p.string());
    std::strncpy(addr.sun_path, p.c_str(), sizeof addr.sun_path - 1);
    return addr;
}

// A peer that vanished must not kill the process with SIGPIPE.
// SO_NOSIGPIPE alone was observed not to suppress SIGPIPE on AF_UNIX sockets whose peer closed with unread data
// (a flaky crash in the oversize-request test), so the signal is ignored process-wide as well: a write to a
// vanished peer must surface as EPIPE, never kill the agent or the CLI.
void ignoreSigpipe() {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

void noSigpipe(int fd) {
    ignoreSigpipe();
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
}

bool writeAll(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        const ssize_t n = ::write(fd, s.data() + done, s.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

// Reads until '\n' (not included). Returns false on timeout / EOF before newline / oversize.
bool readLine(int fd, std::string& out, int timeoutMs) {
    out.clear();
    char buf[1024];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, timeoutMs);
        if (r <= 0) return false;
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') return true;
            out.push_back(buf[i]);
            if (out.size() > kMaxIpcMessageBytes) return false;
        }
    }
}

}  // namespace

IpcServer::IpcServer(std::filesystem::path socketPath, Handler handler)
    : path_(std::move(socketPath)), handler_(std::move(handler)) {}

IpcServer::~IpcServer() {
    stop();
}

void IpcServer::start() {
    ensurePrivateDir(path_.parent_path());
    // Another agent alive on this path?
    {
        UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM, 0));
        sockaddr_un addr = makeAddr(path_);
        if (probe.valid() && ::connect(probe.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0)
            throw MtpError(MtpErrc::DeviceBusy, "an MTPort agent is already running");
    }
    struct stat st{};
    if (::lstat(path_.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
            throw MtpError(MtpErrc::IoError, path_.string() + " exists and is not our socket");
        ::unlink(path_.c_str());  // stale socket of a crashed agent
    }
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "socket() failed");
    ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    sockaddr_un addr = makeAddr(path_);
    const mode_t old = ::umask(077);  // the socket file is created 0600
    const int rc = ::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (rc != 0) throw MtpError(MtpErrc::IoError, std::string("bind failed: ") + std::strerror(errno));
    ::chmod(path_.c_str(), 0600);
    if (::listen(fd.get(), 8) != 0) throw MtpError(MtpErrc::IoError, "listen failed");
    if (::pipe(wakePipe_) != 0) throw MtpError(MtpErrc::IoError, "pipe failed");
    listenFd_ = std::move(fd);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
}

void IpcServer::stop() {
    if (!running_.exchange(false)) return;
    [[maybe_unused]] ssize_t n = ::write(wakePipe_[1], "x", 1);
    if (thread_.joinable()) thread_.join();
    ::close(wakePipe_[0]);
    ::close(wakePipe_[1]);
    wakePipe_[0] = wakePipe_[1] = -1;
    listenFd_.reset();
    ::unlink(path_.c_str());
}

void IpcServer::loop() {
    while (running_) {
        pollfd fds[2] = {{listenFd_.get(), POLLIN, 0}, {wakePipe_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents & POLLIN) return;
        if (!(fds[0].revents & POLLIN)) continue;
        UniqueFd client(::accept(listenFd_.get(), nullptr, nullptr));
        if (!client.valid()) continue;
        noSigpipe(client.get());
        serve(client.get());
    }
}

void IpcServer::serve(int fd) {
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::geteuid())
        return;  // only the logged-in user may talk to the agent
    std::string line;
    if (!readLine(fd, line, 2000)) {
        writeAll(fd,
                 "{\"error\":{\"code\":\"BadRequest\",\"message\":\"incomplete or oversized "
                 "request\"},\"protocolVersion\":1}\n");
        return;
    }
    std::string response;
    try {
        response = handler_(line);
    } catch (...) {
        response = "{\"error\":{\"code\":\"InternalError\",\"message\":\"handler failed\"},\"protocolVersion\":1}";
    }
    writeAll(fd, response + "\n");
}

std::string ipcRoundTrip(const std::filesystem::path& socketPath, const std::string& requestLine, int timeoutMs) {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) throw MtpError(MtpErrc::IoError, "socket() failed");
    noSigpipe(fd.get());
    sockaddr_un addr = makeAddr(socketPath);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        throw MtpError(MtpErrc::DeviceDisconnected, "agent is not running");
    if (!writeAll(fd.get(), requestLine + "\n")) throw MtpError(MtpErrc::IoError, "write failed");
    std::string line;
    if (!readLine(fd.get(), line, timeoutMs)) throw MtpError(MtpErrc::Timeout, "no response from the agent");
    return line;
}

}  // namespace mtport
