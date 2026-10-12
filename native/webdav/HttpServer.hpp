// Minimal HTTP/1.1 server for the loopback WebDAV bridge. It listens on 127.0.0.1 only, requires HTTP Basic
// credentials (a random per-mount password handed to the macOS mount call), and is not reachable from the network.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace mtport::webdav {

struct HttpRequest {
    std::string method;
    std::string target;                          // request-target path, still percent-encoded, query removed
    std::map<std::string, std::string> headers;  // lower-case names
    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }
    // Reads up to `n` bytes of the request body (identity or chunked). Returns 0 at the end of the body, -1 on error.
    std::function<long(void*, size_t)> readBody;
    bool hasBody{false};
    uint64_t contentLength{0};  // identity bodies only
    bool chunked{false};
};

struct HttpResponse {
    int status{200};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // used when `stream` is empty
    // Large bodies: exactly `streamLength` bytes are produced by calling `stream` with a write callback.
    uint64_t streamLength{0};
    std::function<bool(const std::function<bool(const char*, size_t)>& write)> stream;
    bool closeConnection{false};
    void set(const std::string& k, const std::string& v) { headers.emplace_back(k, v); }
};

class HttpServer {
public:
    using Handler = std::function<void(HttpRequest&, HttpResponse&)>;
    HttpServer(std::string user, std::string password, Handler handler);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    uint16_t start();  // binds 127.0.0.1:0 and returns the port
    void stop();
    size_t openConnections() const;

private:
    void acceptLoop();
    void serveConnection(int fd);

    std::string expectedAuth_;
    Handler handler_;
    int listenFd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    mutable std::mutex mutex_;
    std::set<int> clients_;
    std::atomic<size_t> active_{0};
};

std::string percentDecode(const std::string& s);   // "%41" -> "A"; invalid escapes are kept literally
std::string percentEncodePath(const std::string& s);  // keeps '/', encodes everything outside the unreserved set
std::string base64(const std::string& raw);

}  // namespace mtport::webdav
