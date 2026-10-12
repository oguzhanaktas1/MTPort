// Calls the fuse_operations table directly (no mount, no libfuse): verifies the type/errno
// translation of the adapter against the real macFUSE header definitions.
#include <fcntl.h>

#include "MtpFuseAdapter.hpp"
#include "TestHarness.hpp"
#include "TestSupport.hpp"

using namespace mtport;
using aftest::FsFixture;

namespace {
struct Bound {
    FsFixture fx;
    explicit Bound(bool ro = false, bool partial = true) : fx(ro, partial) { fuse_adapter::setFilesystem(fx.fs.get()); }
    ~Bound() { fuse_adapter::setFilesystem(nullptr); }
    const fuse_operations& ops() const { return fuse_adapter::operations(); }
};
int fillCount = 0;
std::vector<std::string> g_names;
int collect(void*, const char* name, const struct stat* st, off_t) {
    ++fillCount;
    g_names.push_back(name);
    return (st && (st->st_mode & (S_IFDIR | S_IFREG))) ? 0 : -1;
}
}  // namespace

AF_TEST(fuse_adapter, getattr_readdir_read) {
    Bound b;
    auto d = b.fx.phone->addDir(b.fx.internal, 0, "DCIM");
    b.fx.phone->addFile(b.fx.internal, d, "a.jpg", "hello fuse");
    struct stat st{};
    CHECK_EQ(b.ops().getattr("/Internal Storage/DCIM/a.jpg", &st), 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK_EQ(static_cast<int>(st.st_mode & 07777), 0644);
    CHECK_EQ(st.st_size, off_t{10});
    CHECK(st.st_ino != 0);
    CHECK_EQ(b.ops().getattr("/Internal Storage/DCIM", &st), 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK_EQ(b.ops().getattr("/nope", &st), -ENOENT);

    fillCount = 0;
    g_names.clear();
    CHECK_EQ(b.ops().readdir("/Internal Storage/DCIM", nullptr, collect, 0, nullptr), 0);
    CHECK_EQ(g_names, (std::vector<std::string>{".", "..", "a.jpg"}));

    struct fuse_file_info fi{};
    fi.flags = O_RDONLY;
    CHECK_EQ(b.ops().open("/Internal Storage/DCIM/a.jpg", &fi), 0);
    char buf[64] = {};
    CHECK_EQ(b.ops().read("", buf, sizeof buf, 0, &fi), 10);
    CHECK_EQ(std::string(buf, 10), std::string("hello fuse"));
    CHECK_EQ(b.ops().read("", buf, sizeof buf, 10, &fi), 0);  // EOF: exactly zero
    CHECK_EQ(b.ops().read("", buf, sizeof buf, 99999, &fi), 0);
    CHECK_EQ(b.ops().read("", buf, 4, -1, &fi), -EINVAL);
    CHECK_EQ(b.ops().release("", &fi), 0);
}

AF_TEST(fuse_adapter, write_path_and_errors) {
    Bound b;
    struct fuse_file_info fi{};
    CHECK_EQ(b.ops().create("/Internal Storage/new.txt", 0644, &fi), 0);
    CHECK_EQ(b.ops().write("", "abc", 3, 0, &fi), 3);
    CHECK_EQ(b.ops().flush("", &fi), 0);
    CHECK_EQ(b.ops().release("", &fi), 0);
    struct stat st{};
    CHECK_EQ(b.ops().getattr("/Internal Storage/new.txt", &st), 0);
    CHECK_EQ(st.st_size, off_t{3});

    struct fuse_file_info again{};
    CHECK_EQ(b.ops().create("/Internal Storage/new.txt", 0644, &again), -EEXIST);
    again.flags = O_WRONLY | O_TRUNC;
    CHECK_EQ(b.ops().open("/Internal Storage/new.txt", &again), -EOPNOTSUPP);  // overwrite refused

    CHECK_EQ(b.ops().mkdir("/Internal Storage/D", 0755), 0);
    CHECK_EQ(b.ops().rmdir("/Internal Storage/D"), 0);
    CHECK_EQ(b.ops().unlink("/Internal Storage/new.txt"), 0);
    CHECK_EQ(b.ops().unlink("/Internal Storage/new.txt"), -ENOENT);
    CHECK_EQ(b.ops().mkdir("/top", 0755), -EPERM);
}

AF_TEST(fuse_adapter, read_only_mount_reports_erofs) {
    Bound b(true);
    struct fuse_file_info fi{};
    CHECK_EQ(b.ops().create("/Internal Storage/x", 0644, &fi), -EROFS);
    CHECK_EQ(b.ops().mkdir("/Internal Storage/x", 0755), -EROFS);
    struct stat st{};
    CHECK_EQ(b.ops().getattr("/", &st), 0);
    CHECK_EQ(static_cast<int>(st.st_mode & 0222), 0);  // no write bits anywhere
}

AF_TEST(fuse_adapter, renamex_semantics) {
    Bound b;
    b.fx.phone->addFile(b.fx.internal, 0, "a", "A");
    b.fx.phone->addFile(b.fx.internal, 0, "b", "B");
    CHECK_EQ(b.ops().renamex("/Internal Storage/a", "/Internal Storage/b", RENAME_EXCL), -EEXIST);
    CHECK_EQ(b.ops().renamex("/Internal Storage/a", "/Internal Storage/c", RENAME_SWAP), -ENOTSUP);
    CHECK_EQ(b.ops().renamex("/Internal Storage/a", "/Internal Storage/c", RENAME_EXCL), 0);
    CHECK_EQ(b.ops().rename("/Internal Storage/b", "/Internal Storage/c"), -EOPNOTSUPP);
    struct stat st{};
    CHECK_EQ(b.ops().getattr("/Internal Storage/c", &st), 0);
    CHECK_EQ(b.ops().getattr("/Internal Storage/b", &st), 0);
}

AF_TEST(fuse_adapter, creation_time_ops_follow_the_utimens_policy) {
    Bound b;
    b.fx.phone->addFile(b.fx.internal, 0, "old", "x");
    struct timespec ts {};
    CHECK_EQ(b.ops().mkdir("/Internal Storage/D", 0755), 0);
    CHECK_EQ(b.ops().setcrtime("/Internal Storage/D", &ts), 0);     // FSKit does this right after mkdir
    CHECK_EQ(b.ops().setbkuptime("/Internal Storage/D", &ts), 0);
    CHECK_EQ(b.ops().setchgtime("/Internal Storage/D", &ts), 0);
    CHECK_EQ(b.ops().setcrtime("/Internal Storage/old", &ts), -EOPNOTSUPP);
}

AF_TEST(fuse_adapter, xattrs_and_statfs) {
    Bound b;
    char v[8];
    CHECK_EQ(b.ops().getxattr("/", "com.apple.FinderInfo", v, sizeof v, 0), -93);  // ENOATTR
    CHECK_EQ(b.ops().setxattr("/", "user.x", "1", 1, 0, 0), -ENOTSUP);             // => copyfile falls back to ._ files
    CHECK_EQ(b.ops().listxattr("/", nullptr, 0), 0);
    struct statvfs sv{};
    CHECK_EQ(b.ops().statfs("/", &sv), 0);
    CHECK(sv.f_blocks > 0);
    CHECK_EQ(sv.f_namemax, 255ul);
}

AF_TEST(fuse_adapter, no_filesystem_is_eio_not_a_crash) {
    fuse_adapter::setFilesystem(nullptr);
    struct stat st{};
    CHECK_EQ(fuse_adapter::operations().getattr("/", &st), -EIO);
}
