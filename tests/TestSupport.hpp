// Shared fixtures for the unit/integration suites.
#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "FakeMtpDevice.hpp"
#include "mtport/MtpFilesystem.hpp"

namespace aftest {

// Unique scratch directory removed on destruction (tests only ever delete directories they created).
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("aftest-" + std::to_string(::getpid()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline std::vector<std::byte> bytesOf(std::string_view s) {
    std::vector<std::byte> b(s.size());
    std::memcpy(b.data(), s.data(), s.size());
    return b;
}
inline std::string str(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Cache config that never serves stale listings (tests that change the phone behind the filesystem's back).
inline mtport::MetadataCache::Config noCache() {
    mtport::MetadataCache::Config c;
    c.dirTtl = std::chrono::milliseconds(0);
    return c;
}

// A filesystem over a FakeMtpDevice with private staging dirs, ready for tests.
struct FsFixture {
    TempDir tmp;
    std::shared_ptr<mtport::fake::FakeMtpDevice> phone;
    std::unique_ptr<mtport::MtpFilesystem> fs;
    mtport::StorageId internal{}, sd{};

    explicit FsFixture(bool readOnly = false, bool partialRead = true, bool withSd = false,
                       mtport::MetadataCache::Config cache = {}) {
        mtport::fake::FakeMtpDevice::Options o;
        o.partialRead = partialRead;
        phone = std::make_shared<mtport::fake::FakeMtpDevice>(o);
        internal = phone->addStorage("Internal Storage", 64ull << 30, 32ull << 30);
        if (withSd) sd = phone->addStorage("SD Card", 16ull << 30, 8ull << 30);
        fs = make(phone, readOnly, cache);
    }

    std::unique_ptr<mtport::MtpFilesystem> make(std::shared_ptr<mtport::IMtpDevice> dev, bool readOnly,
                                                       mtport::MetadataCache::Config cache = {}) {
        mtport::FsConfig cfg;
        cfg.readOnly = readOnly;
        cfg.cache = cache;
        cfg.stagingDir = tmp.path / "staging";
        cfg.readStageDir = tmp.path / "readcache";
        cfg.stagingQuotaBytes = 256ull << 20;
        cfg.readStageQuotaBytes = 256ull << 20;
        cfg.uid = ::getuid();
        cfg.gid = ::getgid();
        return std::make_unique<mtport::MtpFilesystem>(std::move(dev), cfg);
    }
};

}  // namespace aftest
