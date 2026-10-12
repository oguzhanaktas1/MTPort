#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <map>

#include "HttpServer.hpp"
#include "TestHarness.hpp"
#include "TestSupport.hpp"
#include "LoopbackName.hpp"
#include "WebDavMount.hpp"
#include "WebDavService.hpp"
#include "mtport/Sha256.hpp"

using namespace mtport;
using namespace mtport::webdav;
using aftest::FsFixture;

namespace {

const std::string kBase = "/MTPort-Test";
const std::string kStore = kBase + "/Internal%20Storage";

struct Reply {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;
    bool closed{false};  // the server hung up before a complete response
};

struct Rig {
    FsFixture fx;
    std::unique_ptr<WebDavService> svc;
    std::unique_ptr<HttpServer> server;
    uint16_t port{0};
    std::string auth = "Basic " + base64("mtport:s3cret");

    explicit Rig(bool readOnly = false) : fx(readOnly, true, false, aftest::noCache()) {
        svc = std::make_unique<WebDavService>(*fx.fs, "Test Phone", kBase);
        server = std::make_unique<HttpServer>("mtport", "s3cret", [this](HttpRequest& q, HttpResponse& r) { svc->handle(q, r); });
        port = server->start();
    }

    int connectFd() const {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) throw std::runtime_error("connect failed");
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        return fd;
    }

    static Reply readReply(int fd, bool head = false) {
        Reply r;
        std::string buf;
        char tmp[8192];
        size_t headerEnd;
        while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) { r.closed = true; return r; }
            buf.append(tmp, static_cast<size_t>(n));
        }
        std::string headText = buf.substr(0, headerEnd);
        std::string rest = buf.substr(headerEnd + 4);
        r.status = std::atoi(headText.substr(9, 3).c_str());
        size_t pos = headText.find("\r\n");
        while (pos != std::string::npos && pos < headText.size()) {
            const size_t next = headText.find("\r\n", pos + 2);
            const std::string line = headText.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon), v = line.substr(colon + 1);
                v.erase(0, v.find_first_not_of(' '));
                for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                r.headers[k] = v;
            }
            pos = next;
        }
        const size_t len = r.headers.count("content-length") ? std::strtoull(r.headers["content-length"].c_str(), nullptr, 10) : 0;
        if (r.status == 100) return readReply(fd, head);
        if (!head && r.status != 204) {
            while (rest.size() < len) {
                const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
                if (n <= 0) { r.closed = true; break; }
                rest.append(tmp, static_cast<size_t>(n));
            }
        }
        r.body = rest.substr(0, len);
        return r;
    }

    Reply request(const std::string& method, const std::string& path, std::map<std::string, std::string> headers = {},
                  const std::string& body = {}, bool withAuth = true) const {
        const int fd = connectFd();
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
        if (withAuth) req += "Authorization: " + auth + "\r\n";
        for (auto& [k, v] : headers) req += k + ": " + v + "\r\n";
        if (!body.empty() || method == "PUT") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "\r\n" + body;
        ::send(fd, req.data(), req.size(), 0);
        Reply r = readReply(fd, method == "HEAD");
        ::close(fd);
        return r;
    }
};

}  // namespace

AF_TEST(webdav, authentication_is_required) {
    Rig r;
    CHECK_EQ(r.request("PROPFIND", kBase + "/", {{"Depth", "0"}}, {}, false).status, 401);
    CHECK_EQ(r.request("GET", kBase + "/", {}, {}, true).status, 200);
    Rig wrong;
    wrong.auth = "Basic " + base64("mtport:wrong");
    CHECK_EQ(wrong.request("GET", kBase + "/").status, 401);
    CHECK(r.request("OPTIONS", kBase + "/", {}, {}, false).status == 401);   // even OPTIONS needs credentials
}

AF_TEST(webdav, only_loopback_is_listened_on) {
    Rig r;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    socklen_t len = sizeof a;
    CHECK_EQ(::getsockname(r.connectFd(), reinterpret_cast<sockaddr*>(&a), &len), 0);
    ::close(fd);
    CHECK_EQ(ntohl(a.sin_addr.s_addr), uint32_t{INADDR_LOOPBACK});
}

AF_TEST(webdav, options_advertises_locking_only_when_writable) {
    Rig rw;
    auto o = rw.request("OPTIONS", kBase + "/");
    CHECK_EQ(o.status, 200);
    CHECK_EQ(o.headers["dav"], std::string("1, 2"));
    CHECK(o.headers["allow"].find("PUT") != std::string::npos);
    Rig ro(true);
    auto p = ro.request("OPTIONS", kBase + "/");
    CHECK_EQ(p.headers["dav"], std::string("1"));      // class 1 only => macOS mounts it read-only
    CHECK(p.headers["allow"].find("PUT") == std::string::npos);
}

AF_TEST(webdav, propfind_lists_storages_and_folders_with_encoded_hrefs) {
    Rig r;
    auto dcim = r.fx.phone->addDir(r.fx.internal, 0, "DCIM");
    r.fx.phone->addFile(r.fx.internal, dcim, "T\xc3\xbcrk\xc3\xa7" "e \xc5\x9f\xc4\xb1k.jpg", "abc");
    r.fx.phone->addFile(r.fx.internal, 0, "a&b<c>.txt", "x");

    auto root = r.request("PROPFIND", kBase + "/", {{"Depth", "1"}});
    CHECK_EQ(root.status, 207);
    CHECK(root.body.find("<D:href>" + kBase + "/</D:href>") != std::string::npos);
    CHECK(root.body.find("<D:href>" + kStore + "/</D:href>") != std::string::npos);     // storage folder, trailing slash
    CHECK(root.body.find("<D:collection/>") != std::string::npos);
    CHECK(root.body.find("quota-available-bytes") == std::string::npos);      // withheld while mounting
    r.svc->enableQuota();
    auto withQuota = r.request("PROPFIND", kBase + "/", {{"Depth", "0"}},
                               "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop><D:quota-available-bytes/><D:quota-used-bytes/>"
                               "<D:quota/></D:prop></D:propfind>");
    CHECK(withQuota.body.find("<D:quota-available-bytes>") != std::string::npos);
    CHECK(withQuota.body.find("404 Not Found") != std::string::npos);        // the unsupported legacy "quota" gets a 404 propstat

    auto inner = r.request("PROPFIND", kStore + "/", {{"Depth", "1"}});
    CHECK_EQ(inner.status, 207);
    CHECK(inner.body.find("/DCIM/</D:href>") != std::string::npos);
    CHECK(inner.body.find("a%26b%3Cc%3E.txt") != std::string::npos);                    // percent-encoded href
    CHECK(inner.body.find("a&amp;b&lt;c&gt;.txt") != std::string::npos);               // XML-escaped displayname
    CHECK(inner.body.find("<D:getcontentlength>1</D:getcontentlength>") != std::string::npos);

    auto deep = r.request("PROPFIND", kStore + "/DCIM/", {{"Depth", "1"}});
    CHECK(deep.body.find("T%C3%BCrk%C3%A7e%20%C5%9F%C4%B1k.jpg") != std::string::npos);  // Turkish name round-trips

    auto self = r.request("PROPFIND", kStore + "/", {{"Depth", "0"}});
    CHECK(self.body.find("DCIM") == std::string::npos);                                // depth 0 = the folder only
    CHECK_EQ(r.request("PROPFIND", kStore + "/nope", {{"Depth", "0"}}).status, 404);
    CHECK_EQ(r.request("PROPFIND", "/elsewhere/", {{"Depth", "0"}}).status, 404);       // outside the served path
}

AF_TEST(webdav, get_head_and_ranges) {
    Rig r;
    r.fx.phone->addFile(r.fx.internal, 0, "f.txt", "0123456789");
    r.fx.phone->addFile(r.fx.internal, 0, "empty", "");
    auto full = r.request("GET", kStore + "/f.txt");
    CHECK_EQ(full.status, 200);
    CHECK_EQ(full.body, std::string("0123456789"));
    CHECK_EQ(full.headers["accept-ranges"], std::string("bytes"));
    auto head = r.request("HEAD", kStore + "/f.txt");
    CHECK_EQ(head.status, 200);
    CHECK_EQ(head.headers["content-length"], std::string("10"));
    CHECK(head.body.empty());
    auto mid = r.request("GET", kStore + "/f.txt", {{"Range", "bytes=2-5"}});
    CHECK_EQ(mid.status, 206);
    CHECK_EQ(mid.body, std::string("2345"));
    CHECK_EQ(mid.headers["content-range"], std::string("bytes 2-5/10"));
    CHECK_EQ(r.request("GET", kStore + "/f.txt", {{"Range", "bytes=7-"}}).body, std::string("789"));
    CHECK_EQ(r.request("GET", kStore + "/f.txt", {{"Range", "bytes=-3"}}).body, std::string("789"));
    // A range that runs past the end is refused (macOS' client would zero-pad the short answer and grow the file)...
    auto beyond = r.request("GET", kStore + "/f.txt", {{"Range", "bytes=8-99"}});
    CHECK_EQ(beyond.status, 416);
    CHECK_EQ(beyond.headers["content-range"], std::string("bytes */10"));
    CHECK_EQ(r.request("GET", kStore + "/f.txt", {{"Range", "bytes=8-9"}}).body, std::string("89"));  // ...an exact one is served
    CHECK_EQ(r.request("GET", kStore + "/f.txt", {{"Range", "bytes=10-"}}).status, 416);
    CHECK_EQ(r.request("GET", kStore + "/f.txt", {{"Range", "bytes=50-60"}}).status, 416);
    auto empty = r.request("GET", kStore + "/empty");
    CHECK_EQ(empty.status, 200);
    CHECK(empty.body.empty());
    CHECK_EQ(r.request("GET", kStore + "/missing").status, 404);
}

AF_TEST(webdav, large_file_get_matches_hash) {
    Rig r;
    std::vector<std::byte> data(3 * 1024 * 1024 + 5);
    for (size_t i = 0; i < data.size(); ++i) data[i] = std::byte{static_cast<uint8_t>((i * 2654435761u) >> 24)};
    r.fx.phone->addFile(r.fx.internal, 0, "big.bin", data);
    auto got = r.request("GET", kStore + "/big.bin");
    CHECK_EQ(got.status, 200);
    CHECK_EQ(got.body.size(), data.size());
    Sha256 a, b;
    a.update(data.data(), data.size());
    b.update(got.body.data(), got.body.size());
    CHECK_EQ(a.hexDigest(), b.hexDigest());
}

AF_TEST(webdav, put_creates_new_files_and_refuses_to_replace) {
    Rig r;
    CHECK_EQ(r.request("PUT", kStore + "/new.txt", {}, "hello phone").status, 201);
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{1});
    CHECK_EQ(r.request("GET", kStore + "/new.txt").body, std::string("hello phone"));
    // Replacing is refused and the original stays untouched.
    auto again = r.request("PUT", kStore + "/new.txt", {}, "OVERWRITE");
    CHECK_EQ(again.status, 403);
    CHECK_EQ(r.request("GET", kStore + "/new.txt").body, std::string("hello phone"));
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{1});
    // macOS creates files with an empty PUT, then fills them with LOCK + PUT: the placeholder is not uploaded on its own.
    CHECK_EQ(r.request("PUT", kStore + "/created.bin").status, 201);
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{1});
    CHECK_EQ(r.request("PROPFIND", kStore + "/created.bin", {{"Depth", "0"}}).status, 207);     // visible, 0 bytes
    CHECK_EQ(r.request("LOCK", kStore + "/created.bin", {}, "<D:lockinfo xmlns:D=\"DAV:\"/>").status, 200);
    CHECK_EQ(r.request("PUT", kStore + "/created.bin", {}, "the real content").status, 201);   // fills the placeholder
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{2});                                    // one upload, with content
    CHECK_EQ(r.request("GET", kStore + "/created.bin").body, std::string("the real content"));
    CHECK_EQ(r.request("PUT", kStore + "/created.bin", {}, "again").status, 403);               // now it is a phone file
    // Missing parent, folder target, invalid names.
    CHECK_EQ(r.request("PUT", kStore + "/nodir/x", {}, "x").status, 404);
    r.fx.phone->addDir(r.fx.internal, 0, "Folder");
    CHECK_EQ(r.request("PUT", kStore + "/Folder", {}, "x").status, 403);
}

AF_TEST(webdav, put_tiny_sizes_expect_continue_and_chunked_bodies) {
    Rig r;
    for (size_t n : {size_t{1}, size_t{2}, size_t{14}, size_t{15}, size_t{16}, size_t{4097}}) {
        std::string body(n, 'q');
        for (size_t i = 0; i < n; ++i) body[i] = static_cast<char>('a' + i % 26);
        CHECK_EQ(r.request("PUT", kStore + "/t" + std::to_string(n), {}, body).status, 201);
        CHECK_EQ(r.request("GET", kStore + "/t" + std::to_string(n)).body, body);
    }
    // Expect: 100-continue
    CHECK_EQ(r.request("PUT", kStore + "/cont.txt", {{"Expect", "100-continue"}}, "payload").status, 201);
    CHECK_EQ(r.request("GET", kStore + "/cont.txt").body, std::string("payload"));
    // Chunked request body
    {
        const int fd = r.connectFd();
        const std::string req = "PUT " + kStore + "/chunked.txt HTTP/1.1\\r\\nHost: x\\r\\nAuthorization: " + r.auth +
                                "\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n";
        std::string wire;
        wire.reserve(req.size());
        for (size_t i = 0; i < req.size(); ++i) {
            if (req[i] == '\\' && i + 1 < req.size() && req[i + 1] == 'r') { wire += '\r'; ++i; }
            else if (req[i] == '\\' && i + 1 < req.size() && req[i + 1] == 'n') { wire += '\n'; ++i; }
            else wire += req[i];
        }
        wire += "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        ::send(fd, wire.data(), wire.size(), 0);
        auto rep = Rig::readReply(fd);
        ::close(fd);
        CHECK_EQ(rep.status, 201);
        CHECK_EQ(r.request("GET", kStore + "/chunked.txt").body, std::string("hello world"));
    }
}

AF_TEST(webdav, large_put_round_trips_by_hash) {
    Rig r;
    std::string data(2 * 1024 * 1024 + 11, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 40503u) >> 5);
    CHECK_EQ(r.request("PUT", kStore + "/blob.bin", {}, data).status, 201);
    auto got = r.request("GET", kStore + "/blob.bin");
    Sha256 a, b;
    a.update(data.data(), data.size());
    b.update(got.body.data(), got.body.size());
    CHECK_EQ(a.hexDigest(), b.hexDigest());
}

AF_TEST(webdav, failed_upload_is_reported_and_not_faked) {
    Rig r;
    r.fx.phone->setStorageFree(r.fx.internal, 2);   // phone full
    auto rep = r.request("PUT", kStore + "/big.bin", {}, "0123456789");
    CHECK_EQ(rep.status, 507);
    CHECK_EQ(r.request("GET", kStore + "/big.bin").status, 404);       // never shown as existing
    CHECK_EQ(r.fx.fs->recoverableUploads().size(), size_t{1});         // the staged bytes are kept
}

AF_TEST(webdav, mkcol_delete_move_rules) {
    Rig r;
    CHECK_EQ(r.request("MKCOL", kStore + "/Docs").status, 201);
    CHECK_EQ(r.request("MKCOL", kStore + "/Docs").status, 405);                 // exists
    CHECK_EQ(r.request("MKCOL", kStore + "/a/b").status, 409);                  // parent missing
    CHECK_EQ(r.request("PUT", kStore + "/Docs/a.txt", {}, "A").status, 201);

    // DELETE never recurses: a folder with content is refused and the content survives.
    CHECK_EQ(r.request("DELETE", kStore + "/Docs").status, 409);
    CHECK_EQ(r.request("GET", kStore + "/Docs/a.txt").body, std::string("A"));
    CHECK_EQ(r.request("DELETE", kStore + "/Docs/a.txt").status, 204);
    CHECK_EQ(r.request("GET", kStore + "/Docs/a.txt").status, 404);
    CHECK_EQ(r.request("DELETE", kStore + "/Docs").status, 204);
    CHECK_EQ(r.request("DELETE", kStore + "/Docs").status, 404);
    CHECK_EQ(r.request("DELETE", kStore).status, 403);                          // a storage cannot be removed

    r.fx.phone->addFile(r.fx.internal, 0, "x.txt", "X");
    r.fx.phone->addFile(r.fx.internal, 0, "y.txt", "Y");
    r.fx.phone->addDir(r.fx.internal, 0, "Sub");
    const std::string host = "http://127.0.0.1:" + std::to_string(r.port);
    CHECK_EQ(r.request("MOVE", kStore + "/x.txt", {{"Destination", host + kStore + "/z.txt"}}).status, 201);
    CHECK_EQ(r.request("GET", kStore + "/z.txt").body, std::string("X"));
    // Moving onto an existing name would need delete-then-send: refused, both files survive.
    CHECK_EQ(r.request("MOVE", kStore + "/z.txt", {{"Destination", kStore + "/y.txt"}, {"Overwrite", "T"}}).status, 412);
    CHECK_EQ(r.request("GET", kStore + "/y.txt").body, std::string("Y"));
    CHECK_EQ(r.request("GET", kStore + "/z.txt").body, std::string("X"));
    // Cross-folder moves are not silently turned into copy+delete.
    CHECK_EQ(r.request("MOVE", kStore + "/z.txt", {{"Destination", kStore + "/Sub/z.txt"}}).status, 403);
    CHECK_EQ(r.request("MOVE", kStore + "/z.txt").status, 400);
    CHECK_EQ(r.request("COPY", kStore + "/z.txt", {{"Destination", kStore + "/c.txt"}}).status, 501);
}

AF_TEST(webdav, lock_unlock_and_proppatch) {
    Rig r;
    r.fx.phone->addFile(r.fx.internal, 0, "old.txt", "x");
    auto lock = r.request("LOCK", kStore + "/new.txt", {{"Timeout", "Second-600"}}, "<?xml version=\"1.0\"?><D:lockinfo xmlns:D=\"DAV:\"/>");
    CHECK_EQ(lock.status, 200);
    CHECK(lock.headers["lock-token"].find("opaquelocktoken:") != std::string::npos);
    CHECK_EQ(r.request("UNLOCK", kStore + "/new.txt", {{"Lock-Token", lock.headers["lock-token"]}}).status, 204);

    // Existing phone files cannot be locked for writing: macOS then refuses open(O_WRONLY) instead of caching edits.
    CHECK_EQ(r.request("LOCK", kStore + "/old.txt", {}, "<D:lockinfo xmlns:D=\"DAV:\"/>").status, 403);
    r.fx.phone->addDir(r.fx.internal, 0, "Dir");
    CHECK_EQ(r.request("LOCK", kStore + "/Dir", {}, "<D:lockinfo xmlns:D=\"DAV:\"/>").status, 200);
    CHECK_EQ(r.request("PUT", kStore + "/._meta", {}, "v1").status, 201);
    CHECK_EQ(r.request("LOCK", kStore + "/._meta", {}, "<D:lockinfo xmlns:D=\"DAV:\"/>").status, 200);
    CHECK_EQ(r.request("PUT", kStore + "/._meta", {}, "version-two").status, 201);       // RAM-only: replacing is fine
    CHECK_EQ(r.request("GET", kStore + "/._meta").body, std::string("version-two"));

    r.request("PUT", kStore + "/fresh.txt", {}, "f");
    const std::string patch = "<?xml version=\"1.0\"?><D:propertyupdate xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\"><D:set><D:prop>"
                              "<Z:Win32LastModifiedTime>x</Z:Win32LastModifiedTime></D:prop></D:set></D:propertyupdate>";
    auto ok = r.request("PROPPATCH", kStore + "/fresh.txt", {}, patch);
    CHECK_EQ(ok.status, 207);
    CHECK(ok.body.find("200 OK") != std::string::npos);                         // created here: times are a harmless no-op
    auto refused = r.request("PROPPATCH", kStore + "/old.txt", {}, patch);
    CHECK(refused.body.find("403 Forbidden") != std::string::npos);             // pre-existing file: honest refusal
}

AF_TEST(webdav, read_only_volume_refuses_every_write) {
    Rig r(true);
    r.fx.phone->addFile(r.fx.internal, 0, "f", "x");
    const size_t objs = r.fx.phone->objectCount();
    CHECK_EQ(r.request("PUT", kStore + "/n", {}, "x").status, 403);
    CHECK_EQ(r.request("MKCOL", kStore + "/d").status, 403);
    CHECK_EQ(r.request("DELETE", kStore + "/f").status, 403);
    CHECK_EQ(r.request("MOVE", kStore + "/f", {{"Destination", kStore + "/g"}}).status, 403);
    CHECK_EQ(r.request("GET", kStore + "/f").body, std::string("x"));
    CHECK_EQ(r.fx.phone->objectCount(), objs);
}

AF_TEST(webdav, hostile_requests) {
    Rig r;
    CHECK_EQ(r.request("GET", kBase + "/Internal%20Storage/..%2f..%2fetc/passwd").status == 200, false);
    CHECK_EQ(r.request("GET", kStore + "/%2e%2e/x").status, 400);               // decoded ".." is rejected
    CHECK_EQ(r.request("GET", kStore + "/a%00b").status, 400);                  // NUL
    CHECK_EQ(r.request("GET", "/etc/passwd").status, 404);                      // outside the served prefix
    CHECK_EQ(r.request("FROBNICATE", kStore + "/").status, 405);
    // Oversized header block: the server hangs up instead of buffering without bound.
    const int fd = r.connectFd();
    std::string huge = "GET / HTTP/1.1\r\nX-Pad: " + std::string(200 * 1024, 'a') + "\r\n\r\n";
    ::send(fd, huge.data(), huge.size(), 0);
    auto rep = Rig::readReply(fd);
    ::close(fd);
    CHECK(rep.closed);
    CHECK_EQ(r.request("GET", kStore + "/").status, 200);                       // server still healthy
}

AF_TEST(webdav, keep_alive_serves_several_requests_on_one_connection) {
    Rig r;
    r.fx.phone->addFile(r.fx.internal, 0, "f", "abc");
    const int fd = r.connectFd();
    for (int i = 0; i < 3; ++i) {
        const std::string req = "GET " + kStore + "/f HTTP/1.1\r\nHost: x\r\nAuthorization: " + r.auth + "\r\n\r\n";
        ::send(fd, req.data(), req.size(), 0);
        auto rep = Rig::readReply(fd);
        CHECK_EQ(rep.status, 200);
        CHECK_EQ(rep.body, std::string("abc"));
    }
    ::close(fd);
}

AF_TEST(webdav, unplugged_phone_answers_503_and_the_server_survives) {
    Rig r;
    r.fx.phone->addFile(r.fx.internal, 0, "f", "abc");
    CHECK_EQ(r.request("GET", kStore + "/f").status, 200);
    r.fx.phone->setUnplugged(true);
    r.fx.fs->markDisconnected();
    CHECK_EQ(r.request("PROPFIND", kStore + "/", {{"Depth", "1"}}).status, 503);
    CHECK_EQ(r.request("GET", kStore + "/f").status, 503);
    CHECK_EQ(r.request("PUT", kStore + "/n", {}, "x").status, 503);
}

AF_TEST(webdav, spotlight_is_told_never_to_index_the_volume) {
    Rig r;
    // Present in the root listing, readable, harmless to write or delete - and never reaches the phone.
    auto list = r.request("PROPFIND", kBase + "/", {{"Depth", "1"}});
    CHECK(list.body.find(".metadata_never_index") != std::string::npos);
    auto one = r.request("PROPFIND", kBase + "/.metadata_never_index", {{"Depth", "0"}});
    CHECK_EQ(one.status, 207);
    CHECK(one.body.find("<D:getcontentlength>0</D:getcontentlength>") != std::string::npos);
    auto get = r.request("GET", kBase + "/.metadata_never_index");
    CHECK_EQ(get.status, 200);
    CHECK(get.body.empty());
    CHECK_EQ(r.request("HEAD", kBase + "/.metadata_never_index").status, 200);
    CHECK_EQ(r.request("PUT", kBase + "/.metadata_never_index", {}, "").status, 201);
    CHECK_EQ(r.request("DELETE", kBase + "/.metadata_never_index").status, 204);
    CHECK_EQ(r.request("GET", kBase + "/.metadata_never_index").status, 200);       // still there
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{0});
    // Other probe files Spotlight asks for are plain 404s.
    CHECK_EQ(r.request("PROPFIND", kBase + "/.Spotlight-V100", {{"Depth", "0"}}).status, 404);
}

AF_TEST(webdav, finder_metadata_files_stay_in_ram) {
    Rig r;
    CHECK_EQ(r.request("PUT", kStore + "/.DS_Store", {}, "binary").status, 201);
    CHECK_EQ(r.request("PUT", kStore + "/._photo.jpg", {}, "appledouble").status, 201);
    CHECK_EQ(r.fx.phone->calls(fake::Op::Upload), size_t{0});                   // never sent to the phone
    CHECK_EQ(r.request("GET", kStore + "/.DS_Store").body, std::string("binary"));
    CHECK_EQ(r.request("DELETE", kStore + "/._photo.jpg").status, 204);
}

AF_TEST(webdav, sidebar_host_name_comes_from_the_phone_name) {
    CHECK_EQ(hostLabel("Galaxy S21 FE"), std::string("galaxy-s21-fe"));
    CHECK_EQ(loopbackHost("Galaxy S21 FE"), std::string("galaxy-s21-fe.localhost"));
    CHECK_EQ(hostLabel("\xC4\xB0\xC5\x9F\xC4\xB1k'\xC4\xB1n Telefonu"), std::string("isik-in-telefonu"));  // Işık'ın Telefonu (dotted capital İ -> i)
    CHECK_EQ(hostLabel("  --SM-G990E--  "), std::string("sm-g990e"));
    CHECK_EQ(hostLabel("\xF0\x9F\x93\xB1"), std::string("android"));  // nothing usable left
    CHECK_EQ(hostLabel(""), std::string("android"));
    CHECK_EQ(hostLabel(std::string(100, 'a')).size(), size_t{63});         // DNS label limit
}

AF_TEST(webdav, sidebar_name_keeps_the_phone_name_as_typed) {
    const std::string nb = "\xC2\xA0";  // looks like a space; plain spaces cannot be mounted by NetFS
    CHECK_EQ(displayHostLabel("Galaxy S21 FE"), "Galaxy" + nb + "S21" + nb + "FE");
    CHECK_EQ(displayHostLabel("  O\xC4\x9Fuzhan's Phone. v2 \t"), "O\xC4\x9Fuzhan's" + nb + "Phone" + nb + nb + "v2");
    CHECK_EQ(displayHostLabel(" . "), std::string(""));
    CHECK_EQ(displayHostLabel(std::string(62, 'a') + "\xC4\xB1"), std::string(62, 'a'));  // 'i' would straddle 63 bytes
    CHECK_EQ(displayHostLabel(std::string(62, 'a') + " b"), std::string(62, 'a'));           // trailing no-break space trimmed
}
