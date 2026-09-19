// POSIX backing-store primitives. macOS and Linux; Windows gets its own file later.
#include "internal.h"

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifdef __APPLE__
#define WFS_XATTR_NOFOLLOW XATTR_NOFOLLOW
#else
#define WFS_XATTR_NOFOLLOW 0
#endif

// PR #1 review (3rd round): the seam for a partial pthread_create failure. See threads_start.
extern "C" unsigned wfs_test_thread_fail_mask = 0;

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

int threads_start(pthread_t *th, int want, void *(*fn)(void *), void *arg) {
    int started = 0;
    for (int i = 0; i < want; ++i) {
        if (i < 32 && (wfs_test_thread_fail_mask & (1u << i))) continue;
        if (::pthread_create(&th[started], nullptr, fn, arg) == 0) ++started;
    }
    return started;
}

int64_t fs_pid_start_sec(int64_t pid) {
    if (pid <= 0) return 0;
#ifdef __APPLE__
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    if (::sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) return 0;
    return (int64_t)kp.kp_proc.p_starttime.tv_sec;
#else
    // /proc/<pid>/stat field 22 is the start time in clock ticks since boot. The comm field can
    // contain spaces and parentheses, so the scan starts after the LAST ')'.
    char path[64];
    ::snprintf(path, sizeof path, "/proc/%lld/stat", (long long)pid);
    FILE *f = ::fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = ::fread(buf, 1, sizeof buf - 1, f);
    ::fclose(f);
    buf[n] = 0;
    char *p = ::strrchr(buf, ')');
    if (!p) return 0;
    int field = 2;   // the next token is field 3 (state)
    for (char *t = ::strtok(p + 1, " "); t; t = ::strtok(nullptr, " "))
        if (++field == 22) return ::strtoll(t, nullptr, 10);
    return 0;
#endif
}

bool producer_alive(int64_t pid, int64_t start_sec) {
    if (pid <= 0) return false;                       // nobody was recorded
    if (::kill((pid_t)pid, 0) != 0 && errno == ESRCH) return false;
    if (start_sec > 0) {
        int64_t now_start = fs_pid_start_sec(pid);
        if (now_start > 0 && now_start != start_sec) return false;   // the pid was reused
    }
    return true;                                      // alive, or we cannot tell: leave it be
}

int64_t creating_min_age_secs(void) {
    if (const char *e = ::getenv("WORLD_GC_CREATING_MIN_AGE")) {
        char *end = nullptr;
        long long v = ::strtoll(e, &end, 10);
        if (end != e && v >= 0) return (int64_t)v;
    }
    return 60;
}

namespace {
struct ThreadsProbe { unsigned ran = 0; };
void *threads_probe_worker(void *p) {
    __atomic_fetch_add(&((ThreadsProbe *)p)->ran, 1u, __ATOMIC_RELAXED);
    return nullptr;
}
} // namespace

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

// The publish rename (P8), for the one case where `to` is a path the user chose. rename(2) is
// happy to replace an existing empty directory, so the plain call would turn "there is already
// something at --to" into a silent deletion the moment anything creates that directory between
// P7's check and here. RENAME_EXCL makes the kernel do the check at the instant of the rename:
// EEXIST, never a replacement.
int fs_rename_excl(const char *from, const char *to) {
#ifdef __APPLE__
    return ::renameatx_np(AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCL) ? -errno : 0;
#elif defined(RENAME_NOREPLACE) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) ? -errno : 0;
#else
    // Last resort: a window remains, but a target that is there *now* is still never replaced.
    struct stat st;
    if (::lstat(to, &st) == 0) return -EEXIST;
    return ::rename(from, to) ? -errno : 0;
#endif
}

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
//
// T2.4: a directory is read with getattrlistbulk(2) where the file system can (one syscall per
// batch instead of one fstatat per entry, and every entry arrives with the EF_NO_XATTRS verdict
// attached -- see platform_darwin.cpp for the attribute list and the measurements). readdir(3) +
// fstatat(2) is still here, unchanged: it is the whole story on Linux, and on Darwin it is what
// a file system that cannot serve the bulk call falls back to. The fallback verdict is taken
// before a single entry has been reported, so nothing is ever reported twice.

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
    fs_entry_ex_fn fn = nullptr;
    fs_dir_order order = FS_DIRS_PRE;
};

void walk_child(const Job &parent, const char *name, Job &out) {
    out.path = parent.path;
    if (out.path.size() && out.path.c_str()[out.path.size() - 1] != '/') out.path.append("/");
    out.path.append(name);
    out.rel = parent.rel;
    if (out.rel.size()) out.rel.append("/");
    out.rel.append(name);
}

// The per-entry half of walk_dir, shared by the bulk path and the readdir path so that the two
// cannot drift apart. A non-zero return aborts the walk.
struct DirScan {
    Walk *w;
    const Job *job;
    Vec<Job> *subdirs;
};

int walk_one(DirScan &s, const char *name, const struct stat &st, uint8_t xattr) {
    Job child;
    walk_child(*s.job, name, child);
    bool is_dir = S_ISDIR(st.st_mode);
    if (is_dir) s.subdirs->emplace_back(child);
    if (is_dir && s.w->order != FS_DIRS_PRE) return 0;
    FsEntry e{child.path.c_str(), child.rel.c_str(), &st, is_dir, xattr};
    return s.w->fn(s.w->ctx, e);
}

int walk_bulk_entry(void *ctx, const char *name, size_t, const struct stat &st, uint8_t xattr) {
    return walk_one(*(DirScan *)ctx, name, st, xattr);
}

// Returns a negative errno to abort the whole walk.
int walk_dir(Walk &w, const Job &job) {
    Vec<Job> subdirs;
    DirScan scan{&w, &job, &subdirs};
    int rc = 0;

    int fd = ::open(job.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int bulk = fs_bulk_dir(fd, &scan, walk_bulk_entry, &rc);
    if (bulk == 0) {
        ::close(fd);
    } else if (bulk != -ENOTSUP) {
        ::close(fd);
        return bulk;
    } else {
        // No bulk enumeration here (another file system, or an older kernel): readdir(3) +
        // fstatat(2), from the top, with nothing reported yet.
        if (::lseek(fd, 0, SEEK_SET) < 0) { ::close(fd); return -errno; }
        DIR *d = ::fdopendir(fd);
        if (!d) { int e = errno; ::close(fd); return -e; }
        while (struct dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
            struct stat st;
            if (::fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) { rc = -errno; break; }
            if ((rc = walk_one(scan, e->d_name, st, FS_XATTR_UNKNOWN))) break;
        }
        ::closedir(d);
    }
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

int fs_walk_tree_ex(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_ex_fn fn) {
    if (!root || !fn) return -EINVAL;
    struct stat rst;
    uint8_t rxa = FS_XATTR_UNKNOWN;
    if (int rc = fs_lstat_xattr(root, rst, rxa)) return rc;
    if (!S_ISDIR(rst.st_mode)) {
        FsEntry e{root, "", &rst, false, rxa};
        return fn(ctx, e);
    }

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
        FsEntry e{r.path.c_str(), "", &rst, true, rxa};
        if (int rc = fn(ctx, e)) {
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
        int started = threads_start(th, threads, walk_worker, &w);
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
            // Directories only, and a directory's xattr verdict is never used by anyone
            // (diff.cpp compares nothing about a directory that exists on both sides), so a
            // plain lstat is both right and one syscall cheaper than fs_lstat_xattr here.
            if (::lstat(w.dirs[i].path.c_str(), &st) != 0) { rc = -errno; break; }
            FsEntry e{w.dirs[i].path.c_str(), w.dirs[i].rel.c_str(), &st, true, FS_XATTR_UNKNOWN};
            if ((rc = fn(ctx, e))) break;
        }
    }
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    return rc;
}

namespace {
// fs_walk_tree is fs_walk_tree_ex with the xattr verdict dropped: every caller that does not
// compare xattrs (clone, protect, count, scan, verify) keeps the signature it always had.
struct PlainWalk {
    void *ctx;
    fs_entry_fn fn;
};
int plain_entry(void *ctx, const FsEntry &e) {
    PlainWalk *p = (PlainWalk *)ctx;
    return p->fn(p->ctx, e.path, e.rel, *e.st, e.is_dir);
}
} // namespace

int fs_walk_tree(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_fn fn) {
    if (!fn) return -EINVAL;
    PlainWalk p{ctx, fn};
    return fs_walk_tree_ex(root, threads, order, &p, plain_entry);
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

namespace {
struct ScanCtx {
    TreeStats *stats;
    Manifest *man;
};

int scan_entry(void *ctx, const char *, const char *rel, const struct stat &st, bool is_dir) {
    ScanCtx *c = (ScanCtx *)ctx;
    if (*rel && c->stats) {
        bump(c->stats->entries);
        if (is_dir) bump(c->stats->dirs);
        else {
            bump(c->stats->files);
            if (st.st_nlink > 1) bump(c->stats->hardlinks);   // always 0 on a clone; see below
        }
    }
    if (c->man) c->man->line(rel, st, is_dir);
    return 0;
}
} // namespace

// Gate protection reads the tree once to write the manifest that `verify` checks against, and
// touches nothing. The hardlink count here is always 0 for the same reason as in
// fs_protect_tree (clonefile breaks them); the real P9 number comes from a walk of the source.
int fs_scan_tree(const char *root, TreeStats *stats, Manifest *man) {
    if (stats) *stats = TreeStats();
    ScanCtx c{stats, man};
    return fs_walk_tree(root, 4, FS_DIRS_PRE, &c, scan_entry);
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
//
// PR #1 review (4th round): `deadline` (an fs_mono_us() stamp, 0 = no limit) is read once per
// entry, exactly as the parallel deleter's rm_entry reads it. Out of time is -ECANCELED, which
// unwinds without rmdir'ing anything on the way out -- the tree is a `.deleting` one and what
// is left of it is resumable by construction.
int rm_rec(const char *path, int64_t deadline) {
    if (deadline && fs_mono_us() >= deadline) return -ECANCELED;
    struct stat st;
    if (::lstat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode)) return ::unlink(path) == 0 || errno == ENOENT ? 0 : -errno;
    // The write bit that unlink(2) needs is the directory's, not the entry's, and a tree can
    // perfectly well contain a 0555 directory of the source's own making (PR #1 review, 4th
    // round: the parallel deleter's rm_entry already chmods the parent on EACCES, and this is
    // the path that has to survive the same trees). We are deleting the thing, so taking the
    // mode off for good is exactly right -- as it is for a gate-protected 0000 root.
    if ((st.st_mode & 0700) != 0700) ::chmod(path, (mode_t)((st.st_mode & 07777) | 0700));
    DIR *d = ::opendir(path);
    if (!d && errno == EACCES) {
        ::chmod(path, 0700);
        d = ::opendir(path);
    }
    if (!d) return -errno;
    int rc = 0;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        String child(path);
        child.append("/");
        child.append(e->d_name);
        if ((rc = rm_rec(child.c_str(), deadline))) break;
    }
    ::closedir(d);
    if (rc) return rc;
    return ::rmdir(path) == 0 || errno == ENOENT ? 0 : -errno;
}
} // namespace

int fs_remove_tree(const char *root, int64_t deadline_us, int *partial) {
    if (partial) *partial = 0;
    struct stat st;
    if (::lstat(root, &st) != 0) return errno == ENOENT ? 0 : -errno;
    // snapshots are UF_IMMUTABLE all the way down. That walk is O(tree) too, so it gets the
    // deadline as well -- a fallback that spent its whole budget unprotecting would be the
    // same unbounded wake by another name.
    if (S_ISDIR(st.st_mode) && fs_unprotect_tree(root, deadline_us) == -ECANCELED) {
        if (partial) *partial = 1;
        return 0;
    }
    int rc = rm_rec(root, deadline_us);
    if (rc == -ECANCELED) {
        if (partial) *partial = 1;
        return 0;
    }
    return rc;
}

int64_t fs_mono_us(void) {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}

// ---- T2.1: the parallel deleter --------------------------------------------------------------
//
// The same 4-thread walker that clones and scans, used to unlink. Files go in the parallel
// phase (one unlinkat per entry, spread over the workers), directories in the serial tail the
// walker already does deepest-first -- a parent must not be rmdir'ed before its children.
//
// `entries` counts what was actually removed, which is what the gc worker reports as progress.
// Every failure path degrades to the single-threaded rm_rec above rather than leaving half a
// tree: this is deletion, and a partially deleted tree is exactly what the .deleting rename in
// world.cpp exists to make safe.
namespace {

struct RmCtx {
    uint64_t entries = 0;
    int err = 0;
    int64_t deadline = 0;   // fs_mono_us() stamp; 0 = no limit
};

// One unlink. UF_IMMUTABLE (a --hard snapshot that was not unprotected first) and a directory
// whose write bit was stripped both surface as EPERM/EACCES; clear them and try once more.
int rm_entry(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    RmCtx *c = (RmCtx *)ctx;
    // The batch limit, enforced where the work actually is. One clock read against an unlink
    // that measures ~50 us is not worth optimising, and -ECANCELED aborts the walk before the
    // deepest-first directory tail runs, so what is left behind is a tree with whole
    // subdirectories still in it rather than a scattering of empty ones.
    if (c->deadline && fs_mono_us() >= c->deadline) return -ECANCELED;
    if (!*rel && !is_dir) { // the walker was handed a non-directory
        if (::unlink(path) != 0 && errno != ENOENT) return -errno;
        bump(c->entries);
        return 0;
    }
    if (is_dir) {
        if (::rmdir(path) == 0 || errno == ENOENT) { if (*rel) bump(c->entries); return 0; }
        int e = errno;
        if (e == EPERM || e == EACCES || e == ENOTEMPTY) {
#ifdef __APPLE__
            ::lchflags(path, 0);
#endif
            ::chmod(path, 0700);
            if (::rmdir(path) == 0 || errno == ENOENT) { if (*rel) bump(c->entries); return 0; }
            e = errno;
        }
        // Still not empty: the walk unlinks entries while the same DIR stream is being read,
        // and POSIX leaves it unspecified whether readdir(3) returns an entry removed after
        // opendir(3). (Measured on APFS: 4 x 10 401 entries, none missed -- fts(3) and rm -rf
        // rely on the same behaviour.) If it ever does happen, finish this one directory the
        // certain way rather than failing the whole tree.
        if (e == ENOTEMPTY) {
            int r2 = rm_rec(path, c->deadline);
            if (r2 == 0) { if (*rel) bump(c->entries); return 0; }
            if (r2 == -ECANCELED) return -ECANCELED;   // the batch limit, not a failure
            e = ENOTEMPTY;
        }
        return -e;
    }
    if (::unlink(path) == 0 || errno == ENOENT) { bump(c->entries); return 0; }
    int e = errno;
    if (e == EPERM || e == EACCES) {
#ifdef __APPLE__
        ::lchflags(path, 0);
#endif
        // The write bit that matters for unlink(2) is the parent's, not the entry's.
        const char *slash = ::strrchr(path, '/');
        if (slash && slash != path) {
            String parent;
            parent.append(path, (size_t)(slash - path));
#ifdef __APPLE__
            ::lchflags(parent.c_str(), 0);
#endif
            ::chmod(parent.c_str(), 0700);
        }
        if (::unlink(path) == 0 || errno == ENOENT) { bump(c->entries); return 0; }
        e = errno;
    }
    (void)st;
    return -e;
}

} // namespace

int fs_remove_tree_parallel(const char *root, int threads, uint64_t *entries, int64_t deadline_us,
                            int *partial) {
    if (partial) *partial = 0;
    struct stat st;
    if (::lstat(root, &st) != 0) return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode)) {
        if (::unlink(root) != 0 && errno != ENOENT) return -errno;
        if (entries) (*entries)++;
        return 0;
    }
    // A gate-protected root is 0000 and cannot even be opened. We are deleting it, so opening
    // the gate for good is exactly right (docs/M1_DESIGN.md §3 P4).
    if (::access(root, R_OK | X_OK | W_OK) != 0) ::chmod(root, 0700);
    RmCtx c;
    c.deadline = deadline_us;
    int rc = fs_walk_tree(root, threads, FS_DIRS_POST, &c, rm_entry);
    if (entries) *entries += c.entries;
    if (rc == 0) return 0;
    // Out of time, not out of luck: the tree is half gone and the caller is told to come back.
    if (rc == -ECANCELED) {
        if (partial) *partial = 1;
        return 0;
    }
    // Anything at all went wrong: finish the job the slow, certain way -- under the same
    // deadline (PR #1 review, 4th round). Without it a single EACCES directory in a 120k-entry
    // trash tree turned a one-second worker wake into an unbounded one, which is exactly the
    // foreground contention max_secs exists to bound.
    return fs_remove_tree(root, deadline_us, partial);
}

#ifndef __APPLE__
// There is no getattrlistbulk(2) outside Darwin: the walk uses readdir(3) + fstatat(2) and
// nobody can say anything about xattrs without calling listxattr, which is exactly what
// FS_XATTR_UNKNOWN means.
int fs_bulk_dir(int, void *, fs_bulk_entry_fn, int *cb_rc) {
    if (cb_rc) *cb_rc = 0;
    return -ENOTSUP;
}
int fs_lstat_xattr(const char *path, struct stat &st, uint8_t &xattr) {
    xattr = FS_XATTR_UNKNOWN;
    if (!path) return -EINVAL;
    return ::lstat(path, &st) == 0 ? 0 : -errno;
}

// Linux/other: the clonefile world model is Darwin-only for now. overlayfs is the planned
// equivalent (docs/M1_DESIGN.md §4); until then these report "unsupported" honestly rather
// than silently doing a real copy.
int fs_clone_probe(const char *, const char *) { return -ENOTSUP; }
int fs_clone_tree(const char *, const char *, bool) { return -ENOTSUP; }
int fs_protect_tree(const char *root, TreeStats *stats, Manifest *) {
    if (stats) return fs_count_entries(root, *stats);
    return 0;
}
int fs_unprotect_tree(const char *, int64_t) { return 0; }
uint64_t fs_events_current_id(void) { return 0; }
#endif

} // namespace wfs

// PR #1 review (3rd round). A unit test of threads_start, because the bug it fixes is not one a
// caller can reliably observe: joining a pthread_t that was never written is undefined, and on a
// good day it merely fails. Here the array is zeroed first, so an unwritten slot is a handle no
// join can succeed on, and the contract is checked directly: threads_start writes the handles it
// really started into th[0..n), every one of them joins, and the number of workers that ran is
// the number that were joined -- nobody left running behind the caller's back.
extern "C" int wfs_test_threads_start(int want, unsigned fail_mask, int *started, int *joined) {
    if (want <= 0 || want > 16 || !started || !joined) return -EINVAL;
    pthread_t th[16];
    memset(th, 0, sizeof th);
    unsigned save = wfs_test_thread_fail_mask;
    wfs_test_thread_fail_mask = fail_mask;
    wfs::ThreadsProbe probe;
    int n = wfs::threads_start(th, want, wfs::threads_probe_worker, &probe);
    wfs_test_thread_fail_mask = save;
    int ok = 0;
    for (int i = 0; i < n; ++i)
        if (::pthread_join(th[i], nullptr) == 0) ++ok;
    *started = n;
    *joined = ok;
    return (unsigned)ok == __atomic_load_n(&probe.ran, __ATOMIC_RELAXED) ? 0 : -EIO;
}
