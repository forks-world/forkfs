// Snapshot and World lifecycle: the M1 clonefile model (docs/M1_DESIGN.md §1–§4).
//
// Two invariants run through this file:
//   * publish order (P8): every tree is built under <target>.wfs-tmp, made correct there, then
//     renamed into place, and only then does the row become ACTIVE. A crash anywhere leaves a
//     CREATING row plus a .wfs-tmp tree, both of which wfs_gc() removes.
//   * identity is marker + inode (P1/P2), never the path. Any command that touches a world
//     re-checks it and repairs the row when the directory has merely moved.
#include "db.h"
#include "hardlinks.h"
#include "pool.h"
#include "snapshot_access.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

using wfs::copy_str;
using wfs::Guard;
using wfs::Manifest;
using wfs::now_sec;
using wfs::Stmt;
using wfs::String;
using wfs::TreeStats;
using wfs::Txn;
using wfs::Vec;

namespace {

const int64_t kDefaultRetention = 7 * 24 * 3600;
// P11 headroom on top of entries * 1 KiB.
const uint64_t kSpaceFloor = 256ull * 1024 * 1024;

String joinp(const char *a, const char *b) {
    String p(a);
    size_t n = p.size();
    if (n && p.c_str()[n - 1] != '/') p.append("/");
    p.append(b);
    return p;
}

String numbered(const char *dir, char prefix, uint64_t id, const char *suffix) {
    char leaf[64];
    ::snprintf(leaf, sizeof leaf, "%c%llu%s", prefix, (unsigned long long)id, suffix ? suffix : "");
    return joinp(dir, leaf);
}

bool exists(const char *p) {
    struct stat st;
    return ::lstat(p, &st) == 0;
}

int64_t now_us() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}

bool is_dir(const char *p) {
    struct stat st;
    return ::stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void dirname_of(const char *p, String &out) {
    const char *slash = ::strrchr(p, '/');
    if (!slash) { out.assign("."); return; }
    if (slash == p) { out.assign("/"); return; }
    out.assign(p);
    out.resize((size_t)(slash - p));
}

const char *basename_of(const char *p) {
    const char *slash = ::strrchr(p, '/');
    return slash ? slash + 1 : p;
}

// ---- the .world marker ---------------------------------------------------------------------
// Hand-rolled JSON (arch.md §39: no JSON library). Only the keys below are ever written, and
// only this reader ever parses them, so the grammar is: object, string and integer values.

void json_escape(String &out, const char *s) {
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') { out.append("\\"); }
        char c[2] = {*p, 0};
        out.append(c);
    }
}

void marker_text(String &out, const char *store_id, const char *store_path, wfs_id world,
                 const char *name, wfs_id snapshot, wfs_id parent, int64_t created) {
    char num[64];
    out.assign("{\n  \"schema\": ");
    ::snprintf(num, sizeof num, "%d", WFS_STORE_SCHEMA); out.append(num);
    out.append(",\n  \"store\": \""); json_escape(out, store_id);
    // T2.3: the store's *path*, not just its id. The sandboxed FSKit appex resolves
    // NSApplicationSupportDirectory inside its own container, so it cannot guess where the store
    // a world belongs to actually is; `-o` options do not reach it on macOS 27, and the marker in
    // the mount source root is the one channel that does. See macos/fskit/WorldVolume.mm.
    out.append("\",\n  \"store_path\": \""); json_escape(out, store_path ? store_path : "");
    out.append("\",\n  \"world\": ");
    ::snprintf(num, sizeof num, "%llu", (unsigned long long)world); out.append(num);
    out.append(",\n  \"name\": \""); json_escape(out, name);
    out.append("\",\n  \"snapshot\": ");
    ::snprintf(num, sizeof num, "%llu", (unsigned long long)snapshot); out.append(num);
    out.append(",\n  \"parent\": ");
    ::snprintf(num, sizeof num, "%llu", (unsigned long long)parent); out.append(num);
    out.append(",\n  \"created_at\": ");
    ::snprintf(num, sizeof num, "%lld", (long long)created); out.append(num);
    out.append("\n}\n");
}

int marker_write(const char *world_root, const char *store_id, const char *store_path, wfs_id world,
                 const char *name, wfs_id snapshot, wfs_id parent, int64_t created) {
    String text;
    marker_text(text, store_id, store_path, world, name, snapshot, parent, created);
    String p = joinp(world_root, WFS_MARKER_NAME);
    // The marker may already exist (cloned from the source world); replace it wholesale.
    ::unlink(p.c_str());
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    ssize_t n = ::write(fd, text.c_str(), text.size());
    int rc = (n == (ssize_t)text.size()) ? 0 : -EIO;
    ::close(fd);
    return rc;
}

const char *json_find(const char *js, const char *key) {
    String pat("\"");
    pat.append(key);
    pat.append("\"");
    const char *p = ::strstr(js, pat.c_str());
    if (!p) return nullptr;
    p += pat.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return nullptr;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

bool json_str(const char *js, const char *key, char *out, size_t cap) {
    const char *p = json_find(js, key);
    if (!p || *p != '"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (n + 1 < cap) out[n++] = *p;
        ++p;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return *p == '"';
}

bool json_u64(const char *js, const char *key, uint64_t *out) {
    const char *p = json_find(js, key);
    if (!p || *p < '0' || *p > '9') return false;
    *out = ::strtoull(p, nullptr, 10);
    return true;
}

struct MarkerData {
    int schema = 0;
    char store_id[40] = {0};
    char store_path[WFS_PATH_MAX] = {0};   // T2.3; empty in markers written before T2.3
    char name[WFS_NAME_MAX] = {0};
    wfs_id world = 0, snapshot = 0, parent = 0;
};

int marker_read(const char *world_root, MarkerData &out) {
    String p = joinp(world_root, WFS_MARKER_NAME);
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return errno == ENOENT ? WFS_E_NOT_A_WORLD : -errno;
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return WFS_E_NOT_A_WORLD;
    buf[n] = 0;
    uint64_t v = 0;
    if (json_u64(buf, "schema", &v)) out.schema = (int)v;
    json_str(buf, "store", out.store_id, sizeof out.store_id);
    json_str(buf, "store_path", out.store_path, sizeof out.store_path);
    json_str(buf, "name", out.name, sizeof out.name);
    if (json_u64(buf, "world", &v)) out.world = v;
    if (json_u64(buf, "snapshot", &v)) out.snapshot = v;
    if (json_u64(buf, "parent", &v)) out.parent = v;
    if (out.schema != WFS_STORE_SCHEMA) return WFS_E_SCHEMA;
    return 0;
}

// P12: a world-level operation (fork from it, checkpoint it, discard it) takes an exclusive
// flock on the marker file. It is advisory and process-scoped, which is exactly the scope we
// want: two `world` invocations serialise, and nothing an agent does inside the world is
// affected.
struct WorldLock {
    int fd = -1;
    ~WorldLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    WorldLock() = default;
    WorldLock(const WorldLock &) = delete;
    WorldLock &operator=(const WorldLock &) = delete;
    int take(const char *world_root) {
        String p = joinp(world_root, WFS_MARKER_NAME);
        fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) return errno == ENOENT ? WFS_E_NOT_A_WORLD : -errno;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int e = errno;
            ::close(fd);
            fd = -1;
            return (e == EWOULDBLOCK || e == EAGAIN) ? WFS_E_WORLD_BUSY : -e;
        }
        return 0;
    }
};

// ---- P3, the default: the gate directory --------------------------------------------------
//
// The gate itself lives in snapshot_access.{h,cpp}: `fork`, `checkpoint`, `verify` and `diff`
// all have to open the same door, so there is one implementation and one flock. See the header
// for what the gate is and why the entries below the root are never touched.
using wfs::SnapGate;

// ---- P5: the `world exec` lock ---------------------------------------------------------------

String lock_path(wfs_store *s, wfs_id id) {
    return numbered(joinp(s->dir.c_str(), "locks").c_str(), 'W', id, ".lock");
}

// Reads the lock file and decides whether a live holder exists. A file whose pid is gone, or
// whose flock can be taken, was left behind by a killed process: it is removed and reported as
// free. Returns 1 when held, 0 when free.
int lock_probe(const char *path, wfs_lock_info *out) {
    if (out) memset(out, 0, sizeof *out);
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[1024] = {0};
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    long long pid = 0, started = 0;
    const char *cmd = "";
    // "pid <n>\nstart <t>\ncmd <text>\n"
    if (const char *p = ::strstr(buf, "pid ")) pid = ::strtoll(p + 4, nullptr, 10);
    if (const char *p = ::strstr(buf, "start ")) started = ::strtoll(p + 6, nullptr, 10);
    if (const char *p = ::strstr(buf, "cmd ")) cmd = p + 4;
    bool alive = pid > 0 && (::kill((pid_t)pid, 0) == 0 || errno == EPERM);
    if (alive) {
        // The pid may have been recycled; the flock is the authority. LOCK_NB succeeding means
        // nobody holds it any more.
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { ::flock(fd, LOCK_UN); alive = false; }
    }
    if (!alive) {
        ::close(fd);
        ::unlink(path);
        return 0;
    }
    if (out) {
        out->held = 1;
        out->pid = pid;
        out->started_at = started;
        copy_str(out->cmd, sizeof out->cmd, cmd);
        for (char *p = out->cmd; *p; ++p) if (*p == '\n') { *p = 0; break; }
    }
    ::close(fd);
    return 1;
}

// P5 for every destructive world operation. Returns WFS_E_WORLD_BUSY when someone is in there.
int exec_lock_guard(wfs_store *s, wfs_id id, int force) {
    if (force) return 0;
    String p = lock_path(s, id);
    return lock_probe(p.c_str(), nullptr) ? WFS_E_WORLD_BUSY : 0;
}

// ---- row helpers -----------------------------------------------------------------------------

const char *kWorldCols =
    "id, kind, parent_world, snapshot_id, name, path, dir_dev, dir_ino, state, fsevents_id,"
    " entries, created_at, trashed_at, trash_path";

void fill_world(Stmt &q, wfs_world_rec &r) {
    memset(&r, 0, sizeof r);
    r.id = (wfs_id)q.col_i64(0);
    r.origin = (int)q.col_i64(1);
    r.parent_world = (wfs_id)q.col_i64(2);
    r.snapshot_id = (wfs_id)q.col_i64(3);
    copy_str(r.name, sizeof r.name, q.col_text(4));
    copy_str(r.path, sizeof r.path, q.col_text(5));
    r.dir_dev = (uint64_t)q.col_i64(6);
    r.dir_ino = (uint64_t)q.col_i64(7);
    r.state = (int)q.col_i64(8);
    r.fsevents_id = (uint64_t)q.col_i64(9);
    r.entries = (uint64_t)q.col_i64(10);
    r.created_at = q.col_i64(11);
    r.trashed_at = q.col_i64(12);
    const char *trash = q.col_text(13);
    // `path` always stays the home path; a trashed world is checked where it actually sits.
    const char *live = (r.state == WFS_ST_TRASHED && *trash) ? trash : r.path;
    struct stat st;
    r.present = (::stat(live, &st) == 0 && S_ISDIR(st.st_mode) && (uint64_t)st.st_ino == r.dir_ino) ? 1 : 0;
}

int world_row(wfs_store *s, wfs_id id, wfs_world_rec &r) {
    String sql("SELECT ");
    sql.append(kWorldCols);
    sql.append(" FROM worlds WHERE id=?");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return -ENOENT;
    fill_world(q, r);
    return 0;
}

int world_trash_path(wfs_store *s, wfs_id id, String &out) {
    Stmt q(s->db, "SELECT trash_path FROM worlds WHERE id=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return -ENOENT;
    out.assign(q.col_text(0));
    return 0;
}

void fill_snapshot(Stmt &q, wfs_snapshot_rec &r) {
    memset(&r, 0, sizeof r);
    r.id = (wfs_id)q.col_i64(0);
    copy_str(r.name, sizeof r.name, q.col_text(1));
    copy_str(r.path, sizeof r.path, q.col_text(2));
    copy_str(r.src_path, sizeof r.src_path, q.col_text(3));
    r.from_world = (wfs_id)q.col_i64(4);
    r.created_at = q.col_i64(5);
    r.entries = (uint64_t)q.col_i64(6);
    r.hardlinks = (uint64_t)q.col_i64(7);
    r.state = (int)q.col_i64(8);
    r.hard = (int)q.col_i64(9);
    r.root_mode = (uint32_t)q.col_i64(10);
    r.trashed_at = q.col_i64(11);
    r.hl_groups = (uint64_t)q.col_i64(12);
    r.hl_external = (uint64_t)q.col_i64(13);
}

const char *kSnapCols =
    "id, name, path, src_path, from_world, created_at, entries, hardlinks, state, hard, root_mode,"
    " trashed_at, hl_groups, hl_external";

// <store>/snapshots/S<n> -- the directory that holds `root` and `manifest`. The row records the
// root; a discard moves the whole thing, because the manifest is what `verify` needs.
String snapshot_dir_of(const wfs_snapshot_rec &r) {
    String d;
    dirname_of(r.path, d);
    return d;
}

int snapshot_row(wfs_store *s, wfs_id id, wfs_snapshot_rec &r) {
    String sql("SELECT ");
    sql.append(kSnapCols);
    sql.append(" FROM snapshots WHERE id=?");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return -ENOENT;
    fill_snapshot(q, r);
    return 0;
}

// ---- P7: which paths may be touched at all ---------------------------------------------------

enum PathMode { PATH_SOURCE, PATH_TARGET };

bool under_dir(const char *path, const char *dir) {
    size_t n = ::strlen(dir);
    return !::strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

// realpath() of a path whose ancestors cannot be searched — which is every path inside a
// gate-protected snapshot — fails with EACCES before it can tell us where the path is. Resolve
// the deepest ancestor that can still be resolved and re-append the rest, so P7 can recognise
// the store and refuse with a reason instead of an errno.
bool resolve_readable_prefix(const char *in, String &out) {
    String path(in), tail;
    for (int depth = 0; depth < 64; ++depth) {
        String r;
        if (wfs::fs_realpath(path.c_str(), r) == 0) {
            out = r;
            if (tail.size()) { out.append("/"); out.append(tail.c_str()); }
            return true;
        }
        const char *slash = ::strrchr(path.c_str(), '/');
        if (!slash || slash == path.c_str()) return false;
        String leaf(slash + 1);
        if (tail.size()) { leaf.append("/"); leaf.append(tail.c_str()); }
        tail = leaf;
        String up;
        dirname_of(path.c_str(), up);
        if (!::strcmp(up.c_str(), path.c_str())) return false;
        path = up;
    }
    return false;
}

// Resolves `in` and refuses the dangerous roots: /, $HOME, anything inside the store (that is
// every snapshot and the trash), and anything strictly below a world root. `is_world_root` tells
// the caller whether the path itself carries a marker; init refuses that, checkpoint requires it.
int check_path(wfs_store *s, const char *in, PathMode mode, String &real, bool *is_world_root) {
    if (is_world_root) *is_world_root = false;
    if (!in || !*in) return -EINVAL;
    int rc = (mode == PATH_TARGET) ? wfs::fs_realpath_parent(in, real) : wfs::fs_realpath(in, real);
    if (rc) {
        // The path cannot be resolved: it is behind a closed gate (EACCES) or its parent does
        // not exist (ENOENT, e.g. <store>/pool/S1/mine when that snapshot has no pool yet).
        // Resolve as much of it as we can: if that lands inside the store, P7 owns the answer
        // and the caller gets a reason instead of an errno.
        String approx;
        if (resolve_readable_prefix(in, approx) && under_dir(approx.c_str(), s->dir.c_str()))
            return WFS_E_PATH_REFUSED;
        return rc;
    }
    if (mode == PATH_TARGET) {
        if (exists(real.c_str())) return -EEXIST;
        String parent;
        dirname_of(real.c_str(), parent);
        if (!is_dir(parent.c_str())) return -ENOENT;
    } else if (!is_dir(real.c_str())) {
        return -ENOTDIR;
    }
    if (!::strcmp(real.c_str(), "/")) return WFS_E_PATH_REFUSED;
    const char *home = ::getenv("HOME");
    if (home && *home && !::strcmp(real.c_str(), home)) return WFS_E_PATH_REFUSED;
    if (under_dir(real.c_str(), s->dir.c_str())) return WFS_E_PATH_REFUSED;
    // The path itself, then every ancestor: a marker above us means we are inside a world.
    String p(real);
    bool self = true;
    for (;;) {
        String m = joinp(p.c_str(), WFS_MARKER_NAME);
        if (exists(m.c_str())) {
            if (!self) return WFS_E_PATH_REFUSED;
            if (is_world_root) *is_world_root = true;
        }
        if (!::strcmp(p.c_str(), "/")) break;
        String up;
        dirname_of(p.c_str(), up);
        if (!::strcmp(up.c_str(), p.c_str())) break;
        p = up;
        self = false;
    }
    return 0;
}

// P11. `entries` is the source tree's entry count; 1 KiB per entry is three times the measured
// 308 B/entry metadata cost of a clone (CLONE_MODEL_MACOS27 §4), plus a flat floor.
int space_check(const char *near_path, uint64_t entries) {
    uint64_t avail = 0, total = 0;
    if (int rc = wfs::fs_free_space(near_path, &avail, &total)) return rc;
    uint64_t need = entries * 1024 + kSpaceFloor;
    return avail < need ? WFS_E_LOW_SPACE : 0;
}

int remove_tmp(const char *target) {
    String tmp(target);
    tmp.append(WFS_TMP_SUFFIX);
    if (!exists(tmp.c_str())) return 0;
    return wfs::fs_remove_tree(tmp.c_str());
}

// ---- T2.1: deleting a trash entry ------------------------------------------------------------
//
// Two steps, in this order and never the other way round:
//
//   1. rename <trash>/X to <trash>/X.deleting. One rename(2), atomic, and after it the tree is
//      visibly not a world any more no matter what happens next. A worker killed half-way
//      through step 2 leaves a .deleting tree, which the next wake finishes and which
//      wfs_world_restore() refuses (WFS_E_TRASH_DELETING) instead of handing back a tree with
//      an arbitrary fraction of its files missing.
//   2. unlink it with the 4-thread walker.
//
// The measured cost of step 2 is what all of this is for: ~50 us per entry, 525 s for the 1000
// worlds of docs/M1_RESULTS.md §3.
bool ends_with(const char *s_, const char *suffix) {
    size_t n = ::strlen(s_), m = ::strlen(suffix);
    return n > m && !::strcmp(s_ + n - m, suffix);
}

// Step 1. `out` receives the name the tree now has (which may be the one it already had).
int trash_mark_deleting(const char *path, String &out) {
    out.assign(path);
    if (ends_with(path, WFS_DELETING_SUFFIX)) return exists(path) ? 0 : -ENOENT;
    if (!exists(path)) return -ENOENT;
    String d(path);
    d.append(WFS_DELETING_SUFFIX);
    // A leftover from an earlier, interrupted attempt: fold it into this one.
    if (exists(d.c_str())) wfs::fs_remove_tree(d.c_str());
    if (int rc = wfs::fs_rename(path, d.c_str())) return rc;
    out = d;
    return 0;
}

// Step 2.
int trash_unlink(const char *deleting_path, int threads, uint64_t *entries) {
    return wfs::fs_remove_tree_parallel(deleting_path, threads > 0 ? threads : 4, entries);
}

} // namespace

// The one interleaving a test cannot produce from outside: the middle of a pool-backed fork,
// after the claim transaction has committed this fork's CREATING world row and before the
// marker/rename/ACTIVE tail. core_test sets it to run `discard` exactly there; nothing in the
// library ever assigns it, and the pool path pays one predictable branch for it.
extern "C" void (*wfs_test_after_pool_claim)(void *ctx, wfs_id world) = nullptr;
extern "C" void *wfs_test_after_pool_claim_ctx = nullptr;

// ---- snapshots --------------------------------------------------------------------------------

extern "C" int wfs_snapshot_create(wfs_store *s, const char *src_dir, const wfs_snapshot_opts *opts,
                                   wfs_id *out) {
    if (!s || !src_dir || !out) return -EINVAL;
    *out = 0;
    wfs_snapshot_opts o;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
    const char *name = o.name;
    String src;
    bool from_world_root = false;
    if (int rc = check_path(s, src_dir, PATH_SOURCE, src, &from_world_root)) return rc;

    wfs_id from_world = 0;
    if (from_world_root) {
        // checkpoint: the source must be a registered world, not a stray copy (P2).
        wfs_identity id;
        if (int rc = wfs_world_verify_identity(s, src.c_str(), &id)) return rc;
        from_world = id.world_id;
        // P5: someone is working in there; a checkpoint of a moving tree is rarely what the
        // caller meant. --force says they meant it.
        if (int rc = exec_lock_guard(s, from_world, o.force)) return rc;
    }
    // P6: st_dev equality does not predict clonefile success, so really clone something.
    if (int rc = wfs_store_clone_probe(s, src.c_str())) return rc;
    // One walk of the SOURCE before cloning, for two things the clone can no longer tell us:
    // how many entries there will be (P11) and how many of them have more than one link. The
    // clone breaks every hardlink into independent inodes (CLONE_MODEL_MACOS27 §11), so
    // counting nlink>1 afterwards would always return zero.
    // T2.5 folds the P9 groups into that same walk: which names share a backing inode, so the
    // clone can be given them back (hardlinks.h). Nothing else here changes.
    TreeStats src_stats;
    wfs::HardlinkSet hl;
    if (int rc = wfs::hardlinks_scan(src.c_str(), &src_stats, hl)) return rc;
    if (int rc = space_check(s->dir.c_str(), src_stats.entries)) return rc;

    char nm[WFS_NAME_MAX];
    copy_str(nm, sizeof nm, (name && *name) ? name : basename_of(src.c_str()));

    wfs_id id = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt ins(s->db,
                 "INSERT INTO snapshots(name, path, src_path, from_world, created_at, state, hard)"
                 " VALUES(?,'',?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.text(1, nm);
        ins.text(2, src.c_str());
        ins.i64(3, (int64_t)from_world);
        ins.i64(4, now_sec());
        ins.i64(5, WFS_ST_CREATING);
        ins.i64(6, o.hard ? 1 : 0);
        if (ins.step() != SQLITE_DONE) return -EIO;
        id = (wfs_id)sqlite3_last_insert_rowid(s->db);
        t.commit();
    }

    String snapdir = numbered(joinp(s->dir.c_str(), "snapshots").c_str(), 'S', id, nullptr);
    String tmpdir(snapdir);
    tmpdir.append(WFS_TMP_SUFFIX);
    String root = joinp(tmpdir.c_str(), "root");
    int rc = 0;
    TreeStats stats;
    wfs::HardlinkRestore hlr;
    uint32_t root_mode = 0755;
    do {
        if (exists(tmpdir.c_str())) { if ((rc = wfs::fs_remove_tree(tmpdir.c_str()))) break; }
        if ((rc = wfs::fs_mkdir(tmpdir.c_str(), 0755))) break;
        // The source here is always a plain directory or a live world: check_path refuses
        // anything inside the store, so a snapshot is never a snapshot's source and there is
        // no gate to open on this side.
        if ((rc = wfs::fs_clone_tree(src.c_str(), root.c_str(), false))) break;
        // A checkpoint carries the source world's marker; it is not this snapshot's identity.
        String m = joinp(root.c_str(), WFS_MARKER_NAME);
        ::unlink(m.c_str());
        // P9: the clone broke every hardlink the source had. Put them back before anything
        // else looks at the tree, so the manifest below records the nlinks this snapshot
        // really has and a fork from it starts from a faithful copy. The source may be a live
        // world, so a name that moved between the scan and the clone is tolerated, not fixed.
        if (hl.groups.size()) wfs::hardlinks_restore(root.c_str(), hl, nullptr, &hlr);
        {
            struct stat rst;
            if (::stat(root.c_str(), &rst) == 0) root_mode = (uint32_t)(rst.st_mode & 07777);
        }
        Manifest man;
        String mp = joinp(tmpdir.c_str(), "manifest");
        man.f = ::fopen(mp.c_str(), "w");
        if (!man.f) { rc = -errno; break; }
        // One walk either way: --hard also flips every entry to UF_IMMUTABLE, the gate does not.
        rc = o.hard ? wfs::fs_protect_tree(root.c_str(), &stats, &man)
                    : wfs::fs_scan_tree(root.c_str(), &stats, &man);
        // T2.5: the groups ride along at the end of the manifest, in a form every older reader
        // skips (hardlinks.h). This is what a fork from this snapshot replays.
        if (!rc) wfs::hardlinks_manifest_write(man.f, hl);
        if (::fclose(man.f) != 0 && !rc) rc = -EIO;
        man.f = nullptr;
        if (rc) break;
        // Close the gate before the tree becomes visible under its final name (publish order).
        if (!o.hard && ::chmod(root.c_str(), WFS_GATE_CLOSED) != 0) { rc = -errno; break; }
        if ((rc = wfs::fs_rename(tmpdir.c_str(), snapdir.c_str()))) break;
    } while (0);

    if (rc) {
        wfs::fs_remove_tree(tmpdir.c_str());
        Guard g(s->mu);
        Txn t(s->db);
        Stmt del(s->db, "DELETE FROM snapshots WHERE id=?");
        if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
        t.commit();
        return rc;
    }
    {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt u(s->db,
               "UPDATE snapshots SET path=?, entries=?, hardlinks=?, state=?, root_mode=?,"
               " hl_groups=?, hl_external=? WHERE id=?");
        if (!u.ok()) return -EIO;
        String rootfinal = joinp(snapdir.c_str(), "root");
        u.text(1, rootfinal.c_str());
        u.i64(2, (int64_t)stats.entries);
        u.i64(3, (int64_t)src_stats.hardlinks);
        u.i64(4, WFS_ST_ACTIVE);
        u.i64(5, (int64_t)root_mode);
        // hl_groups is what lets a fork skip reading the manifest entirely when there is
        // nothing to replay, which is the common case.
        u.i64(6, (int64_t)hl.groups.size());
        u.i64(7, (int64_t)hl.external_names);
        u.i64(8, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        t.commit();
    }
    *out = id;
    return 0;
}

extern "C" int wfs_snapshot_info(wfs_store *s, wfs_id id, wfs_snapshot_rec *out) {
    if (!s || !out || !id) return -EINVAL;
    Guard g(s->mu);
    return snapshot_row(s, id, *out);
}

extern "C" int wfs_snapshot_list(wfs_store *s, wfs_snapshot_rec *buf, size_t cap, size_t *count) {
    if (!s || !count) return -EINVAL;
    *count = 0;
    Guard g(s->mu);
    String sql("SELECT ");
    sql.append(kSnapCols);
    sql.append(" FROM snapshots WHERE state=1 ORDER BY id");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    size_t n = 0;
    while (q.row()) {
        if (buf && n < cap) fill_snapshot(q, buf[n]);
        ++n;
    }
    *count = n;
    return 0;
}

// ---- worlds -----------------------------------------------------------------------------------

namespace {

// What the pool claim has to write down on the fork's behalf, inside the claim's own
// transaction. `world` comes back out of it.
struct PoolForkRow {
    wfs_store *s;
    wfs_id snapshot;
    const char *name;
    const char *path;
    int64_t created;
    uint64_t fsevents;
    wfs_id world;
};

// Runs inside pool_claim's BEGIN IMMEDIATE (pool.h): re-read the snapshot under the write lock
// and insert the CREATING world row. Re-reading is the other half of the race -- the snapshot
// row this fork looked at was read without the write lock, so a `discard` may have committed in
// between; here we either see it (and refuse the claim) or it sees this row (and refuses).
int pool_fork_insert(void *ctx, const wfs::PoolClaim &c) {
    PoolForkRow *r = (PoolForkRow *)ctx;
    {
        Stmt q(r->s->db, "SELECT state FROM snapshots WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)r->snapshot);
        if (!q.row() || q.col_i64(0) != WFS_ST_ACTIVE) return -ESTALE;
    }
    Stmt ins(r->s->db,
             "INSERT INTO worlds(kind, parent_world, snapshot_id, name, path, state,"
             " fsevents_id, entries, created_at) VALUES(?,?,?,?,?,?,?,?,?)");
    if (!ins.ok()) return -EIO;
    ins.i64(1, WFS_O_SNAPSHOT);
    ins.i64(2, 0);
    ins.i64(3, (int64_t)r->snapshot);
    ins.text(4, r->name);
    ins.text(5, r->path);
    ins.i64(6, WFS_ST_CREATING);
    ins.i64(7, (int64_t)r->fsevents);
    ins.i64(8, (int64_t)c.entries);
    ins.i64(9, r->created);
    if (ins.step() != SQLITE_DONE) return -EIO;
    r->world = (wfs_id)sqlite3_last_insert_rowid(r->s->db);
    return 0;
}

} // namespace

extern "C" int wfs_world_create(wfs_store *s, wfs_ref from, const char *target_path,
                                const wfs_fork_opts *opts, wfs_id *out) {
    if (!out) return -EINVAL;
    wfs_fork_result res;
    memset(&res, 0, sizeof res);
    int rc = wfs_world_create_ex(s, from, target_path, opts, &res);
    *out = res.world;
    return rc;
}

extern "C" int wfs_world_create_ex(wfs_store *s, wfs_ref from, const char *target_path,
                                   const wfs_fork_opts *opts, wfs_fork_result *res) {
    wfs_fork_result local;
    memset(&local, 0, sizeof local);
    if (!res) res = &local;
    memset(res, 0, sizeof *res);
    int64_t t_begin = now_us();
    if (!s || !target_path || from.id == 0) return -EINVAL;
    wfs_fork_opts o;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;

    String src;
    uint64_t entries = 0;
    int64_t snap_created_at = 0;
    wfs_id snapshot_id = 0, parent_world = 0;
    char inherited[WFS_NAME_MAX] = {0};
    bool src_hard = false;    // source is a --hard snapshot: the clone needs an unprotect walk
    bool src_gated = false;   // source is a gate-protected snapshot: open it around the clone
    uint32_t src_root_mode = 0;
    // P9 (T2.5): where the hardlink groups of this source are written down, and whether there
    // are any at all. The manifest belongs to a snapshot; a live world borrows its origin
    // snapshot's groups and has every one of them checked against the tree being cloned.
    uint64_t hl_groups = 0;
    String hl_manifest;
    const char *hl_verify = nullptr;
    WorldLock srclock;

    if (from.kind == WFS_K_SNAPSHOT) {
        wfs_snapshot_rec r;
        {
            Guard g(s->mu);
            if (int rc = snapshot_row(s, from.id, r)) return rc;
        }
        if (r.state != WFS_ST_ACTIVE) return -ESTALE;
        src.assign(r.path);
        entries = r.entries;
        snapshot_id = r.id;
        snap_created_at = r.created_at;
        copy_str(inherited, sizeof inherited, r.name);
        src_hard = r.hard != 0;
        src_gated = !src_hard;
        src_root_mode = r.root_mode ? r.root_mode : 0755;
        hl_groups = r.hl_groups;
        if (hl_groups) hl_manifest = wfs::hardlinks_manifest_path(r.path);
    } else if (from.kind == WFS_K_WORLD) {
        wfs_world_rec r;
        {
            Guard g(s->mu);
            if (int rc = world_row(s, from.id, r)) return rc;
        }
        if (r.state != WFS_ST_ACTIVE) return -ESTALE;
        wfs_identity id;
        if (int rc = wfs_world_verify_identity(s, r.path, &id)) return rc;
        if (!id.registered) return WFS_E_UNREGISTERED;
        src.assign(id.path);
        entries = r.entries;
        snapshot_id = r.snapshot_id;
        parent_world = r.id;
        copy_str(inherited, sizeof inherited, r.name);
        // The world was cloned from a snapshot and has been written to since. Its groups are
        // the snapshot's, minus whatever the agent broke, so they are candidates only:
        // hardlinks_restore() lstats each name in the live tree before it touches the clone.
        // (A hardlink an agent made inside the world is not carried over -- there is no cheap
        // way to find it, and a `checkpoint` rescans the tree and records it properly.)
        if (snapshot_id) {
            wfs_snapshot_rec sn;
            Guard g2(s->mu);
            if (snapshot_row(s, snapshot_id, sn) == 0 && sn.hl_groups) {
                hl_groups = sn.hl_groups;
                hl_manifest = wfs::hardlinks_manifest_path(sn.path);
                hl_verify = src.c_str();
            }
        }
        // P5: someone may be running an agent in there; forking a tree that is being written
        // gives a child world in an arbitrary half-state. --force says that is acceptable.
        if (int rc = exec_lock_guard(s, r.id, o.force)) return rc;
        // P5/P12: cloning a directory tree blocks writers in it for 10–15 ms spikes
        // (CLONE_MODEL_MACOS27 §10). Take the world lock so two commands cannot overlap.
        if (int rc = srclock.take(src.c_str())) return rc;
    } else {
        return -EINVAL;
    }

    String target;
    if (int rc = check_path(s, target_path, PATH_TARGET, target, nullptr)) return rc;
    String parent_dir;
    dirname_of(target.c_str(), parent_dir);

    char nm[WFS_NAME_MAX];
    copy_str(nm, sizeof nm, (o.name && *o.name) ? o.name
                            : (*inherited ? inherited : basename_of(target.c_str())));

    // ---- T1.5: the pool ------------------------------------------------------------------
    // A ready entry for this snapshot IS the clone, made earlier and already wearing the source
    // root's mode. What is left of the fork is the publish order's tail: marker, rename, row.
    // The clone probe (P6) and the space check (P11) are skipped on this path on purpose --
    // the tree already exists, on the store's volume, and the rename either works or tells us
    // it does not (EXDEV), in which case the entry goes back and the ordinary path runs.
    if (from.kind == WFS_K_SNAPSHOT && !o.no_pool) {
        int64_t created = now_sec();
        uint64_t ev = wfs::fs_events_current_id();
        PoolForkRow row{s, snapshot_id, nm, target.c_str(), created, ev, 0};
        wfs::PoolClaim claim;
        // The claim and this fork's CREATING world row commit together (pool.h): from the
        // instant the entry leaves the pool, `discard S<n>` can see that somebody is forking
        // from this snapshot. Anything the hook refuses -- a snapshot that has been discarded
        // since the row above was read, a row that will not insert -- leaves the entry in the
        // pool and falls through to the ordinary clone path, which fails the same way.
        if (wfs::pool_claim(s, snapshot_id, snap_created_at, claim, pool_fork_insert, &row) == 0) {
            wfs_id id = row.world;
            if (wfs_test_after_pool_claim) wfs_test_after_pool_claim(wfs_test_after_pool_claim_ctx, id);
            int rc = marker_write(claim.path.c_str(), s->store_id.c_str(), s->dir.c_str(), id, nm,
                                  snapshot_id, 0, created);
            if (!rc) rc = wfs::fs_rename(claim.path.c_str(), target.c_str());
            struct stat st;
            if (!rc && ::stat(target.c_str(), &st) != 0) rc = -errno;
            if (!rc) {
                Guard g(s->mu);
                Txn t(s->db);
                Stmt u(s->db, "UPDATE worlds SET dir_dev=?, dir_ino=?, state=? WHERE id=?");
                if (!u.ok()) rc = -EIO;
                else {
                    u.i64(1, (int64_t)st.st_dev);
                    u.i64(2, (int64_t)st.st_ino);
                    u.i64(3, WFS_ST_ACTIVE);
                    u.i64(4, (int64_t)id);
                    if (u.step() != SQLITE_DONE) rc = -EIO;
                    else t.commit();
                }
            }
            if (rc == 0) {
                uint64_t left = 0;
                wfs::pool_ready_for(s, snapshot_id, snap_created_at, &left);
                res->world = id;
                res->from_pool = 1;
                res->pool_left = left;
                res->elapsed_us = now_us() - t_begin;
                return 0;
            }
            if (id) {
                Guard g(s->mu);
                Txn t(s->db);
                Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
                if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
                t.commit();
            }
            wfs::pool_return(s, claim);   // and fall through to cloning it here and now
        }
    }

    // P6 between the source volume and the target volume, which need not be the store's.
    // The probe reads a file out of the source, so a gated snapshot has to be opened for it.
    if (!o.allow_fallback) {
        SnapGate probe_gate;
        if (src_gated) { if (int rc = probe_gate.open(src.c_str(), false)) return rc; }
        int rc = wfs::fs_clone_probe(parent_dir.c_str(), src.c_str());
        probe_gate.close();
        if (rc == -EXDEV || rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
        if (rc) return rc;
    }
    if (!o.skip_space_check) {
        if (int rc = space_check(parent_dir.c_str(), entries)) return rc;
    }

    int64_t created = now_sec();
    uint64_t ev = wfs::fs_events_current_id();

    wfs_id id = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        // The same window as the pool path's, and the same answer: under the write lock, either
        // the snapshot is still ACTIVE and this CREATING row makes the fork visible to
        // `discard`, or a discard got here first and there is nothing to fork from. (A fork
        // from a *world* is not asked: its snapshot is only the diff baseline, and `gc
        // --reconcile` is allowed to bury a snapshot whose tree is gone while worlds live on.)
        if (from.kind == WFS_K_SNAPSHOT) {
            Stmt q(s->db, "SELECT state FROM snapshots WHERE id=?");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)snapshot_id);
            if (!q.row() || q.col_i64(0) != WFS_ST_ACTIVE) return -ESTALE;
        }
        Stmt ins(s->db,
                 "INSERT INTO worlds(kind, parent_world, snapshot_id, name, path, state,"
                 " fsevents_id, entries, created_at) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.i64(1, from.kind == WFS_K_SNAPSHOT ? WFS_O_SNAPSHOT : WFS_O_WORLD);
        ins.i64(2, (int64_t)parent_world);
        ins.i64(3, (int64_t)snapshot_id);
        ins.text(4, nm);
        ins.text(5, target.c_str());
        ins.i64(6, WFS_ST_CREATING);
        ins.i64(7, (int64_t)ev);
        ins.i64(8, (int64_t)entries);
        ins.i64(9, created);
        if (ins.step() != SQLITE_DONE) return -EIO;
        id = (wfs_id)sqlite3_last_insert_rowid(s->db);
        t.commit();
    }

    String tmp(target);
    tmp.append(WFS_TMP_SUFFIX);
    int rc = 0;
    do {
        if (exists(tmp.c_str())) { if ((rc = wfs::fs_remove_tree(tmp.c_str()))) break; }
        {
            // T1.1b: the whole cost of forking from a gated snapshot is this one clonefile.
            // The gate is open for exactly its duration and for nothing else.
            SnapGate gate;
            if (src_gated && (rc = gate.open(src.c_str(), false))) break;
            rc = wfs::fs_clone_tree(src.c_str(), tmp.c_str(), o.allow_fallback != 0);
        }
        if (rc) break;
        if (src_gated) {
            // The clone copied the root's open-gate mode; give it the source tree's own mode
            // back (plus owner write, as the unprotect walk does for --hard). Nothing below
            // the root was ever touched, so there is nothing else to undo.
            if (::chmod(tmp.c_str(), (mode_t)(src_root_mode | 0200)) != 0) { rc = -errno; break; }
        }
        // A --hard clone inherits UF_IMMUTABLE and the stripped directory modes.
        if (src_hard && (rc = wfs::fs_unprotect_tree(tmp.c_str()))) break;
        // P9 (T2.5): clonefile broke every hardlink again; replay the source's groups on the
        // clone. After the unprotect (linking onto a UF_IMMUTABLE name fails) and before the
        // marker and the rename, so a crash here leaves nothing but a .wfs-tmp tree.
        if (hl_groups) {
            wfs::HardlinkSet hl;
            if (wfs::hardlinks_manifest_read(hl_manifest.c_str(), hl) == 0 && hl.groups.size()) {
                wfs::HardlinkRestore hr;
                wfs::hardlinks_restore(tmp.c_str(), hl, hl_verify, &hr);
                res->hardlinks = hr.links;
            }
        }
        if ((rc = marker_write(tmp.c_str(), s->store_id.c_str(), s->dir.c_str(), id, nm, snapshot_id, parent_world, created)))
            break;
        if ((rc = wfs::fs_rename(tmp.c_str(), target.c_str()))) break;
    } while (0);

    if (rc) {
        wfs::fs_remove_tree(tmp.c_str());
        Guard g(s->mu);
        Txn t(s->db);
        Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
        if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
        t.commit();
        if (rc == -EXDEV || rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
        return rc;
    }

    struct stat st;
    if (::stat(target.c_str(), &st) != 0) return -errno;
    if (entries == 0) {
        TreeStats ts;
        if (wfs::fs_count_entries(target.c_str(), ts) == 0) entries = ts.entries;
    }
    {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt u(s->db, "UPDATE worlds SET dir_dev=?, dir_ino=?, entries=?, state=? WHERE id=?");
        if (!u.ok()) return -EIO;
        u.i64(1, (int64_t)st.st_dev);
        u.i64(2, (int64_t)st.st_ino);
        u.i64(3, (int64_t)entries);
        u.i64(4, WFS_ST_ACTIVE);
        u.i64(5, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        t.commit();
    }
    res->world = id;
    res->elapsed_us = now_us() - t_begin;
    return 0;
}

extern "C" int wfs_world_info(wfs_store *s, wfs_id id, wfs_world_rec *out) {
    if (!s || !out || !id) return -EINVAL;
    Guard g(s->mu);
    return world_row(s, id, *out);
}

extern "C" int wfs_world_next_id(wfs_store *s, wfs_id *out) {
    if (!s || !out) return -EINVAL;
    Guard g(s->mu);
    Stmt q(s->db, "SELECT COALESCE(MAX(id),0)+1 FROM worlds");
    if (!q.ok()) return -EIO;
    *out = q.row() ? (wfs_id)q.col_i64(0) : 1;
    return 0;
}

extern "C" int wfs_world_list(wfs_store *s, int include_trashed, wfs_world_rec *buf, size_t cap,
                              size_t *count) {
    if (!s || !count) return -EINVAL;
    *count = 0;
    Guard g(s->mu);
    String sql("SELECT ");
    sql.append(kWorldCols);
    sql.append(include_trashed ? " FROM worlds WHERE state IN (1,2) ORDER BY id"
                               : " FROM worlds WHERE state=1 ORDER BY id");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    size_t n = 0;
    while (q.row()) {
        if (buf && n < cap) fill_world(q, buf[n]);
        ++n;
    }
    *count = n;
    return 0;
}

extern "C" int wfs_world_discard(wfs_store *s, wfs_id id, int immediate, int force) {
    if (!s || !id) return -EINVAL;
    wfs_world_rec r;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, id, r)) return rc;
    }
    if (r.state == WFS_ST_TRASHED) {
        if (!immediate) return -EALREADY;
        // Already in the trash: --now just brings the deletion forward.
        String tp;
        {
            Guard g(s->mu);
            if (int rc = world_trash_path(s, id, tp)) return rc;
        }
        String deleting;
        if (int rc = trash_mark_deleting(tp.c_str(), deleting)) { if (rc != -ENOENT) return rc; }
        else {
            Guard g(s->mu);
            Txn t(s->db);
            Stmt u(s->db, "UPDATE worlds SET trash_path=? WHERE id=?");
            if (u.ok()) { u.text(1, deleting.c_str()); u.i64(2, (int64_t)id); u.step(); }
            t.commit();
        }
        if (deleting.size()) { if (int rc = trash_unlink(deleting.c_str(), 4, nullptr)) return rc; }
        Guard g(s->mu);
        Txn t(s->db);
        Stmt u(s->db, "UPDATE worlds SET state=?, trash_path='' WHERE id=?");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_DEAD);
        u.i64(2, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        t.commit();
        return 0;
    }
    if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    // P5: never pull the floor out from under a running `world exec`.
    if (int rc = exec_lock_guard(s, id, force)) return rc;
    wfs_identity ident;
    if (int rc = wfs_world_verify_identity(s, r.path, &ident)) return rc;

    WorldLock lock;
    if (int rc = lock.take(ident.path)) return rc;

    char leaf[80];
    ::snprintf(leaf, sizeof leaf, "W%llu-%lld", (unsigned long long)id, (long long)now_sec());
    String trash = joinp(joinp(s->dir.c_str(), "trash").c_str(), leaf);
    int rc = wfs::fs_rename(ident.path, trash.c_str());
    if (rc == -EXDEV) {
        // The world lives on another volume than the store: keep the trash next to it.
        String side;
        dirname_of(ident.path, side);
        side = joinp(side.c_str(), ".wfs-trash");
        wfs::fs_mkdir_p(side.c_str());
        trash = joinp(side.c_str(), leaf);
        rc = wfs::fs_rename(ident.path, trash.c_str());
    }
    if (rc) return rc;
    {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt u(s->db, "UPDATE worlds SET state=?, trash_path=?, trashed_at=? WHERE id=?");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_TRASHED);
        u.text(2, trash.c_str());
        u.i64(3, now_sec());
        u.i64(4, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        t.commit();
    }
    if (!immediate) return 0;
    {
        String deleting;
        if (int frc = trash_mark_deleting(trash.c_str(), deleting)) return frc;
        {
            Guard g(s->mu);
            Txn t(s->db);
            Stmt u(s->db, "UPDATE worlds SET trash_path=? WHERE id=?");
            if (u.ok()) { u.text(1, deleting.c_str()); u.i64(2, (int64_t)id); u.step(); }
            t.commit();
        }
        if (int frc = trash_unlink(deleting.c_str(), 4, nullptr)) return frc;
    }
    Guard g(s->mu);
    Txn t(s->db);
    Stmt u(s->db, "UPDATE worlds SET state=?, trash_path='' WHERE id=?");
    if (!u.ok()) return -EIO;
    u.i64(1, WFS_ST_DEAD);
    u.i64(2, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    t.commit();
    return 0;
}

namespace {

// T2.1: has the collector already started on this trash entry? Either the row has been updated
// to the .deleting name, or the rename happened and the process died before the row did -- the
// name on disk is the authority in both cases.
bool trash_is_deleting(const String &trash) {
    size_t n = trash.size(), sl = ::strlen(WFS_DELETING_SUFFIX);
    if (n > sl && !::strcmp(trash.c_str() + n - sl, WFS_DELETING_SUFFIX)) return true;
    String d(trash);
    d.append(WFS_DELETING_SUFFIX);
    return exists(d.c_str());
}

} // namespace

extern "C" int wfs_world_restore(wfs_store *s, wfs_id id) {
    if (!s || !id) return -EINVAL;
    wfs_world_rec r;
    String trash;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, id, r)) return rc;
        if (int rc = world_trash_path(s, id, trash)) return rc;
    }
    if (r.state != WFS_ST_TRASHED) return -ESTALE;
    // T2.1: once the collector has renamed the tree, what is left of it is not a world any more.
    if (trash_is_deleting(trash)) return WFS_E_TRASH_DELETING;
    // T2.2: a world whose source snapshot has been discarded cannot be brought back to life --
    // it would have no baseline to diff or verify against, which is the whole point of a World.
    if (r.snapshot_id) {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT state FROM snapshots WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)r.snapshot_id);
        if (!q.row() || q.col_i64(0) != WFS_ST_ACTIVE) return WFS_E_SOURCE_GONE;
    }
    if (!exists(trash.c_str())) return -ENOENT;
    if (exists(r.path)) return -EEXIST;
    if (int rc = wfs::fs_rename(trash.c_str(), r.path)) return rc;
    struct stat st;
    if (::stat(r.path, &st) != 0) return -errno;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt u(s->db, "UPDATE worlds SET state=?, trash_path='', trashed_at=0, dir_dev=?, dir_ino=? WHERE id=?");
    if (!u.ok()) return -EIO;
    u.i64(1, WFS_ST_ACTIVE);
    u.i64(2, (int64_t)st.st_dev);
    u.i64(3, (int64_t)st.st_ino);
    u.i64(4, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    t.commit();
    return 0;
}

// ---- T2.2: discarding a snapshot --------------------------------------------------------------

namespace {

// Who still needs S<n>? ACTIVE worlds are a hard refusal; pool entries are only pre-made clones
// and --force drains them.
struct SnapRefs {
    uint64_t active_worlds = 0;
    uint64_t creating_worlds = 0;
    uint64_t trashed_worlds = 0;
    uint64_t pool_entries = 0;
    wfs_id first_world = 0;
};

// Callers already hold the store mutex *and* an open transaction: this is the reference count
// the discard decides on, so it has to be read under the same write lock the fork's claim takes.
int snapshot_refs_locked(wfs_store *s, wfs_id id, SnapRefs &out) {
    {
        Stmt q(s->db, "SELECT id, state FROM worlds WHERE snapshot_id=? AND state<=2");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)id);
        while (q.row()) {
            int64_t st = q.col_i64(1);
            if (st == WFS_ST_ACTIVE) {
                if (!out.first_world) out.first_world = (wfs_id)q.col_i64(0);
                out.active_worlds++;
            } else if (st == WFS_ST_CREATING) {
                // A fork in flight. It committed this row together with its pool claim, or
                // before it started cloning, so it is about to publish a world that needs this
                // snapshot as its baseline -- exactly the case the PR #1 review found.
                out.creating_worlds++;
            } else {
                out.trashed_worlds++;
            }
        }
    }
    {
        Stmt q(s->db, "SELECT COUNT(*) FROM pool WHERE snapshot_id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)id);
        if (q.row()) out.pool_entries = (uint64_t)q.col_i64(0);
    }
    return 0;
}

} // namespace

extern "C" int wfs_snapshot_discard(wfs_store *s, wfs_id id, int force) {
    if (!s || !id) return -EINVAL;
    // A look before the transaction, for the state errors only. Everything the discard *decides*
    // is decided again below, under the write lock.
    {
        wfs_snapshot_rec r;
        Guard g(s->mu);
        if (int rc = snapshot_row(s, id, r)) return rc;
        if (r.state == WFS_ST_TRASHED) return -EALREADY;
        if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    }
    // Pool entries are clones nobody has taken yet -- losing them costs a re-fill and nothing
    // else -- so --force drains them first. The drain takes the pool lock and runs transactions
    // of its own, so it cannot happen inside the discard's transaction; whatever a filler puts
    // back while it runs is caught by the reference count below, under the write lock. Without
    // --force a pool entry is a refusal, and that refusal also comes from down there.
    if (force) {
        uint64_t drained = 0;
        if (int rc = wfs_pool_drain(s, id, &drained); rc && rc != -ENOENT) return rc;
    }

    // One BEGIN IMMEDIATE for the reference check *and* the state transition (PR #1 review).
    // BEGIN IMMEDIATE takes the database write lock, which is the same lock a pool claim and a
    // world row insert take, so a fork is either entirely before this transaction (its CREATING
    // row is counted) or entirely after it (it finds the snapshot TRASHED and gives up). There
    // is no longer a moment in which neither side can see the other.
    Guard g(s->mu);
    Txn t(s->db);
    wfs_snapshot_rec r;
    if (int rc = snapshot_row(s, id, r)) return rc;
    if (r.state == WFS_ST_TRASHED) return -EALREADY;
    if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    SnapRefs refs;
    if (int rc = snapshot_refs_locked(s, id, refs)) return rc;
    // Never orphan a world's source: diff and verify both need the baseline (P4/P10).
    if (refs.active_worlds || refs.creating_worlds || refs.pool_entries) return WFS_E_SNAPSHOT_IN_USE;

    String snapdir = snapshot_dir_of(r);
    char leaf[80];
    ::snprintf(leaf, sizeof leaf, "S%llu-%lld", (unsigned long long)id, (long long)now_sec());
    String trash = joinp(joinp(s->dir.c_str(), "trash").c_str(), leaf);
    if (!exists(snapdir.c_str())) trash.assign("");   // already gone: a reconcile, not a move
    Stmt u(s->db, "UPDATE snapshots SET state=?, trash_path=?, trashed_at=? WHERE id=?");
    if (!u.ok()) return -EIO;
    u.i64(1, WFS_ST_TRASHED);
    u.text(2, trash.c_str());
    u.i64(3, now_sec());
    u.i64(4, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    // The row first, the tree second: a rename that fails rolls the row back, and nothing moved.
    // The gate is on `root`, one level below what moves; renaming the directory that holds it
    // needs no access to the tree at all, so the gate stays closed until the deleter opens it.
    if (trash.size()) {
        if (int rc = wfs::fs_rename(snapdir.c_str(), trash.c_str())) return rc;
    }
    t.commit();
    return 0;
}

// ---- the exec lock (P5) -------------------------------------------------------------------------

extern "C" int wfs_world_lock_exec(wfs_store *s, wfs_id id, const char *cmd, int *out_fd) {
    if (!s || !id || !out_fd) return -EINVAL;
    *out_fd = -1;
    String p = lock_path(s, id);
    if (lock_probe(p.c_str(), nullptr)) return WFS_E_WORLD_BUSY;   // also drops a stale file
    int fd = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -errno;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        ::close(fd);
        return (e == EWOULDBLOCK || e == EAGAIN) ? WFS_E_WORLD_BUSY : -e;
    }
    // The child must not inherit the lock: it belongs to the `world exec` process itself, so a
    // daemon the child leaves behind does not keep the world busy forever.
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    String text("pid ");
    char num[64];
    ::snprintf(num, sizeof num, "%lld\nstart %lld\ncmd ", (long long)::getpid(), (long long)now_sec());
    text.append(num);
    text.append(cmd && *cmd ? cmd : "-");
    text.append("\n");
    ::ftruncate(fd, 0);
    ssize_t n = ::write(fd, text.c_str(), text.size());
    if (n != (ssize_t)text.size()) { ::flock(fd, LOCK_UN); ::close(fd); return -EIO; }
    *out_fd = fd;
    return 0;
}

extern "C" void wfs_world_unlock_exec(wfs_store *s, wfs_id id, int fd) {
    if (s && id) {
        String p = lock_path(s, id);
        ::unlink(p.c_str());
    }
    if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); }
}

extern "C" int wfs_world_lock_check(wfs_store *s, wfs_id id, wfs_lock_info *out) {
    if (!s || !id) return -EINVAL;
    String p = lock_path(s, id);
    lock_probe(p.c_str(), out);
    return 0;
}

// ---- identity (P1 / P2) -------------------------------------------------------------------------

extern "C" int wfs_marker_store_path(const char *world_root, char *buf, size_t cap) {
    if (!world_root || !buf || !cap) return -EINVAL;
    buf[0] = 0;
    MarkerData m;
    if (int rc = marker_read(world_root, m)) return rc;
    if (!m.store_path[0]) return -ENOENT;
    copy_str(buf, cap, m.store_path);
    return 0;
}

extern "C" int wfs_world_verify_identity(wfs_store *s, const char *path, wfs_identity *out) {
    if (!s || !path || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    String real;
    if (int rc = wfs::fs_realpath(path, real)) return rc;
    if (!is_dir(real.c_str())) return -ENOTDIR;
    copy_str(out->path, sizeof out->path, real.c_str());

    MarkerData m;
    if (int rc = marker_read(real.c_str(), m)) return rc;
    out->has_marker = 1;
    out->world_id = m.world;
    out->snapshot_id = m.snapshot;
    copy_str(out->store_id, sizeof out->store_id, m.store_id);
    copy_str(out->name, sizeof out->name, m.name);
    if (::strcmp(m.store_id, s->store_id.c_str()) != 0) return WFS_E_FOREIGN_STORE;

    struct stat st;
    if (::stat(real.c_str(), &st) != 0) return -errno;
    out->dev = (uint64_t)st.st_dev;
    out->ino = (uint64_t)st.st_ino;

    wfs_world_rec r;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, m.world, r)) { out->is_copy = 1; return rc == -ENOENT ? WFS_E_UNREGISTERED : rc; }
    }
    if (r.dir_ino != out->ino || r.dir_dev != out->dev) { out->is_copy = 1; return WFS_E_UNREGISTERED; }
    out->registered = 1;
    if (r.state == WFS_ST_ACTIVE && ::strcmp(r.path, real.c_str()) != 0) {
        // P1: the directory was moved or renamed. The inode says it is the same world, so the
        // row follows the tree rather than the other way round.
        out->moved = 1;
        Guard g(s->mu);
        Txn t(s->db);
        Stmt u(s->db, "UPDATE worlds SET path=? WHERE id=?");
        if (!u.ok()) return -EIO;
        u.text(1, real.c_str());
        u.i64(2, (int64_t)m.world);
        if (u.step() != SQLITE_DONE) return -EIO;
        t.commit();
    }
    return 0;
}

extern "C" int wfs_world_verify(wfs_store *s, wfs_id id, wfs_identity *out) {
    if (!s || !out || !id) return -EINVAL;
    wfs_world_rec r;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, id, r)) return rc;
    }
    memset(out, 0, sizeof *out);
    const char *where = r.path;
    String trash;
    if (r.state == WFS_ST_TRASHED) {
        Guard g(s->mu);
        if (world_trash_path(s, id, trash) == 0 && trash.size()) where = trash.c_str();
    }
    if (!exists(where)) {
        copy_str(out->path, sizeof out->path, where);
        return WFS_E_WORLD_MISSING;
    }
    return wfs_world_verify_identity(s, where, out);
}

extern "C" int wfs_world_adopt(wfs_store *s, const char *path, const char *name, wfs_id *out) {
    if (!s || !path || !out) return -EINVAL;
    *out = 0;
    String real;
    bool marker = false;
    if (int rc = check_path(s, path, PATH_SOURCE, real, &marker)) return rc;
    if (!marker) return WFS_E_NOT_A_WORLD;

    wfs_identity ident;
    int vrc = wfs_world_verify_identity(s, real.c_str(), &ident);
    if (vrc == 0 && ident.registered) return -EEXIST; // already this store's world
    if (vrc != WFS_E_UNREGISTERED && vrc != WFS_E_FOREIGN_STORE) return vrc;

    MarkerData m;
    if (int rc = marker_read(real.c_str(), m)) return rc;
    struct stat st;
    if (::stat(real.c_str(), &st) != 0) return -errno;
    TreeStats ts;
    wfs::fs_count_entries(real.c_str(), ts);

    wfs_id parent = 0;
    if (vrc == WFS_E_UNREGISTERED) {
        Guard g(s->mu);
        wfs_world_rec pr;
        if (world_row(s, m.world, pr) == 0) parent = pr.id;
    }
    char nm[WFS_NAME_MAX];
    copy_str(nm, sizeof nm, (name && *name) ? name : (*m.name ? m.name : basename_of(real.c_str())));
    int64_t created = now_sec();
    wfs_id id = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt ins(s->db,
                 "INSERT INTO worlds(kind, parent_world, snapshot_id, name, path, dir_dev, dir_ino,"
                 " state, fsevents_id, entries, created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.i64(1, WFS_O_ADOPTED);
        ins.i64(2, (int64_t)parent);
        ins.i64(3, (int64_t)(parent ? m.snapshot : 0));
        ins.text(4, nm);
        ins.text(5, real.c_str());
        ins.i64(6, (int64_t)st.st_dev);
        ins.i64(7, (int64_t)st.st_ino);
        ins.i64(8, WFS_ST_ACTIVE);
        ins.i64(9, (int64_t)wfs::fs_events_current_id());
        ins.i64(10, (int64_t)ts.entries);
        ins.i64(11, created);
        if (ins.step() != SQLITE_DONE) return -EIO;
        id = (wfs_id)sqlite3_last_insert_rowid(s->db);
        t.commit();
    }
    if (int rc = marker_write(real.c_str(), s->store_id.c_str(), s->dir.c_str(), id, nm, parent ? m.snapshot : 0, parent,
                              created)) {
        Guard g(s->mu);
        Txn t(s->db);
        Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
        if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
        t.commit();
        return rc;
    }
    *out = id;
    return 0;
}

extern "C" int wfs_path_check(wfs_store *s, const char *path, int for_target) {
    if (!s || !path) return -EINVAL;
    String real;
    bool marker = false;
    int rc = check_path(s, path, for_target ? PATH_TARGET : PATH_SOURCE, real, &marker);
    if (rc) return rc;
    // A world root is a legal checkpoint source but never a legal init or fork target.
    return marker ? WFS_E_PATH_REFUSED : 0;
}

// ---- snapshot verification (P3) -------------------------------------------------------------

namespace {

struct CountCtx { uint64_t n; };
// Counts the root too, so the total is directly comparable with the manifest's line count
// (the manifest has one line for the root and one per entry).
int count_cb(void *ctx, const char *, const char *, const struct stat &, bool) {
    __atomic_fetch_add(&((CountCtx *)ctx)->n, 1, __ATOMIC_RELAXED);   // four workers, one counter
    return 0;
}

char kind_of(mode_t m) {
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISREG(m)) return 'f';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    return 's';
}

void unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '\\' && r[1]) {
            ++r;
            *w++ = (*r == 'n') ? '\n' : *r;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

} // namespace

extern "C" int wfs_snapshot_verify(wfs_store *s, wfs_id id, wfs_verify_report *out) {
    if (!s || !out || !id) return -EINVAL;
    memset(out, 0, sizeof *out);
    wfs_snapshot_rec r;
    {
        Guard g(s->mu);
        if (int rc = snapshot_row(s, id, r)) return rc;
    }
    if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    String snapdir;
    dirname_of(r.path, snapdir);

    // P3 for a gated snapshot: the protection is the root's mode, so check it before opening
    // the gate. Anything other than 0000 means somebody (or a crashed command) left it open.
    if (!r.hard) {
        struct stat gst;
        if (::stat(r.path, &gst) != 0) return -errno;
        if ((gst.st_mode & 07777) != WFS_GATE_CLOSED) {
            out->unprotected++;
            copy_str(out->first_bad, sizeof out->first_bad, r.path);
        }
    }
    SnapGate gate;
    if (int rc = gate.open(r.path, r.hard != 0)) return rc;

    String mp = joinp(snapdir.c_str(), "manifest");
    FILE *f = ::fopen(mp.c_str(), "r");
    if (!f) return -errno;

    char line[8192];
    while (::fgets(line, sizeof line, f)) {
        size_t n = ::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        char kind = line[0];
        char *p = line + 1;
        if (*p != ' ') continue;
        unsigned mode = 0;
        unsigned long long size = 0, nlink = 0;
        long long sec = 0;
        long nsec = 0;
        int consumed = 0;
        if (::sscanf(p, " %o %llu %lld.%ld %llu %n", &mode, &size, &sec, &nsec, &nlink, &consumed) != 5)
            continue;
        char *rel = p + consumed;
        unescape(rel);
        out->checked++;
        String full = *rel ? joinp(r.path, rel) : String(r.path);
        struct stat st;
        if (::lstat(full.c_str(), &st) != 0) {
            out->missing++;
            if (!out->first_bad[0]) copy_str(out->first_bad, sizeof out->first_bad, full.c_str());
            continue;
        }
        // The root of a gated snapshot is the one entry whose mode is meant to differ from
        // the manifest: it is 0000 when closed and WFS_GATE_OPEN right now. It was checked
        // above, on its own terms.
        bool is_root = !*rel;
        bool skip_mode = is_root && !r.hard;
        bool bad = kind_of(st.st_mode) != kind || (!skip_mode && (unsigned)(st.st_mode & 07777) != mode);
        if (!S_ISDIR(st.st_mode) && (unsigned long long)st.st_size != size) bad = true;
#ifdef __APPLE__
        if ((long long)st.st_mtimespec.tv_sec != sec) bad = true;
        // Only a --hard snapshot carries per-entry flags; the gate protects the whole tree at
        // its root instead, so UF_IMMUTABLE being clear there is not a finding.
        bool prot = !r.hard || (st.st_flags & UF_IMMUTABLE) != 0;
#else
        if ((long long)st.st_mtim.tv_sec != sec) bad = true;
        bool prot = true;
#endif
        if (bad) {
            out->modified++;
            if (!out->first_bad[0]) copy_str(out->first_bad, sizeof out->first_bad, full.c_str());
        } else if (!prot) {
            out->unprotected++;
            if (!out->first_bad[0]) copy_str(out->first_bad, sizeof out->first_bad, full.c_str());
        }
    }
    ::fclose(f);

    CountCtx cc = {0};
    if (int rc = wfs::fs_walk_tree(r.path, 4, wfs::FS_DIRS_PRE, &cc, count_cb)) return rc;
    if (cc.n > out->checked) out->extra = cc.n - out->checked;
    // T1.5: the snapshot's untaken clones. One lstat each -- a full walk per pool entry would
    // cost as much as the fill did, and the entry is nobody's world yet, so the cheap check
    // ("still there, and not written to since it was cloned") is the right one.
    wfs::pool_verify(s, id, &out->pool_checked, &out->pool_dirty, out->first_bad,
                     sizeof out->first_bad);
    if (out->missing || out->modified || out->unprotected || out->extra || out->pool_dirty)
        return WFS_E_SNAPSHOT_DIRTY;
    return 0;
}

// ---- gc (T2.1 / T2.2) --------------------------------------------------------------------------
//
// Everything gc does falls into two classes with very different costs:
//
//   cheap    half-built rows and their *.wfs-tmp trees (P8), stale seatbelt profiles, pool
//            entries of dead snapshots (T1.5), and -- new in T2.2 -- noticing rows whose tree is
//            not on disk any more. All of it is a handful of stat(2)s and one SQLite pass, so it
//            runs inline on every gc, whatever the batch limits say.
//   expensive the trash. 1000 discarded worlds of 10k entries are 10.4M unlink(2) calls and 525 s
//            (docs/M1_RESULTS.md §3). This is what the batch limits and the background worker
//            exist for: bounded work per wake, four threads, and P16 -- it must not make the
//            foreground wait.
//
// The lock is store-level and non-blocking, so `world fs gc --worker` started by two different
// commands collapses into one worker; the loser exits without touching anything.

namespace {

const uint64_t kDefaultGcThreads = 4;

// Collect the parent directories of every world we know about; that is where a crashed fork
// can have left a <target>.wfs-tmp tree.
void add_unique(Vec<String> &v, const char *p) {
    for (size_t i = 0; i < v.size(); ++i)
        if (!::strcmp(v[i].c_str(), p)) return;
    v.emplace_back(p);
}

int rm_tmp_in_dir(const char *dir, uint64_t *removed) {
    DIR *d = ::opendir(dir);
    if (!d) return 0;
    size_t sl = ::strlen(WFS_TMP_SUFFIX);
    while (struct dirent *e = ::readdir(d)) {
        size_t n = ::strlen(e->d_name);
        if (n <= sl || ::strcmp(e->d_name + n - sl, WFS_TMP_SUFFIX) != 0) continue;
        String p = joinp(dir, e->d_name);
        if (wfs::fs_remove_tree(p.c_str()) == 0) (*removed)++;
    }
    ::closedir(d);
    return 0;
}

String gc_lock_path(wfs_store *s) {
    return joinp(joinp(s->dir.c_str(), "locks").c_str(), "gc.lock");
}

// One collector per store. Non-blocking on purpose, exactly like the pool filler's lock: a
// second worker would only double the unlink pressure on the same trash.
struct GcLock {
    int fd = -1;
    // The file is never unlinked: flock(2) locks an inode, so removing the file and letting the
    // next worker create a fresh one would let two workers hold "the" lock at once. It also
    // keeps the last progress line readable after the worker is gone.
    ~GcLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    String path;
    GcLock() = default;
    GcLock(const GcLock &) = delete;
    GcLock &operator=(const GcLock &) = delete;
    int take(wfs_store *s) {
        path = gc_lock_path(s);
        fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) return -errno;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int e = errno;
            ::close(fd);
            fd = -1;
            return (e == EWOULDBLOCK || e == EAGAIN) ? WFS_E_GC_BUSY : -e;
        }
        progress(0, 0);
        return 0;
    }
    // Rewritten in place after every trash entry, so `gc --status` can say where the worker is
    // without any IPC. Four short lines; the file never grows.
    void progress(uint64_t done, uint64_t remaining) {
        if (fd < 0) return;
        char buf[256];
        int n = ::snprintf(buf, sizeof buf, "pid %lld\nstart %lld\ndone %llu\nremaining %llu\n",
                           (long long)::getpid(), (long long)now_sec(), (unsigned long long)done,
                           (unsigned long long)remaining);
        if (n <= 0) return;
        ::ftruncate(fd, 0);
        ::pwrite(fd, buf, (size_t)n, 0);
    }
};

// One trash directory waiting to be unlinked.
struct TrashJob {
    String path;      // where it is now
    wfs_id row = 0;   // 0 = an orphan: a directory in the trash that no row claims
    int is_snapshot = 0;
};

// Reads the gc lock file the way lock_probe reads an exec lock: a pid that is gone, or a flock
// that can be taken, means nobody is collecting.
int gc_worker_probe(wfs_store *s, int64_t *pid, int64_t *started, uint64_t *done, uint64_t *remaining) {
    String p = gc_lock_path(s);
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    char buf[512] = {0};
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    bool free_now = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free_now) ::flock(fd, LOCK_UN);
    ::close(fd);
    if (free_now) return 0;
    if (const char *t = ::strstr(buf, "pid ")) *pid = ::strtoll(t + 4, nullptr, 10);
    if (const char *t = ::strstr(buf, "start ")) *started = ::strtoll(t + 6, nullptr, 10);
    if (const char *t = ::strstr(buf, "done ")) *done = ::strtoull(t + 5, nullptr, 10);
    if (const char *t = ::strstr(buf, "remaining ")) *remaining = ::strtoull(t + 10, nullptr, 10);
    return 1;
}

// Everything the trash currently holds, classified. One readdir plus the two row tables.
struct TrashView {
    Vec<TrashJob> deleting;  // interrupted: finish these first, retention does not apply
    Vec<TrashJob> due;       // past the retention period
    uint64_t waiting = 0;    // inside the retention window
    uint64_t worlds = 0, snapshots = 0;
    uint64_t tree_entries = 0;
};

int trash_scan(wfs_store *s, int64_t cutoff, TrashView &v) {
    Vec<String> claimed;   // trash paths a row points at
    {
        Guard g(s->mu);
        {
            Stmt q(s->db, "SELECT id, trash_path, trashed_at, entries FROM worlds WHERE state=2");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                String tp(q.col_text(1));
                if (!tp.size()) continue;
                claimed.emplace_back(tp);
                v.worlds++;
                v.tree_entries += (uint64_t)q.col_i64(3);
                TrashJob j;
                j.path = tp;
                j.row = (wfs_id)q.col_i64(0);
                if (ends_with(tp.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
                else if (q.col_i64(2) <= cutoff) v.due.emplace_back(j);
                else v.waiting++;
            }
        }
        {
            Stmt q(s->db, "SELECT id, trash_path, trashed_at, entries FROM snapshots WHERE state=2");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                String tp(q.col_text(1));
                if (!tp.size()) continue;
                claimed.emplace_back(tp);
                v.snapshots++;
                v.tree_entries += (uint64_t)q.col_i64(3);
                TrashJob j;
                j.path = tp;
                j.row = (wfs_id)q.col_i64(0);
                j.is_snapshot = 1;
                if (ends_with(tp.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
                else if (q.col_i64(2) <= cutoff) v.due.emplace_back(j);
                else v.waiting++;
            }
        }
    }
    // Directories in <store>/trash that no row claims: a killed discard, a store restored from a
    // backup, or a *.deleting tree whose row was already marked DEAD. They go immediately --
    // there is nothing left that could restore them.
    String trashdir = joinp(s->dir.c_str(), "trash");
    if (DIR *d = ::opendir(trashdir.c_str())) {
        while (struct dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            String p = joinp(trashdir.c_str(), e->d_name);
            bool wanted = false;
            for (size_t i = 0; i < claimed.size(); ++i)
                if (!::strcmp(claimed[i].c_str(), p.c_str())) { wanted = true; break; }
            if (wanted) continue;
            TrashJob j;
            j.path = p;
            if (ends_with(p.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
            else v.due.emplace_back(j);
        }
        ::closedir(d);
    }
    return 0;
}

// Marks a row DEAD once its tree is gone. Worlds and snapshots keep their row: the DAG is
// history, and a dangling reference has to be explainable afterwards.
void mark_dead(wfs_store *s, const TrashJob &j) {
    if (!j.row) return;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt u(s->db, j.is_snapshot ? "UPDATE snapshots SET state=?, trash_path='' WHERE id=?"
                                : "UPDATE worlds SET state=?, trash_path='' WHERE id=?");
    if (u.ok()) { u.i64(1, WFS_ST_DEAD); u.i64(2, (int64_t)j.row); u.step(); }
    t.commit();
}

void set_trash_path(wfs_store *s, const TrashJob &j, const char *p) {
    if (!j.row) return;
    Guard g(s->mu);
    Txn t(s->db);
    Stmt u(s->db, j.is_snapshot ? "UPDATE snapshots SET trash_path=? WHERE id=?"
                                : "UPDATE worlds SET trash_path=? WHERE id=?");
    if (u.ok()) { u.text(1, p); u.i64(2, (int64_t)j.row); u.step(); }
    t.commit();
}

// One entry, the crash-safe way: rename first, record the new name, then unlink.
int gc_delete_one(wfs_store *s, const TrashJob &j, int threads, uint64_t *entries_freed) {
    String deleting;
    int rc = trash_mark_deleting(j.path.c_str(), deleting);
    if (rc == -ENOENT) { mark_dead(s, j); return 0; }   // someone else got there first
    if (rc) return rc;
    if (::strcmp(deleting.c_str(), j.path.c_str())) set_trash_path(s, j, deleting.c_str());
    if ((rc = trash_unlink(deleting.c_str(), threads, entries_freed))) return rc;
    mark_dead(s, j);
    return 0;
}

} // namespace

extern "C" int wfs_gc_status(wfs_store *s, int64_t retention_secs, wfs_trash_stat *out) {
    if (!s || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    int64_t retention = retention_secs < 0 ? kDefaultRetention : retention_secs;
    TrashView v;
    if (int rc = trash_scan(s, now_sec() - retention, v)) return rc;
    out->deleting = v.deleting.size();
    out->due = v.due.size();
    out->entries = v.deleting.size() + v.due.size() + v.waiting;
    out->worlds = v.worlds;
    out->snapshots = v.snapshots;
    out->tree_entries = v.tree_entries;
    // 308 B/entry, the measured clone metadata cost on 27.0 (CLONE_MODEL_MACOS27 §4). df cannot
    // see block sharing and du would count blocks that other worlds still hold, so the real
    // volume numbers come along next to the estimate rather than instead of it.
    out->bytes_estimate = v.tree_entries * 308;
    wfs::fs_free_space(s->dir.c_str(), &out->volume_free_bytes, &out->volume_total_bytes);
    gc_worker_probe(s, &out->worker_pid, &out->worker_started_at, &out->worker_done,
                    &out->worker_remaining);
    return 0;
}

extern "C" int wfs_gc_pending(wfs_store *s, int64_t retention_secs, int *worker_running) {
    if (!s) return 0;
    if (worker_running) {
        int64_t pid = 0, at = 0;
        uint64_t d = 0, r = 0;
        *worker_running = gc_worker_probe(s, &pid, &at, &d, &r);
    }
    int64_t cutoff = now_sec() - (retention_secs < 0 ? kDefaultRetention : retention_secs);
    {
        Guard g(s->mu);
        for (const char *sql : {"SELECT 1 FROM worlds WHERE state=2 AND trashed_at<=? AND trash_path<>'' LIMIT 1",
                                "SELECT 1 FROM snapshots WHERE state=2 AND trashed_at<=? AND trash_path<>'' LIMIT 1"}) {
            Stmt q(s->db, sql);
            if (!q.ok()) continue;
            q.i64(1, cutoff);
            if (q.row()) return 1;
        }
    }
    // An interrupted deletion is always work, whatever the retention says.
    String trashdir = joinp(s->dir.c_str(), "trash");
    if (DIR *d = ::opendir(trashdir.c_str())) {
        int found = 0;
        while (struct dirent *e = ::readdir(d))
            if (ends_with(e->d_name, WFS_DELETING_SUFFIX)) { found = 1; break; }
        ::closedir(d);
        if (found) return 1;
    }
    return 0;
}

extern "C" int wfs_gc_ex(wfs_store *s, const wfs_gc_opts *opts, wfs_gc_report *out) {
    if (!s) return -EINVAL;
    wfs_gc_opts o;
    memset(&o, 0, sizeof o);
    o.retention_secs = -1;
    if (opts) o = *opts;
    int threads = o.threads > 0 ? (int)o.threads : (int)kDefaultGcThreads;
    wfs_gc_report rep;
    memset(&rep, 0, sizeof rep);
    int64_t retention = o.retention_secs < 0 ? kDefaultRetention : o.retention_secs;
    int64_t cutoff = now_sec() - retention;
    int64_t deadline_us = o.max_secs > 0 ? now_us() + o.max_secs * 1000000 : 0;
    uint64_t budget = o.max_entries ? o.max_entries : UINT64_MAX;

    GcLock lock;
    if (o.flags & WFS_GC_BACKGROUND) {
        if (int rc = lock.take(s)) return rc;
    }

    // ---- the cheap half, always run in full ------------------------------------------------
    Vec<String> parents;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT path FROM worlds WHERE state<=2");
        if (!q.ok()) return -EIO;
        while (q.row()) {
            String p;
            dirname_of(q.col_text(0), p);
            add_unique(parents, p.c_str());
        }
    }
    // Worlds whose fork never finished: drop the row and the half-built tree (P8).
    {
        Vec<wfs_id> creating;
        Vec<String> cpaths;
        {
            Guard g(s->mu);
            Stmt q(s->db, "SELECT id, path FROM worlds WHERE state=0");
            if (!q.ok()) return -EIO;
            while (q.row()) { creating.emplace_back((wfs_id)q.col_i64(0)); cpaths.emplace_back(q.col_text(1)); }
        }
        for (size_t i = 0; i < creating.size(); ++i) {
            remove_tmp(cpaths[i].c_str());
            rep.tmp_removed++;
            Guard g(s->mu);
            Txn t(s->db);
            Stmt d(s->db, "DELETE FROM worlds WHERE id=? AND state=0");
            if (d.ok()) { d.i64(1, (int64_t)creating[i]); d.step(); }
            t.commit();
        }
    }
    // Half-built snapshots: the row is the only trace of the tmp tree's name.
    String snaps = joinp(s->dir.c_str(), "snapshots");
    {
        Vec<wfs_id> snap_gone;
        {
            Guard g(s->mu);
            Stmt q(s->db, "SELECT id FROM snapshots WHERE state=0");
            if (!q.ok()) return -EIO;
            while (q.row()) snap_gone.emplace_back((wfs_id)q.col_i64(0));
        }
        for (size_t i = 0; i < snap_gone.size(); ++i) {
            String tmp = numbered(snaps.c_str(), 'S', snap_gone[i], WFS_TMP_SUFFIX);
            wfs::fs_remove_tree(tmp.c_str());
            String dir = numbered(snaps.c_str(), 'S', snap_gone[i], nullptr);
            wfs::fs_remove_tree(dir.c_str());
            rep.snapshots_deleted++;
            Guard g(s->mu);
            Txn t(s->db);
            Stmt d(s->db, "DELETE FROM snapshots WHERE id=? AND state=0");
            if (d.ok()) { d.i64(1, (int64_t)snap_gone[i]); d.step(); }
            t.commit();
        }
    }
    for (size_t i = 0; i < parents.size(); ++i) rm_tmp_in_dir(parents[i].c_str(), &rep.tmp_removed);
    rm_tmp_in_dir(snaps.c_str(), &rep.tmp_removed);

    // ---- T2.2: reconciliation ----------------------------------------------------------------
    //
    // A row whose tree is not on disk. Detection is free and unconditional; acting on it is not,
    // because a World that was merely moved looks exactly the same from here until someone runs
    // `world fs verify <its new path>` (P1). So the default is to report, and WFS_GC_RECONCILE
    // is the operator saying "yes, those are gone".
    {
        Vec<wfs_id> dead_snaps, dead_worlds;
        {
            Guard g(s->mu);
            struct stat st;
            {
                Stmt q(s->db, "SELECT id, path FROM snapshots WHERE state=1");
                if (!q.ok()) return -EIO;
                while (q.row()) {
                    const char *p = q.col_text(1);
                    if (*p && ::stat(p, &st) == 0 && S_ISDIR(st.st_mode)) continue;
                    rep.snapshots_dangling++;
                    dead_snaps.emplace_back((wfs_id)q.col_i64(0));
                }
            }
            {
                Stmt q(s->db, "SELECT id, path FROM worlds WHERE state=1");
                if (!q.ok()) return -EIO;
                while (q.row()) {
                    const char *p = q.col_text(1);
                    if (*p && ::stat(p, &st) == 0 && S_ISDIR(st.st_mode)) continue;
                    rep.worlds_dangling++;
                    dead_worlds.emplace_back((wfs_id)q.col_i64(0));
                }
            }
        }
        if (o.flags & WFS_GC_RECONCILE) {
            for (size_t i = 0; i < dead_snaps.size(); ++i) {
                {
                    Guard g(s->mu);
                    Txn t(s->db);
                    Stmt u(s->db, "UPDATE snapshots SET state=?, trash_path='' WHERE id=?");
                    if (u.ok()) { u.i64(1, WFS_ST_DEAD); u.i64(2, (int64_t)dead_snaps[i]); u.step(); }
                    t.commit();
                }
                // The directory the row named is gone, but <store>/snapshots/S<n> may still hold
                // the manifest or a stump; take it with the row.
                String dir = numbered(snaps.c_str(), 'S', dead_snaps[i], nullptr);
                wfs::fs_remove_tree(dir.c_str());
                rep.snapshots_reconciled++;
            }
            for (size_t i = 0; i < dead_worlds.size(); ++i) {
                Guard g(s->mu);
                Txn t(s->db);
                Stmt u(s->db, "UPDATE worlds SET state=? WHERE id=?");
                if (u.ok()) { u.i64(1, WFS_ST_DEAD); u.i64(2, (int64_t)dead_worlds[i]); u.step(); }
                t.commit();
                rep.worlds_reconciled++;
            }
        }
    }

    // T1.5: pool entries whose snapshot is gone or is a different snapshot now, rows whose
    // filler was killed mid-clone, and trees under <store>/pool that no row claims. After the
    // snapshot sweeps above, so a snapshot that died in this same run takes its pool with it.
    wfs::pool_collect(s, &rep.pool_removed);

    // <store>/tmp holds the seatbelt profiles `world exec` generates. They are removed when the
    // command ends; one that survives an hour belongs to a process that was killed.
    {
        String tmpd = joinp(s->dir.c_str(), "tmp");
        if (DIR *d = ::opendir(tmpd.c_str())) {
            int64_t cut = now_sec() - 3600;
            while (struct dirent *e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                String f = joinp(tmpd.c_str(), e->d_name);
                struct stat st;
                if (::lstat(f.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
#ifdef __APPLE__
                int64_t mt = (int64_t)st.st_mtimespec.tv_sec;
#else
                int64_t mt = (int64_t)st.st_mtim.tv_sec;
#endif
                if (mt <= cut && ::unlink(f.c_str()) == 0) rep.tmp_removed++;
            }
            ::closedir(d);
        }
    }

    // ---- the expensive half: the trash -------------------------------------------------------
    if (o.flags & WFS_GC_NO_TRASH) {
        TrashView left;
        if (trash_scan(s, cutoff, left) == 0 && (left.deleting.size() || left.due.size()))
            rep.work_remains = 1;
        if (out) *out = rep;
        return 0;
    }
    TrashView v;
    if (int rc = trash_scan(s, cutoff, v)) return rc;
    uint64_t total = v.deleting.size() + v.due.size();
    uint64_t done = 0;
    lock.progress(0, total);
    // Interrupted trees first: they are already past the point of no return, and leaving them
    // around is the one state in which the trash lies about what it holds.
    for (size_t pass = 0; pass < 2; ++pass) {
        Vec<TrashJob> &jobs = pass == 0 ? v.deleting : v.due;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (done >= budget || (deadline_us && now_us() >= deadline_us)) {
                rep.work_remains = 1;
                goto finished;
            }
            uint64_t freed = 0;
            if (gc_delete_one(s, jobs[i], threads, &freed) != 0) continue;
            rep.entries_freed += freed;
            if (jobs[i].row == 0) rep.trash_orphans++;
            else if (jobs[i].is_snapshot) rep.snapshots_deleted++;
            else rep.worlds_deleted++;
            ++done;
            lock.progress(done, total > done ? total - done : 0);
        }
    }
finished:
    if (out) *out = rep;
    return 0;
}

extern "C" int wfs_gc(wfs_store *s, int64_t retention_secs, wfs_gc_report *out) {
    wfs_gc_opts o;
    memset(&o, 0, sizeof o);
    o.retention_secs = retention_secs;
    return wfs_gc_ex(s, &o, out);
}
