#include <sys/stat.h>

#include <fstream>

#include "TestHarness.hpp"
#include "TestSupport.hpp"
#include "mtport/WriteStaging.hpp"

using namespace mtport;
using aftest::bytesOf;
using aftest::TempDir;

namespace {
WriteStaging::Config cfgFor(const TempDir& t, uint64_t quota = 1u << 20) {
    WriteStaging::Config c;
    c.directory = t.path / "stage";
    c.quotaBytes = quota;
    c.minFreeDiskBytes = 1;  // the dev machine's real free space is not what is under test
    return c;
}
uint64_t fileSize(const std::filesystem::path& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : UINT64_MAX;
}
}  // namespace

AF_TEST(staged_writes, directory_and_file_permissions) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("a.bin", 1, 0);
    struct stat st{};
    CHECK_EQ(::stat((t.path / "stage").c_str(), &st), 0);
    CHECK_EQ(st.st_mode & 0777, mode_t{0700});
    f->write(0, bytesOf("x"));
    int files = 0;
    for (auto& e : std::filesystem::directory_iterator(t.path / "stage")) {
        ::stat(e.path().c_str(), &st);
        CHECK_EQ(st.st_mode & 0777, mode_t{0600});
        ++files;
    }
    CHECK_EQ(files, 1);
}

AF_TEST(staged_writes, state_machine_commit_is_idempotent) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("a.bin", 7, 3);
    CHECK_EQ(f->state(), StageState::Dirty);  // created files must exist after close, even if empty
    f->write(0, bytesOf("hello"));
    int sends = 0;
    std::string seen;
    auto send = [&](const std::filesystem::path& p) {
        ++sends;
        std::ifstream in(p, std::ios::binary);
        seen.assign(std::istreambuf_iterator<char>(in), {});
        return ObjectId{42};
    };
    CHECK_EQ(f->commit(send), ObjectId{42});
    CHECK_EQ(f->state(), StageState::Committed);
    CHECK_EQ(f->commit(send), ObjectId{42});
    CHECK_EQ(sends, 1);
    CHECK_EQ(seen, std::string("hello"));
    CHECK_EQ(w.usedBytes(), uint64_t{0});  // quota released
    CHECK(!std::filesystem::exists(t.path / "stage" /
                                   (f->stageId() + ".stage")));              // local copy dropped only after success
    CHECK_MTP(f->write(0, bytesOf("more")), MtpErrc::OperationUnsupported);  // no in-place edits after commit
}

AF_TEST(staged_writes, failed_commit_preserves_bytes_and_is_terminal) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("keep.bin", 1, 0);
    f->write(0, bytesOf("precious"));
    int sends = 0;
    auto failing = [&](const std::filesystem::path&) -> ObjectId {
        ++sends;
        throw MtpError(MtpErrc::DeviceStorageFull, "phone full");
    };
    CHECK_MTP(f->commit(failing), MtpErrc::DeviceStorageFull);
    CHECK_EQ(f->state(), StageState::Failed);
    const auto failure = f->failure();
    CHECK(failure.has_value());
    CHECK_EQ(*failure, MtpErrc::DeviceStorageFull);
    // Never a blind retry: the second commit reports the same error without touching the device.
    CHECK_MTP(f->commit([&](const std::filesystem::path&) {
        ++sends;
        return ObjectId{1};
    }),
              MtpErrc::DeviceStorageFull);
    CHECK_EQ(sends, 1);
    CHECK_MTP(f->write(0, bytesOf("x")), MtpErrc::DeviceStorageFull);
    // The user's bytes and a manifest survive for manual recovery.
    CHECK_EQ(fileSize(t.path / "stage" / (f->stageId() + ".stage")), uint64_t{8});
    auto rec = w.listRecoverable();
    CHECK_EQ(rec.size(), size_t{1});
    CHECK_EQ(rec[0].finalName, std::string("keep.bin"));
    CHECK_EQ(rec[0].sizeBytes, uint64_t{8});
    CHECK(rec[0].error.find("phone full") != std::string::npos);
}

AF_TEST(staged_writes, non_mtp_exception_becomes_io_error_and_fails_file) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("a", 1, 0);
    f->write(0, bytesOf("x"));
    CHECK_MTP(f->commit([](const std::filesystem::path&) -> ObjectId { throw std::runtime_error("boom"); }),
              MtpErrc::IoError);
    CHECK_EQ(f->state(), StageState::Failed);
}

AF_TEST(staged_writes, quota_is_enforced_and_released) {
    TempDir t;
    WriteStaging w(cfgFor(t, 1000));
    auto a = w.create("a", 1, 0);
    a->write(0, std::vector<std::byte>(600));
    auto b = w.create("b", 1, 0);
    CHECK_MTP(b->write(0, std::vector<std::byte>(600)), MtpErrc::StagingDiskFull);  // 1200 > 1000
    CHECK_EQ(b->size(), uint64_t{0});                                               // nothing half-written
    b->write(0, std::vector<std::byte>(400));                                       // exactly the rest
    CHECK_EQ(w.usedBytes(), uint64_t{1000});
    a->commit([](const std::filesystem::path&) { return ObjectId{1}; });
    CHECK_EQ(w.usedBytes(), uint64_t{400});
    CHECK_MTP(b->write(0, std::vector<std::byte>(1001)), MtpErrc::StagingDiskFull);
}

AF_TEST(staged_writes, host_free_space_floor) {
    TempDir t;
    auto c = cfgFor(t, 1ull << 60);
    c.minFreeDiskBytes = 1ull << 62;  // impossible floor => "disk full" without filling the disk
    WriteStaging w(c);
    auto f = w.create("a", 1, 0);
    CHECK_MTP(f->write(0, bytesOf("x")), MtpErrc::StagingDiskFull);
}

AF_TEST(staged_writes, tiny_and_odd_writes_round_trip) {
    // Mirrors the macFUSE 5.3.3 corruption class (1-14 byte writes) and the 15-byte boundary.
    for (size_t n :
         {size_t{0}, size_t{1}, size_t{2}, size_t{7}, size_t{14}, size_t{15}, size_t{16}, size_t{4095}, size_t{4097}}) {
        TempDir t;
        WriteStaging w(cfgFor(t));
        auto f = w.create("f", 1, 0);
        std::string expect;
        for (size_t i = 0; i < n; ++i) expect.push_back(static_cast<char>('A' + i % 26));
        for (size_t i = 0; i < n; ++i) f->write(i, bytesOf(std::string(1, expect[i])));  // 1-byte writes
        CHECK_EQ(f->size(), static_cast<uint64_t>(n));
        std::string got;
        f->commit([&](const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            got.assign(std::istreambuf_iterator<char>(in), {});
            return ObjectId{1};
        });
        CHECK_EQ(got, expect);
    }
}

AF_TEST(staged_writes, nonsequential_overwrite_and_sparse_regions) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("f", 1, 0);
    f->write(10, bytesOf("WORLD"));
    f->write(0, bytesOf("HELLO"));
    f->write(2, bytesOf("XX"));  // overwrite inside
    CHECK_EQ(f->size(), uint64_t{15});
    std::vector<std::byte> buf(15);
    CHECK_EQ(f->read(0, buf), size_t{15});
    CHECK_EQ(aftest::str(buf), std::string("HEXXO\0\0\0\0\0WORLD", 15));  // gap reads as zeros, like POSIX
    CHECK_EQ(f->read(15, buf), size_t{0});
    CHECK_EQ(f->read(1ull << 40, buf), size_t{0});
}

AF_TEST(staged_writes, truncate_up_and_down) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("f", 1, 0);
    f->write(0, bytesOf("0123456789"));
    f->truncate(4);
    CHECK_EQ(f->size(), uint64_t{4});
    CHECK_EQ(w.usedBytes(), uint64_t{4});
    f->truncate(100);
    CHECK_EQ(f->size(), uint64_t{100});
    CHECK_MTP(f->truncate(1u << 30), MtpErrc::StagingDiskFull);
}

AF_TEST(staged_writes, offset_overflow_rejected) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("f", 1, 0);
    CHECK_MTP(f->write(UINT64_MAX - 1, bytesOf("abc")), MtpErrc::FileTooLarge);
}

AF_TEST(staged_writes, abandoned_content_is_kept_empty_is_dropped) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    std::string keptId;
    {
        auto empty = w.create("empty", 1, 0);
        auto full = w.create("full", 1, 0);
        full->write(0, bytesOf("data"));
        keptId = full->stageId();
    }  // both dropped without commit (e.g. the mount ended)
    int stages = 0, manifests = 0;
    for (auto& e : std::filesystem::directory_iterator(t.path / "stage")) {
        if (e.path().extension() == ".stage") ++stages;
        if (e.path().extension() == ".manifest") ++manifests;
    }
    CHECK_EQ(stages, 1);
    CHECK_EQ(manifests, 1);
    CHECK(std::filesystem::exists(t.path / "stage" / (keptId + ".stage")));
    // A new staging area counts preserved bytes against its budget.
    WriteStaging w2(cfgFor(t, 100));
    CHECK_EQ(w2.usedBytes(), uint64_t{4});
}

AF_TEST(staged_writes, discard_removes_only_local_staging) {
    TempDir t;
    WriteStaging w(cfgFor(t));
    auto f = w.create("f", 1, 0);
    f->write(0, bytesOf("abc"));
    f->discard();
    CHECK(!std::filesystem::exists(t.path / "stage" / (f->stageId() + ".stage")));
    CHECK_EQ(w.usedBytes(), uint64_t{0});
    CHECK_MTP(f->commit([](const std::filesystem::path&) { return ObjectId{1}; }), MtpErrc::StaleHandle);
}
