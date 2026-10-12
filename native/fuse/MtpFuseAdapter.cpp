#include "MtpFuseAdapter.hpp"

#include <sys/mount.h>

#include <climits>
#include <cstring>

namespace mtport::fuse_adapter {

namespace {

MtpFilesystem* g_fs = nullptr;
Hooks g_hooks;

#ifndef ENOATTR
#define ENOATTR 93
#endif

template <class F>
int guard(F&& f) noexcept {
    try {
        if (!g_fs) return -EIO;
        return f();
    } catch (const std::exception& e) {
        return -errnoFromException(e);
    } catch (...) {
        return -EIO;
    }
}

void fillStat(const FsStat& in, struct stat* st) {
    std::memset(st, 0, sizeof *st);
    st->st_ino = in.ino;
    st->st_mode = static_cast<mode_t>((in.isDir ? S_IFDIR : S_IFREG) | in.mode);
    st->st_nlink = static_cast<nlink_t>(in.nlink);
    st->st_uid = in.uid;
    st->st_gid = in.gid;
    st->st_size = static_cast<off_t>(in.size);
    st->st_blksize = 4096;
    st->st_blocks = static_cast<blkcnt_t>((in.size + 511) / 512);
    struct timespec ts{};
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(in.mtime);
    ts.tv_sec = static_cast<time_t>(secs.time_since_epoch().count());
    st->st_atimespec = st->st_mtimespec = st->st_ctimespec = st->st_birthtimespec = ts;
}

int op_getattr(const char* path, struct stat* st) {
    return guard([&] {
        fillStat(g_fs->getattr(path), st);
        return 0;
    });
}

int op_fgetattr(const char* path, struct stat* st, struct fuse_file_info*) {
    return op_getattr(path, st);
}

int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*) {
    return guard([&] {
        for (const auto& e : g_fs->readdir(path)) {
            struct stat st{};
            st.st_ino = e.ino;
            st.st_mode = e.isDir ? S_IFDIR : S_IFREG;
            // offset 0: libfuse buffers the complete listing itself and serves it in kernel-sized chunks
            if (filler(buf, e.name.c_str(), &st, 0) != 0) return -ENOMEM;
        }
        return 0;
    });
}

int op_open(const char* path, struct fuse_file_info* fi) {
    return guard([&] {
        fi->fh = g_fs->open(path, fi->flags);
        return 0;
    });
}

int op_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    return guard([&] {
        fi->fh = g_fs->create(path, mode);
        return 0;
    });
}

int op_read(const char*, char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    return guard([&] {
        if (off < 0) return -EINVAL;
        if (size > static_cast<size_t>(INT_MAX)) size = INT_MAX;
        const size_t n = g_fs->read(fi->fh, static_cast<uint64_t>(off),
                                    std::span<std::byte>(reinterpret_cast<std::byte*>(buf), size));
        return static_cast<int>(n);
    });
}

int op_write(const char*, const char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    return guard([&] {
        if (off < 0) return -EINVAL;
        if (size > static_cast<size_t>(INT_MAX)) size = INT_MAX;
        const size_t n = g_fs->write(fi->fh, static_cast<uint64_t>(off),
                                     std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), size));
        return static_cast<int>(n);
    });
}

int op_flush(const char*, struct fuse_file_info* fi) {
    return guard([&] {
        g_fs->flush(fi->fh);
        return 0;
    });
}

int op_fsync(const char*, int, struct fuse_file_info* fi) {
    return guard([&] {
        g_fs->fsync(fi->fh);
        return 0;
    });
}

int op_release(const char*, struct fuse_file_info* fi) {
    if (g_fs) g_fs->release(fi->fh);  // noexcept: failures were already recorded
    return 0;
}

int op_mkdir(const char* path, mode_t) {
    return guard([&] {
        g_fs->mkdir(path);
        return 0;
    });
}
int op_unlink(const char* path) {
    return guard([&] {
        g_fs->unlink(path);
        return 0;
    });
}
int op_rmdir(const char* path) {
    return guard([&] {
        g_fs->rmdir(path);
        return 0;
    });
}
int op_rename(const char* from, const char* to) {
    return guard([&] {
        g_fs->rename(from, to);
        return 0;
    });
}

// macFUSE's renamex(): RENAME_EXCL = fail if the target exists (what MTP can honour),
// RENAME_SWAP has no MTP equivalent.
int op_renamex(const char* from, const char* to, unsigned int flags) {
    return guard([&] {
        if (flags & RENAME_SWAP) return -ENOTSUP;
        if (flags & RENAME_EXCL) {
            bool exists = true;
            try {
                g_fs->getattr(to);
            } catch (const FsError& e) {
                if (e.err() != ENOENT) throw;
                exists = false;
            }
            if (exists) return -EEXIST;
        }
        g_fs->rename(from, to);
        return 0;
    });
}

int op_truncate(const char* path, off_t size) {
    return guard([&] {
        if (size < 0) return -EINVAL;
        g_fs->truncate(path, static_cast<uint64_t>(size));
        return 0;
    });
}
int op_ftruncate(const char*, off_t size, struct fuse_file_info* fi) {
    return guard([&] {
        if (size < 0) return -EINVAL;
        g_fs->ftruncate(fi->fh, static_cast<uint64_t>(size));
        return 0;
    });
}
int op_utimens(const char* path, const struct timespec[2]) {
    return guard([&] {
        g_fs->utimens(path);
        return 0;
    });
}
// FSKit sets creation/backup/change times right after creating an item; same policy as utimens.
int op_setcrtime(const char* path, const struct timespec*) {
    return guard([&] { g_fs->utimens(path); return 0; });
}

int op_chmod(const char* path, mode_t mode) {
    return guard([&] {
        g_fs->chmod(path, mode);
        return 0;
    });
}
int op_chown(const char* path, uid_t uid, gid_t gid) {
    return guard([&] {
        g_fs->chown(path, uid == static_cast<uid_t>(-1) ? -1 : static_cast<int64_t>(uid),
                    gid == static_cast<gid_t>(-1) ? -1 : static_cast<int64_t>(gid));
        return 0;
    });
}

int op_statfs(const char* path, struct statvfs* sv) {
    return guard([&] {
        const FsStatvfs s = g_fs->statfs(path);
        std::memset(sv, 0, sizeof *sv);
        sv->f_bsize = static_cast<unsigned long>(s.blockSize);
        sv->f_frsize = static_cast<unsigned long>(s.blockSize);
        sv->f_blocks = static_cast<fsblkcnt_t>(s.blocks);
        sv->f_bfree = static_cast<fsblkcnt_t>(s.blocksFree);
        sv->f_bavail = static_cast<fsblkcnt_t>(s.blocksAvail);
        sv->f_files = static_cast<fsfilcnt_t>(s.files);
        sv->f_ffree = static_cast<fsfilcnt_t>(s.filesFree);
        sv->f_namemax = static_cast<unsigned long>(s.nameMax);
        return 0;
    });
}

// Extended attributes: the phone has none. Reads say "no such attribute", writes say "not
// supported" so that Finder/copyfile fall back to ._ AppleDouble files (kept in RAM by MtpFilesystem).
int op_getxattr(const char*, const char*, char*, size_t, uint32_t) {
    return -ENOATTR;
}
int op_setxattr(const char*, const char*, const char*, size_t, int, uint32_t) {
    return -ENOTSUP;
}
int op_listxattr(const char*, char*, size_t) {
    return 0;
}
int op_removexattr(const char*, const char*) {
    return -ENOATTR;
}

void* op_init(struct fuse_conn_info*) {
    if (g_hooks.onInit) g_hooks.onInit();
    return g_fs;
}
void op_destroy(void*) {
    if (g_hooks.onDestroy) g_hooks.onDestroy();
}

}  // namespace

void setFilesystem(MtpFilesystem* fs) {
    g_fs = fs;
}
void setHooks(Hooks hooks) {
    g_hooks = std::move(hooks);
}

const struct fuse_operations& operations() {
    static const struct fuse_operations ops = [] {
        struct fuse_operations o{};
        o.getattr = op_getattr;
        o.fgetattr = op_fgetattr;
        o.readdir = op_readdir;
        o.open = op_open;
        o.create = op_create;
        o.read = op_read;
        o.write = op_write;
        o.flush = op_flush;
        o.fsync = op_fsync;
        o.release = op_release;
        o.mkdir = op_mkdir;
        o.unlink = op_unlink;
        o.rmdir = op_rmdir;
        o.rename = op_rename;
        o.renamex = op_renamex;
        o.truncate = op_truncate;
        o.ftruncate = op_ftruncate;
        o.utimens = op_utimens;
        o.setcrtime = op_setcrtime;
        o.setbkuptime = op_setcrtime;
        o.setchgtime = op_setcrtime;
        o.chmod = op_chmod;
        o.chown = op_chown;
        o.statfs = op_statfs;
        o.getxattr = op_getxattr;
        o.setxattr = op_setxattr;
        o.listxattr = op_listxattr;
        o.removexattr = op_removexattr;
        o.init = op_init;
        o.destroy = op_destroy;
        return o;
    }();
    return ops;
}

}  // namespace mtport::fuse_adapter
