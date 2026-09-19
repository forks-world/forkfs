// Darwin-only primitives of the M1 clonefile World model: clonefile(2), the EXDEV probe,
// the parallel chflags(UF_IMMUTABLE) protection walk, and the FSEvents cursor.
//
// The measurements these are built on are in docs/CLONE_MODEL_MACOS27.md:
//   §9  a single clonefile(dir) is 7.4 µs/entry; a per-file loop is 14× slower even with
//       4 threads, and copyfile(3)'s recursive clone aborts on a FIFO. Directory clone is
//       therefore the main path and the per-file loop only a fallback.
//   §10 cloning a live tree does not fail writers in it; it costs them 10–15 ms spikes.
//   §11 clonefile(dir) breaks hardlinks (so does every alternative) and does not preserve
//       the mtime of directories that have children.
#include "internal.h"

#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

// From CoreServices/FSEvents.h. Declared rather than included so this file keeps compiling
// with nothing but the C SDK headers; the symbol comes from CoreServices.framework.
extern "C" uint64_t FSEventsGetCurrentEventId(void);

namespace wfs {

uint64_t fs_events_current_id(void) { return FSEventsGetCurrentEventId(); }

// ---- EXDEV probe (P6) ---------------------------------------------------------------------
//
// st_dev equality does NOT predict clonefile success on this system: the data volume and
// /System share an st_dev and clonefile between them still returns EXDEV (CLONE_MODEL §7).
// The only reliable test is to clone something for real.

namespace {

int find_regular(const char *dir, String &out, int depth) {
    DIR *d = ::opendir(dir);
    if (!d) return -errno;
    String subdir;
    int rc = -ENOENT;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        String p(dir);
        p.append("/");
        p.append(e->d_name);
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (S_ISREG(st.st_mode)) { out = p; rc = 0; break; }
        if (S_ISDIR(st.st_mode) && subdir.size() == 0) subdir = p;
    }
    ::closedir(d);
    if (rc == 0) return 0;
    if (depth > 0 && subdir.size()) return find_regular(subdir.c_str(), out, depth - 1);
    return rc;
}

void force_unlink(const char *p) {
    // A clone of a protected snapshot file inherits UF_IMMUTABLE.
    ::lchflags(p, 0);
    ::unlink(p);
}

} // namespace

int fs_clone_probe(const char *dst_dir, const char *src_dir) {
    if (!dst_dir || !src_dir) return -EINVAL;
    char probe[PATH_MAX];
    ::snprintf(probe, sizeof probe, "%s/.wfs-probe-%d", dst_dir, (int)::getpid());
    force_unlink(probe);

    String srcfile;
    bool temp = false;
    if (find_regular(src_dir, srcfile, 3) != 0) {
        char t[PATH_MAX];
        ::snprintf(t, sizeof t, "%s/.wfs-probe-src-%d", src_dir, (int)::getpid());
        int fd = ::open(t, O_CREAT | O_EXCL | O_WRONLY, 0600);
        // An unwritable source with no regular file in it: nothing to probe with. Say "fine"
        // and let the real clonefile report the truth rather than invent a refusal.
        if (fd < 0) return 0;
        ssize_t w = ::write(fd, "wfs", 3);
        (void)w;
        ::close(fd);
        srcfile.assign(t);
        temp = true;
    }
    int rc = 0;
    if (::clonefile(srcfile.c_str(), probe, CLONE_NOFOLLOW) != 0) rc = -errno;
    force_unlink(probe);
    if (temp) ::unlink(srcfile.c_str());
    return rc;
}

// ---- cloning ---------------------------------------------------------------------------------

namespace {

struct CloneCtx {
    const char *dst;
};

int clone_entry(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    CloneCtx *c = (CloneCtx *)ctx;
    String d(c->dst);
    if (*rel) {
        d.append("/");
        d.append(rel);
    }
    if (is_dir) {
        // Keep it writable while we fill it; the metadata pass restores the real mode.
        if (::mkdir(d.c_str(), (st.st_mode & 07777) | 0700) != 0 && errno != EEXIST) return -errno;
        return 0;
    }
    if (S_ISLNK(st.st_mode)) {
        char buf[PATH_MAX];
        ssize_t n = ::readlink(path, buf, sizeof buf - 1);
        if (n < 0) return -errno;
        buf[n] = 0;
        return ::symlink(buf, d.c_str()) == 0 ? 0 : -errno;
    }
    if (S_ISFIFO(st.st_mode)) {
        // clonefileat fails on a FIFO and copyfile(3) aborts the whole tree on one
        // (CLONE_MODEL §11); recreate it.
        return ::mkfifo(d.c_str(), st.st_mode & 07777) == 0 ? 0 : -errno;
    }
    if (!S_ISREG(st.st_mode)) return 0; // sockets and device nodes are not ours to recreate
    if (::clonefile(path, d.c_str(), CLONE_NOFOLLOW) == 0) return 0;
    int e = errno;
    if (e != EXDEV && e != ENOTSUP) return -e;
    // Different volume: a real copy is the only thing left (this is what --copy buys).
    if (::copyfile(path, d.c_str(), nullptr, COPYFILE_ALL | COPYFILE_NOFOLLOW) != 0) return -errno;
    return 0;
}

int dirmeta_entry(void *ctx, const char *path, const char *rel, const struct stat &, bool is_dir) {
    if (!is_dir) return 0;
    CloneCtx *c = (CloneCtx *)ctx;
    String d(c->dst);
    if (*rel) {
        d.append("/");
        d.append(rel);
    }
    // Deepest first, so a parent's mtime is written after all of its children exist.
    ::copyfile(path, d.c_str(), nullptr, COPYFILE_METADATA | COPYFILE_NOFOLLOW);
    return 0;
}

} // namespace

int fs_clone_tree(const char *src, const char *dst, bool allow_fallback) {
    if (!src || !dst) return -EINVAL;
    if (::clonefile(src, dst, CLONE_NOFOLLOW) == 0) return 0;
    int e = errno;
    if (!allow_fallback || (e != EXDEV && e != ENOTSUP)) return -e;
    // 4 workers: 8 and 16 measured slower, the bottleneck is APFS metadata transactions.
    CloneCtx c{dst};
    if (int rc = fs_walk_tree(src, 4, FS_DIRS_PRE, &c, clone_entry)) return rc;
    return fs_walk_tree(src, 1, FS_DIRS_POST, &c, dirmeta_entry);
}

// ---- protection (P3) ---------------------------------------------------------------------------

namespace {

struct ProtectCtx {
    TreeStats *stats;
    Manifest *man;
};

void bump(uint64_t &v) { __atomic_fetch_add(&v, 1, __ATOMIC_RELAXED); }

int protect_entry(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    ProtectCtx *c = (ProtectCtx *)ctx;
    struct stat rec = st;
    if (is_dir) {
        // Strip the write bits rather than forcing 0500: that round-trips (0755 -> 0555 ->
        // 0755, 0700 -> 0500 -> 0700) so unprotecting a clone gives the tree its modes back.
        mode_t pm = (mode_t)(st.st_mode & 07777) & ~(mode_t)0222;
        if (pm != (st.st_mode & 07777) && ::chmod(path, pm) != 0) return -errno;
        rec.st_mode = (st.st_mode & ~(mode_t)07777) | pm;
    }
    if (::lchflags(path, st.st_flags | UF_IMMUTABLE) != 0) return -errno;
    if (*rel && c->stats) {
        // Four workers, one TreeStats: relaxed atomics, read only after the walk joins.
        bump(c->stats->entries);
        if (is_dir) bump(c->stats->dirs);
        else {
            bump(c->stats->files);
            if (st.st_nlink > 1) bump(c->stats->hardlinks); // always 0 here: see fs_protect_tree
        }
    }
    if (c->man) c->man->line(rel, rec, is_dir);
    return 0;
}

int unprotect_entry(void *, const char *path, const char *, const struct stat &st, bool is_dir) {
    uint32_t want = st.st_flags & ~(uint32_t)(UF_IMMUTABLE | UF_APPEND);
    if (want != st.st_flags && ::lchflags(path, want) != 0) return -errno;
    if (is_dir) {
        mode_t m = (mode_t)(st.st_mode & 07777) | 0200; // give the owner write back
        if (m != (st.st_mode & 07777) && ::chmod(path, m) != 0) return -errno;
    }
    return 0;
}

} // namespace

// Note on hardlinks: the tree handed here is already a clone, and clonefile breaks every
// hardlink into independent inodes (CLONE_MODEL_MACOS27 §11), so nlink>1 never shows up.
// The P9 count comes from a walk of the source, before cloning (see wfs_snapshot_create).
int fs_protect_tree(const char *root, TreeStats *stats, Manifest *man) {
    if (stats) *stats = TreeStats();
    ProtectCtx c{stats, man};
    // Directories last: a directory that is already UF_IMMUTABLE refuses chflags on itself.
    return fs_walk_tree(root, 4, FS_DIRS_POST, &c, protect_entry);
}

int fs_unprotect_tree(const char *root) {
    // Directories first: their own flags have to go before anything else about them changes.
    return fs_walk_tree(root, 4, FS_DIRS_PRE, nullptr, unprotect_entry);
}

} // namespace wfs
