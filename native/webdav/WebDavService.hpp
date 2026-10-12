// WebDAV (RFC 4918 subset) in front of MtpFilesystem. Every safety rule lives in MtpFilesystem; this layer only
// translates HTTP methods to filesystem calls and errno values to HTTP status codes.
//
//  PROPFIND/GET/HEAD/OPTIONS  read
//  PUT                        new file only (staged on the Mac, uploaded when the body is complete)
//  MKCOL / DELETE / MOVE      create folder / delete ONE object (folders must be empty) / rename in place
//  LOCK / UNLOCK / PROPPATCH  accepted so that macOS mounts the volume read-write; nothing is really locked
//  COPY                       501 (the macOS client copies by itself with GET + PUT)
#pragma once

#include <atomic>
#include <set>
#include <string>

#include "HttpServer.hpp"
#include "mtport/MtpFilesystem.hpp"

namespace mtport::webdav {

// The properties a PROPFIND asked for (RFC 4918 section 9.1). `all` = allprop / empty body.
struct PropRequest {
    bool all{true};
    std::set<std::string> names;  // local names, e.g. "getlastmodified"
};

class WebDavService {
public:
    // `basePath` is the URL prefix the volume is served under, e.g. "/MTPort-SM-G990E": macOS derives the
    // volume's name in Finder from the last component of the URL it mounts.
    WebDavService(MtpFilesystem& fs, std::string volumeName, std::string basePath = "")
        : fs_(fs), volumeName_(std::move(volumeName)), basePath_(std::move(basePath)) {}
    void handle(HttpRequest& req, HttpResponse& res);
    // Capacity properties (quota-available-bytes / quota-used-bytes) are withheld until the volume is mounted.
    void enableQuota() { quotaEnabled_ = true; }

    static int statusForErrno(int err);  // errno -> HTTP status

private:
    void options(HttpResponse&);
    void neverIndex(HttpRequest&, HttpResponse&);
    void propfind(HttpRequest&, HttpResponse&, const std::string& path);
    void get(HttpRequest&, HttpResponse&, const std::string& path);
    void put(HttpRequest&, HttpResponse&, const std::string& path);
    void mkcol(HttpResponse&, const std::string& path);
    void del(HttpResponse&, const std::string& path);
    void move(HttpRequest&, HttpResponse&, const std::string& path);
    void lock(HttpRequest&, HttpResponse&, const std::string& path);
    void proppatch(HttpRequest&, HttpResponse&, const std::string& path);
    void fail(HttpResponse&, int err, const char* what);
    std::string propEntry(const std::string& href, const std::string& name, const FsStat& st, bool root,
                          const PropRequest& want, const std::string& quotaXml = {});

    MtpFilesystem& fs_;
    std::string volumeName_;
    std::string basePath_;  // "" or "/Name" (no trailing slash)
    std::atomic<bool> quotaEnabled_{false};

    std::string toFsPath(const std::string& target) const;
    std::string href(const std::string& fsPath, bool dir) const;
};

}  // namespace mtport::webdav
