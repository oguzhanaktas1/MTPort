#include "WebDavService.hpp"

#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include "mtport/NameRules.hpp"
#include "mtport/VirtualPathMapper.hpp"

namespace mtport::webdav {

namespace {

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string httpDate(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

std::string isoDate(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string contentType(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, std::string> t = {
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},  {"gif", "image/gif"},
        {"heic", "image/heic"}, {"webp", "image/webp"}, {"mp4", "video/mp4"}, {"mov", "video/quicktime"},
        {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"},   {"pdf", "application/pdf"}, {"txt", "text/plain"},
        {"html", "text/html"}, {"json", "application/json"}, {"zip", "application/zip"}, {"apk", "application/vnd.android.package-archive"}};
    auto it = t.find(e);
    return it == t.end() ? "application/octet-stream" : it->second;
}

// A virtual, always-present empty file at the volume root. Spotlight skips volumes that carry it, so macOS never
// tries to crawl and index the phone's contents through this mount.
constexpr const char* kNeverIndex = "/.metadata_never_index";

std::string lastComponent(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string parentOf(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == 0 || s == std::string::npos ? "/" : p.substr(0, s);
}

}  // namespace

// Request path -> filesystem path ("/Base/a%20b/" -> "/a b"). Throws FsError(EINVAL) for anything unsafe and
// FsError(ENOENT) for paths outside the served prefix.
std::string WebDavService::toFsPath(const std::string& target) const {
    std::string p = percentDecode(target);
    if (p.empty() || p[0] != '/') throw FsError(EINVAL, "bad path");
    if (p.find('\0') != std::string::npos) throw FsError(EINVAL, "NUL in path");
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (!basePath_.empty()) {
        if (p == basePath_) p = "/";
        else if (p.rfind(basePath_ + "/", 0) == 0) p = p.substr(basePath_.size());
        else throw FsError(ENOENT, "outside the served volume");
    }
    splitVirtualPath(p);  // rejects ".", "..", over-long components
    return p;
}

std::string WebDavService::href(const std::string& fsPath, bool dir) const {
    std::string h = basePath_ + percentEncodePath(fsPath == "/" ? std::string{} : fsPath);
    if (dir || h.empty()) h += '/';
    return h;
}

int WebDavService::statusForErrno(int err) {
    switch (err) {
        case ENOENT: return 404;
        case EEXIST: return 405;
        case ENOTDIR: return 409;
        case EISDIR: return 405;
        case ENOTEMPTY: return 409;
        case EBUSY: return 423;
        case EROFS:
        case EPERM:
        case EACCES:
        case EOPNOTSUPP: return 403;
        case EXDEV: return 403;
        case ENOSPC: return 507;
        case EFBIG: return 413;
        case EINVAL:
        case ENAMETOOLONG: return 400;
        case ENODEV:
        case ESTALE: return 503;
        case ETIMEDOUT: return 503;
        default: return 500;
    }
}

void WebDavService::fail(HttpResponse& res, int err, const char* what) {
    res.status = statusForErrno(err);
    res.body = std::string(what) + ": " + std::strerror(err) + "\n";
    res.set("Content-Type", "text/plain; charset=utf-8");
    if (res.status == 503 || res.status == 500) res.closeConnection = false;
}

void WebDavService::handle(HttpRequest& req, HttpResponse& res) {
    try {
        const std::string& m = req.method;
        if (m == "OPTIONS") return options(res);
        std::string path;
        try {
            path = toFsPath(req.target);
        } catch (const FsError& e) {
            // The client may probe the server root above the served volume path.
            if (e.err() == ENOENT && m == "PROPFIND" && percentDecode(req.target) == "/") {
                res.status = 207;
                res.set("Content-Type", "application/xml; charset=utf-8");
                res.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/</D:href>"
                           "<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                           "</D:propstat></D:response></D:multistatus>";
                return;
            }
            throw;
        }
        if (path == kNeverIndex) return neverIndex(req, res);
        if (m == "PROPFIND") return propfind(req, res, path);
        if (m == "GET" || m == "HEAD") return get(req, res, path);
        if (m == "PUT") return put(req, res, path);
        if (m == "MKCOL") return mkcol(res, path);
        if (m == "DELETE") return del(res, path);
        if (m == "MOVE") return move(req, res, path);
        if (m == "LOCK") return lock(req, res, path);
        if (m == "UNLOCK") {
            res.status = 204;
            return;
        }
        if (m == "PROPPATCH") return proppatch(req, res, path);
        if (m == "COPY") {
            res.status = 501;
            res.body = "COPY is not supported; copy with GET and PUT\n";
            return;
        }
        res.status = 405;
        res.set("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND, PROPPATCH, LOCK, UNLOCK");
    } catch (const FsError& e) {
        fail(res, e.err(), "request failed");
    }
}

void WebDavService::neverIndex(HttpRequest& req, HttpResponse& res) {
    const std::string& m = req.method;
    char sink[1024];
    while (req.hasBody && req.readBody(sink, sizeof sink) > 0) {}
    if (m == "PROPFIND") {
        FsStat st;
        st.isDir = false;
        st.mode = 0444;
        st.size = 0;
        st.ino = 2;
        st.mtime = std::chrono::system_clock::from_time_t(1700000000);
        res.status = 207;
        res.set("Content-Type", "application/xml; charset=utf-8");
        res.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">" +
                   propEntry(href(kNeverIndex, false), ".metadata_never_index", st, false, PropRequest{}) + "</D:multistatus>";
    } else if (m == "GET" || m == "HEAD") {
        res.status = 200;
        res.set("Content-Type", "application/octet-stream");
        res.streamLength = 0;
        res.stream = [](const std::function<bool(const char*, size_t)>&) { return true; };
    } else if (m == "PUT") {
        res.status = 201;   // `mdutil -i off` style writes are accepted and ignored: the file is always there
    } else if (m == "DELETE") {
        res.status = 204;   // ...and cannot be removed
    } else if (m == "LOCK") {
        lock(req, res, kNeverIndex);
    } else if (m == "UNLOCK") {
        res.status = 204;
    } else {
        res.status = 403;
    }
}

void WebDavService::options(HttpResponse& res) {
    res.status = 200;
    // Class 2 (locking) makes macOS mount read-write; on a read-only volume advertise class 1 so it mounts read-only.
    res.set("DAV", fs_.config().readOnly ? "1" : "1, 2");
    res.set("Allow", fs_.config().readOnly ? "OPTIONS, GET, HEAD, PROPFIND"
                                           : "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, PROPFIND, PROPPATCH, LOCK, UNLOCK");
    res.set("MS-Author-Via", "DAV");
    res.set("Accept-Ranges", "bytes");
}

static PropRequest parsePropRequest(const std::string& body) {
    PropRequest r;
    const size_t p = body.find("prop>");   // "<D:prop>" / "<prop>"
    if (body.empty() || body.find("allprop") != std::string::npos || p == std::string::npos) return r;   // all properties
    r.all = false;
    const size_t start = body.find('>', body.rfind('<', p)) + 1;
    const size_t end = body.find("</", start);
    size_t endProp = std::string::npos;
    for (size_t k = start; (k = body.find("</", k)) != std::string::npos; k += 2) {
        const size_t gt = body.find('>', k);
        const std::string tag = body.substr(k + 2, gt - k - 2);
        if (tag == "prop" || (tag.size() > 5 && tag.compare(tag.size() - 5, 5, ":prop") == 0)) { endProp = k; break; }
    }
    (void)end;
    if (endProp == std::string::npos) return r;
    const std::string inner = body.substr(start, endProp - start);
    for (size_t k = 0; (k = inner.find('<', k)) != std::string::npos;) {
        const size_t e = inner.find_first_of(" />\t\r\n", k + 1);
        if (e == std::string::npos) break;
        std::string tag = inner.substr(k + 1, e - k - 1);
        k = e;
        if (tag.empty() || tag[0] == '/' || tag[0] == '?') continue;
        if (const size_t colon = tag.find(':'); colon != std::string::npos) tag = tag.substr(colon + 1);
        r.names.insert(tag);
    }
    return r;
}

std::string WebDavService::propEntry(const std::string& href, const std::string& name, const FsStat& st, bool root,
                                     const PropRequest& want, const std::string& quotaXml) {
    const auto mtime = st.mtime;
    std::vector<std::pair<std::string, std::string>> avail;   // local name -> XML element
    avail.emplace_back("displayname", "<D:displayname>" + xmlEscape(root ? volumeName_ : name) + "</D:displayname>");
    avail.emplace_back("resourcetype", std::string("<D:resourcetype>") + (st.isDir ? "<D:collection/>" : "") + "</D:resourcetype>");
    if (!st.isDir) {
        avail.emplace_back("getcontentlength", "<D:getcontentlength>" + std::to_string(st.size) + "</D:getcontentlength>");
        avail.emplace_back("getcontenttype", "<D:getcontenttype>" + xmlEscape(contentType(name)) + "</D:getcontenttype>");
    }
    avail.emplace_back("getlastmodified", "<D:getlastmodified>" + httpDate(mtime) + "</D:getlastmodified>");
    avail.emplace_back("creationdate", "<D:creationdate>" + isoDate(mtime) + "</D:creationdate>");
    {
        std::ostringstream e;
        e << "<D:getetag>\"" << std::hex << st.ino << "-" << st.size << "-"
          << std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count() << std::dec << "\"</D:getetag>";
        avail.emplace_back("getetag", e.str());
    }
    avail.emplace_back("supportedlock",
                       "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype>"
                       "</D:lockentry></D:supportedlock>");
    avail.emplace_back("lockdiscovery", "<D:lockdiscovery/>");
    if (!quotaXml.empty()) {
        // quotaXml holds exactly the two RFC 4331 elements, in this order.
        const size_t split = quotaXml.find("</D:quota-available-bytes>") + std::strlen("</D:quota-available-bytes>");
        avail.emplace_back("quota-available-bytes", quotaXml.substr(0, split));
        avail.emplace_back("quota-used-bytes", quotaXml.substr(split));
    }

    std::string found, missing;
    if (want.all) {
        for (const auto& [n, xml] : avail) found += xml;
    } else {
        for (const auto& n : want.names) {
            auto it = std::find_if(avail.begin(), avail.end(), [&](const auto& a) { return a.first == n; });
            if (it != avail.end()) found += it->second;
            else missing += "<D:" + n + "/>";
        }
    }
    std::string out = "<D:response><D:href>" + xmlEscape(href) + "</D:href>";
    if (!found.empty() || missing.empty()) out += "<D:propstat><D:prop>" + found + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>";
    if (!missing.empty()) out += "<D:propstat><D:prop>" + missing + "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>";
    return out + "</D:response>";
}

void WebDavService::propfind(HttpRequest& req, HttpResponse& res, const std::string& path) {
    // The request body (prop list) is ignored: a fixed property set is returned, which every client accepts.
    std::string requestBody;
    {
        char sink[4096];
        for (long n; req.hasBody && (n = req.readBody(sink, sizeof sink)) > 0;) requestBody.append(sink, static_cast<size_t>(n));
    }
    if (static const bool trace = std::getenv("MTPORT_WEBDAV_TRACE") != nullptr; trace)
        std::fprintf(stderr, "webdav> PROPFIND body: %s\n", requestBody.c_str());
    const PropRequest want = parsePropRequest(requestBody);
    std::string depth = req.header("depth");
    FsStat st;
    try {
        st = fs_.getattr(path);
    } catch (const FsError& e) {
        return fail(res, e.err(), "PROPFIND");
    }
    const std::string selfHref = href(path, st.isDir);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
    std::string quotaXml;
    if (path == "/" && quotaEnabled_.load()) {
        // Free/used space for the client's volume information (RFC 4331). Only offered once the volume is mounted:
        // answering these properties during mount() makes macOS' webdavfs wait ~90 s (observed on macOS 26).
        try {
            const FsStatvfs v = fs_.statfs("/");
            quotaXml = "<D:quota-available-bytes>" + std::to_string(v.blocksAvail * v.blockSize) +
                       "</D:quota-available-bytes><D:quota-used-bytes>" +
                       std::to_string((v.blocks - v.blocksFree) * v.blockSize) + "</D:quota-used-bytes>";
        } catch (const FsError&) {
        }
    }
    body << propEntry(selfHref, lastComponent(path), st, path == "/", want, quotaXml);
    if (path == "/" && depth != "0") {
        FsStat vs;
        vs.isDir = false;
        vs.mode = 0444;
        vs.ino = 2;
        vs.mtime = std::chrono::system_clock::from_time_t(1700000000);
        body << propEntry(href(kNeverIndex, false), ".metadata_never_index", vs, false, want);
    }
    if (st.isDir && depth != "0") {
        try {
            for (const auto& e : fs_.readdir(path)) {
                if (e.name == "." || e.name == "..") continue;
                const std::string child = (path == "/" ? "" : path) + "/" + e.name;
                FsStat cs;
                try {
                    cs = fs_.getattr(child);
                } catch (const FsError&) {
                    continue;  // vanished between readdir and getattr
                }
                body << propEntry(href(child, cs.isDir), e.name, cs, false, want);
            }
        } catch (const FsError& e) {
            return fail(res, e.err(), "PROPFIND");
        }
    }
    body << "</D:multistatus>";
    res.status = 207;
    res.set("Content-Type", "application/xml; charset=utf-8");
    res.body = body.str();
}

void WebDavService::get(HttpRequest& req, HttpResponse& res, const std::string& path) {
    FsStat st;
    try {
        st = fs_.getattr(path, /*fresh=*/true);  // Content-Length must be the phone's real size, not a cached one
    } catch (const FsError& e) {
        return fail(res, e.err(), "GET");
    }
    const std::string name = lastComponent(path);
    if (st.isDir) {
        res.status = 200;
        res.set("Content-Type", "text/html; charset=utf-8");
        res.body = "<html><body>" + xmlEscape(name) + "</body></html>";
        return;
    }
    uint64_t first = 0, last = st.size ? st.size - 1 : 0;
    bool partial = false;
    // macOS' WebDAV client asks for page-aligned 1 MiB ranges. When the last one runs past the end of the file and the
    // server answers with the (RFC-correct) shorter 206, the client zero-pads that page and grows its cache file, so a
    // cold `cp` or Finder copy ends up with extra zero bytes (measured on a real phone: 12 of 40 photos). Refusing
    // such a range with 416 makes the client take those bytes from its whole-file GET instead, which is always
    // exact (40 of 40 identical to a plain HTTP download). The price: the tail of a big file is only readable once
    // the client's own download has reached it. MTPORT_WEBDAV_RANGE_MODE=clamp restores the RFC answer (tests).
    static const bool clampRanges = [] {
        const char* m = std::getenv("MTPORT_WEBDAV_RANGE_MODE");
        return m && std::string(m) == "clamp";
    }();
    if (const std::string range = req.header("range"); range.rfind("bytes=", 0) == 0 && st.size > 0) {
        const std::string spec = range.substr(6);
        const size_t dash = spec.find('-');
        if (dash != std::string::npos && spec.find(',') == std::string::npos) {
            const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
            if (a.empty() && !b.empty()) {  // suffix: last N bytes
                const uint64_t n = std::strtoull(b.c_str(), nullptr, 10);
                first = n >= st.size ? 0 : st.size - n;
            } else if (!a.empty()) {
                first = std::strtoull(a.c_str(), nullptr, 10);
                if (!b.empty()) last = std::min<uint64_t>(std::strtoull(b.c_str(), nullptr, 10), st.size - 1);
            }
            partial = true;
            if (!clampRanges && !a.empty() && !b.empty() && std::strtoull(b.c_str(), nullptr, 10) >= st.size) {
                res.status = 416;
                res.set("Content-Range", "bytes */" + std::to_string(st.size));
                return;
            }
            if (first >= st.size || first > last) {
                res.status = 416;
                res.set("Content-Range", "bytes */" + std::to_string(st.size));
                return;
            }
        }
    }
    const uint64_t length = st.size == 0 ? 0 : last - first + 1;
    res.status = partial ? 206 : 200;
    res.set("Content-Type", contentType(name));
    res.set("Accept-Ranges", "bytes");
    res.set("Last-Modified", httpDate(st.mtime));
    res.set("ETag", "\"" + std::to_string(st.ino) + "-" + std::to_string(st.size) + "\"");
    if (partial) res.set("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(st.size));
    if (req.method == "HEAD") {
        res.streamLength = length;  // HEAD: no body is sent, but Content-Length must describe the entity
        res.stream = [](const std::function<bool(const char*, size_t)>&) { return true; };
        return;
    }
    uint64_t fh;
    try {
        fh = fs_.open(path, O_RDONLY);
    } catch (const FsError& e) {
        res.headers.clear();
        res.status = 200;
        return fail(res, e.err(), "GET");
    }
    res.streamLength = length;
    res.stream = [this, fh, first, length](const std::function<bool(const char*, size_t)>& write) {
        bool ok = true;
        // Sized like the phone likes it (one GetPartialObject round trip per read, see kMaxPartialChunk) but never larger
        // than the response itself.
        std::vector<std::byte> buf(static_cast<size_t>(std::clamp<uint64_t>(length, 1, 16u << 20)));
        uint64_t off = first, left = length;
        try {
            while (left > 0 && ok) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), left));
                const size_t n = fs_.read(fh, off, std::span<std::byte>(buf.data(), want));
                if (n == 0) { ok = false; break; }  // the file is shorter than promised: hang up, never pad
                ok = write(reinterpret_cast<const char*>(buf.data()), n);
                off += n;
                left -= n;
            }
        } catch (const FsError&) {
            ok = false;
        }
        fs_.release(fh);
        return ok && left == 0;
    };
}

void WebDavService::put(HttpRequest& req, HttpResponse& res, const std::string& path) {
    if (fs_.config().readOnly) return fail(res, EROFS, "PUT");
    // macOS creates a file with an EMPTY PUT and fills it afterwards with LOCK + a second PUT. An empty PUT to a new name
    // therefore only creates the placeholder: it is shown as a 0-byte file but uploaded later (when content arrives, or
    // after the grace period / at eject), never as an extra empty object that would block the real upload.
    const bool emptyBody = !req.hasBody;
    uint64_t fh;
    try {
        fh = fs_.create(path, 0644);
    } catch (const FsError& e) {
        if (e.err() != EEXIST) return fail(res, e.err(), "PUT");
        try {
            // Allowed only for files that are not on the phone yet (the placeholder above) and for Finder's RAM-only
            // metadata files. A real phone file makes open(O_TRUNC) fail with EOPNOTSUPP.
            fh = fs_.open(path, O_WRONLY | O_TRUNC);
        } catch (const FsError&) {
            res.status = 403;
            res.body = "Replacing an existing file on the phone is not supported; delete it first or use a new name\n";
            res.set("Content-Type", "text/plain; charset=utf-8");
            return;
        }
    }
    std::vector<char> buf(256 * 1024);
    uint64_t off = 0;
    int err = 0;
    try {
        for (;;) {
            const long n = req.hasBody ? req.readBody(buf.data(), buf.size()) : 0;
            if (n < 0) { err = EIO; break; }
            if (n == 0) break;
            fs_.write(fh, off, std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), static_cast<size_t>(n)));
            off += static_cast<uint64_t>(n);
        }
        // Content arrived: commit now and only answer 201 once the phone confirmed it. An empty PUT stays a placeholder.
        if (!err && !emptyBody) fs_.fsync(fh);
    } catch (const FsError& e) {
        err = e.err();
    }
    fs_.release(fh);
    if (err) {
        res.closeConnection = true;  // the request body was not fully consumed
        return fail(res, err, "PUT");
    }
    res.status = 201;
}

void WebDavService::mkcol(HttpResponse& res, const std::string& path) {
    try {
        fs_.mkdir(path);
        res.status = 201;
    } catch (const FsError& e) {
        fail(res, e.err() == ENOENT ? ENOTDIR : e.err(), "MKCOL");
    }
}

void WebDavService::del(HttpResponse& res, const std::string& path) {
    try {
        const FsStat st = fs_.getattr(path);
        // Exactly one object. A folder must be empty (409 otherwise): DELETE never recurses over MTP.
        if (st.isDir) fs_.rmdir(path);
        else fs_.unlink(path);
        res.status = 204;
    } catch (const FsError& e) {
        fail(res, e.err(), "DELETE");
    }
}

void WebDavService::move(HttpRequest& req, HttpResponse& res, const std::string& path) {
    std::string dest = req.header("destination");
    if (dest.empty()) { res.status = 400; return; }
    if (dest.rfind("http://", 0) == 0 || dest.rfind("https://", 0) == 0) {
        const size_t slash = dest.find('/', dest.find("//") + 2);
        dest = slash == std::string::npos ? "/" : dest.substr(slash);
    }
    try {
        const std::string to = toFsPath(dest);
        bool exists = true;
        try {
            fs_.getattr(to);
        } catch (const FsError& e) {
            if (e.err() != ENOENT) throw;
            exists = false;
        }
        if (exists) {
            // No overwrite: replacing would need delete-then-send, which is never done on the phone.
            res.status = 412;
            res.body = "Destination exists; replacing is not supported\n";
            return;
        }
        fs_.rename(path, to);
        res.status = 201;
    } catch (const FsError& e) {
        fail(res, e.err(), "MOVE");
    }
}

void WebDavService::lock(HttpRequest& req, HttpResponse& res, const std::string& path) {
    char sink[4096];
    while (req.hasBody && req.readBody(sink, sizeof sink) > 0) {}
    // macOS asks for a write lock before it opens a file for writing. An existing file on the phone can never be
    // modified in place, so refusing the lock makes open() fail up front instead of letting the client cache edits that
    // the later PUT would be forced to reject (a silent false success). New names (lock-null), folders and Finder's
    // own RAM-only metadata files are lockable.
    try {
        const FsStat st = fs_.getattr(path);
        if (!st.isDir && !st.pending && !isMacMetadataName(lastComponent(path))) {
            res.status = 403;
            res.body = "Files already on the phone cannot be modified in place\n";
            res.set("Content-Type", "text/plain; charset=utf-8");
            return;
        }
    } catch (const FsError& e) {
        if (e.err() != ENOENT) return fail(res, e.err(), "LOCK");
    }
    // Locks are not tracked (one MTP session already serialises everything); the client only needs a valid answer.
    const std::string token = "opaquelocktoken:mtport-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    res.status = 200;
    res.set("Content-Type", "application/xml; charset=utf-8");
    res.set("Lock-Token", "<" + token + ">");
    res.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
               "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth>"
               "<D:timeout>Second-600</D:timeout><D:locktoken><D:href>" + token + "</D:href></D:locktoken>"
               "<D:lockroot><D:href>" + xmlEscape(href(path, false)) + "</D:href></D:lockroot>"
               "</D:activelock></D:lockdiscovery></D:prop>";
}

void WebDavService::proppatch(HttpRequest& req, HttpResponse& res, const std::string& path) {
    std::string body;
    char buf[4096];
    for (long n; req.hasBody && (n = req.readBody(buf, sizeof buf)) > 0;) body.append(buf, static_cast<size_t>(n));
    // Property names the client tried to set.
    std::vector<std::string> names;
    for (size_t i = 0; (i = body.find('<', i)) != std::string::npos;) {
        const size_t e = body.find_first_of(" />", i + 1);
        if (e == std::string::npos) break;
        std::string tag = body.substr(i + 1, e - i - 1);
        i = e;
        if (tag.empty() || tag[0] == '/' || tag[0] == '?') continue;
        const size_t colon = tag.find(':');
        const std::string local = colon == std::string::npos ? tag : tag.substr(colon + 1);
        if (local == "propertyupdate" || local == "set" || local == "remove" || local == "prop") continue;
        names.push_back(tag);
    }
    // MTP cannot store client-chosen times. Objects created through this mount accept them as a no-op, other
    // files refuse with 403 (the same policy as utimens on the FSKit mount).
    int err = 0;
    try {
        fs_.utimens(path);
    } catch (const FsError& e) {
        err = e.err();
    }
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>"
      << xmlEscape(href(path, false)) << "</D:href><D:propstat><D:prop>";
    for (const auto& n : names) o << "<" << (n.find(':') == std::string::npos ? "D:" : "") << n << "/>";
    o << "</D:prop><D:status>HTTP/1.1 " << (err ? "403 Forbidden" : "200 OK") << "</D:status></D:propstat></D:response></D:multistatus>";
    res.status = 207;
    res.set("Content-Type", "application/xml; charset=utf-8");
    res.body = o.str();
}

}  // namespace mtport::webdav
