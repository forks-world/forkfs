// Linux native-directory backend. FICLONE shares extents on XFS and Btrfs;
// capability is tested by the operation. Ext4 uses sparse-aware independent copies.
#include "internal.h"
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

namespace wfs {
namespace {
bool unsupported(int e) { return e == EXDEV || e == EOPNOTSUPP || e == ENOTTY || e == EINVAL; }

// Btrfs gives each subvolume its own st_dev. Compare filesystem UUIDs before rejecting
// a cross-subvolume clone; the real FICLONE still decides whether the operation works.
int btrfs_fsid(const char *path, struct btrfs_ioctl_fs_info_args &info) {
    int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    struct statfs fs;
    int rc = ::fstatfs(fd, &fs) ? -errno : 0;
    if (!rc && fs.f_type != BTRFS_SUPER_MAGIC) rc = -EXDEV;
    if (!rc && ::ioctl(fd, BTRFS_IOC_FS_INFO, &info)) rc = -errno;
    ::close(fd);
    return rc;
}

// A new inode may inherit +C/+c/+m from its destination parent. Match the source while
// the inode is still empty: Btrfs refuses reflinks with incompatible NOCOW/checksums.
// Preserve compression policy too, including on directories for future file creation.
int btrfs_clone_flags(int src, int dst) {
    struct statfs fs;
    if (::fstatfs(src, &fs)) return -errno;
    // An inherited algorithm property is an xattr, separate from +c. The metadata pass
    // installs the source property, if any; an absent source property must stay absent.
    if (::fremovexattr(dst, "btrfs.compression") && errno != ENODATA) return -errno;
    int source = 0, target = 0;
    if (fs.f_type == BTRFS_SUPER_MAGIC && ::ioctl(src, FS_IOC_GETFLAGS, &source)) return -errno;
    if (::ioctl(dst, FS_IOC_GETFLAGS, &target)) return -errno;
    constexpr int mask = FS_NOCOW_FL | FS_COMPR_FL | FS_NOCOMP_FL;
    int desired = (target & ~mask) | (source & mask);
    if (desired == target) return 0;
    // Changing between NOCOW and compression needs two steps: the kernel checks both
    // the previous and requested flag combinations, even on an empty inode.
    int cleared = target & ~mask;
    if (cleared != target && ::ioctl(dst, FS_IOC_SETFLAGS, &cleared)) return -errno;
    return desired == cleared || ::ioctl(dst, FS_IOC_SETFLAGS, &desired) == 0 ? 0 : -errno;
}

// Ext4 backend and explicit --copy fallback, preserving sparse holes.
int copy_data(int src, int dst, off_t size) {
    char buf[128 * 1024];
    off_t pos = 0;
    while (pos < size) {
        off_t data = ::lseek(src, pos, SEEK_DATA);
        if (data < 0 && errno == ENXIO) break;
        if (data < 0 && errno != EINVAL) return -errno;
        if (data < 0) data = pos;
        off_t end = ::lseek(src, data, SEEK_HOLE);
        if (end < 0 && errno != EINVAL) return -errno;
        if (end < 0 || end > size) end = size;
        if (data >= size) break;
        if (end <= data) return -EIO;
        pos = data;
        while (pos < end) {
            size_t want = (size_t)((end - pos) < (off_t)sizeof buf ? end - pos : sizeof buf);
            ssize_t n = ::pread(src, buf, want, pos);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return n < 0 ? -errno : -EIO;
            ssize_t done = 0;
            while (done < n) {
                ssize_t w = ::pwrite(dst, buf + done, (size_t)(n - done), pos + done);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) return w < 0 ? -errno : -EIO;
                done += w;
            }
            pos += n;
        }
    }
    if (::ftruncate(dst, size)) return -errno;
    // Delayed allocation can defer ENOSPC until writeback. Detect it before publication.
    return ::fdatasync(dst) == 0 ? 0 : -errno;
}

int set_xattr_if_different(const char *dst, const char *name, const void *value, size_t len) {
    ssize_t existing_len = ::lgetxattr(dst, name, nullptr, 0);
    if (existing_len >= 0) {
        if ((size_t)existing_len == len) {
            void *existing = ::malloc(len ? len : 1);
            if (!existing) return -ENOMEM;
            ssize_t got = ::lgetxattr(dst, name, existing, len);
            if (got < 0) {
                int e = errno;
                ::free(existing);
                return -e;
            }
            bool same = (size_t)got == len && !::memcmp(existing, value, len);
            ::free(existing);
            if (same) return 0;
        }
    } else {
        int e = errno;
        if (e != ENODATA && e != EOPNOTSUPP) return -e;
    }
    return ::lsetxattr(dst, name, value, len, 0) == 0 ? 0 : -errno;
}

int xattrs(const char *src, const char *dst) {
    // Creation can inherit ACLs from the destination parent. Absence on the source
    // must remove that inherited ACL, or the clone has different access semantics.
    for (const char *acl : {"system.posix_acl_access", "system.posix_acl_default"}) {
        if (::lgetxattr(src, acl, nullptr, 0) < 0) {
            int e = errno;
            if (e != ENODATA && e != EOPNOTSUPP) return -e;
            if (::lremovexattr(dst, acl) && errno != ENODATA && errno != EOPNOTSUPP) return -errno;
        }
    }
    ssize_t n = ::llistxattr(src, nullptr, 0);
    if (n < 0) return errno == EOPNOTSUPP ? 0 : -errno;
    if (!n) return 0;
    char *names = (char *)::malloc((size_t)n);
    if (!names) return -ENOMEM;
    n = ::llistxattr(src, names, (size_t)n);
    int rc = n < 0 ? -errno : 0;
    for (ssize_t i = 0; !rc && i < n; i += (ssize_t)::strlen(names + i) + 1) {
        ssize_t len = ::lgetxattr(src, names + i, nullptr, 0);
        if (len < 0) { rc = -errno; break; }
        void *value = ::malloc(len ? (size_t)len : 1);
        if (!value) { rc = -ENOMEM; break; }
        ssize_t got = ::lgetxattr(src, names + i, value, (size_t)len);
        if (got < 0) rc = -errno;
        else rc = set_xattr_if_different(dst, names + i, value, (size_t)got);
        ::free(value);
    }
    ::free(names);
    return rc;
}

int metadata(const char *src, const char *dst, const struct stat &st) {
    struct stat d;
    if (::lstat(dst, &d)) return -errno;
    // Ownership first: chown clears set-id bits and capabilities.
    if ((d.st_uid != st.st_uid || d.st_gid != st.st_gid) &&
        ::lchown(dst, st.st_uid, st.st_gid)) return -errno;
    if (!S_ISLNK(st.st_mode) && ::chmod(dst, st.st_mode & 07777)) return -errno;
    if (int rc = xattrs(src, dst)) return rc; // includes POSIX ACLs; never silently drop them
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    return ::utimensat(AT_FDCWD, dst, times, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : -errno;
}

struct CloneCtx {
    const char *dst;
    bool copy;
    bool btrfs = false;
    bool ext_copy = false;
    dev_t device = 0;
};
String target(CloneCtx *c, const char *rel) {
    String d(c->dst);
    if (*rel) { d.append("/"); d.append(rel); }
    return d;
}
int clone_entry(void *ctx, const char *src, const char *rel, const struct stat &st, bool dir) {
    auto *c = (CloneCtx *)ctx;
    String dst = target(c, rel);
    if (dir) {
        if (::mkdir(dst.c_str(), 0700)) return -errno;
        // mkdir(2) applies the caller's umask; make the directory traversable before cloning
        // its children. The post-order metadata pass restores the source's final mode.
        if (::chmod(dst.c_str(), 0700)) return -errno;
        if (!*rel) {
            struct statfs fs;
            if (::statfs(dst.c_str(), &fs)) return -errno;
            c->btrfs = fs.f_type == BTRFS_SUPER_MAGIC;
            // ext2/3 share this magic; only ext4 is covered by our support/test contract.
            c->ext_copy = fs.f_type == EXT4_SUPER_MAGIC;
            struct stat dest;
            if (::stat(dst.c_str(), &dest)) return -errno;
            c->device = dest.st_dev;
            if (c->ext_copy && !c->copy && st.st_dev != c->device) return -EXDEV;
        }
        if (c->btrfs) {
            int in = ::open(src, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (in < 0) return -errno;
            int out = ::open(dst.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int rc = out < 0 ? -errno : btrfs_clone_flags(in, out);
            if (out >= 0) ::close(out);
            ::close(in);
            if (rc) return rc;
        }
        return 0;
    }
    int rc = 0;
    if (S_ISREG(st.st_mode)) {
        int in = ::open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (in < 0) return -errno;
        struct stat opened;
        if (::fstat(in, &opened)) { rc = -errno; ::close(in); return rc; }
        if (!S_ISREG(opened.st_mode) || opened.st_ino != st.st_ino || opened.st_dev != st.st_dev) {
            ::close(in); return -ESTALE;
        }
        int out = ::open(dst.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (out < 0) { rc = -errno; ::close(in); return rc; }
        if (c->btrfs) rc = btrfs_clone_flags(in, out);
        if (!rc && c->ext_copy) {
            // Do not turn nested mounts or cross-volume callers into implicit --copy.
            rc = !c->copy && opened.st_dev != c->device ? -EXDEV
                 : copy_data(in, out, opened.st_size);
        } else if (!rc && ::ioctl(out, FICLONE, in)) {
            int e = errno;
            rc = c->copy && unsupported(e) ? copy_data(in, out, opened.st_size)
                                         : -(unsupported(e) && e != EXDEV ? EOPNOTSUPP : e);
        }
        if (::close(out) && !rc) rc = -errno;
        ::close(in);
    } else if (S_ISLNK(st.st_mode)) {
        char buf[WFS_PATH_MAX];
        ssize_t n = ::readlink(src, buf, sizeof buf - 1);
        if (n < 0) return -errno;
        if ((size_t)n == sizeof buf - 1) return -ENAMETOOLONG;
        buf[n] = 0;
        if (::symlink(buf, dst.c_str())) rc = -errno;
    } else if (S_ISFIFO(st.st_mode)) {
        if (::mkfifo(dst.c_str(), 0600)) rc = -errno;
    } else return -EOPNOTSUPP; // a successful snapshot must never omit an entry
    return rc ? rc : metadata(src, dst.c_str(), st);
}
int dir_metadata(void *ctx, const char *src, const char *rel, const struct stat &st, bool dir) {
    if (!dir) return 0;
    String dst = target((CloneCtx *)ctx, rel);
    return metadata(src, dst.c_str(), st);
}
} // namespace

int fs_clone_probe(const char *dst, const char *src) {
    if (!dst || !src) return -EINVAL;
    struct stat a, b;
    if (::stat(src, &a) || ::stat(dst, &b)) return -errno;
    if (a.st_dev != b.st_dev) {
        struct btrfs_ioctl_fs_info_args from{}, to{};
        if (int rc = btrfs_fsid(src, from)) return rc;
        if (int rc = btrfs_fsid(dst, to)) return rc;
        if (::memcmp(from.fsid, to.fsid, sizeof from.fsid)) return -EXDEV;
    }
    struct statfs fs;
    if (::statfs(dst, &fs)) return -errno;
    // Probe using our own files, without touching the source or following user symlinks.
    char from[WFS_PATH_MAX], to[WFS_PATH_MAX];
    if (::snprintf(from, sizeof from, "%s/.wfs-probe-src-XXXXXX", dst) >= (int)sizeof from ||
        ::snprintf(to, sizeof to, "%s/.wfs-probe-dst-XXXXXX", dst) >= (int)sizeof to) return -ENAMETOOLONG;
    int in = ::mkstemp(from);
    if (in < 0) return -errno;
    int out = ::mkstemp(to);
    int rc = out < 0 ? -errno : 0;
    if (!rc && ::write(in, "wfs", 3) != 3) rc = errno ? -errno : -EIO;
    if (!rc && fs.f_type == EXT4_SUPER_MAGIC) rc = copy_data(in, out, 3);
    else if (!rc && ::ioctl(out, FICLONE, in)) rc = unsupported(errno) ? -EOPNOTSUPP : -errno;
    if (out >= 0) { ::close(out); ::unlink(to); }
    ::close(in); ::unlink(from);
    return rc;
}

int fs_clone_tree(const char *src, const char *dst, bool allow_fallback) {
    if (!src || !dst) return -EINVAL;
    CloneCtx c{dst, allow_fallback};
    if (int rc = fs_walk_tree(src, 4, FS_DIRS_PRE, &c, clone_entry)) return rc;
    // Restore directory ACLs/modes/times only after all children have been created.
    return fs_walk_tree(src, 1, FS_DIRS_POST, &c, dir_metadata);
}
// Linux immutable flags require capabilities and cannot implement unprivileged --hard.
// Gate protection remains the default; never claim a hard snapshot was protected.
int fs_protect_tree(const char *, TreeStats *, Manifest *) { return -EOPNOTSUPP; }
int fs_unprotect_tree(const char *, int64_t) { return 0; }
} // namespace wfs
