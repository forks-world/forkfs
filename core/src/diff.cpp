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

#ifdef __APPLE__
bool xattr_value_equal(const char *a, const char *b, const char *name) {
    ssize_t sa = ::getxattr(a, name, nullptr, 0, 0, XATTR_NOFOLLOW);
    ssize_t sb = ::getxattr(b, name, nullptr, 0, 0, XATTR_NOFOLLOW);
    if (sa != sb) return false;
    if (sa <= 0) return true;
    char *va = (char *)::malloc((size_t)sa * 2);
    if (!va) return true; // cannot tell: do not invent a difference
    char *vb = va + sa;
    bool same = ::getxattr(a, name, va, (size_t)sa, 0, XATTR_NOFOLLOW) == sa &&
                ::getxattr(b, name, vb, (size_t)sa, 0, XATTR_NOFOLLOW) == sa &&
                memcmp(va, vb, (size_t)sa) == 0;
    ::free(va);
    return same;
}

bool xattr_equal(const char *a, const char *b) {
    char la[4096], lb[4096];
    ssize_t na = ::listxattr(a, la, sizeof la, XATTR_NOFOLLOW);
    ssize_t nb = ::listxattr(b, lb, sizeof lb, XATTR_NOFOLLOW);
    if (na < 0) na = 0;
    if (nb < 0) nb = 0;
    if (na != nb) return false;
    if (na == 0) return true;
    if (memcmp(la, lb, (size_t)na) != 0) return false; // a clone keeps the order
    for (ssize_t i = 0; i < na;) {
        const char *name = la + i;
        size_t len = ::strnlen(name, (size_t)(na - i));
        if (len == 0 || (ssize_t)(i + len) >= na) break;
        if (!xattr_value_equal(a, b, name)) return false;
        i += (ssize_t)len + 1;
    }
    return true;
}
#else
bool xattr_equal(const char *, const char *) { return true; }
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
};

// Both sides exist. Returns 0 (identical), 'M' or 'T'.
int classify(Ctx &c, const char *wpath, const struct stat &ws, const char *spath,
             const struct stat &ss) {
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
    if (!(c.flags & WFS_DIFF_NO_XATTR) && !xattr_equal(wpath, spath)) return WFS_C_META;
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

int expand_entry(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    ExpandCtx *e = (ExpandCtx *)ctx;
    if (!*rel && e->skip_self) return 0;
    String full(e->prefix);
    if (*rel) {
        full.append("/");
        full.append(rel);
    }
    if (is_dir) {
        if (dir_is_empty(path)) e->c->sink->add(e->change, WFS_T_DIR, 0, full.c_str());
        return 0;
    }
    e->c->sink->add(e->change, type_of(st.st_mode), (uint64_t)st.st_size, full.c_str());
    return 0;
}

// Called only from the single-threaded candidate loop, so the four workers below are all there
// are; Sink::add is behind a mutex either way.
void expand_side(Ctx &c, const char *rel, const char *path, int change, bool skip_self) {
    ExpandCtx e{&c, rel, change, skip_self};
    wfs::fs_walk_tree(path, 4, wfs::FS_DIRS_PRE, &e, expand_entry);
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

int side_entry(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    SideCtx *s = (SideCtx *)ctx;
    Ctx &c = *s->c;
    if (!*rel) return 0; // the root itself
    if (s->world_side && !::strcmp(rel, WFS_MARKER_NAME)) return 0;

    String other;
    join_rel(other, s->world_side ? c.sroot.c_str() : c.wroot.c_str(), rel);
    struct stat os;
    if (::lstat(other.c_str(), &os) != 0) {
        // Only on this side. The walk visits every descendant itself, so a non-empty directory
        // needs no expansion here -- only an empty one has nothing else to report it.
        if (is_dir) {
            if (dir_is_empty(path)) c.sink->add(s->world_side ? WFS_C_ADDED : WFS_C_DELETED, WFS_T_DIR, 0, rel);
        } else {
            c.sink->add(s->world_side ? WFS_C_ADDED : WFS_C_DELETED, type_of(st.st_mode),
                        (uint64_t)st.st_size, rel);
        }
        return 0;
    }
    if (!s->world_side) return 0; // present on both: the world-side pass already compared it
    if (is_dir && S_ISDIR(os.st_mode)) return 0;
    int ch = classify(c, path, st, other.c_str(), os);
    if (ch) c.sink->add(ch, type_of(st.st_mode), (uint64_t)st.st_size, rel);
    return 0;
}

int full_scan(Ctx &c) {
    SideCtx w{&c, true};
    if (int rc = wfs::fs_walk_tree(c.wroot.c_str(), 4, wfs::FS_DIRS_PRE, &w, side_entry)) return rc;
    SideCtx s{&c, false};
    return wfs::fs_walk_tree(c.sroot.c_str(), 4, wfs::FS_DIRS_PRE, &s, side_entry);
}

// ---- candidate verification ---------------------------------------------------------------------

void verify_candidate(Ctx &c, const char *rel) {
    if (!*rel || !::strcmp(rel, WFS_MARKER_NAME)) return;
    String wpath, spath;
    join_rel(wpath, c.wroot.c_str(), rel);
    join_rel(spath, c.sroot.c_str(), rel);
    struct stat ws, ss;
    bool in_world = ::lstat(wpath.c_str(), &ws) == 0;
    bool in_snap = ::lstat(spath.c_str(), &ss) == 0;
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
    int ch = classify(c, wpath.c_str(), ws, spath.c_str(), ss);
    if (ch) c.sink->add(ch, type_of(ws.st_mode), (uint64_t)ws.st_size, rel);
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

    // The only door into a snapshot's bytes; today a no-op, tomorrow the 0500 window.
    wfs::SnapshotReadGuard gate(sr.path);
    if (gate.rc == -ENOENT || gate.rc == -ENOTDIR) return WFS_E_SOURCE_GONE;
    if (gate.rc) return gate.rc;

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
            for (size_t i = 0; i < bag.size(); ++i) verify_candidate(c, bag.at(i));
        }
    }
    if (full) {
        if (int frc = full_scan(c)) return frc;
    }

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
