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
    {
        Guard g(s->mu);
        Txn t(s->db);
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
    bool gone = !exists(out.path.c_str());
    if (!gone && on_claimed) {
        if (int rc = on_claimed(hook_ctx, out)) return rc;   // the Txn destructor rolls back
    }
    t.commit();
    return gone ? -ENOENT : 0;
}

void pool_return(wfs_store *s, const PoolClaim &c) {
    if (!s || !c.path.size()) return;
    // The hand-out may have written a marker into it; an entry never carries one.
    String m = joinp(c.path.c_str(), WFS_MARKER_NAME);
    ::unlink(m.c_str());
    struct stat st;
    if (::stat(c.path.c_str(), &st) != 0) return;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt ins(s->db,
             "INSERT INTO pool(snapshot_id, snap_created_at, uuid, path, entries, root_mode,"
             " root_mtime, dir_dev, dir_ino, created_at, state) VALUES(?,?,?,?,?,?,?,?,?,?,1)");
    if (!ins.ok()) { t.commit(); fs_remove_tree(c.path.c_str()); return; }
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
    if (ins.step() != SQLITE_DONE) {
        t.commit();
        fs_remove_tree(c.path.c_str());
        return;
    }
    t.commit();
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

// The directories under <store>/pool that no row claims: half-built trees (*.wfs-tmp) and
// entries whose row was claimed by a fork that then died before the rename. `remove` deletes
// them and returns how many went; otherwise they are only counted. The deadline is the gc
// worker's, and a tree it stops in the middle of is still a row-less directory next time, so
// the successor picks it up exactly where this left off.
uint64_t pool_sweep_orphans(wfs_store *s, const Vec<String> &live, bool remove, int64_t deadline_us,
                            bool *out_of_time) {
    uint64_t n = 0;
    String root = pool_root(s);
    DIR *d = ::opendir(root.c_str());
    if (!d) return 0;
    while (struct dirent *e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        String sub = joinp(root.c_str(), e->d_name);
        DIR *sd = ::opendir(sub.c_str());
        if (!sd) continue;
        while (struct dirent *ee = ::readdir(sd)) {
            if (ee->d_name[0] == '.') continue;
            String p = joinp(sub.c_str(), ee->d_name);
            bool wanted = false;
            for (size_t i = 0; i < live.size(); ++i)
                if (!::strcmp(live[i].c_str(), p.c_str())) { wanted = true; break; }
            if (wanted) continue;
            if (!remove) { ++n; continue; }
            if (deadline_us && fs_mono_us() >= deadline_us) {
                if (out_of_time) *out_of_time = true;
                break;
            }
            int partial = 0;
            int rc = fs_remove_tree(p.c_str(), deadline_us, &partial);
            if (partial) { if (out_of_time) *out_of_time = true; break; }
            if (rc == 0) ++n;
        }
        ::closedir(sd);
        if (out_of_time && *out_of_time) break;
        if (remove) ::rmdir(sub.c_str()); // empty S<n> directories go too; harmless otherwise
    }
    ::closedir(d);
    return n;
}

} // namespace

int pool_collect(wfs_store *s, uint64_t *removed, int64_t deadline_us, int *work_remains) {
    if (!s) return -EINVAL;
    uint64_t n = 0;
    Vec<wfs_id> rows;
    Vec<String> trees;
    Vec<String> live; // paths a valid row still points at
    if (int rc = pool_scan(s, rows, trees, live)) return rc;
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
        String tmp(trees[i]);
        tmp.append(WFS_TMP_SUFFIX);
        int partial = 0;
        fs_remove_tree(tmp.c_str(), deadline_us, &partial);
        int partial2 = 0;
        if (!partial) fs_remove_tree(trees[i].c_str(), deadline_us, &partial2);
        if (partial || partial2) { out_of_time = true; break; }
        Guard g(s->mu);
        Txn t(s->db);
        Stmt d(s->db, "DELETE FROM pool WHERE id=?");
        if (d.ok()) { d.i64(1, (int64_t)rows[i]); d.step(); }
        t.commit();
        ++n;
    }
    if (!out_of_time) n += pool_sweep_orphans(s, live, true, deadline_us, &out_of_time);
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
        if (exists(trees[i].c_str()) || exists(tmp.c_str())) ++n;
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
    for (size_t i = 0; i < rows.size(); ++i) {
        {
            wfs::Guard g(s->mu);
            wfs::Txn t(s->db);
            wfs::Stmt d(s->db, "DELETE FROM pool WHERE id=?");
            if (!d.ok()) return -EIO;
            d.i64(1, (int64_t)rows[i]);
            d.step();
            t.commit();
        }
        wfs::String tmp(paths[i]);
        tmp.append(WFS_TMP_SUFFIX);
        wfs::fs_remove_tree(tmp.c_str());
        if (wfs::fs_remove_tree(paths[i].c_str()) == 0 && removed) (*removed)++;
    }
    if (snapshot) {
        wfs::String dir = wfs::pool_dir_of(s, snapshot);
        ::rmdir(dir.c_str());
    }
    return 0;
}
