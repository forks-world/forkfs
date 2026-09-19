// The pre-clone pool (T1.5): fill, hand-out bookkeeping, status, drain, gc and verify.
//
// Why a table and not just a directory listing: the hand-out has to be atomic between
// processes. Claiming is "delete one row inside BEGIN IMMEDIATE", so two concurrent forks take
// two different entries and a crash between the claim and the rename leaves a directory with no
// row, which gc collects. The table also carries what the entry cost (entries) and what it
// looked like when it was made (root mtime), which is what `verify` checks.
#include "pool.h"

#include "db.h"
#include "hardlinks.h"
#include "snapshot_access.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

// The pool collector's own scan window (PR #1 review, 9th round): between the row snapshot
// pool_scan takes and the removals that act on it. A filler or a fork that lands in there
// produces a tree that snapshot has never heard of. Nothing in the library ever assigns these.
extern "C" void (*wfs_test_before_pool_sweep)(void *ctx) = nullptr;
extern "C" void *wfs_test_before_pool_sweep_ctx = nullptr;

// And the filler's own window (PR #1 review, 15th round): between the snapshot row wfs_pool_fill
// reads once and the transaction that inserts an entry's CREATING pool row. Nothing in the
// library ever assigns these either.
extern "C" void (*wfs_test_before_pool_insert)(void *ctx) = nullptr;
extern "C" void *wfs_test_before_pool_insert_ctx = nullptr;

namespace wfs {

namespace {

// P11 headroom, as in world.cpp: entries * 1 KiB is three times the measured 308 B/entry
// metadata cost of a clone, plus a flat floor.
const uint64_t kSpaceFloor = 256ull * 1024 * 1024;

String joinp(const char *a, const char *b) {
    String p(a);
    size_t n = p.size();
    if (n && p.c_str()[n - 1] != '/') p.append("/");
    p.append(b);
    return p;
}

bool exists(const char *p) {
    struct stat st;
    return ::lstat(p, &st) == 0;
}

// PR #1 review (12th round), the same rule world.cpp states: presence is assumed unless absence
// is proven. exists() reads an EACCES, an EIO or an unmounted volume as "not there", and every
// verdict below that deletes a row or stops keeping track of a tree used to be made on it.
bool proven_gone(const char *p) { return fs_gone(fs_probe(p)); }

String pool_root(wfs_store *s) { return joinp(s->dir.c_str(), "pool"); }

String pool_dir_of(wfs_store *s, wfs_id snapshot) {
    char leaf[64];
    ::snprintf(leaf, sizeof leaf, "S%llu", (unsigned long long)snapshot);
    return joinp(pool_root(s).c_str(), leaf);
}

void hex_uuid(char *out, size_t n) { // n >= 33
    unsigned char raw[16];
    if (::getentropy(raw, sizeof raw) != 0)
        for (size_t i = 0; i < sizeof raw; ++i)
            raw[i] = (unsigned char)(::getpid() + i * 31 + (int)now_sec());
    static const char h[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; i < sizeof raw && j + 2 < n; ++i) {
        out[j++] = h[raw[i] >> 4];
        out[j++] = h[raw[i] & 15];
    }
    out[j] = 0;
}

// Nanoseconds, not seconds: an entry can be handed out, or tampered with, in the same second
// it was cloned in, and `verify` has to be able to tell those apart.
int64_t mtime_ns(const struct stat &st) {
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000 + (int64_t)st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000 + (int64_t)st.st_mtim.tv_nsec;
#endif
}

// What the fill loop needs from the snapshot row, without pulling in world.cpp's helpers.
struct SnapInfo {
    String root;
    int64_t created_at = 0;
    uint64_t entries = 0;
    int state = 0;
    int hard = 0;
    uint32_t root_mode = 0755;
    String name;
    uint64_t hl_groups = 0;   // T2.5: P9 groups to replay on the entry, 0 = nothing to do
};

int snap_info(wfs_store *s, wfs_id id, SnapInfo &out) {
    Guard g(s->mu);
    Stmt q(s->db,
           "SELECT path, created_at, entries, state, hard, root_mode, name, hl_groups"
           " FROM snapshots WHERE id=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return -ENOENT;
    out.root.assign(q.col_text(0));
    out.created_at = q.col_i64(1);
    out.entries = (uint64_t)q.col_i64(2);
    out.state = (int)q.col_i64(3);
    out.hard = (int)q.col_i64(4);
    uint32_t m = (uint32_t)q.col_i64(5);
    out.root_mode = m ? m : 0755;
    out.name.assign(q.col_text(6));
    out.hl_groups = (uint64_t)q.col_i64(7);
    return 0;
}

int space_for(wfs_store *s, uint64_t entries) {
    uint64_t avail = 0, total = 0;
    if (int rc = fs_free_space(s->dir.c_str(), &avail, &total)) return rc;
    return avail < entries * 1024 + kSpaceFloor ? WFS_E_LOW_SPACE : 0;
}

// One filler at a time, store-wide. Non-blocking on purpose: the background top-up that `fork`
// spawns must give up immediately when a filler is already running, rather than queue up a
// second clone of the same tree behind it.
struct PoolLock {
    int fd = -1;
    ~PoolLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    PoolLock() = default;
    PoolLock(const PoolLock &) = delete;
    PoolLock &operator=(const PoolLock &) = delete;
    int take(wfs_store *s) {
        String p = joinp(joinp(s->dir.c_str(), "locks").c_str(), "pool.lock");
        fd = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) return -errno;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int e = errno;
            ::close(fd);
            fd = -1;
            return (e == EWOULDBLOCK || e == EAGAIN) ? WFS_E_POOL_BUSY : -e;
        }
        return 0;
    }
};

int ready_count(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, uint64_t *out) {
    *out = 0;
    Guard g(s->mu);
    Stmt q(s->db, "SELECT COUNT(*) FROM pool WHERE snapshot_id=? AND snap_created_at=? AND state=1");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)snapshot);
    q.i64(2, snap_created_at);
    if (q.row()) *out = (uint64_t)q.col_i64(0);
    return 0;
}

// Builds one entry: clone under the gate into <uuid>.wfs-tmp, give the root the source tree's
// own mode back (exactly what a fork does), rename, then mark the row ready. Publish order (P8)
// with the row playing the part it plays for worlds.
int build_one(wfs_store *s, wfs_id snapshot, const SnapInfo &si, const String &dir) {
    char uuid[40];
    hex_uuid(uuid, sizeof uuid);
    String path = joinp(dir.c_str(), uuid);
    String tmp(path);
    tmp.append(WFS_TMP_SUFFIX);
    int64_t created = now_sec();

    wfs_id row = 0;
    if (wfs_test_before_pool_insert) wfs_test_before_pool_insert(wfs_test_before_pool_insert_ctx);
    {
        Guard g(s->mu);
        Txn t(s->db);
        // PR #1 review (15th round, P2): the snapshot, re-read here rather than carried in from
        // the single read at the top of wfs_pool_fill. A `discard S<n>` in between counts its
        // references under BEGIN IMMEDIATE, sees no pool row for an entry that does not exist
        // yet, and commits its row in WFS_ST_TRASHING -- and the filler, whose source tree has
        // not moved yet, cloned it and published a READY entry for a snapshot on its way to the
        // trash. pool_collect() buries such an entry a wake later, but a fork in between takes
        // it as a live baseline.
        //
        // This transaction is BEGIN IMMEDIATE too, so the two serialise: either the discard is
        // first and this read sees a snapshot that is not ACTIVE, or this insert is first and
        // the discard counts the row it just wrote (a refusal without --force, a drain with it).
        // created_at is part of the question because it is the identity the entry carries:
        // snapshot_id plus snap_created_at is what a fork claims an entry by.
        {
            Stmt sq(s->db, "SELECT state, created_at FROM snapshots WHERE id=?");
            if (!sq.ok()) return -EIO;
            sq.i64(1, (int64_t)snapshot);
            if (!sq.row()) return -ESTALE;
            if (sq.col_i64(0) != WFS_ST_ACTIVE || sq.col_i64(1) != si.created_at) return -ESTALE;
        }
        // PR #1 review (3rd round): who is filling this entry, so gc can tell a filler that died
        // from one that is still cloning a 50 000-entry tree.
        int64_t opid = (int64_t)::getpid();
        int64_t ostart = fs_pid_start_sec(opid);
        Stmt ins(s->db,
                 "INSERT INTO pool(snapshot_id, snap_created_at, uuid, path, entries, created_at,"
                 " owner_pid, owner_start, state) VALUES(?,?,?,?,?,?,?,?,0)");
        if (!ins.ok()) return -EIO;
        ins.i64(1, (int64_t)snapshot);
        ins.i64(2, si.created_at);
        ins.text(3, uuid);
        ins.text(4, path.c_str());
        ins.i64(5, (int64_t)si.entries);
        ins.i64(6, created);
        ins.i64(7, opid);
        ins.i64(8, ostart);
        if (ins.step() != SQLITE_DONE) return -EIO;
        row = (wfs_id)sqlite3_last_insert_rowid(s->db);
        t.commit();
    }

    int rc = 0;
    do {
        if (exists(tmp.c_str())) { if ((rc = fs_remove_tree(tmp.c_str()))) break; }
        {
            SnapGate gate;
            if (!si.hard && (rc = gate.open(si.root.c_str(), false))) break;
            rc = fs_clone_tree(si.root.c_str(), tmp.c_str(), false);
        }
        if (rc) break;
        if (!si.hard && ::chmod(tmp.c_str(), (mode_t)(si.root_mode | 0200)) != 0) { rc = -errno; break; }
        // A --hard snapshot clones its UF_IMMUTABLE flags along; undo them here, once, so the
        // hand-out stays O(1) for hard snapshots too.
        if (si.hard && (rc = fs_unprotect_tree(tmp.c_str()))) break;
        // P9 (T2.5): and the same for the hardlinks clonefile broke. Doing it here, in the
        // background filler, is the whole point: the fork that takes this entry does not pay
        // for it, and the hand-out stays a marker plus a rename.
        if (si.hl_groups) {
            HardlinkSet hl;
            String mp = hardlinks_manifest_path(si.root.c_str());
            // PR #1 review (4th round): an entry whose groups could not be replayed is not a
            // faithful clone of the snapshot, and a fork would hand it out as one. Drop it --
            // the filler's error path removes the tree and the row.
            // PR #1 review (6th round): and the same for a manifest that cannot be read or that
            // holds fewer groups than the row says. Both were swallowed here, and the entry then
            // went into the pool as READY with independent files where the snapshot records one
            // inode under n names -- handed to the next fork as a faithful clone. The row's
            // hl_groups and the manifest's group count come from one HardlinkSet written in one
            // place (wfs_snapshot_create), so they disagree only if the manifest was damaged.
            if ((rc = hardlinks_manifest_read(mp.c_str(), hl))) { rc = WFS_E_SNAPSHOT_DIRTY; break; }
            if (hl.groups.size() != si.hl_groups) { rc = WFS_E_SNAPSHOT_DIRTY; break; }
            // PR #1 review (13th round, P2): the same question the fork asks, for the same
            // reason -- a manifest whose members were exchanged between two groups passes every
            // structural check and welds two unrelated files together on replay, and an entry
            // built that way is handed to the next fork as a faithful clone. The snapshot tree
            // answers it (the clone cannot: clonefile broke every link in it), one lstat per
            // hardlinked name, inside the gate window.
            if (hl.groups.size()) {
                SnapGate vgate;
                if (!si.hard) rc = vgate.open(si.root.c_str(), false);
                if (!rc && hardlinks_verify_groups(si.root.c_str(), hl)) rc = WFS_E_SNAPSHOT_DIRTY;
                vgate.close();
                if (rc) break;
            }
            if (hl.groups.size() && (rc = hardlinks_restore(tmp.c_str(), hl, nullptr, nullptr))) break;
        }
        // The snapshot's own marker is never in there (wfs_snapshot_create removes it), so the
        // entry carries no identity at all until a fork writes one.
        if ((rc = fs_rename(tmp.c_str(), path.c_str()))) break;
    } while (0);

    if (rc) {
        fs_remove_tree(tmp.c_str());
        Guard g(s->mu);
        Txn t(s->db);
        Stmt del(s->db, "DELETE FROM pool WHERE id=?");
        if (del.ok()) { del.i64(1, (int64_t)row); del.step(); }
        t.commit();
        return rc;
    }

    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -errno;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt u(s->db, "UPDATE pool SET dir_dev=?, dir_ino=?, root_mode=?, root_mtime=?, state=1 WHERE id=?");
    if (!u.ok()) return -EIO;
    u.i64(1, (int64_t)st.st_dev);
    u.i64(2, (int64_t)st.st_ino);
    u.i64(3, (int64_t)(st.st_mode & 07777));
    u.i64(4, mtime_ns(st));
    u.i64(5, (int64_t)row);
    if (u.step() != SQLITE_DONE) return -EIO;
    t.commit();
    return 0;
}

} // namespace

int pool_claim(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, PoolClaim &out,
               PoolClaimHook on_claimed, void *hook_ctx) {
    if (!s || !snapshot) return -EINVAL;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt q(s->db,
           "SELECT id, uuid, path, entries, root_mode, root_mtime, created_at FROM pool"
           " WHERE snapshot_id=? AND snap_created_at=? AND state=1 ORDER BY id LIMIT 1");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)snapshot);
    q.i64(2, snap_created_at);
    if (!q.row()) return -ENOENT;
    out.row = (wfs_id)q.col_i64(0);
    out.uuid.assign(q.col_text(1));
    out.path.assign(q.col_text(2));
    out.entries = (uint64_t)q.col_i64(3);
    out.root_mode = (uint32_t)q.col_i64(4);
    out.root_mtime = q.col_i64(5);
    out.created_at = q.col_i64(6);
    out.snapshot = snapshot;
    out.snap_created_at = snap_created_at;
    Stmt del(s->db, "DELETE FROM pool WHERE id=?");
    if (!del.ok()) return -EIO;
    del.i64(1, (int64_t)out.row);
    if (del.step() != SQLITE_DONE) return -EIO;
    // The tree must actually be there. A pool directory removed behind the store's back is not
    // an error worth failing the fork for: report "empty pool" and let the caller clone. The row
    // goes either way -- an entry whose tree has vanished is never coming back -- but the hook
    // does not run for a claim that cannot be honoured.
    // PR #1 review (12th round): the row is deleted above either way -- an entry whose tree has
    // vanished is never coming back -- but `gone` also decides whether the hand-out happens, and
    // an lstat(2) that failed for EACCES or EIO is not a vanished tree. Assume it is there: the
    // hook then fails on the tree itself and the Txn destructor rolls the DELETE back, so the
    // entry survives to be collected properly instead of being dropped from the database with
    // its clone left on disk.
    bool gone = proven_gone(out.path.c_str());
    if (!gone && on_claimed) {
        if (int rc = on_claimed(hook_ctx, out)) return rc;   // the Txn destructor rolls back
    }
    t.commit();
    return gone ? -ENOENT : 0;
}

int pool_return(wfs_store *s, const PoolClaim &c, PoolReturnHook on_returned, void *hook_ctx) {
    if (!s || !c.path.size()) return -EINVAL;
    // The hand-out may have written a marker into it; an entry never carries one.
    String m = joinp(c.path.c_str(), WFS_MARKER_NAME);
    ::unlink(m.c_str());
    struct stat st;
    if (::stat(c.path.c_str(), &st) != 0) return -errno;   // nothing to put back
    int rc = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        // PR #1 review (16th round, P2): the same question build_one's insert asks (15th round)
        // and for the same reason -- a READY entry is a promise that the snapshot it names is
        // ACTIVE and is still the snapshot this tree was cloned from. The caller's hook is what
        // holds that true while the entry is out of the pool (the fork's CREATING world row is
        // the snapshot's reference, and it is deleted in this transaction, not before it), so
        // in the unwind this was written for the check cannot fail; it is the entry's identity,
        // asked under the same write lock the discard's reference count is taken under, for
        // this caller and any other.
        {
            Stmt sq(s->db, "SELECT state, created_at FROM snapshots WHERE id=?");
            if (!sq.ok()) rc = -EIO;
            else {
                sq.i64(1, (int64_t)c.snapshot);
                if (!sq.row()) rc = -ESTALE;
                else if (sq.col_i64(0) != WFS_ST_ACTIVE || sq.col_i64(1) != c.snap_created_at)
                    rc = -ESTALE;
            }
        }
        if (!rc) {
            Stmt ins(s->db,
                     "INSERT INTO pool(snapshot_id, snap_created_at, uuid, path, entries, root_mode,"
                     " root_mtime, dir_dev, dir_ino, created_at, state) VALUES(?,?,?,?,?,?,?,?,?,?,1)");
            if (!ins.ok()) rc = -EIO;
            else {
                ins.i64(1, (int64_t)c.snapshot);
                ins.i64(2, c.snap_created_at);
                ins.text(3, c.uuid.c_str());
                ins.text(4, c.path.c_str());
                ins.i64(5, (int64_t)c.entries);
                ins.i64(6, (int64_t)(st.st_mode & 07777));
                ins.i64(7, mtime_ns(st));
                ins.i64(8, (int64_t)st.st_dev);
                ins.i64(9, (int64_t)st.st_ino);
                ins.i64(10, c.created_at ? c.created_at : now_sec());
                if (ins.step() != SQLITE_DONE) rc = -EIO;
            }
        }
        // The claimer's own bookkeeping, in the same transaction as the row that replaces it.
        if (!rc && on_returned) rc = on_returned(hook_ctx, c);
        if (!rc) t.commit();     // and the Txn destructor rolls everything back otherwise
    }
    // The entry did not go back, so its tree is not an entry: remove it rather than leave a
    // row-less clone for the orphan sweep to find. No deadline -- this is a failed fork's
    // unwind, not the gc worker -- and if it will not go, the sweep is where it belongs.
    if (rc) fs_remove_tree(c.path.c_str());
    return rc;
}

int pool_ready_for(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, uint64_t *out) {
    if (!s || !out) return -EINVAL;
    return ready_count(s, snapshot, snap_created_at, out);
}

int pool_verify(wfs_store *s, wfs_id snapshot, uint64_t *checked, uint64_t *dirty, char *first_bad,
                size_t cap) {
    if (!s || !snapshot) return -EINVAL;
    Vec<String> paths;
    Vec<int64_t> mtimes;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT path, root_mtime FROM pool WHERE snapshot_id=? AND state=1 ORDER BY id");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)snapshot);
        while (q.row()) {
            paths.emplace_back(q.col_text(0));
            mtimes.emplace_back(q.col_i64(1));
        }
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        if (checked) (*checked)++;
        struct stat st;
        bool bad = ::lstat(paths[i].c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
                   mtime_ns(st) > mtimes[i];
        if (!bad) continue;
        if (dirty) (*dirty)++;
        if (first_bad && cap && !first_bad[0]) copy_str(first_bad, cap, paths[i].c_str());
    }
    return 0;
}

namespace {

// The classification half, split out of pool_collect (PR #1 review, 6th round) so that
// `gc --status` can count what is waiting without removing any of it: `rows`/`trees` are the
// doomed entries, `live` every path something still alive is using.
int pool_scan(wfs_store *s, Vec<wfs_id> &rows, Vec<String> &trees, Vec<String> &live) {
    {
        Guard g(s->mu);
        // Left joined by hand: one pass over the pool, one lookup per row. A store has a
        // handful of snapshots, so this is cheaper than it looks.
        Stmt q(s->db, "SELECT id, snapshot_id, snap_created_at, path, state, owner_pid,"
                      " owner_start, created_at FROM pool ORDER BY id");
        if (!q.ok()) return -EIO;
        Stmt snap(s->db, "SELECT created_at, state FROM snapshots WHERE id=?");
        if (!snap.ok()) return -EIO;
        int64_t reap_before = now_sec() - creating_min_age_secs();
        while (q.row()) {
            wfs_id id = (wfs_id)q.col_i64(0);
            wfs_id sid = (wfs_id)q.col_i64(1);
            int64_t sat = q.col_i64(2);
            String path(q.col_text(3));
            int state = (int)q.col_i64(4);
            // Still CREATING: the filler died -- or is still cloning. PR #1 review (3rd round):
            // a 50 000-entry clone takes 0.6 s and a pool fill runs several of them, so a gc
            // that started in the middle used to delete the tree the filler was still writing.
            bool building = state != 1;
            if (building && (producer_alive(q.col_i64(5), q.col_i64(6)) || q.col_i64(7) > reap_before)) {
                String t(path);
                t.append(WFS_TMP_SUFFIX);
                live.emplace_back(path);   // and the <uuid>.wfs-tmp it is cloning into
                live.emplace_back(t);
                continue;
            }
            bool doomed = building;
            if (!doomed) {
                sqlite3_reset(snap.s);
                snap.i64(1, (int64_t)sid);
                if (!snap.row()) doomed = true;                                  // snapshot gone
                else if (snap.col_i64(1) != WFS_ST_ACTIVE) doomed = true;        // not usable
                else if (snap.col_i64(0) != sat) doomed = true;                  // a different snapshot now
            }
            if (doomed) { rows.emplace_back(id); trees.emplace_back(path); }
            else live.emplace_back(path);
        }
        // PR #1 review (3rd round): an entry a fork has claimed has no pool row any more -- the
        // claim deletes it -- so the sweep below saw an unclaimed directory and removed the tree
        // the fork was about to rename into place. The fork's CREATING world row records that
        // entry as its tmp_path; while such a row exists, the tree it names is somebody's work
        // in progress, and gc's own CREATING pass is what decides when it is not.
        Stmt w(s->db, "SELECT tmp_path FROM worlds WHERE state=0 AND tmp_path<>''");
        if (w.ok())
            while (w.row()) live.emplace_back(w.col_text(0));
    }
    return 0;
}

// PR #1 review (9th round), P18: pool_scan is a snapshot too, and what it dooms is removed one
// whole clone at a time. So the doom is re-asked of the live row, under the store mutex,
// immediately before the tree goes: the row must still be there, must still name this tree, and
// must still fail the same test it failed at scan time. The caller holds s->mu.
bool pool_row_still_doomed_locked(wfs_store *s, wfs_id id, const String &path) {
    wfs_id sid = 0;
    int64_t sat = 0, opid = 0, ostart = 0, made = 0;
    int state = 0;
    {
        Stmt q(s->db, "SELECT snapshot_id, snap_created_at, path, state, owner_pid, owner_start,"
                      " created_at FROM pool WHERE id=?");
        if (!q.ok()) return false;
        q.i64(1, (int64_t)id);
        if (!q.row()) return false;                                   // claimed, drained, gone
        if (::strcmp(q.col_text(2), path.c_str())) return false;      // not the tree we queued
        sid = (wfs_id)q.col_i64(0);
        sat = q.col_i64(1);
        state = (int)q.col_i64(3);
        opid = q.col_i64(4);
        ostart = q.col_i64(5);
        made = q.col_i64(6);
    }
    if (state != 1)   // still CREATING: doomed only while its filler really is gone
        return !(producer_alive(opid, ostart) || made > now_sec() - creating_min_age_secs());
    Stmt snap(s->db, "SELECT created_at, state FROM snapshots WHERE id=?");
    if (!snap.ok()) return false;
    snap.i64(1, (int64_t)sid);
    if (!snap.row()) return true;                             // snapshot gone
    if (snap.col_i64(1) != WFS_ST_ACTIVE) return true;        // not usable
    return snap.col_i64(0) != sat;                            // a different snapshot now
}

// PR #1 review (9th round), P18: the same question pool_scan's `live` answers from a snapshot,
// asked of the live rows instead -- does any row name this tree right now? A pool row names its
// path and that path plus `.wfs-tmp` (the name it clones into), and a fork's CREATING world row
// names the entry it claimed through tmp_path. The caller holds s->mu.
bool pool_path_claimed_locked(wfs_store *s, const String &p) {
    String base(p);
    size_t n = base.size(), sl = ::strlen(WFS_TMP_SUFFIX);
    if (n > sl && !::strcmp(base.c_str() + n - sl, WFS_TMP_SUFFIX)) base.resize(n - sl);
    {
        Stmt q(s->db, "SELECT 1 FROM pool WHERE path=? OR path=?");
        if (!q.ok()) return true;   // cannot tell, and "cannot tell" is never "delete it"
        q.text(1, p.c_str());
        q.text(2, base.c_str());
        if (q.row()) return true;
    }
    Stmt w(s->db, "SELECT 1 FROM worlds WHERE state=0 AND (tmp_path=? OR tmp_path=?)");
    if (!w.ok()) return true;
    w.text(1, p.c_str());
    w.text(2, base.c_str());
    return w.row();
}

// One pool tree's key in the store's shared gc retry counter (internal.h). The path, because
// that is what a pool entry is: <store>/pool/S<n>/<uuid>, drawn once and never reused. A row and
// the row-less directory it leaves behind therefore go on counting as one thing.
String pool_fail_key(const String &path) {
    String k("gcfail:pool:");
    k.append(path.c_str());
    return k;
}

// The directories under <store>/pool that no row claims: half-built trees (*.wfs-tmp) and
// entries whose row was claimed by a fork that then died before the rename. `remove` deletes
// them and returns how many went; otherwise they are only counted. The deadline is the gc
// worker's, and a tree it stops in the middle of is still a row-less directory next time, so
// the successor picks it up exactly where this left off.
uint64_t pool_sweep_orphans(wfs_store *s, const Vec<String> &live, bool remove, int64_t deadline_us,
                            bool *out_of_time, uint64_t *failed = nullptr,
                            int *work_remains = nullptr) {
    uint64_t n = 0;
    String root = pool_root(s);
    Vec<String> subs, cand;
    {
        DIR *d = ::opendir(root.c_str());
        if (!d) return 0;
        while (struct dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            String sub = joinp(root.c_str(), e->d_name);
            DIR *sd = ::opendir(sub.c_str());
            if (!sd) continue;
            subs.emplace_back(sub);
            while (struct dirent *ee = ::readdir(sd)) {
                if (ee->d_name[0] == '.') continue;
                String p = joinp(sub.c_str(), ee->d_name);
                bool wanted = false;
                for (size_t i = 0; i < live.size(); ++i)
                    if (!::strcmp(live[i].c_str(), p.c_str())) { wanted = true; break; }
                if (!wanted) cand.emplace_back(p);
            }
            ::closedir(sd);
        }
        ::closedir(d);
    }
    // PR #1 review (9th round), P18: `live` is pool_scan's snapshot of the rows, and the readdir
    // above is not. A filler that inserts its CREATING row after the scan has its `.wfs-tmp`
    // deleted out from under the clone it is writing -- and a partially removed entry then goes
    // on to be published READY -- while a fork that claims an entry in the same window has the
    // tree it is about to rename taken away. So every apparent orphan is re-asked of the live
    // rows, under the store mutex, immediately before it is removed (or, for `gc --status`,
    // counted: the report has to say what the collector would actually touch).
    for (size_t i = 0; i < cand.size(); ++i) {
        const String &p = cand[i];
        {
            Guard g(s->mu);
            if (pool_path_claimed_locked(s, p)) continue;
        }
        if (!remove) { ++n; continue; }
        if (deadline_us && fs_mono_us() >= deadline_us) {
            if (out_of_time) *out_of_time = true;
            break;
        }
        int partial = 0;
        int rc = fs_remove_tree(p.c_str(), deadline_us, &partial);
        if (partial) { if (out_of_time) *out_of_time = true; break; }
        if (rc == 0 && proven_gone(p.c_str())) { ++n; gc_fail_clear(s, pool_fail_key(p).c_str()); continue; }
        // PR #1 review (8th round): it did not go. Nothing else in the store names this
        // directory -- the row it belonged to is gone or never existed -- so saying nothing
        // about it means it leaks silently until somebody runs gc by hand. Count it, and
        // keep the worker chain coming back for it under the same cap a trash entry gets.
        if (failed) (*failed)++;
        if (gc_fail_bump(s, pool_fail_key(p).c_str()) < kGcFailCap && work_remains)
            *work_remains = 1;
    }
    if (remove && !(out_of_time && *out_of_time))
        for (size_t i = 0; i < subs.size(); ++i)
            ::rmdir(subs[i].c_str());   // empty S<n> directories go too; harmless otherwise
    return n;
}

} // namespace

int pool_collect(wfs_store *s, uint64_t *removed, int64_t deadline_us, int *work_remains,
                 uint64_t *failed) {
    if (!s) return -EINVAL;
    uint64_t n = 0;
    Vec<wfs_id> rows;
    Vec<String> trees;
    Vec<String> live; // paths a valid row still points at
    if (int rc = pool_scan(s, rows, trees, live)) return rc;
    if (wfs_test_before_pool_sweep) wfs_test_before_pool_sweep(wfs_test_before_pool_sweep_ctx);
    // PR #1 review (6th round): every one of these is a whole clone of a snapshot, not the
    // handful of stat(2)s the rest of gc's cheap half is made of. Under a deadline the removal
    // stops where it is and the ROW STAYS, because that is the state a successor rediscovers:
    // the entry is doomed for a reason that does not go away (its snapshot is gone, is not
    // ACTIVE, or is a different snapshot now, or its filler died), and none of those can be
    // handed to a fork -- pool_claim only ever matches state=1 rows whose snapshot_id and
    // snap_created_at are those of an ACTIVE snapshot somebody is forking from. So a half
    // removed tree is never mistaken for a ready entry; it is simply collected again.
    bool out_of_time = false;
    for (size_t i = 0; i < trees.size(); ++i) {
        if (deadline_us && fs_mono_us() >= deadline_us) { out_of_time = true; break; }
        {
            // P18: the verdict is re-asked of the live row immediately before the clone goes.
            Guard g(s->mu);
            if (!pool_row_still_doomed_locked(s, rows[i], trees[i])) {
                live.emplace_back(trees[i]);   // and it is not the sweep's to take either
                String t(trees[i]);
                t.append(WFS_TMP_SUFFIX);
                live.emplace_back(t);
                continue;
            }
        }
        String tmp(trees[i]);
        tmp.append(WFS_TMP_SUFFIX);
        int partial = 0;
        fs_remove_tree(tmp.c_str(), deadline_us, &partial);
        int partial2 = 0;
        if (!partial) fs_remove_tree(trees[i].c_str(), deadline_us, &partial2);
        if (partial || partial2) { out_of_time = true; break; }
        // PR #1 review (8th round): the results used to be thrown away and the row deleted
        // whatever happened. A removal that fails for a reason the deadline had nothing to do
        // with -- an ACL, an EPERM, a transient EIO -- then left the clone on disk with its row
        // gone, and the orphan sweep below, which would have found it, had no retry counter and
        // set no work_remains either: it leaked until somebody ran gc by hand. So the row stays
        // while its tree does. It is not handed out by that -- pool_claim only ever matches
        // state=1 rows whose snapshot is the ACTIVE one being forked from, and this entry is
        // doomed precisely because that is no longer true -- it is simply collected again.
        String key = pool_fail_key(trees[i]);
        // PR #1 review (12th round): proven gone, both of them, or the row stays.
        if (!proven_gone(trees[i].c_str()) || !proven_gone(tmp.c_str())) {
            if (failed) (*failed)++;
            if (gc_fail_bump(s, key.c_str()) < kGcFailCap && work_remains) *work_remains = 1;
            // The row stayed, so the tree is not row-less: the sweep below must not count it a
            // second time (nor try the removal that has just failed) under the other rule.
            live.emplace_back(trees[i]);
            live.emplace_back(tmp);
            continue;
        }
        gc_fail_clear(s, key.c_str());
        Guard g(s->mu);
        Txn t(s->db);
        // P18: and the row this deletes is the row that named the tree that has just gone.
        Stmt d(s->db, "DELETE FROM pool WHERE id=? AND path=?");
        if (d.ok()) { d.i64(1, (int64_t)rows[i]); d.text(2, trees[i].c_str()); d.step(); }
        t.commit();
        ++n;
    }
    if (!out_of_time)
        n += pool_sweep_orphans(s, live, true, deadline_us, &out_of_time, failed, work_remains);
    if (out_of_time && work_remains) *work_remains = 1;
    if (removed) *removed += n;
    return 0;
}

int pool_stranded(wfs_store *s, uint64_t *out) {
    if (!s || !out) return -EINVAL;
    *out = 0;
    Vec<wfs_id> rows;
    Vec<String> trees;
    Vec<String> live;
    if (int rc = pool_scan(s, rows, trees, live)) return rc;
    uint64_t n = 0;
    for (size_t i = 0; i < trees.size(); ++i) {
        String tmp(trees[i]);
        tmp.append(WFS_TMP_SUFFIX);
        if (!proven_gone(trees[i].c_str()) || !proven_gone(tmp.c_str())) ++n;
        // A doomed row's tree is counted here, and it is not a row-less orphan as well: the two
        // rules used to meet on it and `gc --status` reported one stale entry as two (PR #1
        // review, 8th round -- the same double count the collector's own failure path had).
        live.emplace_back(trees[i]);
        live.emplace_back(tmp);
    }
    n += pool_sweep_orphans(s, live, false, 0, nullptr);
    *out = n;
    return 0;
}

} // namespace wfs

// ---- C ABI ---------------------------------------------------------------------------------

extern "C" int wfs_pool_fill(wfs_store *s, wfs_id snapshot, int target, uint64_t *made) {
    if (!s || !snapshot) return -EINVAL;
    if (made) *made = 0;
    if (target < 0) target = 0;
    wfs::SnapInfo si;
    if (int rc = wfs::snap_info(s, snapshot, si)) return rc;
    if (si.state != WFS_ST_ACTIVE) return -ESTALE;

    wfs::PoolLock lock;
    if (int rc = lock.take(s)) return rc;

    uint64_t ready = 0;
    if (int rc = wfs::ready_count(s, snapshot, si.created_at, &ready)) return rc;
    if (ready >= (uint64_t)target) return 0;

    wfs::String dir = wfs::pool_dir_of(s, snapshot);
    if (int rc = wfs::fs_mkdir_p(dir.c_str())) return rc;
    while (ready < (uint64_t)target) {
        if (int rc = wfs::space_for(s, si.entries)) return rc;
        // PR #1 review (15th round, P2): -ESTALE out of build_one() is "the snapshot stopped
        // being this snapshot while we were filling" -- the insert re-read it under the write
        // lock and it is not ACTIVE any more. Stop, and hand the caller the same -ESTALE a fill
        // aimed at an already-discarded snapshot gets from the check above: `pool fill` prints
        // "S<n> is not an active snapshot", which is what happened.
        if (int rc = wfs::build_one(s, snapshot, si, dir)) return rc;
        ++ready;
        if (made) (*made)++;
    }
    return 0;
}

extern "C" int wfs_pool_ready(wfs_store *s, wfs_id snapshot, uint64_t *out) {
    if (!s || !snapshot || !out) return -EINVAL;
    wfs::SnapInfo si;
    if (int rc = wfs::snap_info(s, snapshot, si)) return rc;
    return wfs::ready_count(s, snapshot, si.created_at, out);
}

extern "C" int wfs_pool_filling(wfs_store *s) {
    if (!s) return 0;
    wfs::PoolLock probe;
    int rc = probe.take(s);          // released again by the destructor
    return rc == WFS_E_POOL_BUSY ? 1 : 0;
}

extern "C" int wfs_pool_status(wfs_store *s, wfs_pool_stat *buf, size_t cap, size_t *count) {
    if (!s || !count) return -EINVAL;
    *count = 0;
    wfs::Vec<wfs_id> ids;
    {
        wfs::Guard g(s->mu);
        wfs::Stmt q(s->db, "SELECT DISTINCT snapshot_id FROM pool ORDER BY snapshot_id");
        if (!q.ok()) return -EIO;
        while (q.row()) ids.emplace_back((wfs_id)q.col_i64(0));
    }
    size_t n = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        wfs_pool_stat st;
        memset(&st, 0, sizeof st);
        st.snapshot = ids[i];
        wfs::SnapInfo si;
        int have = wfs::snap_info(s, ids[i], si);
        if (have == 0) {
            wfs::copy_str(st.snapshot_name, sizeof st.snapshot_name, si.name.c_str());
            st.entries = si.entries;
        }
        wfs::Guard g(s->mu);
        wfs::Stmt q(s->db, "SELECT state, snap_created_at, created_at FROM pool WHERE snapshot_id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)ids[i]);
        while (q.row()) {
            int state = (int)q.col_i64(0);
            int64_t sat = q.col_i64(1);
            int64_t at = q.col_i64(2);
            bool fresh = have == 0 && si.state == WFS_ST_ACTIVE && sat == si.created_at;
            if (!fresh) st.stale++;
            else if (state == 1) st.ready++;
            else st.building++;
            if (!st.oldest_at || at < st.oldest_at) st.oldest_at = at;
            if (at > st.newest_at) st.newest_at = at;
        }
        if (buf && n < cap) buf[n] = st;
        ++n;
    }
    *count = n;
    return 0;
}

extern "C" int wfs_pool_drain(wfs_store *s, wfs_id snapshot, uint64_t *removed) {
    if (!s) return -EINVAL;
    if (removed) *removed = 0;
    wfs::PoolLock lock;
    if (int rc = lock.take(s)) return rc;

    wfs::Vec<wfs_id> rows;
    wfs::Vec<wfs::String> paths;
    {
        wfs::Guard g(s->mu);
        wfs::Stmt q(s->db, snapshot ? "SELECT id, path FROM pool WHERE snapshot_id=? ORDER BY id"
                                    : "SELECT id, path FROM pool ORDER BY id");
        if (!q.ok()) return -EIO;
        if (snapshot) q.i64(1, (int64_t)snapshot);
        while (q.row()) {
            rows.emplace_back((wfs_id)q.col_i64(0));
            paths.emplace_back(q.col_text(1));
        }
    }
    // PR #1 review (16th round, P2): the tree first, the row only when the tree is gone. This
    // used to delete the row and then remove the trees with both results thrown away, and
    // returned 0 whatever had happened -- so one EPERM (an ACL, a transient EIO) left a whole
    // pre-cloned world under <store>/pool with no row at all, and `discard S<n> --force`, whose
    // drain this is, went on to trash the snapshot on the strength of that 0. Nothing came back
    // for the clone after that: the orphan sweep would find it, but gc's pending check only
    // scans the trash and the snapshot's own entry is not due for days. Same rule as
    // pool_collect's (8th and 12th rounds): proven gone, both names, or the row stays and the
    // errno goes to the caller -- who then fails the discard before touching the snapshot, so
    // the entry is left as what it is, an ordinary pool row of a snapshot that is still ACTIVE.
    for (size_t i = 0; i < rows.size(); ++i) {
        wfs::String tmp(paths[i]);
        tmp.append(WFS_TMP_SUFFIX);
        int trc = wfs::fs_remove_tree(tmp.c_str());
        int prc = wfs::fs_remove_tree(paths[i].c_str());
        if (!wfs::proven_gone(paths[i].c_str()) || !wfs::proven_gone(tmp.c_str()))
            return prc ? prc : (trc ? trc : -EIO);
        {
            wfs::Guard g(s->mu);
            wfs::Txn t(s->db);
            // The row that named the tree that has just gone, as in pool_collect (P18).
            wfs::Stmt d(s->db, "DELETE FROM pool WHERE id=? AND path=?");
            if (!d.ok()) return -EIO;
            d.i64(1, (int64_t)rows[i]);
            d.text(2, paths[i].c_str());
            d.step();
            t.commit();
        }
        if (removed) (*removed)++;
    }
    if (snapshot) {
        wfs::String dir = wfs::pool_dir_of(s, snapshot);
        ::rmdir(dir.c_str());
    }
    return 0;
}
