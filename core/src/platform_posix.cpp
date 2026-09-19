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
#include <sys/sysctl.h>
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

// Who packs "." and ".."?
//
// Until Darwin 26.x the VFS layer synthesized both entries for every FSKit directory it enumerated,
// so a module that packed the ones its backing readdir(3) returned made `ls -fa` show each of them
// twice. Darwin 27 dropped that synthesis (Apple's own msdos module has always packed its own) and
// now hands whatever the module packs straight through, so a module that keeps skipping them
// returns a directory with zero dot entries: a 50-file directory enumerates 50 where native APFS
// returns 52.
//
// Straight through means to both directory syscalls, and they do not agree. Native APFS yields the
// dot entries from getdirentries(2) and never from getattrlistbulk(2) -- BSD `ls`/fts depends on
// that, it synthesizes its own dot lines precisely because the bulk call never gives it any. The
// kernel tells the two apart for us: only the getattrlistbulk path asks the module for attributes
// (measured on 27.0 -- readdir(3) arrives as attrs=0, getattrlistbulk / ls / find as attrs=1). So
// pack the dot entries on the attribute-less enumeration only, and native parity holds for both.
//
// There is no capability bit for any of this, no mount option reaches the extension (FSTaskOptions
// is empty on 26.6.2 and 27.0 alike) and the C ABI is frozen, so the kernel half of the decision is
// read from kern.osrelease and cached. Non-Darwin (FUSE) hosts always expect dot entries from the
// file system, with or without attributes.
// `cached` is constant-initialized (arch.md §39 forbids dynamic initialization of statics) and the
// race between two first callers is benign: they compute the same value.
bool fs_readdir_emits_dots(bool with_attrs) {
#ifdef __APPLE__
    static int cached = -1;   // -1 unknown, 0 kernel synthesizes, 1 the module packs
    int v = cached;
    if (v < 0) {
        char rel[64] = {0};
        size_t n = sizeof rel - 1;
        long major = 27;      // unreadable sysctl: assume current behaviour rather than lose entries
        if (::sysctlbyname("kern.osrelease", rel, &n, nullptr, 0) == 0) major = ::strtol(rel, nullptr, 10);
        v = major >= 27 ? 1 : 0;
        cached = v;
    }
    return v != 0 && !with_attrs;
#else
    (void)with_attrs;
    return true;
#endif
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


// ---- paths ------------------------------------------------------------------------------------

// realpath() of a path whose last component need not exist yet (a fork target). Everything
// above the leaf must exist; symlinks in the parent chain are resolved, the leaf never is.
int fs_realpath_parent(const char *path, String &out) {
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return n ? -ENAMETOOLONG : -EINVAL;
    memcpy(buf, path, n + 1);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    char *slash = strrchr(buf, '/');
    String dir;
    const char *leaf;
    if (!slash) {
        if (int rc = fs_realpath(".", dir)) return rc;
        leaf = buf;
    } else if (slash == buf) {
        dir.assign("/");
        leaf = buf + 1;
        if (!*leaf) { out = dir; return 0; }
    } else {
        *slash = 0;
        if (int rc = fs_realpath(buf, dir)) return rc;
        leaf = slash + 1;
    }
    out = dir;
    if (out.size() && out.c_str()[out.size() - 1] != '/') out.append("/");
    out.append(leaf);
    return 0;
}

// ---- parallel tree walk -------------------------------------------------------------------------
//
// One dynamic queue of directories, N pthread workers (arch.md §39 forbids std::thread and
// std::mutex, and 4 workers is where APFS metadata transactions stop scaling -- see
// docs/CLONE_MODEL_MACOS27.md §9.2, where 8 and 16 threads were slower than 4).

namespace {

struct Job {
    String path;
    String rel;
};

struct Walk {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Vec<Job> stack;
    Vec<Job> dirs;      // discovery order; replayed in reverse for FS_DIRS_POST
    int idle = 0;
    int threads = 1;
    bool done = false;
    int err = 0;
    void *ctx = nullptr;
    fs_entry_fn fn = nullptr;
    fs_dir_order order = FS_DIRS_PRE;
};

void walk_child(Walk &w, const Job &parent, const char *name, size_t nlen, Job &out) {
    out.path = parent.path;
    if (out.path.size() && out.path.c_str()[out.path.size() - 1] != '/') out.path.append("/");
    out.path.append(name);
    out.rel = parent.rel;
    if (out.rel.size()) out.rel.append("/");
    out.rel.append(name);
    (void)w;
    (void)nlen;
}

// Returns a negative errno to abort the whole walk.
int walk_dir(Walk &w, const Job &job) {
    DIR *d = ::opendir(job.path.c_str());
    if (!d) return -errno;
    int fd = ::dirfd(d);
    int rc = 0;
    Vec<Job> subdirs;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        struct stat st;
        if (::fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) { rc = -errno; break; }
        Job child;
        walk_child(w, job, e->d_name, strlen(e->d_name), child);
        if (S_ISDIR(st.st_mode)) {
            subdirs.emplace_back(child);
            if (w.order == FS_DIRS_PRE) { if ((rc = w.fn(w.ctx, child.path.c_str(), child.rel.c_str(), st, true))) break; }
        } else {
            if ((rc = w.fn(w.ctx, child.path.c_str(), child.rel.c_str(), st, false))) break;
        }
    }
    ::closedir(d);
    if (rc) return rc;
    if (subdirs.size()) {
        pthread_mutex_lock(&w.mu);
        for (size_t i = 0; i < subdirs.size(); ++i) {
            if (w.order == FS_DIRS_POST) w.dirs.emplace_back(subdirs[i]);
            w.stack.emplace_back(subdirs[i]);
        }
        pthread_cond_broadcast(&w.cv);
        pthread_mutex_unlock(&w.mu);
    }
    return 0;
}

void *walk_worker(void *arg) {
    Walk &w = *(Walk *)arg;
    pthread_mutex_lock(&w.mu);
    for (;;) {
        while (w.stack.size() == 0 && !w.done) {
            if (++w.idle == w.threads) { w.done = true; pthread_cond_broadcast(&w.cv); }
            else pthread_cond_wait(&w.cv, &w.mu);
            --w.idle;
        }
        if (w.stack.size() == 0) break;
        Job job = w.stack[w.stack.size() - 1];
        w.stack.pop_back();
        pthread_mutex_unlock(&w.mu);
        int rc = walk_dir(w, job);
        pthread_mutex_lock(&w.mu);
        if (rc && !w.err) { w.err = rc; w.done = true; pthread_cond_broadcast(&w.cv); }
        if (w.done && w.err) break;
    }
    pthread_mutex_unlock(&w.mu);
    return nullptr;
}

} // namespace

int fs_walk_tree(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_fn fn) {
    if (!root || !fn) return -EINVAL;
    struct stat rst;
    if (::lstat(root, &rst) != 0) return -errno;
    if (!S_ISDIR(rst.st_mode)) return fn(ctx, root, "", rst, false);

    Walk w;
    pthread_mutex_init(&w.mu, nullptr);
    pthread_cond_init(&w.cv, nullptr);
    w.ctx = ctx;
    w.fn = fn;
    w.order = order;
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    w.threads = threads;

    Job r;
    r.path.assign(root);
    r.rel.assign("");
    if (order == FS_DIRS_PRE) {
        if (int rc = fn(ctx, r.path.c_str(), "", rst, true)) {
            pthread_mutex_destroy(&w.mu);
            pthread_cond_destroy(&w.cv);
            return rc;
        }
    } else {
        w.dirs.emplace_back(r);
    }
    w.stack.emplace_back(r);

    if (threads == 1) {
        walk_worker(&w);
    } else {
        pthread_t th[16];
        int started = 0;
        for (int i = 0; i < threads; ++i)
            if (pthread_create(&th[i], nullptr, walk_worker, &w) == 0) ++started;
        if (started == 0) { w.threads = 1; walk_worker(&w); }
        else {
            if (started != threads) { pthread_mutex_lock(&w.mu); w.threads = started; pthread_cond_broadcast(&w.cv); pthread_mutex_unlock(&w.mu); }
            for (int i = 0; i < started; ++i) pthread_join(th[i], nullptr);
        }
    }
    int rc = w.err;
    // Directories last, deepest first: a parent must stay writable until its children are done.
    if (!rc && order == FS_DIRS_POST) {
        for (size_t i = w.dirs.size(); i-- > 0;) {
            struct stat st;
            if (::lstat(w.dirs[i].path.c_str(), &st) != 0) { rc = -errno; break; }
            if ((rc = fn(ctx, w.dirs[i].path.c_str(), w.dirs[i].rel.c_str(), st, true))) break;
        }
    }
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    return rc;
}

namespace {
void bump(uint64_t &v) { __atomic_fetch_add(&v, 1, __ATOMIC_RELAXED); }
int count_entry(void *ctx, const char *, const char *rel, const struct stat &st, bool is_dir) {
    TreeStats *s = (TreeStats *)ctx;
    if (!*rel) return 0;   // the root itself is not an entry of the tree
    // Four workers share these counters; relaxed atomics are enough, nothing reads them
    // until the walk has joined.
    bump(s->entries);
    if (is_dir) bump(s->dirs);
    else {
        bump(s->files);
        if (st.st_nlink > 1) bump(s->hardlinks);   // P9: clonefile breaks these into separate inodes
    }
    return 0;
}
} // namespace

int fs_count_entries(const char *root, TreeStats &out) {
    out = TreeStats();
    return fs_walk_tree(root, 4, FS_DIRS_PRE, &out, count_entry);
}

void Manifest::line(const char *rel, const struct stat &st, bool is_dir) {
    char kind = is_dir ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISREG(st.st_mode) ? 'f'
                : S_ISFIFO(st.st_mode) ? 'p' : S_ISCHR(st.st_mode) ? 'c' : S_ISBLK(st.st_mode) ? 'b' : 's';
#ifdef __APPLE__
    long long sec = (long long)st.st_mtimespec.tv_sec;
    long nsec = (long)st.st_mtimespec.tv_nsec;
#else
    long long sec = (long long)st.st_mtim.tv_sec;
    long nsec = (long)st.st_mtim.tv_nsec;
#endif
    char head[160];
    int n = snprintf(head, sizeof head, "%c %o %llu %lld.%ld %llu ", kind, (unsigned)(st.st_mode & 07777),
                     (unsigned long long)st.st_size, sec, nsec, (unsigned long long)st.st_nlink);
    Guard g(mu);
    if (!f) return;
    fwrite(head, 1, (size_t)n, f);
    for (const char *p = rel; *p; ++p) {
        if (*p == '\\') fputs("\\\\", f);
        else if (*p == '\n') fputs("\\n", f);
        else fputc(*p, f);
    }
    fputc('\n', f);
}

// ---- space, deletion, copying ---------------------------------------------------------------

int fs_free_space(const char *path, uint64_t *avail, uint64_t *total) {
    struct statvfs s;
    if (::statvfs(path, &s) != 0) return -errno;
    if (avail) *avail = (uint64_t)s.f_bavail * (uint64_t)s.f_frsize;
    if (total) *total = (uint64_t)s.f_blocks * (uint64_t)s.f_frsize;
    return 0;
}

namespace {
// Depth-first, single threaded: what this removes is trash and half-built trees, never
// anything on a latency path. Unlinking entries while a parallel readdir is in flight is
// the kind of cleverness that loses a directory.
int rm_rec(const char *path) {
    struct stat st;
    if (::lstat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode)) return ::unlink(path) == 0 || errno == ENOENT ? 0 : -errno;
    DIR *d = ::opendir(path);
    if (!d) return -errno;
    int rc = 0;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        String child(path);
        child.append("/");
        child.append(e->d_name);
        if ((rc = rm_rec(child.c_str()))) break;
    }
    ::closedir(d);
    if (rc) return rc;
    return ::rmdir(path) == 0 || errno == ENOENT ? 0 : -errno;
}
} // namespace

int fs_remove_tree(const char *root) {
    struct stat st;
    if (::lstat(root, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (S_ISDIR(st.st_mode)) fs_unprotect_tree(root);   // snapshots are UF_IMMUTABLE all the way down
    return rm_rec(root);
}

#ifndef __APPLE__
// Linux/other: the clonefile world model is Darwin-only for now. overlayfs is the planned
// equivalent (docs/M1_DESIGN.md §4); until then these report "unsupported" honestly rather
// than silently doing a real copy.
int fs_clone_probe(const char *, const char *) { return -ENOTSUP; }
int fs_clone_tree(const char *, const char *, bool) { return -ENOTSUP; }
int fs_protect_tree(const char *root, TreeStats *stats, Manifest *) {
    if (stats) return fs_count_entries(root, *stats);
    return 0;
}
int fs_unprotect_tree(const char *) { return 0; }
uint64_t fs_events_current_id(void) { return 0; }
#endif

} // namespace wfs
