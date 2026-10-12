#include "HttpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mtport::webdav {

namespace {

constexpr size_t kMaxHeaderBytes = 32 * 1024;
constexpr size_t kMaxConnections = 64;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* reason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 423: return "Locked";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default: return "Status";
    }
}

bool sendAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// Buffered reader over a socket.
struct Reader {
    int fd;
    std::string buf;
    size_t pos{0};
    bool fill() {
        if (pos > 0 && pos == buf.size()) {
            buf.clear();
            pos = 0;
        }
        char tmp[16384];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (static const bool trace = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; trace)
            std::fprintf(stderr, "webdav[fd %d] recv %zd bytes: %.60s\n", fd, n, n > 0 ? std::string(tmp, std::min<size_t>(n, 60)).c_str() : "(closed)");
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        return true;
    }
    long read(void* out, size_t want) {
        if (pos == buf.size() && !fill()) return -1;
        const size_t n = std::min(want, buf.size() - pos);
        std::memcpy(out, buf.data() + pos, n);
        pos += n;
        return static_cast<long>(n);
    }
    bool line(std::string& out, size_t limit) {
        out.clear();
        for (;;) {
            const size_t nl = buf.find('\n', pos);
            if (nl != std::string::npos) {
                out.assign(buf, pos, nl - pos);
                pos = nl + 1;
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (buf.size() - pos > limit) return false;
            if (!fill()) return false;
        }
    }
};

}  // namespace

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string percentEncodePath(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string base64(const std::string& raw) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (i = 0; i + 2 < raw.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8) |
                           static_cast<uint8_t>(raw[i + 2]);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (raw.size() - i == 1) {
        const uint32_t v = static_cast<uint8_t>(raw[i]) << 16;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (raw.size() - i == 2) {
        const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

HttpServer::HttpServer(std::string user, std::string password, Handler handler)
    : expectedAuth_("Basic " + base64(user + ":" + password)), handler_(std::move(handler)) {}

HttpServer::~HttpServer() { stop(); }

uint16_t HttpServer::start() {
    std::signal(SIGPIPE, SIG_IGN);
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) throw std::runtime_error("socket() failed");
    int on = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only, never a routable interface
    addr.sin_port = 0;
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listenFd_, 32) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error("cannot listen on 127.0.0.1");
    }
    socklen_t len = sizeof addr;
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return port_;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::lock_guard lk(mutex_);
        for (int fd : clients_) ::shutdown(fd, SHUT_RDWR);
    }
    for (int i = 0; i < 500 && active_.load() > 0; ++i) ::usleep(10 * 1000);
}

size_t HttpServer::openConnections() const { return active_.load(); }

void HttpServer::acceptLoop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) {
            if (!running_) return;
            continue;
        }
        if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || active_.load() >= kMaxConnections) {
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        timeval tv{120, 0};  // idle keep-alive connections are dropped after two minutes
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        {
            std::lock_guard lk(mutex_);
            clients_.insert(fd);
        }
        ++active_;
        if (static const bool trace = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; trace)
            std::fprintf(stderr, "webdav[fd %d] accepted\n", fd);
        std::thread([this, fd] {
            serveConnection(fd);
            {
                std::lock_guard lk(mutex_);
                clients_.erase(fd);
            }
            ::close(fd);
            --active_;
        }).detach();
    }
}

void HttpServer::serveConnection(int fd) {
    Reader rd{fd, {}, 0};
    for (;;) {
        std::string line;
        if (!rd.line(line, kMaxHeaderBytes)) return;
        if (line.empty()) continue;  // tolerate stray CRLF between pipelined requests
        HttpRequest req;
        {
            const size_t a = line.find(' '), b = line.rfind(' ');
            if (a == std::string::npos || b == a) return;
            req.method = line.substr(0, a);
            std::string target = line.substr(a + 1, b - a - 1);
            if (const size_t q = target.find('?'); q != std::string::npos) target.resize(q);
            if (target.rfind("http://", 0) == 0) {  // absolute-form
                const size_t slash = target.find('/', 7);
                target = slash == std::string::npos ? "/" : target.substr(slash);
            }
            req.target = target;
        }
        size_t headerBytes = 0;
        for (;;) {
            if (!rd.line(line, kMaxHeaderBytes)) return;
            if (line.empty()) break;
            headerBytes += line.size();
            if (headerBytes > kMaxHeaderBytes) return;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string v = line.substr(colon + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            req.headers[lower(line.substr(0, colon))] = v;
        }
        req.chunked = lower(req.header("transfer-encoding")).find("chunked") != std::string::npos;
        if (!req.header("content-length").empty()) req.contentLength = std::strtoull(req.header("content-length").c_str(), nullptr, 10);
        req.hasBody = req.chunked || req.contentLength > 0;

        // Body reader (identity or chunked).
        uint64_t remaining = req.contentLength;
        bool bodyDone = !req.hasBody;
        uint64_t chunkLeft = 0;
        bool chunkEnded = false;
        req.readBody = [&](void* out, size_t n) -> long {
            if (bodyDone) return 0;
            if (!req.chunked) {
                if (remaining == 0) { bodyDone = true; return 0; }
                const long got = rd.read(out, static_cast<size_t>(std::min<uint64_t>(n, remaining)));
                if (got <= 0) { bodyDone = true; return -1; }
                remaining -= static_cast<uint64_t>(got);
                if (remaining == 0) bodyDone = true;
                return got;
            }
            if (chunkEnded) { bodyDone = true; return 0; }
            if (chunkLeft == 0) {
                std::string sz;
                if (!rd.line(sz, 128)) return -1;
                chunkLeft = std::strtoull(sz.c_str(), nullptr, 16);
                if (chunkLeft == 0) {
                    while (rd.line(sz, 1024) && !sz.empty()) {}  // trailers
                    chunkEnded = bodyDone = true;
                    return 0;
                }
            }
            const long got = rd.read(out, static_cast<size_t>(std::min<uint64_t>(n, chunkLeft)));
            if (got <= 0) return -1;
            chunkLeft -= static_cast<uint64_t>(got);
            if (chunkLeft == 0) { std::string crlf; rd.line(crlf, 8); }
            return got;
        };

        if (static const bool trace = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; trace) {
            std::fprintf(stderr, "webdav> %s %s\n", req.method.c_str(), req.target.c_str());
            for (const auto& [k, v] : req.headers)
                if (k != "authorization") std::fprintf(stderr, "webdav>   %s: %s\n", k.c_str(), v.c_str());
        }
        HttpResponse res;
        const bool authed = req.header("authorization") == expectedAuth_;
        if (!authed) {
            res.status = 401;
            res.set("WWW-Authenticate", "Basic realm=\"MTPort\"");
            res.closeConnection = true;
        } else {
            if (lower(req.header("expect")) == "100-continue") {
                static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                if (!sendAll(fd, kContinue, sizeof kContinue - 1)) return;
            }
            try {
                handler_(req, res);
            } catch (const std::exception&) {
                res = HttpResponse{};
                res.status = 500;
                res.closeConnection = true;
            }
        }

        // Drain whatever body the handler left unread so the connection stays in sync; give up (close) if it is huge.
        if (!bodyDone) {
            char sink[16384];
            uint64_t drained = 0;
            while (!bodyDone && drained < (64u << 20)) {
                const long n = req.readBody(sink, sizeof sink);
                if (n <= 0) break;
                drained += static_cast<uint64_t>(n);
            }
            if (!bodyDone) res.closeConnection = true;
        }

        if (static const bool trace = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; trace)
            std::fprintf(stderr, "webdav: %s %s depth=%s -> %d\n", req.method.c_str(), req.target.c_str(),
                         req.header("depth").c_str(), res.status);  // developer aid: paths may contain file names
        const bool keepAlive = !res.closeConnection && lower(req.header("connection")) != "close";
        std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n";
        {
            char date[64];
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now, &tm);
            std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
            head += std::string("Date: ") + date + "\r\n";  // RFC 9110 requires it; macOS' WebDAV client looks at it
        }
        head += "Server: MTPort\r\n";
        const uint64_t len = res.stream ? res.streamLength : res.body.size();
        if (static const bool traceLen = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; traceLen && req.method == "GET")
            std::fprintf(stderr, "webdav:   Content-Length %llu\n", static_cast<unsigned long long>(len));
        const bool noBody = req.method == "HEAD" || res.status == 204 || res.status == 304;
        for (const auto& [k, v] : res.headers) head += k + ": " + v + "\r\n";
        if (res.status != 204) head += "Content-Length: " + std::to_string(len) + "\r\n";
        head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        if (!sendAll(fd, head.data(), head.size())) return;
        if (!noBody) {
            if (res.stream) {
                uint64_t sent = 0;
                const bool ok = res.stream([&](const char* p, size_t n) {
                    sent += n;
                    return sent <= len && sendAll(fd, p, n);
                });
                if (static const bool traceBody = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; traceBody)
                    std::fprintf(stderr, "webdav:   body %llu of %llu bytes sent%s\n", static_cast<unsigned long long>(sent),
                                 static_cast<unsigned long long>(len), ok ? "" : " (FAILED)");
                if (!ok || sent != len) return;  // body promised but not delivered: the only safe thing is to hang up
            } else if (!res.body.empty() && !sendAll(fd, res.body.data(), res.body.size())) {
                return;
            }
        }
        if (!keepAlive || !running_) return;
    }
}

}  // namespace mtport::webdav
