// `world fs diff W<n>` (T1.3): what changed in a world since it was forked.
//
// The shape of it (docs/M1_DESIGN.md §1 diff row, P10; output format arch.md §25):
//
//   1. candidates   FSEvents replay from the fork cursor, in events.h/platform_darwin_events.cpp.
//                   O(changes). Anything it cannot vouch for becomes a full scan.
//   2. verification every candidate is stat'ed on BOTH sides and, when that is not conclusive,
//                   its bytes are read. The event stream is a hint, never the answer: a world
//                   is plain APFS and nothing forces a writer to go through us.
//   3. full scan    `--full`, or any of the fallbacks: two parallel walks (4 threads, the
//                   walker from platform_posix.cpp) doing the same per-path comparison.
//
// T2.4: both the walk and the per-path lookup on the other side come from getattrlistbulk(2) /
// getattrlist(2) now, so every entry arrives with its EF_NO_XATTRS verdict already in hand and
// the xattr leg of the comparison is skipped entirely for the files that have no xattrs -- which
// is most of them. See platform_darwin.cpp for the attribute list and what each call costs.
//
// Reading the snapshot side goes through snapshot_open_for_read() so that there is exactly one
// place to teach about gated snapshot roots.
//
// What is reported: files only. A directory appears only when it is empty and exists on one
// side. A rename is a D and an A in M1 -- FSEvents does report ItemRenamed with both paths, but
// pairing them needs the inode on both sides and a snapshot's inodes were replaced by the
// clone, so the honest answer for now is two lines.
#include "worldfs/worldfs.h"

#include "events.h"
#include "internal.h"
#include "snapshot_access.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/xattr.h>
#endif

using wfs::Guard;
using wfs::Mutex;
using wfs::String;
using wfs::Vec;

namespace {

const size_t kCmpChunk = 128 * 1024;

void bump(uint64_t &v, uint64_t by = 1) { __atomic_fetch_add(&v, by, __ATOMIC_RELAXED); }

int64_t now_us(void) {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

wfs_type type_of(mode_t m) {
    if (S_ISREG(m)) return WFS_T_FILE;
    if (S_ISDIR(m)) return WFS_T_DIR;
    if (S_ISLNK(m)) return WFS_T_SYMLINK;
    if (S_ISFIFO(m)) return WFS_T_FIFO;
    if (S_ISCHR(m)) return WFS_T_CHR;
    if (S_ISBLK(m)) return WFS_T_BLK;
    return WFS_T_SOCK;
}

void mtime_of(const struct stat &st, int64_t *sec, long *nsec) {
#ifdef __APPLE__
    *sec = (int64_t)st.st_mtimespec.tv_sec;
    *nsec = (long)st.st_mtimespec.tv_nsec;
#else
    *sec = (int64_t)st.st_mtim.tv_sec;
    *nsec = (long)st.st_mtim.tv_nsec;
#endif
}

// A fork unprotects its clone, so UF_IMMUTABLE and UF_APPEND differ between every snapshot
// entry and its world copy by construction. They are the protection, not a user change.
uint32_t user_flags(const struct stat &st) {
#ifdef __APPLE__
    return (uint32_t)st.st_flags & ~(uint32_t)(UF_IMMUTABLE | UF_APPEND | SF_IMMUTABLE | SF_APPEND);
#else
    (void)st;
    return 0;
#endif
}

void join_rel(String &out, const char *root, const char *rel) {
    out.assign(root);
    if (rel && *rel) {
        out.append("/");
        out.append(rel);
    }
}

bool dir_is_empty(const char *p) {
    DIR *d = ::opendir(p);
    if (!d) return false;
    bool empty = true;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;
        empty = false;
        break;
    }
    ::closedir(d);
    return empty;
}

bool link_target_equal(const char *a, const char *b) {
    char ba[PATH_MAX], bb[PATH_MAX];
    ssize_t na = ::readlink(a, ba, sizeof ba);
    ssize_t nb = ::readlink(b, bb, sizeof bb);
    if (na < 0 || nb < 0) return false;
    return na == nb && memcmp(ba, bb, (size_t)na) == 0;
}

// The files are clones, so an unchanged one shares its extents with the snapshot's copy -- but
// user space cannot see that, and there is no "are these the same extents" call. So when size
// and mtime disagree about whether anything happened, read both.
bool content_equal(const char *a, const char *b, uint64_t *bytes) {
    int fa = ::open(a, O_RDONLY | O_CLOEXEC);
    if (fa < 0) return false;
    int fb = ::open(b, O_RDONLY | O_CLOEXEC);
    if (fb < 0) {
        ::close(fa);
        return false;
    }
    char *buf = (char *)::malloc(kCmpChunk * 2);
    bool same = buf != nullptr;
    while (same) {
        ssize_t na = ::read(fa, buf, kCmpChunk);
        ssize_t nb = ::read(fb, buf + kCmpChunk, kCmpChunk);
        if (na < 0 || nb < 0 || na != nb) { same = false; break; }
        if (na == 0) break;
        bump(*bytes, (uint64_t)na);
        if (memcmp(buf, buf + kCmpChunk, (size_t)na) != 0) { same = false; break; }
    }
    ::free(buf);
    ::close(fa);
    ::close(fb);
    return same;
}

// PR #1 review (P2): a listxattr(2) or getxattr(2) that *fails* says nothing whatever about the
// attributes, and the old code read every negative return as "no attributes". Two files whose
// xattrs could not be read then came out equal -- silently, and exactly in the cases where
// silence is worst: EACCES (an ACL denying readextattr), EIO, an ERANGE retry that could not
// allocate. Only a successful zero-length listxattr means "none"; every failure is a third
// answer, and the caller turns it into a `T` and a counter, or into the diff's error.
enum XattrCmp { XA_EQUAL = 0, XA_DIFFER = 1, XA_ERROR = 2 };

// On XA_ERROR, which side could not be read and why. `a` is the world side, `b` the snapshot
// side -- classify() cares about the difference: a snapshot is ours and is read under the gate,
// so EACCES there is a broken store and not news about the workspace.
struct XattrErr {
    int a = 0, b = 0;
};

#ifdef __APPLE__
// An errno that is never 0, whatever the libc left behind.
int errno_or(int fallback) { return errno ? errno : fallback; }

// The slow path: a value too big for the stack buffer below, so its size has to be asked for
// first. Two getxattr(2) per side instead of one, and getxattr is 14 µs on APFS.
XattrCmp xattr_value_equal(const char *a, const char *b, const char *name, XattrErr &e) {
    errno = 0;
    ssize_t sa = ::getxattr(a, name, nullptr, 0, 0, XATTR_NOFOLLOW);
    int ea = sa < 0 ? errno_or(EIO) : 0;
    errno = 0;
    ssize_t sb = ::getxattr(b, name, nullptr, 0, 0, XATTR_NOFOLLOW);
    int eb = sb < 0 ? errno_or(EIO) : 0;
    // ENOATTR is an answer, not a failure: the name listxattr(2) handed us was removed in
    // between, so that side genuinely does not have it any more. Anything else is a failure.
    if (ea && ea != ENOATTR) { e.a = ea; return XA_ERROR; }
    if (eb && eb != ENOATTR) { e.b = eb; return XA_ERROR; }
    if ((ea != 0) != (eb != 0)) return XA_DIFFER;
    if (ea && eb) return XA_EQUAL;   // gone from both
    if (sa != sb) return XA_DIFFER;
    if (sa == 0) return XA_EQUAL;
    char *va = (char *)::malloc((size_t)sa * 2);
    if (!va) { e.a = ENOMEM; return XA_ERROR; }   // cannot tell: and "cannot tell" is not "equal"
    char *vb = va + sa;
    errno = 0;
    ssize_t ga = ::getxattr(a, name, va, (size_t)sa, 0, XATTR_NOFOLLOW);
    int ra = ga < 0 ? errno_or(EIO) : 0;
    errno = 0;
    ssize_t gb = ::getxattr(b, name, vb, (size_t)sa, 0, XATTR_NOFOLLOW);
    int rb = gb < 0 ? errno_or(EIO) : 0;
    XattrCmp rc;
    if (ra) { e.a = ra; rc = XA_ERROR; }
    else if (rb) { e.b = rb; rc = XA_ERROR; }
    else if (ga != sa || gb != sa || memcmp(va, vb, (size_t)sa) != 0) rc = XA_DIFFER;
    else rc = XA_EQUAL;
    ::free(va);
    return rc;
}

// T2.4: ask for the value straight away rather than for its size and then its value. Almost
// every xattr in the wild fits (com.apple.provenance is 11 bytes, FinderInfo 32, quarantine
// ~70), and it halves the getxattr count: 55 µs per pair measured before, 35 µs after.
const size_t kXattrInline = 1024;

// The values behind a list of names (NUL-separated, `n` bytes), on both sides.
XattrCmp xattr_values_equal(const char *a, const char *b, const char *names, size_t n, XattrErr &e) {
    for (size_t i = 0; i < n;) {
        const char *name = names + i;
        size_t len = ::strnlen(name, n - i);
        if (len == 0 || i + len >= n) break;
        char va[kXattrInline], vb[kXattrInline];
        ssize_t sa = ::getxattr(a, name, va, sizeof va, 0, XATTR_NOFOLLOW);
        ssize_t sb = ::getxattr(b, name, vb, sizeof vb, 0, XATTR_NOFOLLOW);
        if (sa < 0 || sb < 0) {
            // ERANGE (a value over kXattrInline), the attribute going away between the
            // listxattr and now, or a read that simply failed. Ask the careful way, which is
            // the only one that tells the three apart.
            XattrCmp c = xattr_value_equal(a, b, name, e);
            if (c != XA_EQUAL) return c;
        } else if (sa != sb || memcmp(va, vb, (size_t)sa) != 0) {
            return XA_DIFFER;
        }
        i += len + 1;
    }
    return XA_EQUAL;
}

// listxattr(2) for one side, onto the stack when it fits and onto the heap when it does not.
// Returns the byte count with *err == 0, or -1 with *err set -- so "empty" and "could not be
// read" can never be confused, which is the whole point of this round of the review.
ssize_t list_names(const char *path, char *stackbuf, size_t cap, char **heap, char **out, int *err) {
    *heap = nullptr;
    *err = 0;
    errno = 0;
    ssize_t n = ::listxattr(path, stackbuf, cap, XATTR_NOFOLLOW);
    if (n >= 0) { *out = stackbuf; return n; }
    if (errno != ERANGE) { *err = errno_or(EIO); return -1; }
    errno = 0;
    ssize_t need = ::listxattr(path, nullptr, 0, XATTR_NOFOLLOW);
    if (need < 0) { *err = errno_or(EIO); return -1; }
    if (need == 0) { *out = stackbuf; return 0; }
    char *h = (char *)::malloc((size_t)need);
    if (!h) { *err = ENOMEM; return -1; }
    errno = 0;
    n = ::listxattr(path, h, (size_t)need, XATTR_NOFOLLOW);
    if (n < 0) { ::free(h); *err = errno_or(EIO); return -1; }
    *heap = h;
    *out = h;
    return n;
}

// Every name both sides have, compared. The fallback for a file with more than kXattrNames
// bytes of names, where the filtered-name path below cannot hold them.
XattrCmp xattr_equal_raw(const char *a, const char *b, XattrErr &e) {
    char sa[4096], sb[4096], *ha = nullptr, *hb = nullptr, *la = nullptr, *lb = nullptr;
    int ea = 0, eb = 0;
    ssize_t na = list_names(a, sa, sizeof sa, &ha, &la, &ea);
    ssize_t nb = list_names(b, sb, sizeof sb, &hb, &lb, &eb);
    XattrCmp rc;
    if (na < 0) { e.a = ea; rc = XA_ERROR; }
    else if (nb < 0) { e.b = eb; rc = XA_ERROR; }
    else if (na != nb) rc = XA_DIFFER;
    else if (na == 0) rc = XA_EQUAL;
    else if (memcmp(la, lb, (size_t)na) != 0) rc = XA_DIFFER; // a clone keeps the order
    else rc = xattr_values_equal(a, b, la, (size_t)na, e);
    ::free(ha);
    ::free(hb);
    return rc;
}

// M2: `com.apple.provenance` is not workspace state. macOS 27 stamps it on every file a local
// process creates, and it cannot be taken off again (removexattr fails; `xattr -d` silently
// does nothing) -- it is the kernel's record of *which application created this file*, which
// a fork's clone inherits and an agent never sets. Comparing it can therefore only ever
// confirm what is never news, and it costs two listxattr(2) plus two getxattr(2) per
// otherwise-identical file to do it. It is left out of the comparison by default;
// `diff --all-xattrs` puts it back. `com.apple.quarantine` and everything else stay compared:
// those are things that happen to a workspace, not to the kernel's bookkeeping.
const char kProvenance[] = "com.apple.provenance";

bool xattr_ignored(const char *name, size_t len, int flags) {
    if (flags & WFS_DIFF_ALL_XATTRS) return false;
    return len == sizeof kProvenance - 1 && memcmp(name, kProvenance, len) == 0;
}

const size_t kXattrNames = 4096;

// xattr_names() return codes, so that the three outcomes stay distinguishable all the way up.
const ssize_t kNamesTooMany = -1;   // more names than `out` holds: fall back to xattr_equal_raw
const ssize_t kNamesFailed = -2;    // listxattr(2) failed: NOT "no attributes"

// One side's names, in the order listxattr(2) reports them, with the ignored ones dropped. A
// side the walk already declared EF_NO_XATTRS is not asked at all -- that is the free half of
// the shortcut, and it stays, because EF_NO_XATTRS is the file system *succeeding* at saying
// "none at all". A listxattr that fails is the opposite of that, and returns kNamesFailed.
ssize_t xattr_names(const char *path, uint8_t state, int flags, char *out, size_t cap, int *err) {
    *err = 0;
    if (state == wfs::FS_XATTR_NONE) return 0;
    char stackbuf[4096], *heap = nullptr, *raw = nullptr;
    ssize_t n = list_names(path, stackbuf, sizeof stackbuf, &heap, &raw, err);
    if (n < 0) return kNamesFailed;
    if (n == 0) { ::free(heap); return 0; } // a real, successful "this file has no attributes"
    ssize_t used = 0;
    for (ssize_t i = 0; i < n;) {
        const char *name = raw + i;
        size_t len = ::strnlen(name, (size_t)(n - i));
        if (len == 0 || (ssize_t)(i + len) >= n) break;
        if (!xattr_ignored(name, len, flags)) {
            if ((size_t)used + len + 1 > cap) {
                ::free(heap);
                return kNamesTooMany;
            }
            memcpy(out + used, name, len + 1);
            used += (ssize_t)len + 1;
        }
        i += (ssize_t)len + 1;
    }
    ::free(heap);
    return used;
}

// The xattr leg of the comparison, *and* the decision whether to make it at all.
//
// T2.4 gave the walk a free verdict per entry (ATTR_CMNEXT_EXT_FLAGS / EF_NO_XATTRS): when both
// sides say "none at all", there is nothing to list and nothing to compare, and not one syscall
// is made. EF_NO_XATTRS only ever denies, so that shortcut can skip work but never a difference
// -- and, unlike a failed listxattr, it is an answer the file system gave on purpose, which is
// why the PR #1 review's rule leaves it standing. The moment only *one* side has the bit, the
// other side is listed for real, and a failure there is an error, not an empty list.
//
// M2 adds the other half. The flag is never set on a file this machine created (provenance is
// always there), so on such a tree the shortcut never fired and the whole default scan paid for
// it. Now, when the flag is not set, the names are listed and the ignored ones dropped first: a
// file whose only xattr is provenance comes back with an empty list and counts as xattr-free,
// exactly as if the file system had set the flag. Two listxattr(2) at 2.1 µs is what that costs;
// the four getxattr(2) at 14 µs that used to follow are gone.
XattrCmp xattr_equal(const char *a, const char *b, uint8_t axa, uint8_t bxa, int flags,
                     XattrErr &e) {
    if (axa == wfs::FS_XATTR_NONE && bxa == wfs::FS_XATTR_NONE) return XA_EQUAL;
    char na[kXattrNames], nb[kXattrNames];
    int ea = 0, eb = 0;
    ssize_t la = xattr_names(a, axa, flags, na, sizeof na, &ea);
    ssize_t lb = xattr_names(b, bxa, flags, nb, sizeof nb, &eb);
    if (la == kNamesFailed) { e.a = ea; return XA_ERROR; }
    if (lb == kNamesFailed) { e.b = eb; return XA_ERROR; }
    if (la < 0 || lb < 0) return xattr_equal_raw(a, b, e);  // more names than kXattrNames holds
    if (la == 0 && lb == 0) return XA_EQUAL;                // nothing, or only ignored names
    if (la != lb || memcmp(na, nb, (size_t)la) != 0) return XA_DIFFER; // a clone keeps the order
    return xattr_values_equal(a, b, na, (size_t)la, e);
}
#else
XattrCmp xattr_equal(const char *, const char *, uint8_t, uint8_t, int, XattrErr &) {
    return XA_EQUAL;
}
#endif

// ---- the record sink -------------------------------------------------------------------------
// Paths go into one growing char buffer; the slots keep offsets, so growing never moves a
// record and the final sort is a sort of pointers.

struct Slot {
    uint32_t off;
    uint8_t change;
    uint8_t type;
    uint64_t size;
};

struct Rec {
    const char *path;
    uint8_t change;
    uint8_t type;
    uint64_t size;
};

struct Sink {
    Mutex mu;
    Vec<char> buf;
    Vec<Slot> slots;
    void add(int change, wfs_type t, uint64_t size, const char *rel) {
        Guard g(mu);
        size_t n = ::strlen(rel);
        size_t at = buf.size();
        buf.resize(at + n + 1);
        if (n) memcpy(buf.data() + at, rel, n);
        buf.data()[at + n] = 0;
        Slot s;
        s.off = (uint32_t)at;
        s.change = (uint8_t)change;
        s.type = (uint8_t)t;
        s.size = size;
        slots.emplace_back(s);
    }
};

int rec_cmp(const void *a, const void *b) {
    return ::strcmp(((const Rec *)a)->path, ((const Rec *)b)->path);
}

// ---- comparison ------------------------------------------------------------------------------

struct Ctx {
    String wroot, sroot;
    int flags = 0;
    Sink *sink = nullptr;
    uint64_t compared = 0, content_cmp = 0, bytes_read = 0;
    // PR #1 review (P2): entries whose xattrs could not be read on one side. They are reported
    // as `T` -- never as equal -- and counted here so the caller can say the comparison was
    // incomplete rather than pretend it was clean.
    uint64_t xattr_errors = 0;
    int fatal = 0;   // the first error that must end the whole diff (see note_fatal)
};

// A failure that is not about the workspace but about us. Recorded once, by whichever of the
// four walk threads gets there first, and returned instead of a diff.
void note_fatal(Ctx &c, int rc) {
    int none = 0;
    __atomic_compare_exchange_n(&c.fatal, &none, rc, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// The other side of a path. Normally one getattrlist(2), which brings the xattr verdict with
// it; when that verdict cannot be used -- WFS_DIFF_NO_XATTR, or a directory, whose xattrs are
// never compared (a directory present on both sides says nothing, see classify) -- it is a plain
// lstat(2) instead, which is 1.41 µs against 1.78 and, for a directory, one syscall against two.
int other_side(const Ctx &c, const char *path, struct stat &st, uint8_t &xa, bool want_xattr) {
    if (!want_xattr || (c.flags & WFS_DIFF_NO_XATTR)) {
        xa = wfs::FS_XATTR_UNKNOWN;
        return ::lstat(path, &st) == 0 ? 0 : -errno;
    }
    return wfs::fs_lstat_xattr(path, st, xa);
}

// Both sides exist. Returns 0 (identical), 'M' or 'T'. `wxa`/`sxa` are the two sides'
// fs_xattr_state as the walk (or fs_lstat_xattr) already knows them.
int classify(Ctx &c, const char *wpath, const struct stat &ws, uint8_t wxa, const char *spath,
             const struct stat &ss, uint8_t sxa) {
    bump(c.compared);
    if ((ws.st_mode & S_IFMT) != (ss.st_mode & S_IFMT)) return WFS_C_MODIFIED;

    if (S_ISREG(ws.st_mode)) {
        if ((uint64_t)ws.st_size != (uint64_t)ss.st_size) return WFS_C_MODIFIED;
        int64_t wsec, ssec;
        long wns, sns;
        mtime_of(ws, &wsec, &wns);
        mtime_of(ss, &ssec, &sns);
        if (wsec != ssec || wns != sns) {
            if (c.flags & WFS_DIFF_NO_CONTENT) return WFS_C_MODIFIED;
            bump(c.content_cmp);
            if (!content_equal(wpath, spath, &c.bytes_read)) return WFS_C_MODIFIED;
        }
    } else if (S_ISLNK(ws.st_mode)) {
        if (!link_target_equal(wpath, spath)) return WFS_C_MODIFIED;
    } else if (S_ISDIR(ws.st_mode)) {
        return 0; // a directory present in both says nothing; its entries do
    }

    // Same bytes. Anything else that differs is metadata-only.
    if ((ws.st_mode & 07777) != (ss.st_mode & 07777)) return WFS_C_META;
    if (ws.st_uid != ss.st_uid || ws.st_gid != ss.st_gid) return WFS_C_META;
    if (user_flags(ws) != user_flags(ss)) return WFS_C_META;
    {
        int64_t wsec, ssec;
        long wns, sns;
        mtime_of(ws, &wsec, &wns);
        mtime_of(ss, &ssec, &sns);
        if (wsec != ssec || wns != sns) return WFS_C_META;
    }
    if (!(c.flags & WFS_DIFF_NO_XATTR)) {
        XattrErr xe;
        XattrCmp x = xattr_equal(wpath, spath, wxa, sxa, c.flags, xe);
        if (x == XA_ERROR) {
            // Never "equal": the attributes were not compared, so the entry is reported as the
            // metadata change it may well be, and counted.
            bump(c.xattr_errors);
            // ... except on the snapshot side. That tree is ours, it was cloned by us, and it
            // is read inside the SnapGate window with the gate open, so a permission failure
            // there is a broken store and not news about the world. Say so, loudly.
            if (xe.b == EACCES || xe.b == EPERM) note_fatal(c, -xe.b);
            return WFS_C_META;
        }
        if (x == XA_DIFFER) return WFS_C_META;
    }
    return 0;
}

// A path that exists on one side only and is a directory: the files under it are the change.
// An empty directory has no files to speak for it, so it is reported itself.
struct ExpandCtx {
    Ctx *c;
    const char *prefix; // relative path of the expansion root inside the world
    int change;
    bool skip_self;     // the root of the expansion is already being reported as something else
};

int expand_entry(void *ctx, const wfs::FsEntry &en) {
    ExpandCtx *e = (ExpandCtx *)ctx;
    if (!*en.rel && e->skip_self) return 0;
    String full(e->prefix);
    if (*en.rel) {
        full.append("/");
        full.append(en.rel);
    }
    if (en.is_dir) {
        if (dir_is_empty(en.path)) e->c->sink->add(e->change, WFS_T_DIR, 0, full.c_str());
        return 0;
    }
    e->c->sink->add(e->change, type_of(en.st->st_mode), (uint64_t)en.st->st_size, full.c_str());
    return 0;
}

// Called only from the single-threaded candidate loop, so the four workers below are all there
// are; Sink::add is behind a mutex either way.
void expand_side(Ctx &c, const char *rel, const char *path, int change, bool skip_self) {
    ExpandCtx e{&c, rel, change, skip_self};
    wfs::fs_walk_tree_ex(path, 4, wfs::FS_DIRS_PRE, &e, expand_entry);
}

void report_one_side(Ctx &c, const char *rel, const char *path, const struct stat &st, int change) {
    if (!S_ISDIR(st.st_mode)) {
        c.sink->add(change, type_of(st.st_mode), (uint64_t)st.st_size, rel);
        return;
    }
    expand_side(c, rel, path, change, false);
}

// ---- full scan ---------------------------------------------------------------------------------

struct SideCtx {
    Ctx *c;
    bool world_side;
};

int side_entry(void *ctx, const wfs::FsEntry &en) {
    SideCtx *s = (SideCtx *)ctx;
    Ctx &c = *s->c;
    const char *rel = en.rel;
    if (!*rel) return 0; // the root itself
    if (s->world_side && !::strcmp(rel, WFS_MARKER_NAME)) return 0;

    String other;
    join_rel(other, s->world_side ? c.sroot.c_str() : c.wroot.c_str(), rel);
    struct stat os;
    uint8_t oxa = wfs::FS_XATTR_UNKNOWN;
    if (other_side(c, other.c_str(), os, oxa, !en.is_dir) != 0) {
        // Only on this side. The walk visits every descendant itself, so a non-empty directory
        // needs no expansion here -- only an empty one has nothing else to report it.
        if (en.is_dir) {
            if (dir_is_empty(en.path)) c.sink->add(s->world_side ? WFS_C_ADDED : WFS_C_DELETED, WFS_T_DIR, 0, rel);
        } else {
            c.sink->add(s->world_side ? WFS_C_ADDED : WFS_C_DELETED, type_of(en.st->st_mode),
                        (uint64_t)en.st->st_size, rel);
        }
        return 0;
    }
    if (!s->world_side) return 0; // present on both: the world-side pass already compared it
    if (en.is_dir && S_ISDIR(os.st_mode)) return 0;
    int ch = classify(c, en.path, *en.st, en.xattr, other.c_str(), os, oxa);
    if (ch) c.sink->add(ch, type_of(en.st->st_mode), (uint64_t)en.st->st_size, rel);
    return 0;
}

int full_scan(Ctx &c) {
    SideCtx w{&c, true};
    if (int rc = wfs::fs_walk_tree_ex(c.wroot.c_str(), 4, wfs::FS_DIRS_PRE, &w, side_entry)) return rc;
    SideCtx s{&c, false};
    return wfs::fs_walk_tree_ex(c.sroot.c_str(), 4, wfs::FS_DIRS_PRE, &s, side_entry);
}

// ---- candidate verification ---------------------------------------------------------------------

void verify_candidate(Ctx &c, const char *rel) {
    if (!*rel || !::strcmp(rel, WFS_MARKER_NAME)) return;
    String wpath, spath;
    join_rel(wpath, c.wroot.c_str(), rel);
    join_rel(spath, c.sroot.c_str(), rel);
    struct stat ws, ss;
    uint8_t wxa = wfs::FS_XATTR_UNKNOWN, sxa = wfs::FS_XATTR_UNKNOWN;
    bool in_world = other_side(c, wpath.c_str(), ws, wxa, true) == 0;
    bool in_snap = other_side(c, spath.c_str(), ss, sxa, true) == 0;
    if (!in_world && !in_snap) return; // created and removed again inside the same world
    if (in_world && !in_snap) {
        report_one_side(c, rel, wpath.c_str(), ws, WFS_C_ADDED);
        return;
    }
    if (!in_world && in_snap) {
        report_one_side(c, rel, spath.c_str(), ss, WFS_C_DELETED);
        return;
    }
    if (S_ISDIR(ws.st_mode) && S_ISDIR(ss.st_mode)) return;
    // A directory replaced by a file (or the other way round): the path itself is an M, but
    // everything that used to live under the directory is gone, or newly here, and FSEvents
    // says nothing about it (a subtree moved in or out produces one event, for the directory).
    if (S_ISDIR(ws.st_mode)) expand_side(c, rel, wpath.c_str(), WFS_C_ADDED, true);
    else if (S_ISDIR(ss.st_mode)) expand_side(c, rel, spath.c_str(), WFS_C_DELETED, true);
    int ch = classify(c, wpath.c_str(), ws, wxa, spath.c_str(), ss, sxa);
    if (ch) c.sink->add(ch, type_of(ws.st_mode), (uint64_t)ws.st_size, rel);
}

// How many recorded entries a world needs before the FSEvents path is worth its fixed cost.
// WFS_DIFF_EVENTS_MIN_ENTRIES by default; the environment variable of the same name overrides
// it (that is how the tests exercise both paths on a 10k fixture). Garbage is ignored.
uint64_t events_min_entries() {
    const char *e = ::getenv("WFS_DIFF_EVENTS_MIN_ENTRIES");
    if (!e || !*e) return WFS_DIFF_EVENTS_MIN_ENTRIES;
    char *end = nullptr;
    unsigned long long v = ::strtoull(e, &end, 10);
    if (end == e || (end && *end)) return WFS_DIFF_EVENTS_MIN_ENTRIES;
    return (uint64_t)v;
}

int fallback_for(int ev_status) {
    switch (ev_status) {
    case wfs::FS_EV_MUST_SCAN: return WFS_DF_MUST_SCAN;
    case wfs::FS_EV_DROPPED: return WFS_DF_DROPPED;
    case wfs::FS_EV_WRAPPED: return WFS_DF_WRAPPED;
    case wfs::FS_EV_STALE: return WFS_DF_STALE;
    case wfs::FS_EV_TIMEOUT: return WFS_DF_TIMEOUT;
    default: return WFS_DF_UNSUPPORTED;
    }
}

} // namespace

extern "C" int wfs_world_diff_ex(wfs_store *s, wfs_id world, int flags, wfs_diff_cb cb, void *ctx,
                                 wfs_diff_stats *stats) {
    if (!s || !world) return -EINVAL;
    int64_t t0 = now_us();
    if (stats) memset(stats, 0, sizeof *stats);

    wfs_world_rec wr;
    if (int rc = wfs_world_info(s, world, &wr)) return rc;
    if (wr.state == WFS_ST_TRASHED || wr.state == WFS_ST_DEAD) return -ESTALE;
    if (wr.state != WFS_ST_ACTIVE) return -ESTALE;

    // P1: follow the world if it merely moved; refuse if it is not where we can see it.
    wfs_identity ident;
    if (int rc = wfs_world_verify(s, world, &ident)) return rc;

    if (wr.snapshot_id == 0) return WFS_E_SOURCE_GONE;
    wfs_snapshot_rec sr;
    int rc = wfs_snapshot_info(s, wr.snapshot_id, &sr);
    if (rc == -ENOENT) return WFS_E_SOURCE_GONE;
    if (rc) return rc;
    if (sr.state != WFS_ST_ACTIVE || !sr.path[0]) return WFS_E_SOURCE_GONE;

    // The snapshot has to still be on disk to be compared against; the gate on it is opened
    // further down, around the comparison itself.
    {
        struct stat sst;
        if (::lstat(sr.path, &sst) != 0 || !S_ISDIR(sst.st_mode)) return WFS_E_SOURCE_GONE;
    }

    Ctx c;
    c.wroot.assign(ident.path[0] ? ident.path : wr.path);
    c.sroot.assign(sr.path);
    c.flags = flags;
    Sink sink;
    c.sink = &sink;

    int fallback = WFS_DF_NONE;
    uint64_t candidates = 0;
    bool full = (flags & WFS_DIFF_FULL) != 0;
    if (full) {
        fallback = WFS_DF_REQUESTED;
    } else if (!(flags & WFS_DIFF_EVENTS) && wr.entries < events_min_entries()) {
        // The default. See the header: on anything short of a very large tree the two-tree walk
        // is both cheaper and exact, and the events path has a fixed cost plus a journal-flush
        // race that can hide a change made moments ago.
        full = true;
        fallback = WFS_DF_SMALL_TREE;
    } else if (wr.origin != WFS_O_SNAPSHOT) {
        // The cursor was taken when THIS world forked, but the comparison is against the
        // snapshot its parent came from: everything the parent changed before the fork happened
        // before the cursor. Only a full scan can see those.
        full = true;
        fallback = WFS_DF_FROM_WORLD;
    } else if (wr.fsevents_id == 0) {
        full = true;
        fallback = WFS_DF_NO_CURSOR;
    }

    wfs::PathBag bag;
    if (!full) {
        // Gathering candidates reads the world and the FSEvents journal, never the snapshot, so
        // it happens outside the gate: building the stream and waiting for its watermark can
        // take hundreds of milliseconds and no fork should queue behind that.
#ifdef __APPLE__
        int ev = wfs::fs_events_replay(c.wroot.c_str(), wr.fsevents_id, wr.dir_dev, wr.created_at,
                                       wfs_store_dir(s), bag);
#else
        int ev = wfs::FS_EV_UNSUPPORTED;
#endif
        if (ev != wfs::FS_EV_OK) {
            full = true;
            fallback = fallback_for(ev);
        } else {
            bag.finish();
            candidates = bag.size();
        }
    }

    {
        // The comparison phase, and the only part that reads snapshot bytes. The gate holds an
        // exclusive flock on the snapshot's manifest, so every fork from this same snapshot
        // waits here (a full scan is 0.16-1.4 s on 50k entries, a candidate list is
        // milliseconds). It is opened as late as possible and the guard closes it -- restoring
        // mode 0000 -- on every path out, including the error returns below.
        wfs::SnapshotReadGuard gate(sr.path);
        if (gate.rc == -ENOENT || gate.rc == -ENOTDIR) return WFS_E_SOURCE_GONE;
        if (gate.rc) return gate.rc;

        if (!full)
            for (size_t i = 0; i < bag.size(); ++i) verify_candidate(c, bag.at(i));
        else if (int frc = full_scan(c))
            return frc;
        // PR #1 review (P2): an unreadable snapshot side is not a diff result. Both paths
        // above reach it, and the guard below closes the gate on the way out either way.
        if (c.fatal) return c.fatal;
    }
    // From here on nothing touches the snapshot: sorting and reporting are pure bookkeeping.

    // Sort, drop the duplicates an expanded directory can produce, then hand them over.
    Vec<Rec> recs;
    recs.reserve(sink.slots.size());
    for (size_t i = 0; i < sink.slots.size(); ++i) {
        Rec r;
        r.path = sink.buf.data() + sink.slots[i].off;
        r.change = sink.slots[i].change;
        r.type = sink.slots[i].type;
        r.size = sink.slots[i].size;
        recs.emplace_back(r);
    }
    if (recs.size() > 1) qsort(recs.data(), recs.size(), sizeof(Rec), rec_cmp);

    uint64_t counts[4] = {0, 0, 0, 0};
    int cbrc = 0;
    const char *prev = nullptr;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (prev && !::strcmp(prev, recs[i].path)) continue;
        prev = recs[i].path;
        switch (recs[i].change) {
        case WFS_C_ADDED: counts[0]++; break;
        case WFS_C_MODIFIED: counts[1]++; break;
        case WFS_C_DELETED: counts[2]++; break;
        default: counts[3]++; break;
        }
        if (cb) {
            wfs_diff_entry e;
            e.change = recs[i].change;
            e.type = (wfs_type)recs[i].type;
            e.size = recs[i].size;
            e.path = recs[i].path;
            // A non-zero return stops the walk, so the counters below stop with it.
            if ((cbrc = cb(ctx, &e)) != 0) break;
        }
    }

    if (stats) {
        stats->added = counts[0];
        stats->modified = counts[1];
        stats->deleted = counts[2];
        stats->meta = counts[3];
        stats->candidates = candidates;
        stats->compared = c.compared;
        stats->content_cmp = c.content_cmp;
        stats->bytes_read = c.bytes_read;
        stats->xattr_errors = c.xattr_errors;
        stats->events_id = wr.fsevents_id;
        stats->full_scan = full ? 1 : 0;
        stats->fallback = fallback;
        stats->elapsed_us = now_us() - t0;
    }
    return cbrc;
}

extern "C" int wfs_world_diff(wfs_store *s, wfs_id world, int flags, wfs_diff_cb cb, void *ctx) {
    return wfs_world_diff_ex(s, world, flags, cb, ctx, nullptr);
}
