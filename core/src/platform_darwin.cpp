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
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

// From CoreServices/FSEvents.h. Declared rather than included so this file keeps compiling
// with nothing but the C SDK headers; the symbol comes from CoreServices.framework.
extern "C" uint64_t FSEventsGetCurrentEventId(void);

namespace wfs {

uint64_t fs_events_current_id(void) { return FSEventsGetCurrentEventId(); }

// ---- getattrlistbulk(2): what a directory costs (T2.4) -----------------------------------------
//
// The old walker paid one fstatat(2) per entry, and diff.cpp paid listxattr(2) on top of it for
// both sides of every file. Measured on this machine (M1, 27.0, APFS, warm):
//
//     lstat(2)                          1.41 µs      listxattr(2)              2.1 µs
//     getattrlist(2), the list below    1.78 µs      getxattr(2)              14.0 µs
//     lstat + listxattr                 2.56 µs      the old xattr_equal()    55 µs per pair
//     getattrlistbulk(2), per entry     0.23 µs      the new one              35 µs per pair
//
// So a file with no xattrs at all is the case worth detecting, and ATTR_CMNEXT_EXT_FLAGS carries
// EF_NO_XATTRS for exactly that. It is a one-way answer -- the file system either says "there are
// none" or says nothing -- which is the safe direction: a missing bit costs a listxattr, never a
// wrong result. Verified over 137,665 entries of /Applications and /usr/share: 117,168 carried
// EF_NO_XATTRS and not one of them had a listxattr(2) that returned anything.
//
// What is asked for, and why:
//   ATTR_CMN_RETURNED_ATTRS   which of the rest actually came back; always packed first
//   ATTR_CMN_NAME             the entry's name (required for a bulk call)
//   ATTR_CMN_DEVID            st_dev
//   ATTR_CMN_OBJTYPE          VREG/VDIR/VLNK/...; kept as the cross-check on ACCESSMASK
//   ATTR_CMN_CRTIME/MODTIME/CHGTIME/ACCTIME   st_birthtimespec / mtime / ctime / atime
//   ATTR_CMN_OWNERID/GRPID    st_uid, st_gid
//   ATTR_CMN_ACCESSMASK       st_mode -- **with S_IFMT included**: measured, a directory comes
//                             back as 0o41755, a symlink as 0o120755, a FIFO as 0o10644
//   ATTR_CMN_FLAGS            st_flags (P3's UF_IMMUTABLE, and the diff's flag comparison)
//   ATTR_CMN_FILEID           st_ino
//   ATTR_FILE_LINKCOUNT       st_nlink (P9 counts hardlinks)
//   ATTR_FILE_ALLOCSIZE       st_blocks, as allocsize/512
//   ATTR_FILE_DATALENGTH      st_size -- for a symlink this is the target's length, as lstat's is
//   ATTR_CMNEXT_EXT_FLAGS     EF_NO_XATTRS; a forkattr, so it needs FSOPT_ATTR_CMN_EXTENDED
//
// Directories are deliberately NOT taken from the bulk buffer. ATTR_DIR_LINKCOUNT is the number
// of hard links to the directory (measured: 1 where lstat's st_nlink is 3) and ATTR_DIR_DATALENGTH
// is not st_size either, so a directory costs one extra fstatat(2) and its struct stat stays
// byte-for-byte what it has always been -- which matters, because Manifest::line writes it and
// `snapshot verify` reads those lines back. Directories are ~1% of a tree and the walker is about
// to open each of them anyway. The same fstatat is the per-entry fallback for anything whose
// attributes did not all come back.
//
// The whole synthesized struct stat was cross-checked against lstat(2) field by field --  mode,
// uid, gid, nlink, size, blocks, flags, ino, dev and all four timestamps -- over 148,593 non
// directory entries of /Applications, /usr/share and /usr/lib: zero mismatches.

namespace {

const uint32_t kBulkCommon = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_DEVID |
                             ATTR_CMN_OBJTYPE | ATTR_CMN_CRTIME | ATTR_CMN_MODTIME |
                             ATTR_CMN_CHGTIME | ATTR_CMN_ACCTIME | ATTR_CMN_OWNERID |
                             ATTR_CMN_GRPID | ATTR_CMN_ACCESSMASK | ATTR_CMN_FLAGS |
                             ATTR_CMN_FILEID;
const uint32_t kBulkFile = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
const uint32_t kBulkFork = ATTR_CMNEXT_EXT_FLAGS;
// Everything but the bitmap itself has to come back, or the entry falls back to fstatat.
const uint32_t kBulkCommonNeeded = kBulkCommon & ~(uint32_t)ATTR_CMN_RETURNED_ATTRS;

// The kernel packs attributes in ascending bit order with no alignment padding, so reading
// them back is just "advance past each one that the bitmap says is there". `end` is the record
// length the kernel wrote in front of the record, so nothing here can run off the buffer.
struct Unpack {
    const char *p;
    const char *end;
    bool ok = true;
    template <typename T>
    void take(T &out, bool present) {
        if (!present || !ok) return;
        if ((size_t)(end - p) < sizeof(T)) { ok = false; return; }
        memcpy(&out, p, sizeof(T));
        p += sizeof(T);
    }
};

void fill_stat(struct stat &st, dev_t dv, uint32_t mode, uid_t uid, gid_t gid, uint32_t nlink,
               uint64_t ino, int64_t size, int64_t alloc, uint32_t flags, const struct timespec &bt,
               const struct timespec &mt, const struct timespec &ct, const struct timespec &at) {
    memset(&st, 0, sizeof st);
    st.st_dev = dv;
    st.st_mode = (mode_t)mode;
    st.st_nlink = (nlink_t)nlink;
    st.st_ino = (ino_t)ino;
    st.st_uid = uid;
    st.st_gid = gid;
    st.st_size = (off_t)size;
    st.st_blocks = (blkcnt_t)(alloc / 512);
    st.st_blksize = 4096;
    st.st_flags = flags;
    st.st_birthtimespec = bt;
    st.st_mtimespec = mt;
    st.st_ctimespec = ct;
    st.st_atimespec = at;
}

bool is_dot(const char *n) {
    // getattrlistbulk never yields "." or ".." (docs/TASKS.md T1.7); belt and braces.
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

} // namespace

int fs_bulk_dir(int dirfd, void *ctx, fs_bulk_entry_fn fn, int *cb_rc) {
    if (dirfd < 0 || !fn) return -EINVAL;
    if (cb_rc) *cb_rc = 0;
    struct attrlist al;
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = kBulkCommon;
    al.fileattr = kBulkFile;
    al.forkattr = kBulkFork;
    // 32 KiB is ~150 entries of this shape. A real tree's syscall count is set by its directory
    // count, not by the batch size: /Applications is 134,641 entries in 10,302 directories and
    // 9,974 calls, one per non-empty directory plus the one that returns 0.
    char buf[32 * 1024];
    bool reported = false;
    for (;;) {
        // No FSOPT_PACK_INVAL_ATTRS on purpose. With it, an attribute the file system cannot
        // serve still gets its ATTR_CMN_RETURNED_ATTRS bit set and a zero packed in its place --
        // a silently wrong mtime or size. Without it the bit stays clear, the unpacker notices,
        // and the entry falls back to fstatat(2). Measured: on devfs, 352 of 353 entries have
        // ATTR_CMNEXT_EXT_FLAGS absent without the flag and bogus-but-"present" with it; on APFS
        // the two are byte-identical over 144,133 entries.
        int n = ::getattrlistbulk(dirfd, &al, buf, sizeof buf, FSOPT_ATTR_CMN_EXTENDED);
        // Before the first entry has been handed over this means "this file system cannot",
        // and the caller starts the directory again with readdir(3). Afterwards it is a real
        // failure and has to be one, or entries would be reported twice.
        if (n < 0) return reported ? -errno : -ENOTSUP;
        if (n == 0) return 0;
        const char *p = buf;
        const char *bufend = buf + sizeof buf;
        for (int i = 0; i < n; ++i) {
            // Everything below trusts the record length the kernel wrote, so check it first:
            // one bad length would walk the rest of the loop off the end of the buffer.
            uint32_t reclen = 0;
            if ((size_t)(bufend - p) < sizeof reclen) return reported ? -EIO : -ENOTSUP;
            memcpy(&reclen, p, sizeof reclen);
            if (reclen < sizeof(uint32_t) + sizeof(attribute_set_t) ||
                reclen > (size_t)(bufend - p))
                return reported ? -EIO : -ENOTSUP;
            Unpack u{p + sizeof(uint32_t), p + reclen};
            attribute_set_t ret;
            memset(&ret, 0, sizeof ret);
            u.take(ret, true);
            const char *name = nullptr;
            if (ret.commonattr & ATTR_CMN_NAME) {
                const char *at = u.p;
                attrreference_t ar;
                u.take(ar, true);
                if (u.ok && ar.attr_dataoffset >= 0 && ar.attr_length > 0 &&
                    at + (size_t)ar.attr_dataoffset + ar.attr_length <= p + reclen &&
                    at[(size_t)ar.attr_dataoffset + ar.attr_length - 1] == 0)
                    name = at + ar.attr_dataoffset;
            }
            if (!name) return reported ? -EIO : -ENOTSUP;

            dev_t dv = 0;
            fsobj_type_t ot = 0;
            struct timespec bt = {0, 0}, mt = {0, 0}, ct = {0, 0}, at = {0, 0};
            uid_t uid = 0;
            gid_t gid = 0;
            uint32_t mode = 0, flags = 0, nlink = 0;
            uint64_t ino = 0, ext = 0;
            off_t alloc = 0, len = 0;
            u.take(dv, (ret.commonattr & ATTR_CMN_DEVID) != 0);
            u.take(ot, (ret.commonattr & ATTR_CMN_OBJTYPE) != 0);
            u.take(bt, (ret.commonattr & ATTR_CMN_CRTIME) != 0);
            u.take(mt, (ret.commonattr & ATTR_CMN_MODTIME) != 0);
            u.take(ct, (ret.commonattr & ATTR_CMN_CHGTIME) != 0);
            u.take(at, (ret.commonattr & ATTR_CMN_ACCTIME) != 0);
            u.take(uid, (ret.commonattr & ATTR_CMN_OWNERID) != 0);
            u.take(gid, (ret.commonattr & ATTR_CMN_GRPID) != 0);
            u.take(mode, (ret.commonattr & ATTR_CMN_ACCESSMASK) != 0);
            u.take(flags, (ret.commonattr & ATTR_CMN_FLAGS) != 0);
            u.take(ino, (ret.commonattr & ATTR_CMN_FILEID) != 0);
            u.take(nlink, (ret.fileattr & ATTR_FILE_LINKCOUNT) != 0);
            u.take(alloc, (ret.fileattr & ATTR_FILE_ALLOCSIZE) != 0);
            u.take(len, (ret.fileattr & ATTR_FILE_DATALENGTH) != 0);
            u.take(ext, (ret.forkattr & ATTR_CMNEXT_EXT_FLAGS) != 0);
            p += reclen;
            if (is_dot(name)) continue;

            // EF_NO_XATTRS comes back for directories too, and it costs nothing to keep it even
            // when the struct stat below is an fstatat's.
            uint8_t xattr = FS_XATTR_UNKNOWN;
            if (u.ok && (ret.forkattr & ATTR_CMNEXT_EXT_FLAGS))
                xattr = (ext & EF_NO_XATTRS) ? FS_XATTR_NONE : FS_XATTR_SOME;

            bool whole = u.ok && (ret.commonattr & kBulkCommonNeeded) == kBulkCommonNeeded &&
                         (mode & S_IFMT) != 0;
            // OBJTYPE is the cross-check: if the two disagree, trust neither.
            if (whole && ((ot == VDIR) != S_ISDIR((mode_t)mode) || (ot == VLNK) != S_ISLNK((mode_t)mode)))
                whole = false;
            bool dir = whole && S_ISDIR((mode_t)mode);
            if (whole && !dir && (ret.fileattr & kBulkFile) != kBulkFile) whole = false;

            struct stat st;
            if (!whole || dir) {
                if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return -errno;
            } else {
                fill_stat(st, dv, mode, uid, gid, nlink, ino, (int64_t)len, (int64_t)alloc, flags,
                          bt, mt, ct, at);
            }
            reported = true;
            if (int rc = fn(ctx, name, ::strlen(name), st, xattr)) {
                if (cb_rc) *cb_rc = rc;
                return 0;
            }
        }
    }
}

int fs_lstat_xattr(const char *path, struct stat &st, uint8_t &xattr) {
    xattr = FS_XATTR_UNKNOWN;
    if (!path) return -EINVAL;
    // One getattrlist(2) where the old code had one lstat(2): 1.78 µs against 1.41 µs, and it
    // brings EF_NO_XATTRS with it, which is worth far more than the 0.37 µs (a listxattr the
    // diff then does not have to make is 2.1 µs, and the getxattr behind it 14 µs).
    struct attrlist al;
    memset(&al, 0, sizeof al);
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = kBulkCommon & ~(uint32_t)ATTR_CMN_NAME;
    al.fileattr = kBulkFile;
    al.forkattr = kBulkFork;
    char buf[512];
    bool whole = false;
    uint32_t mode = 0;
    if (::getattrlist(path, &al, buf, sizeof buf,
                      FSOPT_NOFOLLOW | FSOPT_ATTR_CMN_EXTENDED) == 0) {   // see fs_bulk_dir
        uint32_t reclen = 0;
        memcpy(&reclen, buf, sizeof reclen);
        if (reclen >= sizeof(uint32_t) + sizeof(attribute_set_t) && reclen <= sizeof buf) {
            Unpack u{buf + sizeof(uint32_t), buf + reclen};
            attribute_set_t ret;
            memset(&ret, 0, sizeof ret);
            u.take(ret, true);
            dev_t dv = 0;
            fsobj_type_t ot = 0;
            struct timespec bt = {0, 0}, mt = {0, 0}, ct = {0, 0}, at = {0, 0};
            uid_t uid = 0;
            gid_t gid = 0;
            uint32_t flags = 0, nlink = 0;
            uint64_t ino = 0, ext = 0;
            off_t alloc = 0, len = 0;
            u.take(dv, (ret.commonattr & ATTR_CMN_DEVID) != 0);
            u.take(ot, (ret.commonattr & ATTR_CMN_OBJTYPE) != 0);
            u.take(bt, (ret.commonattr & ATTR_CMN_CRTIME) != 0);
            u.take(mt, (ret.commonattr & ATTR_CMN_MODTIME) != 0);
            u.take(ct, (ret.commonattr & ATTR_CMN_CHGTIME) != 0);
            u.take(at, (ret.commonattr & ATTR_CMN_ACCTIME) != 0);
            u.take(uid, (ret.commonattr & ATTR_CMN_OWNERID) != 0);
            u.take(gid, (ret.commonattr & ATTR_CMN_GRPID) != 0);
            u.take(mode, (ret.commonattr & ATTR_CMN_ACCESSMASK) != 0);
            u.take(flags, (ret.commonattr & ATTR_CMN_FLAGS) != 0);
            u.take(ino, (ret.commonattr & ATTR_CMN_FILEID) != 0);
            u.take(nlink, (ret.fileattr & ATTR_FILE_LINKCOUNT) != 0);
            u.take(alloc, (ret.fileattr & ATTR_FILE_ALLOCSIZE) != 0);
            u.take(len, (ret.fileattr & ATTR_FILE_DATALENGTH) != 0);
            u.take(ext, (ret.forkattr & ATTR_CMNEXT_EXT_FLAGS) != 0);
            if (u.ok && (ret.forkattr & ATTR_CMNEXT_EXT_FLAGS))
                xattr = (ext & EF_NO_XATTRS) ? FS_XATTR_NONE : FS_XATTR_SOME;
            whole = u.ok && (ret.commonattr & (kBulkCommonNeeded & ~(uint32_t)ATTR_CMN_NAME)) ==
                                (kBulkCommonNeeded & ~(uint32_t)ATTR_CMN_NAME) &&
                    (mode & S_IFMT) != 0 && (ot == VDIR) == S_ISDIR((mode_t)mode) &&
                    (ot == VLNK) == S_ISLNK((mode_t)mode);
            if (whole && !S_ISDIR((mode_t)mode) && (ret.fileattr & kBulkFile) != kBulkFile)
                whole = false;
            if (whole && !S_ISDIR((mode_t)mode))
                fill_stat(st, dv, mode, uid, gid, nlink, ino, (int64_t)len, (int64_t)alloc, flags,
                          bt, mt, ct, at);
            else
                whole = false;   // a directory: same reason as in fs_bulk_dir
        }
    }
    if (whole) return 0;
    return ::lstat(path, &st) == 0 ? 0 : -errno;
}

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

int unprotect_entry(void *ctx, const char *path, const char *, const struct stat &st, bool is_dir) {
    // PR #1 review (4th round): the deadline of whoever asked for the walk, read per entry.
    // fs_remove_tree's fallback runs this on trees of millions of entries.
    if (int64_t dl = *(const int64_t *)ctx) { if (fs_mono_us() >= dl) return -ECANCELED; }
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

int fs_unprotect_tree(const char *root, int64_t deadline_us) {
    // Directories first: their own flags have to go before anything else about them changes.
    return fs_walk_tree(root, 4, FS_DIRS_PRE, &deadline_us, unprotect_entry);
}

} // namespace wfs
