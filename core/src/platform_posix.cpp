// POSIX backing-store primitives. macOS and Linux; Windows gets its own file later.
#include "internal.h"

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifdef __APPLE__
#define WFS_XATTR_NOFOLLOW XATTR_NOFOLLOW
#else
#define WFS_XATTR_NOFOLLOW 0
#endif

namespace wfs {

namespace {
wfs_type type_of(mode_t m) {
    switch (m & S_IFMT) {
    case S_IFREG: return WFS_T_FILE;
    case S_IFDIR: return WFS_T_DIR;
    case S_IFLNK: return WFS_T_SYMLINK;
    case S_IFIFO: return WFS_T_FIFO;
    case S_IFCHR: return WFS_T_CHR;
    case S_IFBLK: return WFS_T_BLK;
    case S_IFSOCK: return WFS_T_SOCK;
    default: return WFS_T_UNKNOWN;
    }
}
wfs_type type_of_dt(unsigned char t) {
    switch (t) {
    case DT_REG: return WFS_T_FILE;
    case DT_DIR: return WFS_T_DIR;
    case DT_LNK: return WFS_T_SYMLINK;
    case DT_FIFO: return WFS_T_FIFO;
    case DT_CHR: return WFS_T_CHR;
    case DT_BLK: return WFS_T_BLK;
    case DT_SOCK: return WFS_T_SOCK;
    default: return WFS_T_UNKNOWN;
    }
}
wfs_timespec ts(const struct timespec &t) { return wfs_timespec{(int64_t)t.tv_sec, (int64_t)t.tv_nsec}; }
} // namespace

int fs_lstat(const char *path, wfs_attr &a) {
    struct stat st;
    if (::lstat(path, &st) != 0) return -errno;
    memset(&a, 0, sizeof a);
    a.ino = (wfs_ino)st.st_ino;
    a.type = type_of(st.st_mode);
    a.mode = st.st_mode & ~S_IFMT;
    a.uid = st.st_uid;
    a.gid = st.st_gid;
    a.nlink = (uint32_t)st.st_nlink;
    a.size = (uint64_t)st.st_size;
    a.alloc_size = (uint64_t)st.st_blocks * 512;
#ifdef __APPLE__
    a.flags = st.st_flags;
    a.atime = ts(st.st_atimespec); a.mtime = ts(st.st_mtimespec);
    a.ctime = ts(st.st_ctimespec); a.btime = ts(st.st_birthtimespec);
#else
    a.atime = ts(st.st_atim); a.mtime = ts(st.st_mtim); a.ctime = ts(st.st_ctim); a.btime = ts(st.st_ctim);
#endif
    return 0;
}

int fs_readlink(const char *path, char *buf, size_t cap, size_t *len) {
    ssize_t n = ::readlink(path, buf, cap);
    if (n < 0) return -errno;
    *len = (size_t)n;
    return 0;
}

#ifdef __APPLE__
// macOS treats a file system module as a foreign source: every file the (sandboxed) FSKit
// extension creates is stamped with com.apple.quarantine, and the extension is not allowed to
// remove it (EPERM). A quarantined ad-hoc binary fails Gatekeeper/AMFI on exec/dlopen, after
// which the kernel wedges the process (measured: unkillable, needs reboot). The kernel learns
// about quarantine by asking this file system, so the namespace layer hides quarantine entries
// whose agent field is this process. Quarantine set by other agents (a browser saving into the
// workspace) stays visible. Do not try removexattr here: it is denied, and every sandbox denial
// costs ~1ms of kernel violation reporting per create.
#endif

int fs_mkfile(const char *path, uint32_t mode) {
    int fd = ::open(path, O_CREAT | O_EXCL | O_WRONLY, (mode_t)mode);
    if (fd < 0) return -errno;
    ::close(fd);
    return 0;
}
int fs_mkdir(const char *path, uint32_t mode) { return ::mkdir(path, (mode_t)mode) ? -errno : 0; }
int fs_mkfifo(const char *path, uint32_t mode) { return ::mkfifo(path, (mode_t)mode) ? -errno : 0; }
int fs_symlink(const char *target, const char *path) { return ::symlink(target, path) ? -errno : 0; }
int fs_link(const char *existing, const char *path) { return ::link(existing, path) ? -errno : 0; }
int fs_unlink(const char *path, bool is_dir) { return (is_dir ? ::rmdir(path) : ::unlink(path)) ? -errno : 0; }
int fs_rename(const char *from, const char *to) { return ::rename(from, to) ? -errno : 0; }

int fs_setattr(const char *path, const wfs_setattr_req &r) {
    if (r.valid & WFS_SET_SIZE) { if (::truncate(path, (off_t)r.size) != 0) return -errno; }
    if (r.valid & WFS_SET_MODE) {
#ifdef __APPLE__
        if (::lchmod(path, (mode_t)(r.mode & 07777)) != 0) return -errno;
#else
        if (::chmod(path, (mode_t)(r.mode & 07777)) != 0) return -errno;
#endif
    }
    if (r.valid & (WFS_SET_UID | WFS_SET_GID)) {
        uid_t u = (r.valid & WFS_SET_UID) ? r.uid : (uid_t)-1;
        gid_t g = (r.valid & WFS_SET_GID) ? r.gid : (gid_t)-1;
        if (::lchown(path, u, g) != 0) return -errno;
    }
#ifdef __APPLE__
    if (r.valid & WFS_SET_FLAGS) { if (::lchflags(path, r.flags) != 0) return -errno; }
#endif
    if (r.valid & (WFS_SET_ATIME | WFS_SET_MTIME)) {
        struct timespec t[2];
        t[0].tv_sec = 0; t[0].tv_nsec = UTIME_OMIT;
        t[1].tv_sec = 0; t[1].tv_nsec = UTIME_OMIT;
        if (r.valid & WFS_SET_ATIME) { t[0].tv_sec = (time_t)r.atime.sec; t[0].tv_nsec = (long)r.atime.nsec; }
        if (r.valid & WFS_SET_MTIME) { t[1].tv_sec = (time_t)r.mtime.sec; t[1].tv_nsec = (long)r.mtime.nsec; }
        if (::utimensat(AT_FDCWD, path, t, AT_SYMLINK_NOFOLLOW) != 0) return -errno;
    }
    return 0;
}

int fs_getxattr(const char *path, const char *name, void *buf, size_t cap, size_t *len) {
#ifdef __APPLE__
    ssize_t n = ::getxattr(path, name, buf, cap, 0, WFS_XATTR_NOFOLLOW);
#else
    ssize_t n = ::lgetxattr(path, name, buf, cap);
#endif
    if (n < 0) return -errno;
    *len = (size_t)n;
    return 0;
}
int fs_setxattr(const char *path, const char *name, const void *data, size_t len, int flags) {
    int f = 0;
    if (flags & WFS_XATTR_CREATE) f |= XATTR_CREATE;
    if (flags & WFS_XATTR_REPLACE) f |= XATTR_REPLACE;
#ifdef __APPLE__
    return ::setxattr(path, name, data, len, 0, f | WFS_XATTR_NOFOLLOW) ? -errno : 0;
#else
    return ::lsetxattr(path, name, data, len, f) ? -errno : 0;
#endif
}
int fs_removexattr(const char *path, const char *name) {
#ifdef __APPLE__
    return ::removexattr(path, name, WFS_XATTR_NOFOLLOW) ? -errno : 0;
#else
    return ::lremovexattr(path, name) ? -errno : 0;
#endif
}
int fs_listxattr(const char *path, char *buf, size_t cap, size_t *len) {
#ifdef __APPLE__
    ssize_t n = ::listxattr(path, buf, cap, WFS_XATTR_NOFOLLOW);
#else
    ssize_t n = ::llistxattr(path, buf, cap);
#endif
    if (n < 0) return -errno;
    *len = (size_t)n;
    return 0;
}

bool fs_xattr_is_own_quarantine(const char *path, const char *name, size_t name_len) {
#ifdef __APPLE__
    static const char kQ[] = "com.apple.quarantine";
    if (name_len != sizeof(kQ) - 1 || memcmp(name, kQ, name_len) != 0) return false;
    char val[512];
    ssize_t n = ::getxattr(path, kQ, val, sizeof val - 1, 0, XATTR_NOFOLLOW);
    if (n <= 0) return false;
    val[n] = 0;
    // format: flags;timestamp;agent;uuid
    const char *p = strchr(val, ';'); if (!p) return false;
    p = strchr(p + 1, ';'); if (!p) return false;
    const char *agent = p + 1;
    const char *end = strchr(agent, ';');
    size_t alen = end ? (size_t)(end - agent) : strlen(agent);
    const char *self = getprogname();
    return self && strlen(self) == alen && memcmp(self, agent, alen) == 0;
#else
    (void)path; (void)name; (void)name_len;
    return false;
#endif
}

int fs_statfs(const char *path, wfs_statfs_info &o) {
    struct statvfs s;
    if (::statvfs(path, &s) != 0) return -errno;
    o.block_size = s.f_frsize;
    o.total_blocks = s.f_blocks;
    o.free_blocks = s.f_bfree;
    o.avail_blocks = s.f_bavail;
    o.total_files = s.f_files;
    o.free_files = s.f_ffree;
    return 0;
}

int fs_realpath(const char *path, String &out) {
    char buf[PATH_MAX];
    if (!::realpath(path, buf)) return -errno;
    out.assign(buf);
    return 0;
}

int fs_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -ENAMETOOLONG;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (i == n || tmp[i] == '/') {
            char c = tmp[i]; tmp[i] = 0;
            if (::mkdir(tmp, 0755) != 0 && errno != EEXIST) return -errno;
            tmp[i] = c;
        }
    }
    return 0;
}

int fs_readdir(const char *path, uint64_t skip,
               int (*cb)(void *, const char *, size_t, uint64_t, wfs_type, uint64_t), void *ctx) {
    DIR *d = ::opendir(path);
    if (!d) return -errno;
    uint64_t index = 0;
    while (struct dirent *e = ::readdir(d)) {
        uint64_t i = index++;
        if (i < skip) continue;
#ifdef __APPLE__
        size_t len = e->d_namlen;
#else
        size_t len = strlen(e->d_name);
#endif
        if (cb(ctx, e->d_name, len, (uint64_t)e->d_ino, type_of_dt(e->d_type), i)) break;
    }
    ::closedir(d);
    return 0;
}

} // namespace wfs
