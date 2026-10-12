// Versioned JSON-lines control channel over a Unix domain socket.
// Control messages only: never file contents. One request line in, one response line out.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "mtport/UniqueFd.hpp"

namespace mtport {

inline constexpr size_t kMaxIpcMessageBytes = 64 * 1024;

class IpcServer {
public:
    // handler: request line (without '\n') -> response line (without '\n').
    using Handler = std::function<std::string(const std::string&)>;

    IpcServer(std::filesystem::path socketPath, Handler handler);
    ~IpcServer();
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Creates the 0700 parent directory and a 0600 socket. Throws MtpError{DeviceBusy} if another
    // agent is already serving this path; removes a stale socket file left by a crashed agent.
    void start();
    void stop();
    const std::filesystem::path& path() const { return path_; }

private:
    void loop();
    void serve(int fd);

    std::filesystem::path path_;
    Handler handler_;
    UniqueFd listenFd_;
    int wakePipe_[2]{-1, -1};
    std::thread thread_;
    std::atomic<bool> running_{false};
};

// Blocking client used by tests and by the CLI; mirrors the server's limits.
std::string ipcRoundTrip(const std::filesystem::path& socketPath, const std::string& requestLine, int timeoutMs = 3000);

}  // namespace mtport
