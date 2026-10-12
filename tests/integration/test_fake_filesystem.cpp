#include <fcntl.h>

#include <algorithm>
#include <set>
#include <chrono>
#include <thread>

#include "TestHarness.hpp"
#include "TestSupport.hpp"
#include "mtport/Sha256.hpp"

using namespace mtport;
using namespace mtport::fake;
using aftest::bytesOf;
using aftest::FsFixture;
using aftest::str;

namespace {

const std::string kInt = "/Internal Storage";

std::string readAll(MtpFilesystem& fs, const std::string& path, size_t chunk = 4096) {
    const uint64_t fh = fs.open(path, O_RDONLY);
    std::string out;
    std::vector<std::byte> buf(chunk);
    for (uint64_t off = 0;;) {
        const size_t n = fs.read(fh, off, buf);
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n);
        off += n;
    }
    fs.release(fh);
    return out;
}

void writeFile(MtpFilesystem& fs, const std::string& path, std::string_view data) {
    const uint64_t fh = fs.create(path, 0644);
    if (!data.empty()) fs.write(fh, 0, bytesOf(data));
    fs.flush(fh);
    fs.release(fh);
}

std::set<std::string> names(MtpFilesystem& fs, const std::string& path) {
    std::set<std::string> out;
    for (auto& e : fs.readdir(path))
        if (e.name != "." && e.name != "..") out.insert(e.name);
    return out;
}

}  // namespace

// ---- namespace & metadata -----------------------------------------------------------------------

AF_TEST(fake_filesystem, root_lists_real_storages_only) {
    FsFixture fx(false, true, true);
    auto n = names(*fx.fs, "/");
    CHECK_EQ(n, (std::set<std::string>{"Internal Storage", "SD Card"}));
    auto st = fx.fs->getattr("/");
    CHECK(st.isDir);
    CHECK_EQ(st.ino, uint64_t{1});
    // No fabricated folders: an empty phone shows empty storages.
    CHECK(names(*fx.fs, kInt).empty());
}

AF_TEST(fake_filesystem, empty_root_when_phone_reports_no_storage) {
    FsFixture fx;
    FsFixture empty;
    auto bare = std::make_shared<FakeMtpDevice>();
    auto fs = empty.make(bare, false);
    CHECK(names(*fs, "/").empty());
    CHECK_ERRNO(fs->getattr("/Internal Storage"), ENOENT);
}

AF_TEST(fake_filesystem, duplicate_storage_labels_get_suffixes) {
    FsFixture fx;
    fx.phone->addStorage("Internal Storage", 1ull << 30, 1ull << 29);
    auto fs = fx.make(fx.phone, false);
    CHECK_EQ(names(*fs, "/"), (std::set<std::string>{"Internal Storage", "Internal Storage (2)"}));
}

AF_TEST(fake_filesystem, getattr_modes_reflect_capabilities) {
    FsFixture rw;
    auto d = rw.phone->addDir(rw.internal, 0, "DCIM");
    rw.phone->addFile(rw.internal, d, "a.jpg", "abc");
    auto st = rw.fs->getattr(kInt + "/DCIM/a.jpg");
    CHECK(!st.isDir);
    CHECK_EQ(st.size, uint64_t{3});
    CHECK_EQ(st.mode, uint32_t{0644});
    CHECK_EQ(st.uid, static_cast<uint32_t>(::getuid()));
    CHECK_EQ(rw.fs->getattr(kInt + "/DCIM").mode, uint32_t{0755});

    FsFixture ro(true);
    auto d2 = ro.phone->addDir(ro.internal, 0, "DCIM");
    ro.phone->addFile(ro.internal, d2, "a.jpg", "abc");
    CHECK_EQ(ro.fs->getattr(kInt + "/DCIM/a.jpg").mode, uint32_t{0444});  // no false writable bits
    CHECK_EQ(ro.fs->getattr(kInt + "/DCIM").mode, uint32_t{0555});
    CHECK_EQ(ro.fs->getattr("/").mode, uint32_t{0555});
}

AF_TEST(fake_filesystem, read_only_storage_is_not_writable) {
    FsFixture fx;
    auto ro = fx.phone->addStorage("Locked Card", 1ull << 30, 1ull << 29, true);
    (void)ro;
    auto fs = fx.make(fx.phone, false);
    CHECK_EQ(fs->getattr("/Locked Card").mode, uint32_t{0555});
    CHECK_ERRNO(fs->create("/Locked Card/x", 0644), EROFS);
    CHECK_ERRNO(fs->mkdir("/Locked Card/d"), EROFS);
}

AF_TEST(fake_filesystem, nonexistent_and_traversal_paths) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "file.txt", "x");
    CHECK_ERRNO(fx.fs->getattr("/nope"), ENOENT);
    CHECK_ERRNO(fx.fs->getattr(kInt + "/nope/deeper"), ENOENT);
    CHECK_ERRNO(fx.fs->getattr(kInt + "/../Internal Storage"), EINVAL);
    CHECK_ERRNO(fx.fs->getattr("relative"), EINVAL);
    CHECK_ERRNO(fx.fs->getattr(kInt + "/file.txt/inside"), ENOTDIR);
    CHECK_ERRNO(fx.fs->readdir(kInt + "/file.txt"), ENOTDIR);
}

AF_TEST(fake_filesystem, readdir_includes_dot_entries_and_consistent_inodes) {
    FsFixture fx;
    auto d = fx.phone->addDir(fx.internal, 0, "DCIM");
    fx.phone->addFile(fx.internal, d, "a.jpg", "abc");
    auto entries = fx.fs->readdir(kInt + "/DCIM");
    CHECK_EQ(entries[0].name, std::string("."));
    CHECK_EQ(entries[1].name, std::string(".."));
    CHECK_EQ(entries.size(), size_t{3});
    CHECK_EQ(entries[0].ino, fx.fs->getattr(kInt + "/DCIM").ino);
    CHECK_EQ(entries[1].ino, fx.fs->getattr(kInt).ino);
    CHECK_EQ(entries[2].ino, fx.fs->getattr(kInt + "/DCIM/a.jpg").ino);
    CHECK_EQ(fx.fs->getattr(kInt).ino, fx.fs->getattr(kInt).ino);  // stable
}

AF_TEST(fake_filesystem, listing_is_lazy_and_cached) {
    FsFixture fx;
    auto dcim = fx.phone->addDir(fx.internal, 0, "DCIM");
    auto cam = fx.phone->addDir(fx.internal, dcim, "Camera");
    auto pics = fx.phone->addDir(fx.internal, 0, "Pictures");
    auto other = fx.phone->addDir(fx.internal, pics, "Screenshots");
    (void)other;
    fx.phone->addFile(fx.internal, cam, "a.jpg", "abc");
    names(*fx.fs, kInt + "/DCIM/Camera");
    // Only the folders on the path were listed: root of the storage, DCIM and Camera - never Pictures.
    auto parents = fx.phone->listedParents();
    CHECK(std::count(parents.begin(), parents.end(), pics) == 0);
    CHECK(std::count(parents.begin(), parents.end(), ObjectId{0}) >= 1);
    CHECK_EQ(fx.phone->calls(Op::List), size_t{3});
    // A second resolution within the TTL is served from the cache.
    fx.fs->getattr(kInt + "/DCIM/Camera/a.jpg");
    CHECK_EQ(fx.phone->calls(Op::List), size_t{3});
}

AF_TEST(fake_filesystem, ten_thousand_entries) {
    FsFixture fx;
    auto big = fx.phone->addDir(fx.internal, 0, "Big");
    auto untouched = fx.phone->addDir(fx.internal, 0, "Untouched");
    for (int i = 0; i < 10000; ++i) fx.phone->addFile(fx.internal, big, "IMG_" + std::to_string(i) + ".jpg", "x");
    fx.phone->addFile(fx.internal, untouched, "secret.txt", "x");
    auto entries = fx.fs->readdir(kInt + "/Big");
    CHECK_EQ(entries.size(), size_t{10002});
    std::set<uint64_t> inos;
    for (auto& e : entries) inos.insert(e.ino);
    CHECK(inos.size() >= 10001);  // unique inodes (".." may equal storage's)
    auto parents = fx.phone->listedParents();
    CHECK(std::count(parents.begin(), parents.end(), untouched) == 0);  // no recursive scan of unrelated folders
    const size_t listCalls = fx.phone->calls(Op::List);
    fx.fs->getattr(kInt + "/Big/IMG_9999.jpg");
    fx.fs->getattr(kInt + "/Big/IMG_5000.jpg");
    CHECK_EQ(fx.phone->calls(Op::List), listCalls);  // lookups use the cached listing
}

AF_TEST(fake_filesystem, cache_expires_and_sees_phone_changes) {
    auto now = std::chrono::steady_clock::now();
    MetadataCache::Config cfg;
    FsFixture fx;
    auto d = fx.phone->addDir(fx.internal, 0, "D");
    // Use real TTL expiry with a 0 ms TTL: every access re-lists.
    cfg.dirTtl = std::chrono::milliseconds(0);
    auto fs = fx.make(fx.phone, false, cfg);
    CHECK(names(*fs, kInt + "/D").empty());
    fx.phone->addFile(fx.internal, d, "late.txt", "x");
    CHECK_EQ(names(*fs, kInt + "/D"), (std::set<std::string>{"late.txt"}));
    (void)now;
}

// ---- unicode / odd names ---------------------------------------------------------------------------

AF_TEST(fake_filesystem, turkish_emoji_and_nfd_lookup) {
    FsFixture fx;
    const std::string tr =
        "T\xc3\xbcrk\xc3\xa7"
        "e \xc5\x9f\xc4\xb1k \xc4\xb0stanbul.txt";  // Türkçe şık İstanbul.txt
    const std::string emoji = "\xf0\x9f\x93\xb7 foto.jpg";
    fx.phone->addFile(fx.internal, 0, tr, "merhaba");
    fx.phone->addFile(fx.internal, 0, emoji, "cam");
    fx.phone->addFile(fx.internal, 0, "caf\xc3\xa9.txt", "nfc");
    CHECK_EQ(readAll(*fx.fs, kInt + "/" + tr), std::string("merhaba"));
    CHECK_EQ(readAll(*fx.fs, kInt + "/" + emoji), std::string("cam"));
    CHECK_EQ(readAll(*fx.fs, kInt + "/cafe\xcc\x81.txt"),
             std::string("nfc"));  // Finder/NFD spelling finds the NFC file
}

AF_TEST(fake_filesystem, duplicate_and_case_colliding_names_are_all_reachable) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "a.txt", "first");
    fx.phone->addFile(fx.internal, 0, "a.txt", "second");
    fx.phone->addFile(fx.internal, 0, "A.TXT", "upper");
    CHECK_EQ(names(*fx.fs, kInt), (std::set<std::string>{"a.txt", "a (2).txt", "A.TXT"}));
    CHECK_EQ(readAll(*fx.fs, kInt + "/a.txt"), std::string("first"));
    CHECK_EQ(readAll(*fx.fs, kInt + "/a (2).txt"), std::string("second"));
    CHECK_EQ(readAll(*fx.fs, kInt + "/A.TXT"), std::string("upper"));
}

// ---- reads ----------------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, read_exact_bytes_offsets_and_sizes) {
    FsFixture fx(false, true, false, aftest::noCache());
    for (size_t n : {size_t{0}, size_t{1}, size_t{14}, size_t{15}, size_t{1024}, size_t{100000}}) {
        std::string content;
        for (size_t i = 0; i < n; ++i) content.push_back(static_cast<char>(i * 31 + 7));
        const std::string name = "f" + std::to_string(n);
        fx.phone->addFile(fx.internal, 0, name, content);
        CHECK_EQ(readAll(*fx.fs, kInt + "/" + name), content);
        CHECK_EQ(readAll(*fx.fs, kInt + "/" + name, 7), content);  // odd chunk size
        CHECK_EQ(fx.fs->getattr(kInt + "/" + name).size, static_cast<uint64_t>(n));
    }
    // Odd offsets.
    fx.phone->addFile(fx.internal, 0, "odd", "0123456789abcdef");
    const auto fh = fx.fs->open(kInt + "/odd", O_RDONLY);
    std::vector<std::byte> b(5);
    CHECK_EQ(fx.fs->read(fh, 3, b), size_t{5});
    CHECK_EQ(str(b), std::string("34567"));
    CHECK_EQ(fx.fs->read(fh, 13, b), size_t{3});  // short only at EOF
    CHECK_EQ(std::string(reinterpret_cast<char*>(b.data()), 3), std::string("def"));
    fx.fs->release(fh);
}

AF_TEST(eof_regression, zero_length_and_past_eof_reads) {
    // Targets the macFUSE/FSKit issue #1196 class at the filesystem layer: a read at/after EOF
    // returns exactly 0 bytes, never padding, never stale data, never an error.
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "empty", "");
    fx.phone->addFile(fx.internal, 0, "five", "12345");
    std::vector<std::byte> buf(64, std::byte{0xAA});

    auto fh = fx.fs->open(kInt + "/empty", O_RDONLY);
    CHECK_EQ(fx.fs->read(fh, 0, buf), size_t{0});
    CHECK_EQ(buf[0], std::byte{0xAA});  // destination untouched
    CHECK_EQ(fx.fs->read(fh, 1000, buf), size_t{0});
    fx.fs->release(fh);

    fh = fx.fs->open(kInt + "/five", O_RDONLY);
    CHECK_EQ(fx.fs->read(fh, 5, buf), size_t{0});           // exactly at EOF
    CHECK_EQ(fx.fs->read(fh, 6, buf), size_t{0});           // past EOF
    CHECK_EQ(fx.fs->read(fh, UINT64_MAX, buf), size_t{0});  // absurd offset, no overflow
    CHECK_EQ(fx.fs->read(fh, UINT64_MAX - 2, buf), size_t{0});
    CHECK_EQ(fx.fs->read(fh, 0, buf), size_t{5});                     // oversize request returns the whole file
    CHECK_EQ(fx.fs->read(fh, 0, std::span<std::byte>{}), size_t{0});  // zero-length request
    CHECK_EQ(buf[5], std::byte{0xAA});                                // nothing written beyond EOF
    fx.fs->release(fh);
}

AF_TEST(eof_regression, short_device_reads_are_never_reported_as_eof) {
    // A phone that returns fewer bytes than asked mid-file must not truncate the read, and a
    // phone that returns nothing before the advertised size is an error (never padded/faked).
    struct ShortDevice final : IMtpDevice {
        FakeMtpDevice inner;
        bool dropAll{false};
        size_t cap{3};
        DeviceDescriptor descriptor() const override { return inner.descriptor(); }
        DeviceCapabilities capabilities() const override { return inner.capabilities(); }
        std::vector<StorageDescriptor> storages() override { return inner.storages(); }
        std::vector<FileNode> list(StorageId s, ObjectId p) override { return inner.list(s, p); }
        FileNode stat(ObjectId i) override { return inner.stat(i); }
        size_t read(ObjectId i, uint64_t off, std::span<std::byte> d) override {
            if (dropAll) return 0;
            return inner.read(i, off, d.first(std::min(d.size(), cap)));
        }
        void download(ObjectId i, const std::filesystem::path& t, ProgressCallback p) override {
            inner.download(i, t, p);
        }
        ObjectId upload(StorageId s, ObjectId p, const std::filesystem::path& f, std::string_view n,
                        ProgressCallback pr) override {
            return inner.upload(s, p, f, n, pr);
        }
        ObjectId mkdir(StorageId s, ObjectId p, std::string_view n) override { return inner.mkdir(s, p, n); }
        void remove(ObjectId i, bool d) override { inner.remove(i, d); }
        void rename(ObjectId i, std::string_view n) override { inner.rename(i, n); }
    };
    FsFixture fx;
    auto dev = std::make_shared<ShortDevice>();
    const auto sid = dev->inner.addStorage("Internal Storage", 1ull << 30, 1ull << 29);
    dev->inner.addFile(sid, 0, "f", "0123456789");
    auto fs = fx.make(dev, false);
    CHECK_EQ(readAll(*fs, kInt + "/f"), std::string("0123456789"));  // looped until satisfied
    dev->dropAll = true;
    const auto fh = fs->open(kInt + "/f", O_RDONLY);
    std::vector<std::byte> buf(10);
    CHECK_ERRNO(fs->read(fh, 0, buf), EIO);
}

AF_TEST(fake_filesystem, sizes_above_4gib_use_64_bit_offsets) {
    FsFixture fx;
    const uint64_t size = (5ull << 30) + 123;  // 5 GiB + 123
    fx.phone->addSyntheticFile(fx.internal, 0, "huge.bin", size, 9);
    CHECK_EQ(fx.fs->getattr(kInt + "/huge.bin").size, size);
    const auto fh = fx.fs->open(kInt + "/huge.bin", O_RDONLY);
    for (uint64_t off : {(4ull << 30) - 3, (4ull << 30) + 7, size - 20}) {
        std::vector<std::byte> b(16);
        CHECK_EQ(fx.fs->read(fh, off, b), size_t{16});
        for (size_t i = 0; i < b.size(); ++i) CHECK_EQ(b[i], std::byte{FakeMtpDevice::patternByte(9, off + i)});
    }
    std::vector<std::byte> tail(500);
    CHECK_EQ(fx.fs->read(fh, size - 100, tail), size_t{100});
    CHECK_EQ(fx.fs->read(fh, size, tail), size_t{0});
    fx.fs->release(fh);
    auto s = fx.fs->statfs(kInt);
    CHECK_EQ(s.blocks, (64ull << 30) / 4096);
}

AF_TEST(fake_filesystem, staged_read_fallback_when_partial_reads_unsupported) {
    FsFixture fx(false, /*partialRead=*/false);
    std::string content(300000, 'x');
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i * 7);
    fx.phone->addFile(fx.internal, 0, "big.bin", content);
    CHECK_EQ(readAll(*fx.fs, kInt + "/big.bin", 8192), content);
    CHECK_EQ(readAll(*fx.fs, kInt + "/big.bin", 1000), content);
    CHECK_EQ(fx.phone->calls(Op::Download), size_t{1});  // downloaded once, then served locally
    CHECK_EQ(fx.phone->calls(Op::Read), size_t{0});      // no partial reads attempted on a device without them
}

AF_TEST(fake_filesystem, staged_read_refuses_files_over_the_budget) {
    FsFixture fx(false, false);
    fx.phone->addSyntheticFile(fx.internal, 0, "toolarge.bin", 300ull << 20, 1);  // > 256 MiB read-cache quota
    const auto fh = fx.fs->open(kInt + "/toolarge.bin", O_RDONLY);
    std::vector<std::byte> b(16);
    CHECK_ERRNO(fx.fs->read(fh, 0, b), EFBIG);  // explicit, not truncated or faked
}

AF_TEST(fake_filesystem, device_refusing_partial_reads_at_runtime_falls_back) {
    FsFixture fx;  // advertises GetPartialObject but then refuses (buggy responder)
    fx.phone->addFile(fx.internal, 0, "f", "abcdefghij");
    fx.phone->failNext(Op::Read, MtpErrc::OperationUnsupported, -1);
    CHECK_EQ(readAll(*fx.fs, kInt + "/f", 4), std::string("abcdefghij"));
}

AF_TEST(fake_filesystem, concurrent_readers_get_correct_data) {
    FsFixture fx;
    std::vector<std::string> contents;
    for (int i = 0; i < 8; ++i) {
        std::string c(50000, static_cast<char>('a' + i));
        for (size_t k = 0; k < c.size(); k += 97) c[k] = static_cast<char>(k);
        contents.push_back(c);
        fx.phone->addFile(fx.internal, 0, "f" + std::to_string(i), c);
    }
    std::vector<std::thread> ts;
    std::atomic<int> bad{0};
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&, i] {
            try {
                for (int r = 0; r < 5; ++r)
                    if (readAll(*fx.fs, kInt + "/f" + std::to_string(i), 3000 + i) != contents[static_cast<size_t>(i)])
                        ++bad;
            } catch (...) {
                ++bad;
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(bad.load(), 0);
}

// ---- open flags / in-place edits ------------------------------------------------------------------------

AF_TEST(fake_filesystem, open_flags_for_existing_files) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "f", "original");
    // FSKit may open read/write even for read-only use: reading keeps working...
    const auto fh = fx.fs->open(kInt + "/f", O_RDWR);
    std::vector<std::byte> b(8);
    CHECK_EQ(fx.fs->read(fh, 0, b), size_t{8});
    // ...but modifying the phone's file in place is refused, truthfully.
    CHECK_ERRNO(fx.fs->write(fh, 0, bytesOf("X")), EOPNOTSUPP);
    fx.fs->release(fh);
    CHECK_ERRNO(fx.fs->open(kInt + "/f", O_WRONLY), EOPNOTSUPP);
    CHECK_ERRNO(fx.fs->open(kInt + "/f", O_RDWR | O_TRUNC), EOPNOTSUPP);
    CHECK_ERRNO(fx.fs->open(kInt, O_RDONLY), EISDIR);
    CHECK_EQ(readAll(*fx.fs, kInt + "/f"), std::string("original"));  // untouched
}

AF_TEST(fake_filesystem, read_only_mount_open_semantics) {
    FsFixture fx(true);
    fx.phone->addFile(fx.internal, 0, "f", "data");
    const auto fh = fx.fs->open(kInt + "/f", O_RDWR);  // allowed: reads work
    std::vector<std::byte> b(4);
    CHECK_EQ(fx.fs->read(fh, 0, b), size_t{4});
    CHECK_ERRNO(fx.fs->write(fh, 0, bytesOf("X")), EROFS);
    fx.fs->release(fh);
    CHECK_ERRNO(fx.fs->open(kInt + "/f", O_WRONLY), EROFS);
}

// ---- writes -------------------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, staged_create_commits_only_after_flush) {
    FsFixture fx;
    const size_t before = fx.phone->objectCount();
    const auto fh = fx.fs->create(kInt + "/new.txt", 0644);
    fx.fs->write(fh, 0, bytesOf("hello phone"));
    // Nothing reached the phone yet: no premature success.
    CHECK_EQ(fx.phone->objectCount(), before);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});
    // The file is visible to the writer's own process meanwhile (getattr/readdir/read-back).
    CHECK_EQ(fx.fs->getattr(kInt + "/new.txt").size, uint64_t{11});
    CHECK(names(*fx.fs, kInt).count("new.txt") == 1);
    std::vector<std::byte> rb(11);
    CHECK_EQ(fx.fs->read(fh, 0, rb), size_t{11});
    CHECK_EQ(str(rb), std::string("hello phone"));

    fx.fs->flush(fh);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    CHECK_EQ(fx.phone->objectCount(), before + 1);
    fx.fs->release(fh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/new.txt"), std::string("hello phone"));
    CHECK_EQ(fx.fs->pendingUploadCount(), size_t{0});
    CHECK_EQ(fx.fs->counters().uploadsOk.load(), uint64_t{1});
}

AF_TEST(fake_filesystem, flush_and_fsync_are_idempotent_and_release_does_not_reupload) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/n", 0644);
    fx.fs->write(fh, 0, bytesOf("abc"));
    fx.fs->flush(fh);
    fx.fs->flush(fh);
    fx.fs->fsync(fh);
    fx.fs->release(fh);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
}

AF_TEST(fake_filesystem, release_without_flush_still_commits) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/n", 0644);
    fx.fs->write(fh, 0, bytesOf("abc"));
    fx.fs->release(fh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/n"), std::string("abc"));
}

AF_TEST(fake_filesystem, write_after_commit_is_refused) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/n", 0644);
    fx.fs->write(fh, 0, bytesOf("abc"));
    fx.fs->flush(fh);
    CHECK_ERRNO(fx.fs->write(fh, 3, bytesOf("def")), EOPNOTSUPP);
    fx.fs->release(fh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/n"), std::string("abc"));
}

AF_TEST(fake_filesystem, empty_file_create_waits_then_uploads) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/empty", 0644);
    fx.fs->flush(fh);
    fx.fs->release(fh);
    // Not on the phone yet (FSKit may re-open it), but visible and empty for the user.
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});
    CHECK_EQ(fx.fs->getattr(kInt + "/empty").size, uint64_t{0});
    CHECK_EQ(fx.fs->pendingUploadCount(), size_t{1});
    fx.fs->housekeeping();                               // grace period (5 s) has not elapsed
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});
    fx.fs->housekeeping(true);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    CHECK_EQ(fx.fs->pendingUploadCount(), size_t{0});
    CHECK_EQ(fx.fs->getattr(kInt + "/empty").size, uint64_t{0});
    CHECK_EQ(readAll(*fx.fs, kInt + "/empty"), std::string());
}

AF_TEST(fake_filesystem, grace_period_expiry_uploads_a_touched_file) {
    FsFixture fx;
    FsConfig cfg = fx.fs->config();
    cfg.emptyFileGrace = std::chrono::milliseconds(20);
    MtpFilesystem fs(fx.phone, cfg);
    fs.release(fs.create(kInt + "/touched", 0644));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    fs.housekeeping();
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
}

AF_TEST(fake_filesystem, fskit_create_release_reopen_write_pattern) {
    // macFUSE's FSKit backend issues CREATE, SETATTR, RELEASE and only then a separate OPEN(O_WRONLY) for the
    // application's writes. The empty file must survive that gap and be uploaded once, with the real content.
    FsFixture fx;
    fx.fs->release(fx.fs->create(kInt + "/f.bin", 0644));
    fx.fs->utimens(kInt + "/f.bin");                      // SETATTR between create and open
    const auto fh = fx.fs->open(kInt + "/f.bin", O_WRONLY);
    fx.fs->write(fh, 0, bytesOf("payload"));
    fx.fs->flush(fh);
    fx.fs->release(fh);
    fx.fs->housekeeping(true);                            // nothing left to do
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    CHECK_EQ(readAll(*fx.fs, kInt + "/f.bin"), std::string("payload"));
    // A re-opened lingering file stops lingering: housekeeping must not upload it empty underneath the writer.
    fx.fs->release(fx.fs->create(kInt + "/g.bin", 0644));
    const auto gh = fx.fs->open(kInt + "/g.bin", O_WRONLY);
    fx.fs->housekeeping(true);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    fx.fs->write(gh, 0, bytesOf("late"));
    fx.fs->release(gh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/g.bin"), std::string("late"));
}

AF_TEST(fake_filesystem, lingering_empty_file_can_be_removed_or_renamed) {
    FsFixture fx;
    fx.fs->release(fx.fs->create(kInt + "/gone", 0644));
    fx.fs->unlink(kInt + "/gone");                        // only the local staging file disappears
    CHECK_ERRNO(fx.fs->getattr(kInt + "/gone"), ENOENT);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});

    fx.fs->release(fx.fs->create(kInt + "/before", 0644));
    fx.fs->rename(kInt + "/before", kInt + "/after");     // needs the object on the phone first
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    CHECK_ERRNO(fx.fs->getattr(kInt + "/before"), ENOENT);
    CHECK_EQ(fx.fs->getattr(kInt + "/after").size, uint64_t{0});

    const auto open = fx.fs->create(kInt + "/busy", 0644);   // a file with an open handle is never touched
    CHECK_ERRNO(fx.fs->unlink(kInt + "/busy"), EBUSY);
    CHECK_ERRNO(fx.fs->rename(kInt + "/busy", kInt + "/busy2"), EBUSY);
    fx.fs->release(open);
}

AF_TEST(fake_filesystem, explicit_fsync_uploads_an_empty_file_immediately) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/durable", 0644);
    fx.fs->fsync(fh);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    fx.fs->release(fh);
}

AF_TEST(fake_filesystem, tiny_file_round_trips_1_to_15_bytes) {
    FsFixture fx;
    for (size_t n = 1; n <= 16; ++n) {
        std::string c;
        for (size_t i = 0; i < n; ++i) c.push_back(static_cast<char>('a' + i));
        const std::string path = kInt + "/t" + std::to_string(n);
        const auto fh = fx.fs->create(path, 0644);
        for (size_t i = 0; i < n; ++i)
            fx.fs->write(fh, i, bytesOf(std::string(1, c[i])));  // byte-at-a-time like a 1-byte writer
        fx.fs->flush(fh);
        fx.fs->release(fh);
        CHECK_EQ(readAll(*fx.fs, path), c);
        CHECK_EQ(fx.fs->getattr(path).size, static_cast<uint64_t>(n));
    }
}

AF_TEST(fake_filesystem, large_nonsequential_upload_hash_round_trip) {
    FsFixture fx;
    std::string data(3 * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 2654435761u) >> 24);
    const auto fh = fx.fs->create(kInt + "/big.bin", 0644);
    // Write 64 KiB blocks in reverse order, plus an unaligned tail.
    const size_t blk = 65536;
    for (size_t off = (data.size() / blk) * blk;; off -= blk) {
        fx.fs->write(fh, off, bytesOf(std::string_view(data).substr(off, std::min(blk, data.size() - off))));
        if (off == 0) break;
    }
    fx.fs->flush(fh);
    fx.fs->release(fh);
    const std::string back = readAll(*fx.fs, kInt + "/big.bin", 100000);
    Sha256 a, b;
    a.update(data.data(), data.size());
    b.update(back.data(), back.size());
    CHECK_EQ(a.hexDigest(), b.hexDigest());
}

AF_TEST(fake_filesystem, create_rejects_existing_and_bad_targets) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "exists.txt", "keep");
    CHECK_ERRNO(fx.fs->create(kInt + "/exists.txt", 0644), EEXIST);
    CHECK_ERRNO(fx.fs->create("/rootfile", 0644), EPERM);  // next to storages
    CHECK_ERRNO(fx.fs->create(kInt + "/nodir/x", 0644), ENOENT);
    CHECK_ERRNO(fx.fs->create(kInt + "/exists.txt/x", 0644), ENOTDIR);
    CHECK_ERRNO(fx.fs->create(kInt + "/" + std::string(256, 'n'), 0644), ENAMETOOLONG);
    CHECK_EQ(readAll(*fx.fs, kInt + "/exists.txt"), std::string("keep"));
}

AF_TEST(fake_filesystem, failed_upload_reports_error_and_preserves_staged_bytes) {
    FsFixture fx;
    fx.phone->setStorageFree(fx.internal, 4);  // phone full
    const size_t before = fx.phone->objectCount();
    const auto fh = fx.fs->create(kInt + "/doomed.bin", 0644);
    fx.fs->write(fh, 0, bytesOf("0123456789"));
    CHECK_ERRNO(fx.fs->flush(fh), ENOSPC);  // the caller of close()/fsync() sees the failure
    CHECK_ERRNO(fx.fs->flush(fh), ENOSPC);  // durable: same answer next time, no blind retry
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
    fx.fs->release(fh);
    CHECK_EQ(fx.phone->objectCount(), before);                  // nothing created, nothing deleted
    CHECK_ERRNO(fx.fs->getattr(kInt + "/doomed.bin"), ENOENT);  // a file that is not on the phone is not shown
    CHECK_EQ(fx.fs->counters().uploadsFailed.load(), uint64_t{1});
    auto rec = fx.fs->recoverableUploads();
    CHECK_EQ(rec.size(), size_t{1});
    CHECK_EQ(rec[0].finalName, std::string("doomed.bin"));
    CHECK_EQ(rec[0].sizeBytes, uint64_t{10});
    CHECK(std::filesystem::exists(rec[0].stagedFile));  // local bytes preserved for recovery
}

AF_TEST(fake_filesystem, upload_duplicate_appearing_on_phone_is_not_overwritten) {
    FsFixture fx(false, true, false, aftest::noCache());
    const auto fh = fx.fs->create(kInt + "/race.txt", 0644);
    fx.fs->write(fh, 0, bytesOf("mine"));
    fx.phone->addFile(fx.internal, 0, "race.txt", "theirs");  // appears between create and flush
    CHECK_ERRNO(fx.fs->flush(fh), EEXIST);
    fx.fs->release(fh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/race.txt"), std::string("theirs"));
}

AF_TEST(fake_filesystem, staging_quota_exhaustion_is_enospc) {
    FsFixture fx;
    auto fs = fx.make(fx.phone, false);  // 256 MiB quota
    const auto fh = fs->create(kInt + "/huge", 0644);
    CHECK_ERRNO(fs->write(fh, 300ull << 20, bytesOf("x")), ENOSPC);
    fs->release(fh);
}

AF_TEST(fake_filesystem, ftruncate_and_truncate_on_staged_files) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/t", 0644);
    fx.fs->write(fh, 0, bytesOf("0123456789"));
    fx.fs->ftruncate(fh, 4);
    fx.fs->truncate(kInt + "/t", 3);
    fx.fs->flush(fh);
    fx.fs->release(fh);
    CHECK_EQ(readAll(*fx.fs, kInt + "/t"), std::string("012"));
}

AF_TEST(fake_filesystem, truncate_existing_file_semantics) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "f", "12345");
    fx.fs->truncate(kInt + "/f", 5);  // no-op to the same size is harmless
    CHECK_ERRNO(fx.fs->truncate(kInt + "/f", 0), EOPNOTSUPP);
    CHECK_ERRNO(fx.fs->truncate(kInt + "/f", 99), EOPNOTSUPP);
    CHECK_EQ(readAll(*fx.fs, kInt + "/f"), std::string("12345"));
}

// ---- Finder metadata files ------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, finder_metadata_files_stay_in_ram) {
    FsFixture fx;
    const size_t before = fx.phone->objectCount();
    writeFile(*fx.fs, kInt + "/.DS_Store", "binary-ds-store");
    writeFile(*fx.fs, kInt + "/._photo.jpg", "appledouble");
    CHECK_EQ(fx.phone->objectCount(), before);  // never sent to the phone
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});
    CHECK_EQ(readAll(*fx.fs, kInt + "/.DS_Store"), std::string("binary-ds-store"));  // but they really read back
    CHECK(names(*fx.fs, kInt).count("._photo.jpg") == 1);
    fx.fs->unlink(kInt + "/._photo.jpg");
    CHECK(names(*fx.fs, kInt).count("._photo.jpg") == 0);
    fx.fs->rename(kInt + "/.DS_Store", kInt + "/._renamed");
    CHECK_EQ(readAll(*fx.fs, kInt + "/._renamed"), std::string("binary-ds-store"));
    CHECK_ERRNO(fx.fs->mkdir(kInt + "/.Spotlight-V100"), EPERM);
}

AF_TEST(fake_filesystem, metadata_passthrough_uploads_them_instead) {
    FsFixture fx;
    FsConfig cfg = fx.fs->config();
    cfg.metadataOverlay = false;
    MtpFilesystem fs(fx.phone, cfg);
    writeFile(fs, kInt + "/.DS_Store", "x");
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{1});
}

// ---- mutations -----------------------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, mkdir_unlink_rmdir) {
    FsFixture fx;
    fx.fs->mkdir(kInt + "/Docs");
    CHECK(fx.fs->getattr(kInt + "/Docs").isDir);
    CHECK_ERRNO(fx.fs->mkdir(kInt + "/Docs"), EEXIST);
    CHECK_ERRNO(fx.fs->mkdir("/Newstorage"), EPERM);
    CHECK_ERRNO(fx.fs->mkdir(kInt + "/a/b"), ENOENT);
    writeFile(*fx.fs, kInt + "/Docs/a.txt", "a");
    CHECK_ERRNO(fx.fs->rmdir(kInt + "/Docs"), ENOTEMPTY);  // never a silent recursive delete
    CHECK_EQ(readAll(*fx.fs, kInt + "/Docs/a.txt"), std::string("a"));
    CHECK_ERRNO(fx.fs->unlink(kInt + "/Docs"), EISDIR);
    CHECK_ERRNO(fx.fs->rmdir(kInt + "/Docs/a.txt"), ENOTDIR);
    fx.fs->unlink(kInt + "/Docs/a.txt");
    CHECK_ERRNO(fx.fs->getattr(kInt + "/Docs/a.txt"), ENOENT);
    fx.fs->rmdir(kInt + "/Docs");
    CHECK_ERRNO(fx.fs->getattr(kInt + "/Docs"), ENOENT);
    CHECK_ERRNO(fx.fs->unlink(kInt + "/missing"), ENOENT);
    CHECK_ERRNO(fx.fs->rmdir(kInt), EPERM);
    CHECK_ERRNO(fx.fs->unlink("/"), EPERM);
}

AF_TEST(fake_filesystem, delete_touches_only_the_target_object) {
    FsFixture fx;
    auto d = fx.phone->addDir(fx.internal, 0, "D");
    auto keep1 = fx.phone->addFile(fx.internal, d, "keep1", "1");
    auto victim = fx.phone->addFile(fx.internal, d, "victim", "2");
    auto keep2 = fx.phone->addFile(fx.internal, 0, "victim", "3");  // same name elsewhere
    auto before = fx.phone->objectCount();
    fx.fs->unlink(kInt + "/D/victim");
    CHECK_EQ(fx.phone->objectCount(), before - 1);
    CHECK(!fx.phone->exists(victim));
    CHECK(fx.phone->exists(keep1) && fx.phone->exists(keep2) && fx.phone->exists(d));
}

AF_TEST(fake_filesystem, rename_rules) {
    FsFixture fx(false, true, true);
    fx.phone->addFile(fx.internal, 0, "a.txt", "A");
    fx.phone->addFile(fx.internal, 0, "b.txt", "B");
    auto sub = fx.phone->addDir(fx.internal, 0, "Sub");
    (void)sub;
    auto fs = fx.make(fx.phone, false);
    fs->rename(kInt + "/a.txt", kInt + "/renamed.txt");
    CHECK_ERRNO(fs->getattr(kInt + "/a.txt"), ENOENT);
    CHECK_EQ(readAll(*fs, kInt + "/renamed.txt"), std::string("A"));
    // No fake atomic overwrite: replacing an existing name is refused and both files survive.
    CHECK_ERRNO(fs->rename(kInt + "/renamed.txt", kInt + "/b.txt"), EOPNOTSUPP);
    CHECK_EQ(readAll(*fs, kInt + "/b.txt"), std::string("B"));
    CHECK_EQ(readAll(*fs, kInt + "/renamed.txt"), std::string("A"));
    // No silent copy+delete across folders or storages.
    CHECK_ERRNO(fs->rename(kInt + "/b.txt", kInt + "/Sub/b.txt"), EXDEV);
    CHECK_ERRNO(fs->rename(kInt + "/b.txt", "/SD Card/b.txt"), EXDEV);
    CHECK_ERRNO(fs->rename(kInt, "/Other"), EPERM);
    CHECK_ERRNO(fs->rename(kInt + "/missing", kInt + "/x"), ENOENT);
    fs->rename(kInt + "/b.txt", kInt + "/b.txt");  // same name: no-op, no device call
    CHECK_EQ(fx.phone->calls(Op::Rename), size_t{1});
}

AF_TEST(fake_filesystem, rename_unsupported_by_device) {
    FakeMtpDevice::Options o;
    o.renameSupported = false;
    auto phone = std::make_shared<FakeMtpDevice>(o);
    const auto s = phone->addStorage("Internal Storage", 1ull << 30, 1ull << 29);
    phone->addFile(s, 0, "a", "A");
    FsFixture fx;
    auto fs = fx.make(phone, false);
    CHECK_ERRNO(fs->rename(kInt + "/a", kInt + "/b"), EOPNOTSUPP);
    CHECK_EQ(readAll(*fs, kInt + "/a"), std::string("A"));
}

AF_TEST(fake_filesystem, mutations_are_refused_on_read_only_mount) {
    FsFixture fx(true);
    fx.phone->addFile(fx.internal, 0, "a", "A");
    fx.phone->addDir(fx.internal, 0, "d");
    const size_t objs = fx.phone->objectCount();
    CHECK_ERRNO(fx.fs->create(kInt + "/n", 0644), EROFS);
    CHECK_ERRNO(fx.fs->mkdir(kInt + "/n"), EROFS);
    CHECK_ERRNO(fx.fs->unlink(kInt + "/a"), EROFS);
    CHECK_ERRNO(fx.fs->rmdir(kInt + "/d"), EROFS);
    CHECK_ERRNO(fx.fs->rename(kInt + "/a", kInt + "/z"), EROFS);
    CHECK_ERRNO(fx.fs->truncate(kInt + "/a", 0), EROFS);
    CHECK_ERRNO(fx.fs->utimens(kInt + "/a"), EROFS);
    CHECK_EQ(fx.phone->objectCount(), objs);
    CHECK_EQ(fx.phone->calls(Op::Upload) + fx.phone->calls(Op::Remove) + fx.phone->calls(Op::Rename) +
                 fx.phone->calls(Op::Mkdir),
             size_t{0});
}

AF_TEST(fake_filesystem, attribute_changes_are_honest) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "a", "A");
    CHECK_ERRNO(fx.fs->utimens(kInt + "/a"), EOPNOTSUPP);
    CHECK_ERRNO(fx.fs->chmod(kInt + "/a", 0600), EPERM);
    fx.fs->chmod(kInt + "/a", 0644);  // equal to what we already report
    CHECK_ERRNO(fx.fs->chown(kInt + "/a", 0, 0), EPERM);
    fx.fs->chown(kInt + "/a", -1, -1);
    FsConfig cfg = fx.fs->config();
    cfg.acceptMetadataNoops = true;
    MtpFilesystem lenient(fx.phone, cfg);
    lenient.utimens(kInt + "/a");
    lenient.chmod(kInt + "/a", 0600);
    // Newly written files accept the utimens that cp/Finder issue right after a copy.
    const auto fh = fx.fs->create(kInt + "/new", 0644);
    fx.fs->utimens(kInt + "/new");
    fx.fs->release(fh);
}

AF_TEST(fake_filesystem, timestamps_accepted_only_for_objects_created_here) {
    // FSKit sets creation/modification times right after mkdir/create; MTP cannot store them.
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "old.txt", "x");
    fx.fs->mkdir(kInt + "/newdir");
    fx.fs->utimens(kInt + "/newdir");                       // fresh: accepted as a no-op
    writeFile(*fx.fs, kInt + "/new.txt", "abc");
    fx.fs->utimens(kInt + "/new.txt");                      // committed a moment ago in this session
    CHECK_ERRNO(fx.fs->utimens(kInt + "/old.txt"), EOPNOTSUPP);   // a file that was already on the phone: honest refusal
}

AF_TEST(fake_filesystem, statfs_values) {
    FsFixture fx(false, true, true);
    auto root = fx.fs->statfs("/");
    CHECK_EQ(root.blocks, ((64ull << 30) + (16ull << 30)) / 4096);
    CHECK_EQ(root.blocksFree, ((32ull << 30) + (8ull << 30)) / 4096);
    auto sd = fx.fs->statfs("/SD Card");
    CHECK_EQ(sd.blocks, (16ull << 30) / 4096);
    FsFixture ro(true);
    CHECK_EQ(ro.fs->statfs("/").blocksAvail, uint64_t{0});
    auto bare = std::make_shared<FakeMtpDevice>();
    bare->addStorage("NoCapacity", 0, 0);
    auto fs = fx.make(bare, false);
    CHECK_ERRNO(fs->statfs("/"), EOPNOTSUPP);  // unknown capacity is reported as such, not as "0 bytes free"
}

// ---- disconnection / reconnection
// ---------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, unplug_during_list_is_enodev_and_sticky) {
    FsFixture fx;
    fx.phone->addDir(fx.internal, 0, "D");
    int notified = 0;
    fx.fs->setDisconnectCallback([&] { ++notified; });
    fx.phone->unplugDuring(Op::List);
    CHECK_ERRNO(fx.fs->readdir(kInt), ENODEV);
    CHECK(fx.fs->isDisconnected());
    CHECK_ERRNO(fx.fs->getattr("/"), ENODEV);  // no more device traffic
    CHECK_ERRNO(fx.fs->readdir(kInt), ENODEV);
    CHECK_EQ(notified, 1);
}

AF_TEST(fake_filesystem, unplug_during_read) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "f", std::string(1000, 'x'));
    const auto fh = fx.fs->open(kInt + "/f", O_RDONLY);
    fx.phone->unplugDuring(Op::Read);
    std::vector<std::byte> b(100);
    CHECK_ERRNO(fx.fs->read(fh, 0, b), ENODEV);
    CHECK(fx.fs->isDisconnected());
    fx.fs->release(fh);  // releasing handles after unplug must not hang or throw
}

AF_TEST(fake_filesystem, unplug_during_upload_keeps_staged_bytes_and_never_fakes_success) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/n.bin", 0644);
    fx.fs->write(fh, 0, bytesOf("precious data"));
    fx.phone->unplugDuring(Op::Upload);
    CHECK_ERRNO(fx.fs->flush(fh), ENODEV);
    fx.fs->release(fh);
    CHECK_EQ(fx.phone->objectCount(), size_t{0});
    auto rec = fx.fs->recoverableUploads();
    CHECK_EQ(rec.size(), size_t{1});
    CHECK(std::filesystem::exists(rec[0].stagedFile));
    CHECK_EQ(fx.fs->counters().uploadsOk.load(), uint64_t{0});
}

AF_TEST(fake_filesystem, unplug_during_rename_and_fsync) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "a", "A");
    fx.fs->getattr(kInt + "/a");
    fx.phone->unplugDuring(Op::Rename);
    CHECK_ERRNO(fx.fs->rename(kInt + "/a", kInt + "/b"), ENODEV);
    FsFixture fy;
    const auto fh = fy.fs->create(kInt + "/n", 0644);
    fy.fs->write(fh, 0, bytesOf("x"));
    fy.phone->unplugDuring(Op::Upload);
    CHECK_ERRNO(fy.fs->fsync(fh), ENODEV);
}

AF_TEST(fake_filesystem, reconnect_invalidates_handles_inodes_and_cache) {
    FsFixture fx(false, true, false, aftest::noCache());
    fx.phone->addFile(fx.internal, 0, "f", "abc");
    const auto fh = fx.fs->open(kInt + "/f", O_RDONLY);
    const auto ino1 = fx.fs->getattr(kInt + "/f").ino;
    const auto gen1 = fx.fs->generation();

    fx.phone->setUnplugged(true);
    CHECK_ERRNO(fx.fs->readdir(kInt), ENODEV);
    CHECK(fx.fs->isDisconnected());

    fx.phone->setUnplugged(false);
    fx.phone->addFile(fx.internal, 0, "after.txt", "new");
    fx.fs->reconnect(fx.phone);  // a fresh MTP session
    CHECK(!fx.fs->isDisconnected());
    CHECK(fx.fs->generation() > gen1);
    std::vector<std::byte> b(3);
    CHECK_ERRNO(fx.fs->read(fh, 0, b), ESTALE);  // old handle: no stale bytes
    fx.fs->release(fh);
    CHECK(names(*fx.fs, kInt).count("after.txt") == 1);
    CHECK_EQ(readAll(*fx.fs, kInt + "/f"), std::string("abc"));
    (void)ino1;
}

AF_TEST(fake_filesystem, staged_write_survives_reconnect_but_is_not_committed_into_new_session) {
    FsFixture fx;
    const auto fh = fx.fs->create(kInt + "/n", 0644);
    fx.fs->write(fh, 0, bytesOf("abc"));
    fx.fs->reconnect(fx.phone);
    CHECK_ERRNO(fx.fs->flush(fh), ESTALE);
    fx.fs->release(fh);
    CHECK_EQ(fx.phone->calls(Op::Upload), size_t{0});
}

AF_TEST(fake_filesystem, detach_releases_the_session_then_reconnect_resumes) {
    aftest::TempDir home;
    ::setenv("MTPORT_HOME", home.path.c_str(), 1);
    auto phone = std::make_shared<FakeMtpDevice>();
    const auto sid = phone->addStorage("Internal Storage", 1ull << 30, 1ull << 29);
    phone->addFile(sid, 0, "f", "abc");
    FakeMtpDeviceFactory factory(phone);
    const auto id = phone->descriptor().id;
    FsFixture fx;
    auto fs = fx.make(factory.open(id), false);  // the filesystem is the session's only owner
    int notified = 0;
    fs->setDisconnectCallback([&] { ++notified; });
    CHECK_EQ(readAll(*fs, kInt + "/f"), std::string("abc"));
    CHECK(DeviceLock::isHeld(id));

    fs->detachDevice();              // e.g. system wake: hand the session back
    CHECK(!DeviceLock::isHeld(id));  // a new session can now be opened
    CHECK_ERRNO(fs->getattr("/"), ENODEV);
    CHECK_EQ(notified, 0);  // an intentional hand-back is not an unplug

    fs->reconnect(factory.open(id));
    CHECK_EQ(readAll(*fs, kInt + "/f"), std::string("abc"));
    CHECK(DeviceLock::isHeld(id));
    ::unsetenv("MTPORT_HOME");
}

// ---- injected device faults
// ---------------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, device_errors_map_to_errno) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, 0, "f", "abc");
    fx.phone->failNext(Op::List, MtpErrc::DeviceBusy);
    CHECK_ERRNO(fx.fs->readdir(kInt), EBUSY);
    fx.phone->failNext(Op::List, MtpErrc::MtpNotReady);
    CHECK_ERRNO(fx.fs->readdir(kInt), EACCES);
    fx.phone->failNext(Op::List, MtpErrc::Timeout);
    CHECK_ERRNO(fx.fs->readdir(kInt), ETIMEDOUT);
    CHECK(!fx.fs->isDisconnected());  // transient errors do not kill the mount
    CHECK(names(*fx.fs, kInt).count("f") == 1);
    fx.phone->failNext(Op::Mkdir, MtpErrc::OperationUnsupported);
    CHECK_ERRNO(fx.fs->mkdir(kInt + "/d"), EOPNOTSUPP);
    fx.phone->failNext(Op::Remove, MtpErrc::ObjectNotFound);
    CHECK_ERRNO(fx.fs->unlink(kInt + "/f"), ENOENT);
}

// ---- single owner
// ----------------------------------------------------------------------------------------------------------

AF_TEST(fake_filesystem, second_simultaneous_owner_is_rejected) {
    aftest::TempDir home;
    ::setenv("MTPORT_HOME", home.path.c_str(), 1);
    auto phone = std::make_shared<FakeMtpDevice>();
    FakeMtpDeviceFactory factory(phone);
    auto id = phone->descriptor().id;
    auto first = factory.open(id);
    CHECK_MTP(factory.open(id), MtpErrc::DeviceBusy);
    CHECK(DeviceLock::isHeld(id));
    first.reset();
    CHECK(!DeviceLock::isHeld(id));
    auto again = factory.open(id);
    CHECK(again != nullptr);
    again.reset();
    factory.setPresent(false);
    CHECK_MTP(factory.open(id), MtpErrc::DeviceDisconnected);
    CHECK(factory.enumerate().empty());
    ::unsetenv("MTPORT_HOME");
}

AF_TEST(fake_filesystem, fresh_getattr_and_open_ask_the_phone_for_the_real_size) {
    FsFixture fx;
    fx.phone->addFile(fx.internal, kRootObject, "clip.mp4", "0123456789");
    CHECK_EQ(fx.fs->getattr(kInt + "/clip.mp4").size, uint64_t{10});
    const size_t before = fx.phone->calls(Op::Stat);
    fx.fs->getattr(kInt + "/clip.mp4");                    // plain: from the (possibly old) listing
    CHECK_EQ(fx.phone->calls(Op::Stat), before);
    CHECK_EQ(fx.fs->getattr(kInt + "/clip.mp4", true).size, uint64_t{10});
    CHECK_EQ(fx.phone->calls(Op::Stat), before + 1);       // fresh: asked the phone
    const uint64_t fh = fx.fs->open(kInt + "/clip.mp4", O_RDONLY);
    fx.fs->release(fh);
    CHECK_EQ(fx.phone->calls(Op::Stat), before + 2);       // opening re-reads the size before any read is bounded by it
}

AF_TEST(fake_filesystem, prefetch_warms_folders_and_ignores_missing_ones) {
    FsFixture fx;
    const ObjectId dcim = fx.phone->addDir(fx.internal, kRootObject, "DCIM");
    const ObjectId cam = fx.phone->addDir(fx.internal, dcim, "Camera");
    for (int i = 0; i < 5; ++i) fx.phone->addFile(fx.internal, cam, "IMG_" + std::to_string(i) + ".jpg", "x");
    fx.fs->prefetch({"DCIM/Camera", "Does/Not/Exist"}, nullptr);  // no throw for the missing folder
    const size_t lists = fx.phone->calls(Op::List);
    CHECK_EQ(fx.fs->readdir(kInt + "/DCIM/Camera").size(), size_t{2 + 5});  // "." ".." + 5 photos
    CHECK_EQ(fx.phone->calls(Op::List), lists);                              // served from what prefetch read
    FsFixture other;
    other.phone->addDir(other.internal, kRootObject, "DCIM");
    other.fs->prefetch({"DCIM"}, [] { return true; });                       // told to stop: reads nothing
    CHECK_EQ(other.phone->calls(Op::List), size_t{0});
}
