// Snapshot and World lifecycle: the M1 clonefile model (docs/M1_DESIGN.md §1–§4).
//
// Two invariants run through this file:
//   * publish order (P8): every tree is built under a temporary name, made correct there, then
//     renamed into place, and only then does the row become ACTIVE. A crash anywhere leaves a
//     CREATING row plus the tree it names, both of which wfs_gc() collects. Inside the store the
//     temporary is derived from the row id; in a user's directory it is drawn at random and
//     written onto the row first, because nothing out there may be assumed to be ours.
//   * identity is marker + inode (P1/P2), never the path. Any command that touches a world
//     re-checks it and repairs the row when the directory has merely moved.
#include "db.h"
#include "hardlinks.h"
#include "pool.h"
#include "git.h"
#include "snapshot_access.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/random.h>
#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>   // std::move only

using wfs::copy_str;
using wfs::Guard;
using wfs::Manifest;
using wfs::Mutex;
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

// PR #1 review (12th round): "there is nothing at this path" -- proven, not assumed.
// exists() folds every lstat(2) failure into "no", and three of this round's findings were the
// same consequence of that: an EACCES on a parent directory, an EIO, a volume that went away
// mid-run, and the collector marked a row DEAD, deleted a row, or quietly stopped keeping track
// of a tree that is still on disk. Presence is assumed unless absence is proven: every verdict
// that destroys something, or writes a row nothing can walk back, asks this instead. The plain
// "does this exist, so may I create it" checks keep exists() -- getting those wrong costs an
// EEXIST from the create itself, not a world.
bool proven_gone(const char *p) { return wfs::fs_gone(wfs::fs_probe(p)); }

// PR #1 review (25th round, P1): is the tree at this path the one that row was written for?
//
// Ownership of a trash entry used to be decided by the row still NAMING the path -- and for a
// world discarded across volumes that path is `<parent>/.wfs-trash/W<id>-<timestamp>`, a
// directory of the user's under a name anybody can work out. During the retention period they
// can move the tree away and leave something else there, and every name-only check then says
// the stranger is the world: the collector renamed it to `.deleting` and deleted it whole,
// `--now` did the same and exited 0, and `restore` carried it home and wrote its dev/ino into
// the row. Ownership is an invariant we wrote down, not a lookalike name (P18).
//
// The invariant is the inode. A world row carries dir_dev/dir_ino from the moment its tree was
// published (fork/adopt), `wfs_world_discard` verifies them against the tree before it moves
// anything and does not touch the two columns afterwards, and every rename on the way into and
// out of the trash is same-volume -- <store>/trash when the store shares the world's volume, a
// `.wfs-trash` beside the world when the discard hit EXDEV -- so the inode the row remembers is
// the entry's identity for as long as the entry exists, `.deleting` spelling included.
//
// Returns 0 when it matches, WFS_E_TRASH_FOREIGN when what is there is provably something else,
// and any other errno as itself: a probe that could not answer is not an identity either (12th
// round), so the entry is left alone and reported rather than deleted on a guess.
//
// dir_ino == 0 is a row from before the columns were written (the schema defaults them), which
// nothing this store produces leaves behind: there is no identity to compare with, so the entry
// is treated as the row's, exactly as it was before this round.
//
// PR #1 review (26th round, P1): and this is now the READING half only -- `gc --status`, which
// counts and names the entries the collector will refuse, and trash_is_deleting(), which
// answers a restore's "is the collector already on this". Neither of them touches anything, so
// a question asked of a path is the right shape for them. Everything that renames, deletes or
// registers asks the same question of a descriptor it then keeps (TrashClaim, below): between
// an answer about a path and an act on that path there is a window, and the party on the other
// side of it is the owner of the directory the entry sits in.
int trash_identity(const char *path, uint64_t dev, uint64_t ino) {
    if (!ino) return 0;
    struct stat st;
    if (int prc = wfs::fs_probe(path, &st)) return prc;
    if ((uint64_t)st.st_ino != ino || (uint64_t)st.st_dev != dev) return WFS_E_TRASH_FOREIGN;
    return 0;
}

// The deleter checks the gc worker's deadline against the same clock (internal.h).
int64_t now_us() { return wfs::fs_mono_us(); }

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

bool json_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Past the closing quote of the string that starts at `p`, or null if it never closes. An
// escape consumes the byte after the backslash, which is all a `\"` or a `\\` needs and is
// enough to walk over a `\uXXXX` too (the four hex digits are ordinary bytes afterwards) --
// this walker never has to decode a value, only to find where it ends.
const char *json_skip_string(const char *p) {
    if (*p != '"') return nullptr;
    for (++p; *p; ++p) {
        if (*p == '"') return p + 1;
        if (*p == '\\') { if (!p[1]) return nullptr; ++p; }
    }
    return nullptr;
}

// Past the value that starts at `p`. The marker is flat -- strings and integers only -- but a
// value this does not understand must still be stepped over rather than guessed at, so objects
// and arrays are counted by depth (with their strings skipped, so a brace inside one does not
// count) and every other literal runs to the next separator.
const char *json_skip_value(const char *p) {
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                const char *q = json_skip_string(p);
                if (!q) return nullptr;
                p = q;
                continue;
            }
            if (*p == '{' || *p == '[') ++depth;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            ++p;
        }
        return nullptr;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !json_ws(*p)) ++p;
    return p;
}

// PR #1 review (19th round): the key of a pair, not the bytes of a value. This used to be
// strstr(js, "\"<key>\"") over the whole marker, and the values are written before the keys
// that follow them: json_escape turns a `"` inside a value into `\"`, so a store directory
// named `q"world` puts the sequence `"world` into the store_path value, the value's own closing
// quote completes `"world"`, strstr stopped there, the byte after it was `,` rather than `:`,
// and this returned not-found -- for a key that is right there, two lines below. marker_read
// then left the world id 0 and every world in that store was WFS_E_UNREGISTERED to `verify`,
// to `mount` and to `fork`. A world NAME does the same to every key written after it.
// So: walk the object's pairs. Same grammar, same file, same markers -- what changes is only
// that a key has to be a key.
const char *json_find(const char *js, const char *key) {
    const char *p = js;
    while (*p && json_ws(*p)) ++p;
    if (*p != '{') return nullptr;
    ++p;
    size_t klen = ::strlen(key);
    for (;;) {
        while (*p && json_ws(*p)) ++p;
        if (*p != '"') return nullptr;            // `}` (no such key) or a marker we cannot read
        const char *k = p + 1;
        const char *after = json_skip_string(p);
        if (!after) return nullptr;
        size_t n = (size_t)(after - 1 - k);
        p = after;
        while (*p && json_ws(*p)) ++p;
        if (*p != ':') return nullptr;
        ++p;
        while (*p && json_ws(*p)) ++p;
        // The keys this file writes hold no escapes, so a raw comparison is exact: a key
        // spelled with one simply is not one of ours.
        if (n == klen && !::memcmp(k, key, klen)) return p;
        const char *nv = json_skip_value(p);
        if (!nv) return nullptr;
        p = nv;
        while (*p && json_ws(*p)) ++p;
        if (*p != ',') return nullptr;
        ++p;
    }
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
    // PR #1 review (17th round, P2): parsed since the marker's store path can now be refreshed
    // in place, and a refresh that forgot this would silently re-date the world.
    int64_t created = 0;
};

int marker_parse_fd(int fd, MarkerData &out) {
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
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
    if (json_u64(buf, "created_at", &v)) out.created = (int64_t)v;
    // PR #1 review (24th round, P1): the store schema is 3 now (store.cpp), and a marker carries
    // the schema of the core that wrote it. The marker's grammar did not change between 2 and 3
    // -- the bump is about what a collector may delete, not about this file -- and the worlds of
    // a store we upgrade in place keep the markers they already have, so a 2 is read rather than
    // refused. A schema this core does not know yet still is: that is the direction P13 is about.
    if (out.schema < WFS_STORE_SCHEMA_M1 || out.schema > WFS_STORE_SCHEMA) return WFS_E_SCHEMA;
    return 0;
}

int marker_read(const char *world_root, MarkerData &out) {
    String p = joinp(world_root, WFS_MARKER_NAME);
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return errno == ENOENT ? WFS_E_NOT_A_WORLD : -errno;
    int rc = marker_parse_fd(fd, out);
    ::close(fd);
    return rc;
}

// PR #1 review (26th round, P1): the same read, through a descriptor on the world root, for the
// one caller that then DELETES the tree it just asked about (gc_tmp_is_removable). Reading the
// marker by name and removing the tree by name are two lookups of the same name, and that tree
// lives in the user's own directory -- so the marker is read through the descriptor the removal
// uses, and the two cannot come apart. Unlike marker_read this tells "there is no marker"
// (-ENOENT, which gc reads as "nothing claims this tree either way") from "there is one and it
// does not read as a marker" (WFS_E_NOT_A_WORLD), which gc may not act on at all. O_NOFOLLOW: a
// symlink standing in for the marker is refused rather than followed, and a tree we could not
// ask about is kept rather than removed (12th round).
int marker_read_at(int dirfd, MarkerData &out) {
    int fd = ::openat(dirfd, WFS_MARKER_NAME, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = marker_parse_fd(fd, out);
    ::close(fd);
    return rc;
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
    // PR #1 review (32nd round, P2): a row that is not there and a read that failed are two
    // different answers, and every caller of this one branches on -ENOENT.
    if (!q.row()) return q.done() ? -ENOENT : -EIO;
    fill_world(q, r);
    return 0;
}

int snapshot_trash_path(wfs_store *s, wfs_id id, String &out) {
    Stmt q(s->db, "SELECT trash_path FROM snapshots WHERE id=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return q.done() ? -ENOENT : -EIO;   // 32nd round, P2
    out.assign(q.col_text(0));
    return 0;
}

int world_trash_path(wfs_store *s, wfs_id id, String &out) {
    Stmt q(s->db, "SELECT trash_path FROM worlds WHERE id=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return q.done() ? -ENOENT : -EIO;   // 32nd round, P2
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
    if (!q.row()) return q.done() ? -ENOENT : -EIO;   // 32nd round, P2
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
        // PR #1 review (12th round): "no marker here" has to mean no marker, not "the question
        // could not be asked". This was a bool over lstat(2), so an EACCES or an EIO on an
        // ancestor read as "clean" and P7's one guard against forking *into* somebody's world
        // silently stood down. An answer that is not an answer is a refusal with its errno.
        int prc = wfs::fs_probe(m.c_str());
        if (prc && !wfs::fs_gone(prc)) return prc;
        if (prc == 0) {
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
int space_check(const char *near_path, uint64_t entries, uint64_t extra_bytes = 0) {
    uint64_t avail = 0, total = 0;
    if (int rc = wfs::fs_free_space(near_path, &avail, &total)) return rc;
    if (extra_bytes > UINT64_MAX - kSpaceFloor || entries > (UINT64_MAX - kSpaceFloor - extra_bytes) / 1024) return WFS_E_LOW_SPACE;
    uint64_t need = entries * 1024 + kSpaceFloor + extra_bytes;
    return avail < need ? WFS_E_LOW_SPACE : 0;
}

// PR #1 review (3rd round): who is building this tree. The pid and this process's own start
// time go onto every CREATING row, so gc can tell "the producer crashed" from "the producer is
// still cloning". wfs_test_fork_owner_pid lets core_test write a pid that is not running, which
// is the only way to produce the abandoned case from inside one process.
void owner_now(int64_t &pid, int64_t &start) {
    pid = wfs_test_fork_owner_pid ? wfs_test_fork_owner_pid : (int64_t)::getpid();
    start = wfs::fs_pid_start_sec(pid);
}

// ---- the fork's temporary name ---------------------------------------------------------------
//
// PR #1 review, third round. A fork builds its clone next to the target -- it has to, because
// rename(2) is only atomic within one directory and the target's parent is the only directory
// guaranteed to be on the target's volume. That parent is the *user's*, so the name has to come
// from somewhere no user name can collide with, and nothing already sitting there may be
// touched. The old name was `<target>.wfs-tmp` with an `if (exists) fs_remove_tree()` in front
// of it, which destroyed `~/w/a.wfs-tmp` on `fork --to ~/w/a`.
//
//     .wfs-fork-<pid>-<counter>-<16 hex from getentropy(2)>
//
// clonefile(2) creates the destination and fails with EEXIST rather than replacing it, exactly
// as O_EXCL does for open(2), so the successful clone *is* the claim. EEXIST means "draw
// another name", never "remove what is there". The name that was drawn goes on the CREATING row
// before the clone starts (worlds.tmp_path), so a crash leaves a row that names the leftover and
// gc has something to remove that it did not have to guess.
const int kTmpTries = 8;

void fork_tmp_leaf(char *out, size_t cap) {
    static uint64_t seq = 0;
    uint64_t n = __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED);
    unsigned char raw[8];
    if (::getentropy(raw, sizeof raw) != 0)
        for (size_t i = 0; i < sizeof raw; ++i)
            raw[i] = (unsigned char)(::getpid() + i * 31 + (int)n);
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof raw; ++i) r = (r << 8) | raw[i];
    ::snprintf(out, cap, ".wfs-fork-%d-%llu-%016llx", (int)::getpid(), (unsigned long long)n,
               (unsigned long long)r);
}

// Write (or clear) the temporary path a CREATING row is building at.
int world_set_tmp_path(wfs_store *s, wfs_id id, const char *p) {
    Guard g(s->mu);
    Txn t(s->db);
    if (!t.ok()) return t.err();   // 34th round, P1
    Stmt u(s->db, "UPDATE worlds SET tmp_path=? WHERE id=?");
    if (!u.ok()) return -EIO;
    u.text(1, p ? p : "");
    u.i64(2, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    return t.commit();
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

// The collector renames a trash entry to `<name>.deleting` first and records the new name
// second, so between the two -- and after any interruption in that window -- the row still names
// the tree by the name it no longer has. Both names now belong to the row (claim_trash_paths),
// which means the row has to be the one that finishes it: follow the rename.
void trash_follow_deleting(String &tp) {
    if (ends_with(tp.c_str(), WFS_DELETING_SUFFIX) || exists(tp.c_str())) return;
    String d(tp);
    d.append(WFS_DELETING_SUFFIX);
    if (exists(d.c_str())) tp = d;
}

// PR #1 review (20th round, P1): is something sitting on the name this entry's collection
// needs? True when the tree is still at its own name AND `<name>.deleting` is there too -- the
// one state the collector cannot resolve by itself, because that pair is never of its making
// (see trash_mark_deleting below). `out` gets the name of the stray, which is what an operator
// has to be told: it is a directory in their own `.wfs-trash` as often as in the store's trash.
// Two probes, and "cannot tell" counts as there: a name we cannot prove absent is never renamed
// over either (12th round).
bool trash_blocked_by(const String &trash, String &out) {
    if (!trash.size() || ends_with(trash.c_str(), WFS_DELETING_SUFFIX)) return false;
    if (proven_gone(trash.c_str())) return false;
    String d(trash);
    d.append(WFS_DELETING_SUFFIX);
    if (proven_gone(d.c_str())) return false;
    out = d;
    return true;
}

// PR #1 review (20th round, P1): there used to be a step 0 here -- trash_fold_leftover(), which
// removed a `<name>.deleting` sitting beside the entry, recursively, as "an interrupted attempt
// of ours". It cannot be one. Since the 15th round the rename to `.deleting` and the row update
// that records it are ONE transaction (gc_claim_deleting, and trash_delete_now for `--now`), so
// by our own doing exactly one of the two names exists at any instant: a crash between the
// rename and the commit leaves the row naming X with the tree at X.deleting and X gone, which
// is trash_follow_deleting()'s case, and a discard (5th round: row, rename, row) never writes a
// `.deleting` name at all. So "X and X.deleting both there" is always somebody else's directory
// -- and a world discarded from another volume keeps its trash in `<parent>/.wfs-trash`, which
// is the user's own directory, under the entirely predictable name `W<id>-<timestamp>`. The
// fold deleted whatever was at `W<id>-<timestamp>.deleting` without one check that it was ours.
// Nothing folds anything now: a name we did not create is never removed and never renamed over.

// Step 1. `out` receives the name the tree now has (which may be the one it already had).
// PR #1 review (12th round): -ENOENT out of here means "at neither name, so somebody else
// finished it", and both of its callers bury the row on it. It was a bool over lstat(2), so an
// EACCES or an EIO said that too -- a DEAD row for a tree still sitting in the trash. Every
// errno but ENOENT/ENOTDIR is now returned as itself: gc counts the entry in trash_failed and
// retries it, and `--now` hands the reason to the caller.
int trash_mark_deleting(const char *path, String &out) {
    out.assign(path);
    if (ends_with(path, WFS_DELETING_SUFFIX)) {
        int prc = wfs::fs_probe(path);
        return wfs::fs_gone(prc) ? -ENOENT : prc;
    }
    if (int prc = wfs::fs_probe(path)) {
        if (!wfs::fs_gone(prc)) return prc;
        return -ENOENT;
    }
    String d(path);
    d.append(WFS_DELETING_SUFFIX);
    // PR #1 review (20th round, P1): and we only ever rename onto a name that is proven not to
    // be there. rename(2) would swallow an empty directory whole and answer ENOTEMPTY for a
    // full one, and the fold that used to run before this removed it recursively -- a user's
    // directory, in the user's own `.wfs-trash`, destroyed on a name coincidence. Whatever is
    // there is left exactly as it is; the entry is skipped and reported (WFS_E_TRASH_BLOCKED),
    // and a probe that cannot answer is not an absence either (12th round).
    if (int prc = wfs::fs_probe(d.c_str())) {
        if (!wfs::fs_gone(prc)) return prc;
    } else {
        return WFS_E_TRASH_BLOCKED;
    }
    if (int rc = wfs::fs_rename(path, d.c_str())) return rc;
    out = d;
    return 0;
}

// Step 2. `deadline_us` and `partial` are the gc worker's batch limit reaching all the way into
// the unlink walk (internal.h): a wake that runs out of time in the middle of a tree stops
// there, and the tree keeps its `.deleting` name for the next one.
int trash_unlink(const char *deleting_path, int threads, uint64_t *entries,
                 int64_t deadline_us = 0, int *partial = nullptr) {
    return wfs::fs_remove_tree_parallel(deleting_path, threads > 0 ? threads : 4, entries,
                                        deadline_us, partial);
}

// ---- PR #1 review (26th round, P1): the identity that was checked is the identity deleted ----
//
// The 25th round put an identity check in front of all this: lstat the entry, compare its
// dev/ino with the row's. What it did not change is that everything after the check is done by
// NAME -- trash_mark_deleting renames `j.path`, trash_unlink deletes `<j.path>.deleting` -- and
// those are separate file system calls. The transaction around them serialises this core's own
// writers and nothing else; the other party here is not a writer of ours but the owner of the
// directory the entry sits in, and for a world discarded across volumes that directory is the
// user's own `<parent>/.wfs-trash`. One `mv` between the lstat and the rename and the check
// passed on the world's tree while the rename and the recursive delete took a directory of
// theirs, contents and all (core_test, "the identity that was checked is the identity
// deleted": before this round the stranger's file was gone and gc reported `1 world deleted`).
//
// So the claim is a descriptor, not a name. The entry is opened (O_DIRECTORY|O_NOFOLLOW), the
// descriptor's dev/ino are what is compared with the row, and that descriptor is held for the
// whole job:
//
//   1. open + fstat: this fd IS the row's tree, or WFS_E_TRASH_FOREIGN and nothing is touched.
//   2. rename `<entry>` -> `<entry>.deleting`, by name, exactly as before. Harmless whatever it
//      moves: a rename moves a directory, it never removes one.
//   3. fstatat(parent_fd, "<leaf>.deleting") must be the fd from step 1. If it is not, the
//      rename moved somebody else's directory: it is renamed straight back, the entry is
//      reported foreign, and the transaction rolls back -- nothing was deleted, and the only
//      thing that happened to the stranger is that it had a different name for a few
//      microseconds.
//   4. the contents go through the fd (fs_remove_tree_fd): no name is resolved again, and no
//      symlink is ever followed, so nothing outside the tree we proved can be reached.
//   5. the entry itself: fstatat(parent_fd, "<leaf>.deleting") is checked against the fd once
//      more and then unlinkat(..., AT_REMOVEDIR). That last pair is the one place a name still
//      decides anything, and the bound on it is exact: AT_REMOVEDIR removes an empty directory
//      or nothing at all, so the worst a directory swapped in between those two calls can cost
//      is an empty directory -- no data can be lost there.
//
// What keeps the path-based remover, and why:
//   * snapshot trash entries. A snapshot's tree is always inside the store (<store>/trash), the
//     store allocates that directory, and a snapshot row has no dev/ino columns to check
//     against in the first place -- there is no identity here to keep, so there is nothing to
//     keep it through. They also carry UF_IMMUTABLE all the way down (`--hard`), which the
//     path-based remover unprotects in one walk.
//   * row-less orphans, for the first half of the same reason: a directory under <store>/trash
//     that no row claims has no row to be the tree of.
//   * a world row from before the dir_dev/dir_ino columns (nothing this store writes leaves
//     one): no identity was recorded, so none can be checked -- the 25th round's own rule.
// Those three keep the 4-thread deleter as well, which is where it matters most.
struct TrashClaim {
    int fd = -1;       // the entry, opened and proven to be the row's tree
    int parent = -1;   // the directory it sits in
    String leaf;       // its name under `parent` -- the `.deleting` one once step 2 has run
    uint64_t dev = 0, ino = 0;
    // PR #1 review (30th round, P2): the bits are LENT, not taken. trash_claim_open() chmods an
    // entry it cannot open (the 0000 gate, and any root whose owner left it searchable but not
    // readable -- 0311 is an ordinary mode for a directory meant to be entered and not listed),
    // and the mode it found is kept here so that every exit which does not delete the tree can
    // put it back. `restore` is the exit that has to: it opens the entry through this chmod,
    // renames the tree home and returns 0, so before this round a discard/restore round trip
    // silently rewrote the world root's mode to 0700 and left it there. The two refusals after
    // a lend (the rename verification finds a stranger and the tree is put back) are the same
    // debt, and the collector and `--now` have none: trash_unlink_claim() drops the lend the
    // moment the tree is actually gone, because there is nothing left to give it back to.
    bool lent = false;
    mode_t orig_mode = 0;
    TrashClaim() = default;
    TrashClaim(const TrashClaim &) = delete;
    TrashClaim &operator=(const TrashClaim &) = delete;
    ~TrashClaim() {
        return_lend();
        if (fd >= 0) ::close(fd);
        if (parent >= 0) ::close(parent);
    }
    bool held() const { return fd >= 0; }
    // Best effort, idempotent, and through the DESCRIPTOR: the mode goes back on the inode the
    // claim proved, wherever that inode's name is by now -- never on whatever a name resolves
    // to at this instant. That is the 26th round's rule applied to the way back out.
    void return_lend() {
        if (!lent) return;
        lent = false;
        if (fd >= 0) ::fchmod(fd, orig_mode);
    }
    // The tree is gone: the lend died with it.
    void forget_lend() { lent = false; }
};

// The same, for the two exits of trash_claim_open() that have no descriptor to give it back
// with -- the second open failed, or the fstat on it did. By name, so it is guarded the way the
// chmod out was: only an lstat that still shows the very inode we lent to gets its mode back. A
// stranger who has moved in since keeps whatever mode they have (27th round).
void trash_unlend_path(const char *path, uint64_t dev, uint64_t ino, mode_t mode) {
    struct stat st;
    if (::lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && (uint64_t)st.st_ino == ino &&
        (uint64_t)st.st_dev == dev)
        ::chmod(path, mode);
}

// Step 1. Returns 0 with the claim held, WFS_E_TRASH_FOREIGN when what is there is provably
// something else, and any other errno as itself -- including fs_gone(), which the callers read
// exactly as they read trash_identity's: a tree that is genuinely gone goes on to
// trash_mark_deleting and its -ENOENT, and the row is buried the way it always was.
int trash_claim_open(const char *path, uint64_t dev, uint64_t ino, TrashClaim &c) {
    bool lent = false;        // PR #1 review (30th round, P2): see TrashClaim::lent
    mode_t orig_mode = 0;
    int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        // A gate-protected root is 0000 and cannot be opened at all (docs/M1_DESIGN.md §3 P4);
        // the path-based deleter chmods it for the same reason. We are deleting the thing, so
        // opening the gate for good is right -- and the fstat below is what then says the thing
        // we opened is the row's tree.
        //
        // PR #1 review (27th round, P2): but the gate is opened only after what is standing
        // there has been shown to be ours. The chmod used to come first and the identity check
        // after it, so a directory somebody else had put at this name -- and for a world
        // discarded across volumes the entry sits in the user's own `<parent>/.wfs-trash`,
        // whose names they can work out -- was chmod'ed to 0700 on its way to being refused as
        // foreign. A refusal may not leave a mark on the thing it refuses: a 0000 directory is
        // 0000 because its owner wants it shut, and gc, `discard --now` and `restore` all come
        // through here.
        //
        // The check in front of the chmod has to be made on the name, because there is no
        // descriptor to make it on yet: a 0000 directory cannot be opened at all on this
        // platform, not even for a stat -- O_EVTONLY|O_DIRECTORY|O_NOFOLLOW is refused with
        // EACCES exactly as O_RDONLY is (measured, Darwin 27/APFS, as the owner), and macOS has
        // no O_PATH. So it is lstat, compare, chmod, open -- and the fstat below then proves
        // the identity a second time, on the descriptor every later step actually uses. The
        // bound on what a swap between the lstat and the chmod can achieve is exact, and it is
        // not a deletion: a stranger moved in there gets its mode set to 0700, after which the
        // fstat refuses it and nothing else is done to it. Nothing of theirs can be renamed or
        // unlinked, because from here on only the descriptor decides (26th round).
        int oe = errno;
        struct stat lst;
        if (::lstat(path, &lst) != 0) {
            errno = oe;   // why the open failed is the answer here; why the lstat did is not
        } else if (!S_ISDIR(lst.st_mode) || (uint64_t)lst.st_ino != ino ||
                   (uint64_t)lst.st_dev != dev) {
            return WFS_E_TRASH_FOREIGN;
        } else {
            // PR #1 review (30th round, P2): and the mode it had is remembered here, because
            // this is a loan. The caller that deletes the tree owes nothing; every other one
            // gives it back (TrashClaim::return_lend, and the two exits below).
            orig_mode = (mode_t)(lst.st_mode & 07777);
            if (::chmod(path, 0700) == 0) lent = true;
            fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
    }
    if (fd < 0) {
        int e = -errno;
        if (lent) trash_unlend_path(path, dev, ino, orig_mode);
        // ENOTDIR (not a directory) and ELOOP (a symlink, which O_NOFOLLOW refuses) are not
        // absences: a world tree is a directory, so anything else standing at that name is
        // provably not it. fs_probe tells the two apart -- "nothing is there" keeps the old
        // -ENOENT path, and an errno that cannot answer is returned as itself (12th round).
        if (wfs::fs_gone(e) || e == -ELOOP) {
            int prc = wfs::fs_probe(path);
            if (prc) return prc;
            return WFS_E_TRASH_FOREIGN;
        }
        return e;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        int e = -errno;
        ::close(fd);
        if (lent) trash_unlend_path(path, dev, ino, orig_mode);
        return e;
    }
    if ((uint64_t)st.st_ino != ino || (uint64_t)st.st_dev != dev) {
        ::close(fd);
        // A swap landed between the chmod and the open. The descriptor is the stranger's and
        // gets nothing done to it; the lend is offered back by name, which by construction
        // reaches our tree only if it is still standing there -- and if it is not, the bound is
        // the 27th round's unchanged one: a mode, never a rename and never an unlink.
        if (lent) trash_unlend_path(path, dev, ino, orig_mode);
        return WFS_E_TRASH_FOREIGN;
    }
    c.fd = fd;
    c.dev = (uint64_t)st.st_dev;
    c.ino = (uint64_t)st.st_ino;
    c.lent = lent;
    c.orig_mode = orig_mode;
    return 0;
}

// Opens the directory `path` sits in and records its leaf name, so that every later step can be
// taken relative to a descriptor. O_NOFOLLOW: a symlink standing in for <store>/trash or for a
// `.wfs-trash` is refused rather than followed.
int trash_claim_parent(TrashClaim &c, const char *path) {
    String dir;
    dirname_of(path, dir);
    c.leaf.assign(basename_of(path));
    if (c.parent >= 0) {
        ::close(c.parent);
        c.parent = -1;
    }
    c.parent = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return c.parent < 0 ? -errno : 0;
}

// Is `c.leaf` (under `c.parent`) still the tree the claim was made on?
int trash_claim_is_ours(const TrashClaim &c, bool *gone = nullptr) {
    if (gone) *gone = false;
    struct stat st;
    if (::fstatat(c.parent, c.leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = -errno;
        if (wfs::fs_gone(e) && gone) *gone = true;
        return e;
    }
    if ((uint64_t)st.st_ino != c.ino || (uint64_t)st.st_dev != c.dev) return WFS_E_TRASH_FOREIGN;
    return 0;
}

// Step 3. `renamed` is false when the entry was already at its `.deleting` name and step 2 did
// nothing, in which case there is no rename to undo.
int trash_claim_verify_rename(TrashClaim &c, const char *deleting, bool renamed) {
    if (int rc = trash_claim_parent(c, deleting)) return rc;
    int vrc = trash_claim_is_ours(c);
    if (vrc != WFS_E_TRASH_FOREIGN) return vrc;
    if (renamed) {
        // Somebody else's directory went to the `.deleting` name. Put it back where they had
        // it. Best effort: if the old name has been filled in the meantime the rename fails and
        // the directory stays where it is, under a name the operator is told about -- nothing
        // of theirs has been removed either way.
        String back(c.leaf);
        back.resize(back.size() - ::strlen(WFS_DELETING_SUFFIX));
        ::renameat(c.parent, c.leaf.c_str(), c.parent, back.c_str());
    }
    // PR #1 review (30th round, P2): and this claim deletes nothing, so anything it borrowed on
    // the way in goes back. The tree that was lent to is ours (a stranger is never lent to --
    // 27th round) and it is wherever the swap put it; the fchmod is on the descriptor, so it
    // finds it there. ~TrashClaim would do it a few lines later in both callers anyway; saying
    // it here is what makes "a refusal gives the lend back" a property of the refusal itself.
    c.return_lend();
    return WFS_E_TRASH_FOREIGN;
}

// Steps 4 and 5.
int trash_unlink_claim(TrashClaim &c, uint64_t *entries, int64_t deadline_us = 0,
                       int *partial = nullptr) {
    if (int rc = wfs::fs_remove_tree_fd(c.fd, 0, entries, deadline_us, partial)) return rc;
    // The deadline: the tree keeps its `.deleting` name, and keeps the lend with it -- the
    // claim is about to be dropped, so ~TrashClaim gives the mode back and the next wake's
    // claim borrows it again (PR #1 review, 30th round). A tree we did not finish deleting is
    // left exactly as we found it, which is the whole of this round's rule.
    if (partial && *partial) return 0;
    bool gone = false;
    int vrc = trash_claim_is_ours(c, &gone);
    if (gone) { c.forget_lend(); return 0; }   // somebody else finished it; it is not there
    if (vrc) return vrc;
    // The only name-decided step left, and its bound is exact: AT_REMOVEDIR removes an empty
    // directory or nothing at all, so a directory swapped in between the fstatat above and this
    // call can cost an empty directory and cannot cost one byte of anybody's data.
    if (::unlinkat(c.parent, c.leaf.c_str(), AT_REMOVEDIR) == 0 || errno == ENOENT) {
        c.forget_lend();   // 30th round: nothing left to give the mode back to
        return 0;
    }
    int e = errno;
    if (e == EPERM || e == EACCES) {
        struct stat pst;
        if (::fstat(c.parent, &pst) == 0 && (pst.st_mode & 0700) != 0700)
            ::fchmod(c.parent, (mode_t)((pst.st_mode & 07777) | 0700));
#ifdef __APPLE__
        ::fchflags(c.parent, 0);
        ::fchflags(c.fd, 0);
#endif
        if (::unlinkat(c.parent, c.leaf.c_str(), AT_REMOVEDIR) == 0 || errno == ENOENT) {
            c.forget_lend();
            return 0;
        }
        e = errno;
    }
    return -e;
}

} // namespace

namespace {

// PR #1 review (5th round): what a snapshot may say about itself is what its own tree has. The
// groups the replay did not leave whole (hardlinks.h, HardlinkRestore::broken) come out of the
// set before the manifest is written and before hl_groups goes on the row. Linear in the groups
// and the handful of broken indices, and in the ordinary case it is not called at all.
void hl_drop_broken(wfs::HardlinkSet &hl, const Vec<uint64_t> &broken) {
    wfs::HardlinkSet kept;
    kept.external_groups = hl.external_groups;
    kept.external_names = hl.external_names;
    for (size_t i = 0; i < hl.groups.size(); ++i) {
        bool drop = false;
        for (size_t j = 0; j < broken.size(); ++j)
            if (broken[j] == (uint64_t)i) { drop = true; break; }
        if (drop) continue;
        kept.names += (uint64_t)hl.groups[i].paths.size();
        kept.groups.emplace_back(hl.groups[i]);
    }
    hl = std::move(kept);
}

// --committed-only resets the clone to HEAD after the replay, and Git rewrites a file by
// replacing it: a hardlinked tracked file that differed from HEAD, or an untracked one that was
// removed, leaves its group no longer whole in this tree. Such groups come out of the set for
// the same reason as hl_drop_broken's -- the manifest must describe the tree it is written for.
void hl_drop_changed(const char *root, wfs::HardlinkSet &hl) {
    Vec<uint64_t> broken;
    for (size_t i = 0; i < hl.groups.size(); ++i) {
        const auto &g = hl.groups[i];
        struct stat first;
        bool whole = !g.paths.empty();
        for (size_t j = 0; whole && j < g.paths.size(); ++j) {
            String p = joinp(root, g.paths[j].c_str());
            struct stat st;
            if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) whole = false;
            else if (j == 0) first = st;
            else if (st.st_dev != first.st_dev || st.st_ino != first.st_ino) whole = false;
        }
        if (!whole) broken.emplace_back((uint64_t)i);
    }
    if (broken.size()) hl_drop_broken(hl, broken);
}

} // namespace

// The one interleaving a test cannot produce from outside: the middle of a pool-backed fork,
// after the claim transaction has committed this fork's CREATING world row and before the
// marker/rename/ACTIVE tail. core_test sets it to run `discard` exactly there; nothing in the
// library ever assigns it, and the pool path pays one predictable branch for it.
extern "C" void (*wfs_test_after_pool_claim)(void *ctx, wfs_id world) = nullptr;
extern "C" void *wfs_test_after_pool_claim_ctx = nullptr;

// And the other one: the instant before the publish rename, with the CREATING row and the clone
// it recorded both on disk. Returning non-zero makes wfs_world_create() return that code without
// unwinding anything, which is what a `kill -9` there looks like to the next process.
extern "C" int (*wfs_test_before_fork_publish)(void *ctx, wfs_id world, const char *tmp_path) = nullptr;
extern "C" void *wfs_test_before_fork_publish_ctx = nullptr;
extern "C" void (*wfs_test_before_hl_replay)(void *ctx, wfs_id world, const char *tmp_path) = nullptr;
extern "C" void *wfs_test_before_hl_replay_ctx = nullptr;

// And the unwind of a pool hand-out that failed (PR #1 review, 16th round): the entry is out of
// the pool, its tree is back under its pool name, and nothing but this fork's CREATING world row
// says the snapshot is still needed. core_test runs `discard S<n>` exactly there.
extern "C" void (*wfs_test_in_pool_unwind)(void *ctx) = nullptr;
extern "C" void *wfs_test_in_pool_unwind_ctx = nullptr;

// And the instant after the publish rename of a pool hand-out, with the entry's tree sitting at
// the user's --to under a marker and the row that owns it not committed yet (PR #1 review, 18th
// round). core_test moves the tree away in there, which is the one way to make the unwind's own
// rollback rename fail.
extern "C" void (*wfs_test_after_pool_publish)(void *ctx, wfs_id world, const char *target) = nullptr;
extern "C" void *wfs_test_after_pool_publish_ctx = nullptr;

// And the window between a snapshot's walk of its source and the clone of it: the source is the
// user's live directory, and nothing stops it changing in there.
extern "C" void (*wfs_test_before_snapshot_clone)(void *ctx, const char *src_dir) = nullptr;
extern "C" void *wfs_test_before_snapshot_clone_ctx = nullptr;

// And the pid a CREATING row records as its producer: 0 (always, outside a test) means getpid().
extern "C" int64_t wfs_test_fork_owner_pid = 0;

// And the two halves of a discard (PR #1 review, 5th round): phase 0 is the row committed in
// TRASHING with the tree still at home, phase 1 is the tree renamed with the row not yet
// TRASHED. A non-zero return comes straight back out of the discard with nothing unwound.
extern "C" int (*wfs_test_trash_crash)(void *ctx, int phase, int is_snapshot, wfs_id id,
                                       const char *trash_path) = nullptr;
extern "C" void *wfs_test_trash_crash_ctx = nullptr;

// And the window the trash collector has between the scan that queued an entry and the rename
// that starts deleting it (PR #1 review, 8th round): what a test has to be able to do in there
// is a whole `restore` of the row the collector is holding.
extern "C" void (*wfs_test_before_trash_delete)(void *ctx, int is_snapshot, wfs_id row,
                                                const char *path) = nullptr;
extern "C" void *wfs_test_before_trash_delete_ctx = nullptr;

// And the window INSIDE the claim (PR #1 review, 26th round): between the identity check on the
// trash entry and the rename that takes it, which is the one seam the store's own transaction
// cannot serialise -- the other side of it is a user with a `mv`, not another writer. What a
// test does in there is move the genuine tree away and put a directory of its own at the name.
extern "C" void (*wfs_test_between_trash_claim)(void *ctx, int is_snapshot, wfs_id row,
                                                const char *path) = nullptr;
extern "C" void *wfs_test_between_trash_claim_ctx = nullptr;

// And the window the collector's own *scan* has (PR #1 review, 9th round): between the snapshot
// of the trash paths the rows claim and the readdir that decides what nothing claims. A
// `discard` that lands in there leaves a tree that snapshot has never heard of.
extern "C" void (*wfs_test_before_trash_orphans)(void *ctx) = nullptr;
extern "C" void *wfs_test_before_trash_orphans_ctx = nullptr;

// And reconciliation's window (PR #1 review, 9th round): between the scan that finds an ACTIVE
// row with no tree at its recorded path and the update that buries it. A `world fs verify` on
// the path the directory was moved to relocates the row by inode in exactly that window.
extern "C" void (*wfs_test_before_reconcile)(void *ctx) = nullptr;
extern "C" void *wfs_test_before_reconcile_ctx = nullptr;

// ---- snapshots --------------------------------------------------------------------------------

extern "C" int wfs_snapshot_create(wfs_store *s, const char *src_dir, const wfs_snapshot_opts *opts,
                                   wfs_id *out) {
    if (!s || !src_dir || !out) return -EINVAL;
    *out = 0;
    wfs_snapshot_opts o;
    memset(&o, 0, sizeof o);
    if (opts) o = *opts;
#ifdef __linux__
    if (o.hard) return -ENOTSUP;
#endif
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
    wfs::GitSource git_source;
    if (int rc = wfs::git_source(src.c_str(), o.include_changes != 0, git_source, o.committed_only != 0)) return rc;

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
    // PR #1 review (14th round, P2): and the marker this checkpoint is about to unlink out of
    // the clone (below) is excluded from that walk. It is not a file of the tree this snapshot
    // publishes, so a marker somebody's tool had hardlinked must not put a name into a group
    // the published tree cannot have: the scan recorded the group, the marker was then removed,
    // and the snapshot went out advertising a member it does not contain -- WFS_E_SNAPSHOT_DIRTY
    // from every verify and every fork afterwards. Exactly the name that is removed, so a
    // `.world` in a sub-world (which is not removed) is untouched.
    // An external Git import replaces the source's `.git` (directory or gitfile) with owned
    // administration, so its names are not files of the published tree either: excluding them
    // here keeps the groups and the external counts about exactly the tree that is published,
    // including worktree files linked from outside it (a rescan of the clone would lose those).
    const char *git_admin = git_source.present && !git_source.managed ? ".git" : nullptr;
    if (int rc = wfs::hardlinks_scan(src.c_str(), WFS_MARKER_NAME, &src_stats, hl, git_admin)) return rc;
    if (int rc = space_check(s->dir.c_str(), src_stats.entries, git_source.import_bytes)) return rc;

    char nm[WFS_NAME_MAX];
    copy_str(nm, sizeof nm, (name && *name) ? name : basename_of(src.c_str()));

    wfs_id id = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        int64_t opid = 0, ostart = 0;
        owner_now(opid, ostart);
        Stmt ins(s->db,
                 "INSERT INTO snapshots(name, path, src_path, from_world, created_at, state, hard,"
                 " owner_pid, owner_start) VALUES(?,'',?,?,?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.text(1, nm);
        ins.text(2, src.c_str());
        ins.i64(3, (int64_t)from_world);
        ins.i64(4, now_sec());
        ins.i64(5, WFS_ST_CREATING);
        ins.i64(6, o.hard ? 1 : 0);
        ins.i64(7, opid);
        ins.i64(8, ostart);
        if (ins.step() != SQLITE_DONE) return -EIO;
        id = (wfs_id)sqlite3_last_insert_rowid(s->db);
        if (int crc = t.commit()) return crc;
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
        if (wfs_test_before_snapshot_clone)
            wfs_test_before_snapshot_clone(wfs_test_before_snapshot_clone_ctx, src.c_str());
        if ((rc = wfs::fs_clone_tree(src.c_str(), root.c_str(), false))) break;
        // A checkpoint carries the source world's marker; it is not this snapshot's identity.
        String m = joinp(root.c_str(), WFS_MARKER_NAME);
        ::unlink(m.c_str());
        // P9: the clone broke every hardlink the source had. Put them back before anything
        // else looks at the tree, so the manifest below records the nlinks this snapshot
        // really has and a fork from it starts from a faithful copy. The source may be a live
        // world, so a name that moved between the scan and the clone is tolerated, not fixed.
        // PR #1 review (4th round): a replay the file system refused is not tolerated. The
        // manifest written below and the hl_groups on the row would then describe a tree that
        // does not exist, so the snapshot fails here and the half-built tree goes away with it.
        // PR #1 review (5th round): and the live source is the verify root, not nullptr. The
        // scan and the clone are two separate walks of a tree the user may be writing to, and
        // nullptr disabled the only check that notices -- a member replaced in that window with
        // a file of the same size and the same mtime was linked over, silently, with the
        // snapshot then holding one name's contents under both names. A group the source has
        // broken since the scan is left alone here...
        if (hl.groups.size() && (rc = wfs::hardlinks_restore(root.c_str(), hl, src.c_str(), &hlr))) break;
        // ... and dropped from what this snapshot claims about itself. A fork replays the
        // manifest against the snapshot without a verify root (a snapshot is immutable), so a
        // group left in there would be rebuilt one step later, in the fork, over the very
        // contents this replay declined to overwrite.
        if (hlr.broken.size()) hl_drop_broken(hl, hlr.broken);
        // The source scan already left the replaced `.git` out, so `hl` describes this tree.
        if ((rc = wfs::git_import(git_source, root.c_str()))) break;
        if (git_source.committed_only && hl.groups.size()) hl_drop_changed(root.c_str(), hl);
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
        // 34th round, P1: no transaction, no tidying up. The row stays CREATING with no tree,
        // which is the shape gc's half-built-snapshot sweep is for, and what the caller is owed
        // is the error that brought us here rather than one about the clean-up.
        if (t.ok()) {
            Stmt del(s->db, "DELETE FROM snapshots WHERE id=?");
            if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
            (void)t.commit();   // best effort: the sweep is the backstop either way
        }
        return rc;
    }
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
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
        // 34th round, P1: this row IS the publish -- it is what makes the tree at S<n> an ACTIVE
        // snapshot. A COMMIT that failed leaves it CREATING, and handing back an id for it would
        // be reporting a snapshot the store does not have.
        if (int crc = t.commit()) return crc;
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
    if (!q.done()) return -EIO;   // 32nd round, P2: a listing that stopped is not a listing
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
// The other half of the same arrangement (PR #1 review, 16th round): the row this fork wrote
// with its claim, deleted inside pool_return's BEGIN IMMEDIATE, in the transaction that puts the
// entry back. While the entry is out of the pool this row is the snapshot's only reference, so
// it can go out only as the pool row comes in.
struct PoolUnwind {
    wfs_store *s;
    wfs_id world;
};

int pool_fork_unwind(void *ctx, const wfs::PoolClaim &c) {
    (void)c;
    PoolUnwind *u = (PoolUnwind *)ctx;
    if (!u->world) return 0;
    Stmt del(u->s->db, "DELETE FROM worlds WHERE id=?");
    if (!del.ok()) return -EIO;
    del.i64(1, (int64_t)u->world);
    if (del.step() != SQLITE_DONE) return -EIO;
    return 0;
}

int pool_fork_insert(void *ctx, const wfs::PoolClaim &c) {
    PoolForkRow *r = (PoolForkRow *)ctx;
    {
        Stmt q(r->s->db, "SELECT state FROM snapshots WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)r->snapshot);
        if (!q.row()) return q.done() ? -ESTALE : -EIO;   // 32nd round, P2
        if (q.col_i64(0) != WFS_ST_ACTIVE) return -ESTALE;
    }
    int64_t opid = 0, ostart = 0;
    owner_now(opid, ostart);
    // tmp_path is the pool entry itself: it is where this fork's tree is until the rename, and
    // it is what keeps pool_collect from sweeping a claimed entry out from under a live fork.
    Stmt ins(r->s->db,
             "INSERT INTO worlds(kind, parent_world, snapshot_id, name, path, state,"
             " fsevents_id, entries, created_at, tmp_path, owner_pid, owner_start)"
             " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
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
    ins.text(10, c.path.c_str());
    ins.i64(11, opid);
    ins.i64(12, ostart);
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
        // PR #1 review (6th round): and only while that snapshot is ACTIVE. Reading the manifest
        // is fatal when it fails from here on (see the replay below), and a snapshot that has
        // been trashed or reconciled away has no manifest where its row says any more -- a world
        // whose baseline is gone is still a world, and forking it must not fail for the sake of
        // an optimisation. Without the groups the clone simply has none of them rebuilt, which
        // is what a `checkpoint` of the world then records properly.
        if (snapshot_id) {
            wfs_snapshot_rec sn;
            Guard g2(s->mu);
            if (snapshot_row(s, snapshot_id, sn) == 0 && sn.state == WFS_ST_ACTIVE && sn.hl_groups) {
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
    wfs::GitSource git_source;
    bool git_snapshot = false;
    if (from.kind == WFS_K_WORLD) {
        if (int rc = wfs::git_source(src.c_str(), o.include_changes != 0, git_source, o.committed_only != 0)) return rc;
    } else {
        // A snapshot's content was fixed when it was taken; there is no working state to leave
        // behind. Refused rather than ignored, since the caller asked for something specific.
        if (o.committed_only) return -EINVAL;
        SnapGate gate;
        if (src_gated) { if (int rc = gate.open(src.c_str(), false)) return rc; }
        String dot = joinp(src.c_str(), ".git"); struct stat gst;
        if (::lstat(dot.c_str(), &gst) == 0) git_snapshot = true;
        else if (errno != ENOENT) return -errno;
    }
    // Git branch setup mutates the clone. A failed setup must never return a modified pool
    // entry to READY; use the normal CREATING temporary-tree rollback path for Git worlds.
    if (git_snapshot || git_source.present) o.no_pool = 1;
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
            // P7 again: the entry is store-internal, the target is not. RENAME_EXCL, so a
            // directory that appeared at `--to` since check_path looked is EEXIST and the entry
            // goes back into the pool -- never a replacement, whatever raced us.
            bool renamed = false;
            if (!rc && (rc = wfs::fs_rename_excl(claim.path.c_str(), target.c_str())) == 0)
                renamed = true;
            if (renamed && wfs_test_after_pool_publish)
                wfs_test_after_pool_publish(wfs_test_after_pool_publish_ctx, id, target.c_str());
            struct stat st;
            if (!rc && ::stat(target.c_str(), &st) != 0) rc = -errno;
            if (!rc) {
                Guard g(s->mu);
                Txn t(s->db);
                Stmt u(s->db, "UPDATE worlds SET dir_dev=?, dir_ino=?, state=?, tmp_path='',"
                              " owner_pid=0, owner_start=0 WHERE id=? AND state=?");
                if (!t.ok()) rc = t.err();   // 34th round, P1
                else if (!u.ok()) rc = -EIO;
                else {
                    u.i64(1, (int64_t)st.st_dev);
                    u.i64(2, (int64_t)st.st_ino);
                    u.i64(3, WFS_ST_ACTIVE);
                    u.i64(4, (int64_t)id);
                    u.i64(5, WFS_ST_CREATING);
                    // The row has to still be there (PR #1 review, 3rd round). If it is not,
                    // the tree goes back into the pool below rather than being left at --to
                    // with nothing in the database that knows about it.
                    if (u.step() != SQLITE_DONE) rc = -EIO;
                    else if (sqlite3_changes(s->db) != 1) rc = -ESTALE;
                    else if (int crc = t.commit()) rc = crc;   // and the entry goes back below
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
            // The rename may already have happened (the failure was the stat or the row); put
            // the tree back under its pool name first, or pool_return would find nothing there
            // and the entry would be left at --to with no row that knows about it.
            //
            // PR #1 review (18th round, P2): and that rollback is a rename that can fail. Its
            // result was dropped, so pool_return() then stat'ed a pool path with nothing at it,
            // returned -ENOENT, and the branch below read that as "the entry did not go back,
            // so the row goes on its own" and deleted the CREATING row -- while the tree sat at
            // the user's --to with a `.world` marker in it and nothing in the store naming it.
            // Not gc's abandoned-fork sweep, which works from CREATING rows and their tmp_path
            // (5th/7th rounds), and not `world fs adopt`, whose marker has to name a world this
            // store has. The fork then fell through and cloned a SECOND tree over the same --to.
            //
            // So a rollback that did not happen ends the fork here: the row stays CREATING and
            // its tmp_path is moved to the tree's real name, which is the published target. The
            // row is then exactly the row a fork killed before its publish leaves -- the tree's
            // only name in the database -- and gc_tmp_is_removable(), which asks the marker who
            // it belongs to and the row whether it still says CREATING with that tmp_path,
            // accepts it as it stands. The caller gets the failure that started all this rather
            // than a fresh clone beside a tree nobody owns.
            if (renamed) {
                if (wfs::fs_rename_excl(target.c_str(), claim.path.c_str()) != 0) {
                    Guard g(s->mu);
                    Txn t(s->db);
                    // 34th round, P1: with no transaction the row keeps the tmp_path it has --
                    // the pool entry's name, which pool_collect() will not sweep while a
                    // CREATING row claims it -- and the caller still gets the original failure.
                    if (t.ok()) {
                        Stmt u(s->db, "UPDATE worlds SET tmp_path=? WHERE id=? AND state=?");
                        if (u.ok()) {
                            u.text(1, target.c_str());
                            u.i64(2, (int64_t)id);
                            u.i64(3, WFS_ST_CREATING);
                            if (u.step() == SQLITE_DONE) (void)t.commit();   // best effort
                        }
                    }
                    return rc;
                }
            }
            if (wfs_test_in_pool_unwind) wfs_test_in_pool_unwind(wfs_test_in_pool_unwind_ctx);
            // PR #1 review (16th round, P2): the CREATING row goes out in the very transaction
            // the entry comes back in. It used to be deleted first, and in the gap between the
            // two commits nothing at all referenced the snapshot: `discard S<n>` counted no
            // world and no pool row, committed WFS_ST_TRASHING, and the return then inserted a
            // READY entry for a snapshot on its way to the trash -- a whole stale clone, handed
            // to the next fork as a live baseline until pool_collect() got to it. A reference
            // exists at every instant between the claim and the return now: the world row until
            // the commit, the pool row after it.
            PoolUnwind uw{s, id};
            if (wfs::pool_return(s, claim, pool_fork_unwind, &uw) && id) {
                // The entry did not go back -- pool_return has removed its tree -- so nothing
                // references the snapshot through it any more and the row goes on its own.
                Guard g(s->mu);
                Txn t(s->db);
                // 34th round, P1: falling through to a fresh clone with that row still CREATING
                // would put a second tree at --to under a row that names neither. The fork ends
                // here instead; the row is the abandoned-fork shape gc already collects.
                if (!t.ok()) return rc;
                Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
                if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
                (void)t.commit();   // best effort: gc's CREATING sweep is the backstop
            }
            // ... and fall through to cloning it here and now
        }
    }

    // P6 between the source volume and the target volume, which need not be the store's.
    // The probe reads a file out of the source, so a gated snapshot has to be opened for it.
    if (!o.allow_fallback) {
        SnapGate probe_gate;
        if (src_gated) { if (int rc = probe_gate.open(src.c_str(), false)) return rc; }
        int rc = wfs::fs_clone_probe(parent_dir.c_str(), src.c_str());
        probe_gate.close();
        if (rc == -EXDEV) return WFS_E_CROSS_VOLUME;
#ifdef __APPLE__
        if (rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
#endif
        if (rc) return rc;
    }
    if (!o.skip_space_check) {
        if (int rc = space_check(parent_dir.c_str(), entries, git_source.import_bytes)) return rc;
    }

    int64_t created = now_sec();
    uint64_t ev = wfs::fs_events_current_id();

    wfs_id id = 0;
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        // The same window as the pool path's, and the same answer: under the write lock, either
        // the snapshot is still ACTIVE and this CREATING row makes the fork visible to
        // `discard`, or a discard got here first and there is nothing to fork from. (A fork
        // from a *world* is not asked: its snapshot is only the diff baseline, and `gc
        // --reconcile` is allowed to bury a snapshot whose tree is gone while worlds live on.)
        if (from.kind == WFS_K_SNAPSHOT) {
            Stmt q(s->db, "SELECT state FROM snapshots WHERE id=?");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)snapshot_id);
            if (!q.row()) return q.done() ? -ESTALE : -EIO;   // 32nd round, P2
            if (q.col_i64(0) != WFS_ST_ACTIVE) return -ESTALE;
        }
        int64_t opid = 0, ostart = 0;
        owner_now(opid, ostart);
        Stmt ins(s->db,
                 "INSERT INTO worlds(kind, parent_world, snapshot_id, name, path, state,"
                 " fsevents_id, entries, created_at, owner_pid, owner_start)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?)");
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
        ins.i64(10, opid);
        ins.i64(11, ostart);
        if (ins.step() != SQLITE_DONE) return -EIO;
        id = (wfs_id)sqlite3_last_insert_rowid(s->db);
        // 34th round, P1: nothing is cloned until this row is committed -- it is the only name
        // the tree about to be built will have.
        if (int crc = t.commit()) return crc;
    }

    // The clone goes to a name of ours in the target's parent directory, recorded on the row
    // before it exists. Nothing that was already there is ever removed: EEXIST is answered with
    // another name, and after kTmpTries draws of 64 random bits we give up rather than insist.
    String tmp;
    bool tmp_is_ours = false;   // did our own clonefile create it?
    int rc = 0;
    do {
        for (int t = 0; t < kTmpTries; ++t) {
            char leaf[64];
            fork_tmp_leaf(leaf, sizeof leaf);
            tmp = joinp(parent_dir.c_str(), leaf);
            if (exists(tmp.c_str())) { rc = -EEXIST; continue; }
            // Before the clone, so a crash between here and the rename leaves a CREATING row
            // that names the leftover tree (P8).
            if ((rc = world_set_tmp_path(s, id, tmp.c_str()))) break;
            {
                // T1.1b: the whole cost of forking from a gated snapshot is this one clonefile.
                // The gate is open for exactly its duration and for nothing else.
                SnapGate gate;
                if (src_gated && (rc = gate.open(src.c_str(), false))) break;
                rc = wfs::fs_clone_tree(src.c_str(), tmp.c_str(), o.allow_fallback != 0);
            }
            if (rc == -EEXIST) continue;   // somebody else's name, drawn against all odds
            tmp_is_ours = true;
            break;
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
        // marker and the rename, so a crash here leaves nothing but the recorded temp tree.
        if (hl_groups) {
            wfs::HardlinkSet hl;
            // PR #1 review (6th round): a manifest that cannot be read, or that holds fewer
            // groups than the row says, is a failed fork -- not a fork without hardlinks. The
            // read error and the short manifest were both swallowed here, and the tree was then
            // published with independent files where the snapshot records one inode under n
            // names: the very thing P9 exists to prevent, and invisible afterwards, because
            // nothing downstream re-reads the manifest. hl_groups and the manifest's group
            // count are written from the same HardlinkSet, after the broken groups have been
            // dropped from both (wfs_snapshot_create), so they agree in every store this code
            // ever wrote; disagreeing means the manifest has been damaged since.
            if ((rc = wfs::hardlinks_manifest_read(hl_manifest.c_str(), hl))) {
                rc = WFS_E_SNAPSHOT_DIRTY;
                break;
            }
            if (hl.groups.size() != hl_groups) { rc = WFS_E_SNAPSHOT_DIRTY; break; }
            // PR #1 review (13th round, P2): and the groups have to be the tree's groups. Every
            // check the reader makes is a count, and a manifest whose members were exchanged
            // between two groups -- (a,b),(c,d) written as (a,c),(b,d) -- keeps all of them; the
            // replay then welds `a` to `c` and loses one file's content, in a fork that reported
            // success. The snapshot tree is the immutable original, so it is the authority: one
            // lstat per hardlinked name, inside its own gate window (the root is mode 0000 until
            // the gate opens), before anything is linked in the clone. A fork from a live WORLD
            // keeps the 5th round's arrangement instead -- its verify root is the world, checked
            // group by group inside the replay, because a live tree is allowed to have moved on.
            if (hl.groups.size() && from.kind == WFS_K_SNAPSHOT) {
                wfs::SnapGate vgate;
                if (src_gated) rc = vgate.open(src.c_str(), false);
                // PR #1 review (23rd round): -EINVAL is "this snapshot is not what its
                // manifest says"; any other errno is a tree we could not read, and saying
                // SNAPSHOT_DIRTY about an EACCES would send the user off to rebuild a
                // perfectly good snapshot.
                if (!rc) {
                    int vrc = wfs::hardlinks_verify_groups(src.c_str(), hl);
                    if (vrc == -EINVAL) rc = WFS_E_SNAPSHOT_DIRTY;
                    else if (vrc) rc = vrc;
                }
                vgate.close();
                if (rc) break;
            }
            if (hl.groups.size()) {
                wfs::HardlinkRestore hr;
                // The test seam for "a member of the clone cannot be looked at" (PR #1 review,
                // 23rd round): the clone is here, nothing has been linked yet.
                if (wfs_test_before_hl_replay)
                    wfs_test_before_hl_replay(wfs_test_before_hl_replay_ctx, id, tmp.c_str());
                rc = wfs::hardlinks_restore(tmp.c_str(), hl, hl_verify, &hr);
                res->hardlinks = hr.links;
                // PR #1 review (4th round): the fork's own tree would disagree with the
                // snapshot it claims to be a copy of. Unwind rather than publish it.
                if (rc) break;
            }
        }
        if ((rc = wfs::git_import(git_source, tmp.c_str()))) break;
        if ((rc = wfs::git_branch(tmp.c_str(), id))) break;
        if ((rc = marker_write(tmp.c_str(), s->store_id.c_str(), s->dir.c_str(), id, nm, snapshot_id, parent_world, created)))
            break;
        // The test seam for "the process died here": the CREATING row and the tree it names are
        // left exactly as they are, which is what gc has to be able to clean up.
        if (wfs_test_before_fork_publish) {
            int hrc = wfs_test_before_fork_publish(wfs_test_before_fork_publish_ctx, id, tmp.c_str());
            if (hrc) return hrc;
        }
        // P7: the target is the user's path. A directory that appeared there since check_path
        // looked is a refusal, not something to rename over.
        if ((rc = wfs::fs_rename_excl(tmp.c_str(), target.c_str()))) break;
    } while (0);

    if (rc) {
        // Only ever our own clone. A name we drew but never created (EEXIST, or a gate that
        // would not open) belongs to whoever else is holding it.
        if (tmp_is_ours) {
            wfs::fs_remove_tree(tmp.c_str());
            // PR #1 review (20th round, P2): and a tree that could not be removed is not a tree
            // that was removed -- the same rule as the 5th, 7th, 8th, 12th, 16th and 18th
            // rounds, in the one place that still threw the result away. This tmp tree lives in
            // the user's own target directory under a name drawn from 64 random bits, and the
            // CREATING row is the ONLY record of that name anywhere: gc deliberately never
            // sweeps a user directory by suffix (rm_tmp_in_store_dir). Deleting the row behind
            // a failed removal stranded the whole clone for good, invisible to gc --status, to
            // `gc` and to `adopt`. So the row stays CREATING with its tmp_path -- which is
            // exactly the row a fork killed before its publish leaves, the shape
            // gc_tmp_is_removable() accepts (marker names this world, row still CREATING and
            // still naming this tree) and gc --status counts in creating_stranded -- and the
            // caller gets the error that started all this.
            if (!proven_gone(tmp.c_str())) {
                if (rc == -EXDEV) return WFS_E_CROSS_VOLUME;
#ifdef __APPLE__
                if (rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
#endif
                return rc;
            }
        }
        Guard g(s->mu);
        Txn t(s->db);
        // 34th round, P1: the tree is proven gone by here, so a row left behind is a CREATING
        // row naming nothing -- what gc's abandoned-fork sweep buries -- and the caller is owed
        // the failure that brought it here, not the one from the tidying up.
        if (t.ok()) {
            Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
            if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
            (void)t.commit();   // best effort: the sweep is the backstop either way
        }
        if (rc == -EXDEV) return WFS_E_CROSS_VOLUME;
#ifdef __APPLE__
        if (rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
#endif
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
        if (!t.ok()) return t.err();   // 34th round, P1
        // tmp_path goes with the publish: the tree is at `target` now, and a row that still
        // named the temporary would be pointing gc at a path that is not there any more.
        Stmt u(s->db, "UPDATE worlds SET dir_dev=?, dir_ino=?, entries=?, state=?, tmp_path='',"
                      " owner_pid=0, owner_start=0 WHERE id=? AND state=?");
        if (!u.ok()) return -EIO;
        u.i64(1, (int64_t)st.st_dev);
        u.i64(2, (int64_t)st.st_ino);
        u.i64(3, (int64_t)entries);
        u.i64(4, WFS_ST_ACTIVE);
        u.i64(5, (int64_t)id);
        u.i64(6, WFS_ST_CREATING);
        if (u.step() != SQLITE_DONE) return -EIO;
        // PR #1 review (3rd round): this used to be a fire-and-forget UPDATE. If something had
        // buried the row in the meantime -- which is exactly what an over-eager gc did -- the
        // fork returned success and left a directory at --to that no row knows about: not a
        // world, not collectable, and `verify` would call it an unregistered copy. A row that
        // is not there is a failure, and the tree goes back where it came from.
        if (sqlite3_changes(s->db) != 1) {   // the Txn destructor rolls back
            if (wfs::fs_rename_excl(target.c_str(), tmp.c_str()) == 0)
                wfs::fs_remove_tree(tmp.c_str());
            return -ESTALE;
        }
        // 34th round, P1: and a COMMIT that failed is the same situation one step later -- the
        // tree is at --to with a row that still says CREATING -- so it takes the same way out.
        if (int crc = t.commit()) {
            if (wfs::fs_rename_excl(target.c_str(), tmp.c_str()) == 0)
                wfs::fs_remove_tree(tmp.c_str());
            return crc;
        }
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
    // 32nd round, P2: COALESCE always has a row to give, so no row at all is a failed read --
    // and answering 1 to "what is the next id" would hand out an id the store already used.
    if (!q.row()) return -EIO;
    *out = (wfs_id)q.col_i64(0);
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
    if (!q.done()) return -EIO;   // 32nd round, P2: a listing that stopped is not a listing
    *count = n;
    return 0;
}

namespace {

// ---- PR #1 review (5th round): the three-step trashing protocol -------------------------------
//
// A discard used to be one rename and one commit, in one order for worlds (tree first) and the
// other for snapshots (row first, inside the transaction). Both orders have the same hole: a
// process killed between the two leaves a tree in <store>/trash that no row names -- the row was
// either never updated or rolled back to ACTIVE -- and the collector's row-less-orphan rule then
// deletes it. For a snapshot that is the baseline of every world forked from it (P4/P10).
//
// So the row is written twice, and it names the tree's future place before the tree is anywhere
// near it:
//
//   (a) one transaction: the reference check, state = TRASHING, trash_path = the name the tree
//       is about to get. Commit.
//   (b) rename the tree to that name.
//   (c) one transaction: state = TRASHED.
//
// A kill anywhere in there leaves a TRASHING row whose trash_path is the only name the tree can
// have besides its own, which is what trashing_recover() below resolves -- and which is why the
// orphan rule now spares every directory any row names, in any state.
//
// PR #1 review (8th round): "a kill" -- and only a kill. Step (a) also writes the owner (this
// pid and this process's own start time, exactly as a CREATING row records its producer), and
// trashing_recover() skips a TRASHING row whose owner is still alive. Without that, a second
// process opening the store in the middle of a live discard -- every `world fs ...` invocation
// opens the store, and the open runs the recovery -- read "the tree is still at home" and put
// the row back to ACTIVE with an empty trash_path. The discard then renamed the tree into the
// trash, its conditional step (c) matched no row, and it returned 0: an ACTIVE row whose tree
// is a row-less orphan in the trash, which the very next collector deletes. `restore` has the
// mirror image, and both are gone now: while the owner runs, the row is in flight, not crashed.
int trashing_set_path(wfs_store *s, wfs_id id, int is_snapshot, const char *p) {
    Guard g(s->mu);
    Txn t(s->db);
    if (!t.ok()) return t.err();   // 34th round, P1
    Stmt u(s->db, is_snapshot ? "UPDATE snapshots SET trash_path=? WHERE id=? AND state=4"
                              : "UPDATE worlds SET trash_path=? WHERE id=? AND state=4");
    if (!u.ok()) return -EIO;
    u.text(1, p);
    u.i64(2, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    // The row has to name the new place before the tree can be there: a commit that failed is a
    // rename that must not happen (34th round, P1).
    return t.commit();
}

// Who is moving this tree: the pid and that process's own start time, so a reused pid is not
// mistaken for the owner. The same two columns a CREATING row uses, filled the same way, and
// cleared again by step (c) and by the undo -- a row that is not in flight has no owner.
void trashing_own(Stmt &u, int pid_idx, int start_idx) {
    int64_t pid = 0, start = 0;
    owner_now(pid, start);
    u.i64(pid_idx, pid);
    u.i64(start_idx, start);
}

// The one combination that may never outlive a discard: the tree is in the trash and the row
// says ACTIVE. It can only be reached by somebody resolving our row from under us -- a recovery
// in another process whose lstat ran before our rename -- which the ownership above already
// rules out. Belt and braces, then, and the cheapest honest outcome for each case:
//
//   the row is TRASHED at our path   somebody finished the discard for us. That is what the
//                                    caller asked for: 0.
//   the row is ACTIVE                the verdict was made against a tree that has since moved.
//                                    Re-mark it TRASHED, naming the tree where it actually is,
//                                    and the row and the tree agree again: 0.
//   the tree is not at our path,     the tree is home (or gone) and the row was resolved to
//   or the row is neither            match it. The discard did not happen: -ESTALE, and the
//                                    caller is told rather than handed a silent success.
//
// A reference cannot have appeared for a snapshot in that window: fork, pool fill, adopt and
// restore all re-read the snapshot row under the write lock and refuse anything but ACTIVE, and
// for the moment this row was ACTIVE its tree was already in the trash -- so any such claim's
// clone fails with ENOENT and unwinds itself.
int trashing_reclaim(wfs_store *s, wfs_id id, int is_snapshot, const char *trash_path) {
    // PR #1 review (12th round): -ESTALE here is the sentence "the discard did not happen".
    // An lstat(2) that failed for a reason other than absence is not evidence of that, so it is
    // reported as itself rather than dressed up as a verdict.
    if (!trash_path || !*trash_path) return -ESTALE;
    if (int prc = wfs::fs_probe(trash_path)) return wfs::fs_gone(prc) ? -ESTALE : prc;
    Guard g(s->mu);
    Txn t(s->db);
    if (!t.ok()) return t.err();   // 34th round, P1
    int64_t state = -1;
    String tp;
    {
        Stmt q(s->db, is_snapshot ? "SELECT state, trash_path FROM snapshots WHERE id=?"
                                  : "SELECT state, trash_path FROM worlds WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)id);
        if (!q.row()) return q.done() ? -ESTALE : -EIO;   // 32nd round, P2
        state = q.col_i64(0);
        tp.assign(q.col_text(1));
    }
    if (state == WFS_ST_TRASHED && !::strcmp(tp.c_str(), trash_path)) return 0;
    if (state != WFS_ST_ACTIVE) return -ESTALE;
    Stmt u(s->db, is_snapshot
                      ? "UPDATE snapshots SET state=?, trash_path=?, trashed_at=?, owner_pid=0,"
                        " owner_start=0 WHERE id=? AND state=1"
                      : "UPDATE worlds SET state=?, trash_path=?, trashed_at=?, owner_pid=0,"
                        " owner_start=0 WHERE id=? AND state=1");
    if (!u.ok()) return -EIO;
    u.i64(1, WFS_ST_TRASHED);
    u.text(2, trash_path);
    u.i64(3, now_sec());
    u.i64(4, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    if (sqlite3_changes(s->db) == 0) return -ESTALE;
    return t.commit();
}

// Step (c). Only ever applied to a row this process put in TRASHING -- and since the 8th round
// of the PR #1 review it says so: zero rows changed means the row is not ours any more, and the
// tree that is now in the trash has to be squared with whatever the row says before this can
// return anything (trashing_reclaim above). It used to return 0 on a no-op UPDATE.
int trashing_commit(wfs_store *s, wfs_id id, int is_snapshot, const char *trash_path) {
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        Stmt u(s->db,
               is_snapshot
                   ? "UPDATE snapshots SET state=?, owner_pid=0, owner_start=0 WHERE id=? AND state=4"
                   : "UPDATE worlds SET state=?, owner_pid=0, owner_start=0 WHERE id=? AND state=4");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_TRASHED);
        u.i64(2, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        int changed = sqlite3_changes(s->db);
        if (int crc = t.commit()) return crc;
        if (changed) return 0;
    }
    return trashing_reclaim(s, id, is_snapshot, trash_path);
}

// The rename never happened, so neither did the discard: put the row back exactly where
// trashing_recover() would put it, rather than leave the next process to work it out.
void trashing_undo(wfs_store *s, wfs_id id, int is_snapshot) {
    Guard g(s->mu);
    Txn t(s->db);
    // 34th round, P1: with no transaction the row is left TRASHING, owned by this process --
    // which is the "in flight" state trashing_recover() resolves from the tree's own position
    // the moment this process is gone. Putting it back is what this is for; guessing is not.
    if (!t.ok()) return;
    Stmt u(s->db,
           is_snapshot ? "UPDATE snapshots SET state=?, trash_path='', trashed_at=0, owner_pid=0,"
                         " owner_start=0 WHERE id=? AND state=4"
                       : "UPDATE worlds SET state=?, trash_path='', trashed_at=0, owner_pid=0,"
                         " owner_start=0 WHERE id=? AND state=4");
    if (u.ok()) { u.i64(1, WFS_ST_ACTIVE); u.i64(2, (int64_t)id); u.step(); }
    (void)t.commit();   // best effort: trashing_recover() reaches the same verdict from the tree
}

int trash_crash_seam(int phase, int is_snapshot, wfs_id id, const char *trash_path) {
    if (!wfs_test_trash_crash) return 0;
    return wfs_test_trash_crash(wfs_test_trash_crash_ctx, phase, is_snapshot, id, trash_path);
}

// PR #1 review (26th round, P1): the seam between "this is the row's tree" and the rename that
// takes it. All three claims call it in the same place, because all three had the same split.
void trash_claim_seam(int is_snapshot, wfs_id row, const char *path) {
    if (wfs_test_between_trash_claim)
        wfs_test_between_trash_claim(wfs_test_between_trash_claim_ctx, is_snapshot, row, path);
}

// --now: delete the trash entry here instead of leaving it to the collector, which is what
// `discard --now` has always advertised for a world and (since the 5th round) for a snapshot.
// The same two steps in the same order as the collector's: rename to *.deleting first, so an
// interrupted delete is visibly not a world (or a snapshot) any more, record the new name,
// unlink, and only then is the row DEAD.
//
// PR #1 review (6th round): and the entry is followed to whatever name it has right now. The
// collector renames it to `<name>.deleting` and records that a moment later, so in between --
// and after any interruption in that window -- the row still names the tree by the name it no
// longer has and trash_mark_deleting() answers -ENOENT. The world paths read that as "already
// gone", marked the row DEAD and returned while the collector was still unlinking: `--now`
// returning before the space was back, and a DEAD row for a tree that is still on disk. The
// snapshot path already followed the rename; both go through here now, and -ENOENT means what
// it is supposed to mean -- at neither name, so somebody else finished it.
// PR #1 review (9th round), P18: and every row write it makes is predicated on the row it
// followed. `trash` is what the row said when this call started, and both UPDATEs below carry
// it -- still TRASHED, still naming the tree this call is deleting. `discard W<n> --now`
// overlapping `restore W<n>` was the case that had to answer for it: the restore renames the
// tree home and marks the row ACTIVE while this helper is between its lstat and its mark, the
// mark then answers -ENOENT ("already gone"), and the final UPDATE -- which had no predicate at
// all -- buried the world that had just come back. Zero rows changed is -ESTALE, never 0: the
// caller is told its `--now` did not happen rather than told the tree is gone.
// PR #1 review (15th round), P1: and the predicate on the *first* of those two writes moved into
// the transaction that makes the rename, because the rename is the claim -- see
// gc_claim_deleting(). The row is re-read there, the tree is renamed there, and the new name is
// recorded there; a restore that has taken the row to TRASHING wins or loses that transaction
// whole, and never in between.
int trash_delete_now(wfs_store *s, wfs_id id, int is_snapshot, const String &trash) {
    String expect(trash);   // the trash_path this call is acting on behalf of
    String tp(trash);
    trash_follow_deleting(tp);
    if (int hrc = trash_crash_seam(4, is_snapshot, id, tp.c_str())) return hrc;
    if (tp.size()) {
        // PR #1 review (20th round, P1): no leftover fold here, and none anywhere else -- see
        // trash_mark_deleting(). A directory at `<tp>.deleting` is not this call's to remove,
        // so `--now` comes back WFS_E_TRASH_BLOCKED out of the claim below with the entry and
        // the stray both untouched, and the CLI names the directory that is in the way.
        String deleting;
        TrashClaim claim;   // PR #1 review (26th round, P1): held from the check to the rmdir
        int mrc;
        {
            // PR #1 review (15th round, P1): the collector's claim, made the same way here. The
            // rename used to happen out here and the UPDATE behind it to be the conditional one,
            // which left `--now` the same window gc_claim_deleting() closes: a `restore W<n>`
            // that has committed its row in TRASHING and not yet renamed the tree home would
            // find the tree renamed to `.deleting` under it, and fall back to TRASHED with
            // nothing left to restore. Row and rename are one transaction now, and because
            // restore's step (a) is BEGIN IMMEDIATE too, the loser of the two does nothing at
            // all: -ESTALE, tree untouched.
            Guard g(s->mu);
            Txn t(s->db);
            if (!t.ok()) return t.err();   // 34th round, P1
            Stmt q(s->db,
                   is_snapshot ? "SELECT state, trash_path, 0, 0 FROM snapshots WHERE id=?"
                               : "SELECT state, trash_path, dir_dev, dir_ino FROM worlds WHERE id=?");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)id);
            if (!q.row()) return q.done() ? -ESTALE : -EIO;   // 32nd round, P2
            if (q.col_i64(0) != WFS_ST_TRASHED || ::strcmp(q.col_text(1), expect.c_str()))
                return -ESTALE;
            // PR #1 review (25th round, P1): and it is this row's tree, not merely this row's
            // name. Read from the row in the same transaction as the rename below, so what is
            // checked is what the claim acts on. `--now` refuses rather than delete a
            // directory somebody else put at the name -- the same shape as the 20th round's
            // WFS_E_TRASH_BLOCKED, one step earlier: there the stray was on the working name,
            // here it is on the entry itself.
            // (A tree that is genuinely gone is not a foreign one: trash_mark_deleting()
            // answers -ENOENT for it a line further down and the row is buried the way it
            // always was.)
            // PR #1 review (26th round, P1): and the check is made on a descriptor that is
            // then held for the rest of the job, because the rename and the delete below are
            // fresh name lookups and the owner of the directory the entry sits in is not one of
            // the writers this transaction serialises. Same protocol as the collector's, same
            // exceptions (a snapshot row has no identity to hold on to) -- see TrashClaim.
            uint64_t rdev = (uint64_t)q.col_i64(2), rino = (uint64_t)q.col_i64(3);
            if (rino) {
                int irc = trash_claim_open(tp.c_str(), rdev, rino, claim);
                if (irc && !wfs::fs_gone(irc)) return irc;
            }
            trash_claim_seam(is_snapshot, id, tp.c_str());
            mrc = trash_mark_deleting(tp.c_str(), deleting);
            if (mrc && mrc != -ENOENT) return mrc;
            if (!mrc && claim.held()) {
                if (int vrc = trash_claim_verify_rename(
                        claim, deleting.c_str(), ::strcmp(deleting.c_str(), tp.c_str()) != 0))
                    return vrc;
            }
            if (!mrc && ::strcmp(deleting.c_str(), expect.c_str())) {
                // Unconditional: the SELECT above read this row in this transaction.
                Stmt u(s->db, is_snapshot ? "UPDATE snapshots SET trash_path=? WHERE id=?"
                                          : "UPDATE worlds SET trash_path=? WHERE id=?");
                if (!u.ok()) return -EIO;
                u.text(1, deleting.c_str());
                u.i64(2, (int64_t)id);
                if (u.step() != SQLITE_DONE) return -EIO;
                expect = deleting;
            }
            // The rename has happened; the unlink below follows it. A commit that failed rolls
            // the recorded name back, so the unlink must not run on it (34th round, P1) --
            // trash_follow_deleting() picks the tree up at either name on the next attempt.
            if (int crc = t.commit()) return crc;
        }
        if (!mrc) {
            int rc = claim.held() ? trash_unlink_claim(claim, nullptr)
                                  : trash_unlink(deleting.c_str(), 4, nullptr);
            if (rc) return rc;
        }
    }
    Guard g(s->mu);
    Txn t(s->db);
    if (!t.ok()) return t.err();   // 34th round, P1
    Stmt u(s->db,
           is_snapshot
               ? "UPDATE snapshots SET state=?, trash_path='' WHERE id=? AND state=2 AND trash_path=?"
               : "UPDATE worlds SET state=?, trash_path='' WHERE id=? AND state=2 AND trash_path=?");
    if (!u.ok()) return -EIO;
    u.i64(1, WFS_ST_DEAD);
    u.i64(2, (int64_t)id);
    u.text(3, expect.c_str());
    if (u.step() != SQLITE_DONE) return -EIO;
    int changed = sqlite3_changes(s->db);
    if (int crc = t.commit()) return crc;
    return changed ? 0 : -ESTALE;
}

} // namespace

extern "C" int wfs_world_discard(wfs_store *s, wfs_id id, int immediate, int force) {
    if (!s || !id) return -EINVAL;
    wfs_world_rec r;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, id, r)) return rc;
    }
    if (r.state == WFS_ST_TRASHED) {
        if (!immediate) return -EALREADY;
        // Already in the trash: --now just brings the deletion forward -- including over a
        // collector that has already renamed the entry to `.deleting` under this row
        // (PR #1 review, 6th round: that used to leave the tree and bury the row).
        String tp;
        {
            Guard g(s->mu);
            if (int rc = world_trash_path(s, id, tp)) return rc;
        }
        return trash_delete_now(s, id, 0, tp);
    }
    if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    // P5: never pull the floor out from under a running `world exec`.
    if (int rc = exec_lock_guard(s, id, force)) return rc;
    wfs_identity ident;
    if (int rc = wfs_world_verify_identity(s, r.path, &ident)) return rc;

    WorldLock lock;
    if (int rc = lock.take(ident.path)) return rc;

    if (int rc = wfs::git_discard_check(ident.path)) return rc;

    char leaf[80];
    ::snprintf(leaf, sizeof leaf, "W%llu-%lld", (unsigned long long)id, (long long)now_sec());
    String trash = joinp(joinp(s->dir.c_str(), "trash").c_str(), leaf);
    // (a) the row first, in TRASHING, naming the place the tree is about to move to, and owned
    // by this process for as long as the move takes (8th round). A crash from here to (c) leaves
    // a tree that one row names, never a row-less orphan; a *live* discard leaves a row no other
    // process may resolve.
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        Stmt u(s->db, "UPDATE worlds SET state=?, trash_path=?, trashed_at=?, owner_pid=?,"
                      " owner_start=? WHERE id=? AND state=1");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_TRASHING);
        u.text(2, trash.c_str());
        u.i64(3, now_sec());
        trashing_own(u, 4, 5);
        u.i64(6, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        if (sqlite3_changes(s->db) == 0) return -ESTALE;   // somebody else moved it meanwhile
        // 34th round, P1: the rename in (b) is allowed only once this row is on disk. A commit
        // that failed leaves an ACTIVE world, and moving its tree into the trash behind that is
        // exactly the row-less orphan the three-step protocol exists to make impossible.
        if (int crc = t.commit()) return crc;
    }
    if (int hrc = trash_crash_seam(0, 0, id, trash.c_str())) return hrc;
    // (b)
    int rc = wfs::fs_rename(ident.path, trash.c_str());
    if (rc == -EXDEV) {
        // The world lives on another volume than the store: keep the trash next to it. The row
        // has to name the new place before the tree can be there, so this is an extra commit --
        // and until it lands the tree is still at home, which is what the recovery reads.
        String side;
        dirname_of(ident.path, side);
        side = joinp(side.c_str(), ".wfs-trash");
        wfs::fs_mkdir_p(side.c_str());
        trash = joinp(side.c_str(), leaf);
        if (int urc = trashing_set_path(s, id, 0, trash.c_str())) { trashing_undo(s, id, 0); return urc; }
        rc = wfs::fs_rename(ident.path, trash.c_str());
    }
    if (rc) { trashing_undo(s, id, 0); return rc; }
    if (int hrc = trash_crash_seam(1, 0, id, trash.c_str())) return hrc;
    // (c)
    if (int crc = trashing_commit(s, id, 0, trash.c_str())) return crc;
    if (!immediate) return 0;
    // The row is TRASHED now, so a collector running beside us may already have picked this
    // entry up: the same helper, following the same rename (PR #1 review, 6th round).
    return trash_delete_now(s, id, 0, trash);
}

namespace {

// T2.1: has the collector already started on this trash entry? Either the row has been updated
// to the .deleting name, or the rename happened and the process died before the row did -- the
// name on disk is the authority in both cases.
//
// PR #1 review (20th round, P1): and the second case is "the tree is at `.deleting` AND NOT at
// its own name", which is what the rename leaves behind -- exactly the pair trash_follow_deleting
// resolves. A bare `exists(<name>.deleting)` was enough to refuse a restore while the tree sat
// untouched at its own name, and since the fold is gone (see trash_mark_deleting) that state is
// no longer transient: a directory somebody else left beside the entry -- in the user's own
// `.wfs-trash`, under a name they can guess -- would tell them for ever that their world "is
// already being deleted by the collector", which it is not. The collector's claim and the row
// that records it are one transaction, so a tree still at its own name has been claimed by
// nobody. ("Cannot prove it gone" counts as still there: the restore's own rename decides.)
//
// PR #1 review (25th round, P1): and the tree at that `.deleting` name is the collector's work
// on THIS world only if it is this world's tree. A same-volume rename keeps the inode and every
// rename on this path is same-volume, so the row's dir_dev/dir_ino answer it. Without that, a
// directory somebody else left at `<entry>.deleting` while the world's own tree was moved away
// still told the owner for ever that "the collector is deleting this world" -- which is exactly
// the false statement the 20th round removed from the other half of this check.
bool trash_is_deleting(const String &trash, uint64_t dev, uint64_t ino) {
    size_t n = trash.size(), sl = ::strlen(WFS_DELETING_SUFFIX);
    if (n > sl && !::strcmp(trash.c_str() + n - sl, WFS_DELETING_SUFFIX)) return true;
    if (!proven_gone(trash.c_str())) return false;
    String d(trash);
    d.append(WFS_DELETING_SUFFIX);
    if (!exists(d.c_str())) return false;
    return trash_identity(d.c_str(), dev, ino) != WFS_E_TRASH_FOREIGN;
}

} // namespace

// T2.1: bringing a trashed world back. Symmetrical to the discard above, and for the same
// reason: a rename and a row that must never disagree about where the tree is.
//
// PR #1 review (7th round): and the decision has to be made under the write lock, not read and
// then acted on. It used to check "the source snapshot is still ACTIVE" in a standalone SELECT,
// rename the tree home, and only then mark the row ACTIVE in a transaction of its own. A
// `discard S<n>` running in that window counts references under BEGIN IMMEDIATE and refuses
// ACTIVE worlds, CREATING worlds and pool entries -- and this world was still TRASHED, which
// refuses nothing. So both succeeded: the snapshot went to the trash and the restore published
// a live world whose baseline was gone, one whose every `diff` and `verify` answers
// WFS_E_SOURCE_GONE for ever.
//
// So restore is the discard's own three-step protocol run backwards:
//
//   (a) one BEGIN IMMEDIATE: the row is TRASHED, its tree is not already being deleted, and the
//       baseline is ACTIVE -- checked here, inside the transaction, and the row goes to
//       WFS_ST_TRASHING. That is the state that means "in flight, lstat decides", and it is a
//       hard reference: snapshot_refs_locked() counts it and wfs_snapshot_discard() refuses it.
//       trash_path is left exactly as it was, because the tree has not moved yet.
//   (b) rename the tree from the trash back home.
//   (c) one transaction: ACTIVE, trash_path cleared.
//
// A kill anywhere in there leaves the same TRASHING row trashing_recover() already resolves by
// lstat: the tree is at trash_path (the rename never happened, so neither did the restore -- the
// row goes back to TRASHED) or at home (it did -- ACTIVE). dir_dev/dir_ino need no re-stat on
// that path: a world's trash is always on the world's own volume -- <store>/trash when the store
// shares it, and a `.wfs-trash` beside the world when the discard hit EXDEV -- so both renames
// keep the inode.
//
// PR #1 review (25th round, P1): which is also what makes the entry identifiable, and step (a)
// now requires it. The `.wfs-trash` half of that sentence is a directory of the user's under a
// name they can work out, so "the row names this path" never meant "this path holds the world":
// the tree could be moved away and another directory left in its place, and the restore renamed
// that one home and wrote its dev/ino into the row -- somebody else's data registered as the
// world, exit 0. (c) no longer takes the row's identity from a stat of the home path either;
// the numbers it writes are the ones (a) checked.
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
    if (trash_is_deleting(trash, r.dir_dev, r.dir_ino)) return WFS_E_TRASH_DELETING;
    // PR #1 review (12th round): and -ENOENT is reserved for a tree that is really not there.
    if (int prc = wfs::fs_probe(trash.c_str())) return wfs::fs_gone(prc) ? -ENOENT : prc;
    if (exists(r.path)) return -EEXIST;   // a create check: the rename below refuses anyway
    // (a). Everything the restore decides is decided again in here, under the write lock a
    // discard takes: the row, the tree's name, the tree's identity, and the baseline.
    uint64_t ident_dev = 0, ident_ino = 0;   // what the row says its tree is, read under (a)
    // PR #1 review (26th round, P1): and the descriptor that check is made on, held across the
    // rename home -- see TrashClaim. (a) proves the entry is this world's tree with an lstat and
    // (b) renames a path; between the two the owner of the directory the entry sits in can put
    // another one there, and for the EXDEV trash that directory is their own `.wfs-trash`. The
    // rename would then carry a stranger's tree home and the row would be made ACTIVE over it.
    TrashClaim claim;
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        wfs_world_rec rr;
        if (int rc = world_row(s, id, rr)) return rc;
        if (rr.state != WFS_ST_TRASHED) return -ESTALE;
        String tp;
        if (int rc = world_trash_path(s, id, tp)) return rc;
        if (::strcmp(tp.c_str(), trash.c_str())) return -ESTALE;   // it moved under us
        if (trash_is_deleting(tp, rr.dir_dev, rr.dir_ino)) return WFS_E_TRASH_DELETING;
        // PR #1 review (25th round, P1): and what is at that path has to BE this world's tree.
        // The row has carried dir_dev/dir_ino since the world was published and every rename on
        // the way into the trash is same-volume, so the inode is the world's identity while it
        // sits there -- and the cross-volume entry sits in the user's own `.wfs-trash` under a
        // name they can guess. Without this the restore renamed whatever was at that path home,
        // re-stat'ed it and wrote ITS dev/ino into the row in step (c): a stranger's directory
        // registered as the world, exit 0, and the world's own tree left with nothing naming
        // it. Checked in here, under the write lock the discard takes, and nothing else may
        // make the row TRASHING while we hold it.
        if (rr.dir_ino) {
            if (int irc = trash_claim_open(tp.c_str(), rr.dir_dev, rr.dir_ino, claim)) return irc;
        }
        trash_claim_seam(0, id, tp.c_str());
        // The pair step (c) writes back is the pair the descriptor was accepted on -- the same
        // numbers as the row's by construction, and read from the thing itself rather than from
        // the row or, as before the 25th round, from a stat of whatever ends up at the home path.
        ident_dev = claim.held() ? claim.dev : rr.dir_dev;
        ident_ino = claim.held() ? claim.ino : rr.dir_ino;
        // T2.2: a world whose source snapshot has been discarded cannot be brought back to life
        // -- it would have no baseline to diff or verify against, which is the whole point of a
        // World. Under the write lock, so "the snapshot is ACTIVE" is still true when the row
        // becomes a reference a paragraph further down.
        if (rr.snapshot_id) {
            Stmt q(s->db, "SELECT state FROM snapshots WHERE id=?");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)rr.snapshot_id);
            if (!q.row()) return q.done() ? WFS_E_SOURCE_GONE : -EIO;   // 32nd round, P2
            if (q.col_i64(0) != WFS_ST_ACTIVE) return WFS_E_SOURCE_GONE;
        }
        // ... and owned by this process while the rename runs (8th round): a TRASHING row whose
        // owner is alive is an operation in flight, and no other process's recovery may decide
        // for it. Without that, a store opened next door read "the tree is still at trash_path"
        // and put the row back to TRASHED, after which this restore renamed the tree home and
        // its step (c) matched nothing -- a TRASHED row for a world sitting at its home path.
        Stmt u(s->db, "UPDATE worlds SET state=?, owner_pid=?, owner_start=? WHERE id=? AND state=2");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_TRASHING);
        trashing_own(u, 2, 3);
        u.i64(4, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        if (sqlite3_changes(s->db) == 0) return -ESTALE;
        // The mirror of the discard's (a): the rename home in (b) may only follow a row that is
        // on disk in TRASHING and owned by us (34th round, P1).
        if (int crc = t.commit()) return crc;
    }
    if (int hrc = trash_crash_seam(2, 0, id, trash.c_str())) return hrc;
    // (b)
    if (int rc = wfs::fs_rename(trash.c_str(), r.path)) {
        // The tree never left the trash, so the row says what it said before: TRASHED, with the
        // same trash_path. That is trashing_commit()'s whole job -- "the tree is in the trash"
        // -- and it is the verdict trashing_recover() would reach here by itself.
        trashing_commit(s, id, 0, trash.c_str());
        return rc;
    }
    if (claim.held()) {
        // The rename moved *a* directory home; this is what says it moved this world's. If it
        // did not, the rename is undone and the restore refuses: nothing of the stranger's is
        // touched, nothing is registered, and the row goes back to TRASHED exactly where the
        // failed-rename branch above puts it. (If the undo itself cannot be made -- the entry's
        // old name has been filled in the meantime -- the row is left TRASHING, which is the
        // "in flight, lstat decides" state trashing_recover() resolves; the refusal stands
        // either way, and nothing has been deleted.)
        int vrc = trash_claim_parent(claim, r.path);
        if (!vrc) vrc = trash_claim_is_ours(claim);
        if (vrc) {
            if (wfs::fs_rename(r.path, trash.c_str()) == 0)
                trashing_commit(s, id, 0, trash.c_str());
            return vrc;   // the lend goes back with ~TrashClaim (30th round)
        }
        // PR #1 review (30th round, P2): the tree is home and proven ours, so the bits the
        // claim borrowed to open it are given back HERE rather than at the end of the function
        // -- a restore is a round trip, and the mode the world comes home with has to be the
        // mode the discard found on it. (A world root that is searchable and not readable, 0311
        // and its relatives, discards untouched and used to come back 0700 for ever.) Every
        // other exit of this function after the claim -- the refusals above, a failed row
        // write, an -ESTALE below -- is covered by ~TrashClaim, which does exactly this.
        claim.return_lend();
    }
    if (int hrc = trash_crash_seam(3, 0, id, trash.c_str())) return hrc;
    // (c). The tree is at home: whatever the row says, that is now the only fact on disk, and
    // this UPDATE has to be the one that makes the row agree with it. PR #1 review (8th round):
    // it is conditional and it is checked. A row somebody else put back in TRASHED while our
    // rename was in flight is picked up too -- a TRASHED world whose tree is at its home path is
    // a state only this restore can have produced -- and a row that is ACTIVE with no trash_path
    // is the outcome we wanted, reached by somebody else. Anything else is not ours to overwrite.
    // PR #1 review (25th round, P1): the row's own dev/ino go back in, and they are not read
    // off the path we just renamed to. A same-volume rename keeps the inode, so in every
    // legitimate case the two are the same number -- but a stat of the home path is a question
    // about whatever is standing there, and answering the row with it is how a substituted tree
    // used to become the world (the identity check in (a) is what now keeps one out, and a row
    // must not be able to learn a new identity from the floor in the first place).
    if (!ident_ino) {
        // No identity was recorded for this world, so there was none to check either and the
        // path is all there is to go on -- the pre-25th-round behaviour, kept for a row the
        // columns predate. Nothing this store writes leaves a published world without them.
        struct stat st;
        if (::stat(r.path, &st) != 0) return -errno;
        ident_dev = (uint64_t)st.st_dev;
        ident_ino = (uint64_t)st.st_ino;
    }
    {
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        Stmt u(s->db, "UPDATE worlds SET state=?, trash_path='', trashed_at=0, dir_dev=?,"
                      " dir_ino=?, owner_pid=0, owner_start=0 WHERE id=? AND (state=4 OR state=2)");
        if (!u.ok()) return -EIO;
        u.i64(1, WFS_ST_ACTIVE);
        u.i64(2, (int64_t)ident_dev);
        u.i64(3, (int64_t)ident_ino);
        u.i64(4, (int64_t)id);
        if (u.step() != SQLITE_DONE) return -EIO;
        int changed = sqlite3_changes(s->db);
        if (int crc = t.commit()) return crc;
        if (changed) return 0;
    }
    Guard g(s->mu);
    Stmt q(s->db, "SELECT state, trash_path FROM worlds WHERE id=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (q.row()) return (q.col_i64(0) == WFS_ST_ACTIVE && !*q.col_text(1)) ? 0 : -ESTALE;
    return q.done() ? -ESTALE : -EIO;   // 32nd round, P2
}

// ---- T2.2: discarding a snapshot --------------------------------------------------------------

namespace {

// Who still needs S<n>? ACTIVE worlds are a hard refusal; pool entries are only pre-made clones
// and --force drains them.
struct SnapRefs {
    uint64_t active_worlds = 0;
    uint64_t creating_worlds = 0;
    // PR #1 review (7th round): a world in flight between the trash and its home, either way
    // round. `discard W<n>` and `restore W<n>` both park the row in WFS_ST_TRASHING while the
    // rename happens, and a restore that is about to finish is a world that will need this
    // baseline a millisecond from now. It used to land in trashed_worlds, which refuses nothing.
    uint64_t trashing_worlds = 0;
    uint64_t trashed_worlds = 0;
    uint64_t pool_entries = 0;
    wfs_id first_world = 0;
};

// Callers already hold the store mutex *and* an open transaction: this is the reference count
// the discard decides on, so it has to be read under the same write lock the fork's claim takes.
int snapshot_refs_locked(wfs_store *s, wfs_id id, SnapRefs &out) {
    {
        // Every state but DEAD: a world in the middle of its own discard (TRASHING) is a row
        // that still names this snapshot, and its own recovery may yet put it back (5th round).
        Stmt q(s->db, "SELECT id, state FROM worlds WHERE snapshot_id=? AND state<>3");
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
            } else if (st == WFS_ST_TRASHING) {
                // A discard or a restore between its row and its rename. Which of the two it is
                // cannot be told from here, and neither can be allowed to lose its baseline: a
                // restore finishes into an ACTIVE world, and a discard's own recovery may yet
                // put it back. Hence a refusal, not a count (PR #1 review, 7th round).
                if (!out.first_world) out.first_world = (wfs_id)q.col_i64(0);
                out.trashing_worlds++;
            } else {
                out.trashed_worlds++;
            }
        }
        // PR #1 review (32nd round, P2): and the loop has to have ended because the rows ran
        // out. sqlite3_step(2) fails at step time as well as at prepare time -- an EIO off the
        // database file, a BUSY past the busy timeout, a NOMEM -- and `row()` reported all of
        // those exactly as it reports SQLITE_DONE. This is the count `wfs_snapshot_discard()`
        // decides on: a short read here is a baseline with no references, so the snapshot goes
        // to the trash under live worlds and the collector deletes it out from under them.
        if (!q.done()) return -EIO;
    }
    {
        Stmt q(s->db, "SELECT COUNT(*) FROM pool WHERE snapshot_id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)id);
        if (!q.row()) return -EIO;   // COUNT(*) always has a row: no row is a failed read
        out.pool_entries = (uint64_t)q.col_i64(0);
    }
    return 0;
}

} // namespace

extern "C" int wfs_snapshot_discard(wfs_store *s, wfs_id id, int immediate, int force) {
    if (!s || !id) return -EINVAL;
    // A look before the transaction, for the state errors only. Everything the discard *decides*
    // is decided again below, under the write lock.
    {
        wfs_snapshot_rec r;
        String tp;
        {
            Guard g(s->mu);
            if (int rc = snapshot_row(s, id, r)) return rc;
            if (r.state == WFS_ST_TRASHED && immediate) {
                if (int rc = snapshot_trash_path(s, id, tp)) return rc;
            }
        }
        if (r.state == WFS_ST_TRASHED) {
            // PR #1 review (5th round): --now on a snapshot that is already in the trash brings
            // the deletion forward, exactly as it does for a world, instead of being refused and
            // sending the caller to a store-wide gc. The reference check happened when it was
            // trashed; what is left is the unlink the collector would have done later.
            if (!immediate) return -EALREADY;
            // An interrupted collection is still this row's entry: trash_delete_now() follows
            // the collector's rename rather than calling the tree gone.
            return trash_delete_now(s, id, 1, tp);
        }
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

    String trash, snapdir;
    bool tree_gone = false;
    {
    // One BEGIN IMMEDIATE for the reference check *and* the state transition (PR #1 review).
    // BEGIN IMMEDIATE takes the database write lock, which is the same lock a pool claim and a
    // world row insert take, so a fork is either entirely before this transaction (its CREATING
    // row is counted) or entirely after it (it finds the snapshot TRASHED and gives up). There
    // is no longer a moment in which neither side can see the other.
    Guard g(s->mu);
    Txn t(s->db);
    // 34th round, P1: and it is one transaction only if the BEGIN took the write lock. Without
    // it the reference count below and the UPDATE that follows are two separate writes, with a
    // fork free to commit its CREATING row between them -- the very interleaving this whole
    // transaction was written to make impossible.
    if (!t.ok()) return t.err();
    wfs_snapshot_rec r;
    if (int rc = snapshot_row(s, id, r)) return rc;
    if (r.state == WFS_ST_TRASHED) return -EALREADY;
    if (r.state != WFS_ST_ACTIVE) return -ESTALE;
    SnapRefs refs;
    if (int rc = snapshot_refs_locked(s, id, refs)) return rc;
    // Never orphan a world's source: diff and verify both need the baseline (P4/P10).
    if (refs.active_worlds || refs.creating_worlds || refs.trashing_worlds || refs.pool_entries)
        return WFS_E_SNAPSHOT_IN_USE;

    snapdir = snapshot_dir_of(r);
    char leaf[80];
    ::snprintf(leaf, sizeof leaf, "S%llu-%lld", (unsigned long long)id, (long long)now_sec());
    trash = joinp(joinp(s->dir.c_str(), "trash").c_str(), leaf);
    // PR #1 review (3rd round): already gone is a reconcile, not a move -- and it has to be
    // written down as one. A TRASHED row with an empty trash_path is invisible to both halves of
    // gc (trash_scan skips a row with no path, reconciliation only looks at ACTIVE rows), so
    // without --now the snapshot stayed "trashed" in `status` and `list` forever, with no
    // collector that could ever finish it. The row goes straight to DEAD instead; pool entries
    // are already gone by here (a ref refuses the discard, --force drained them), and
    // pool_collect buries anything a filler puts back for a snapshot that is no longer ACTIVE.
    // PR #1 review (12th round): and "already gone" has to be an absence, not any lstat(2)
    // failure. An EACCES on <store>/snapshots or an EIO took this branch too, and the row then
    // went DEAD with the tree still sitting at S<n> -- where nothing would ever look at it
    // again: reconciliation scans ACTIVE rows and the suffix sweep only `*.wfs-tmp`. Any errno
    // that is not ENOENT/ENOTDIR is handed back to the caller instead (the Txn rolls back, so
    // the row is untouched) and `discard` fails with the reason the operator can act on.
    if (int prc = wfs::fs_probe(snapdir.c_str())) {
        if (!wfs::fs_gone(prc)) return prc;
        trash.assign("");
        tree_gone = true;
    }
    // Step (a) of the three-step protocol above: the reference check and the state change in one
    // transaction, with the name the tree is about to get written down. The rename used to be
    // *inside* this transaction, which meant a crash between the two rolled the row back to
    // ACTIVE while the tree sat in the trash with nothing naming it -- and the next collector
    // deleted it as an orphan, taking the baseline of every world forked from it with it
    // (PR #1 review, 5th round).
    // PR #1 review (8th round): and owned by this process while the rename runs, so that a
    // recovery in another process leaves this row alone until it is either finished or orphaned.
    // A row that goes straight to DEAD (the tree was already gone) is nobody's work in progress
    // and gets no owner.
    Stmt u(s->db, "UPDATE snapshots SET state=?, trash_path=?, trashed_at=?, owner_pid=?,"
                  " owner_start=? WHERE id=?");
    if (!u.ok()) return -EIO;
    u.i64(1, tree_gone ? WFS_ST_DEAD : WFS_ST_TRASHING);
    u.text(2, trash.c_str());
    u.i64(3, now_sec());
    if (tree_gone) { u.i64(4, 0); u.i64(5, 0); } else { trashing_own(u, 4, 5); }
    u.i64(6, (int64_t)id);
    if (u.step() != SQLITE_DONE) return -EIO;
    // Step (b) renames the tree into the trash, and it may do that only behind a row that is on
    // disk naming the place it is going (34th round, P1).
    if (int crc = t.commit()) return crc;
    }
    if (tree_gone) return 0;   // nothing to move, nothing to unlink, and the row is already DEAD
    if (int hrc = trash_crash_seam(0, 1, id, trash.c_str())) return hrc;
    // (b). The gate is on `root`, one level below what moves; renaming the directory that holds
    // it needs no access to the tree at all, so the gate stays closed until the deleter opens it.
    if (int rc = wfs::fs_rename(snapdir.c_str(), trash.c_str())) { trashing_undo(s, id, 1); return rc; }
    if (int hrc = trash_crash_seam(1, 1, id, trash.c_str())) return hrc;
    // (c)
    if (int crc = trashing_commit(s, id, 1, trash.c_str())) return crc;
    if (!immediate) return 0;
    return trash_delete_now(s, id, 1, trash);
}

// ---- PR #1 review (5th round): resolving a discard that was killed in the middle ---------------
//
// One TRASHING row means: the tree is at its home path or at trash_path, and at no third place.
// Which one it is, is the only question, and lstat answers it. Everything here is idempotent and
// runs under the store mutex a row at a time, so two processes doing it at once agree.
//
// PR #1 review (8th round): and only for a row nobody is working on. A TRASHING row carries its
// owner -- the pid of the `discard` or `restore` that wrote it, plus that process's own start
// time -- exactly as a CREATING row carries its producer's, and while that process is alive the
// row is in flight rather than crashed. lstat cannot tell the two apart: "the tree is still at
// home" is equally true of a discard that died before its rename and of one that is a
// microsecond away from making it, and this runs on every single store open. So a row whose
// owner is alive is skipped, whole, and the operation that owns it finishes it.
//
//   tree at trash_path   the rename happened. Finish the discard the user asked for -- unless
//                        something still references the snapshot, in which case the tree goes
//                        back and the row with it. A reference cannot appear after step (a)
//                        through fork or pool (both re-read the row under the write lock and
//                        refuse anything that is not ACTIVE), and since the 6th round of the
//                        PR #1 review it cannot appear through `adopt` either -- that used to
//                        register a world from its marker alone, carrying the snapshot id over
//                        whatever state the snapshot was in, and now refuses anything but an
//                        ACTIVE snapshot under the same write lock. So the count below is
//                        belt-and-braces: it is made against the present rather than against
//                        that argument, and a store another binary is also writing to is
//                        exactly the case an argument is worth nothing against.
//   tree at home         the rename never happened: the discard did not happen either.
//   tree nowhere         the same verdict, and the reconciliation half of gc then reports the
//                        row as dangling (and, with --reconcile, buries it). Better a row that
//                        says "active, not present" -- which `verify <path>` can repair when the
//                        directory merely moved -- than one that says "deleted" about a tree
//                        nobody deleted.
namespace wfs {

int trashing_recover(wfs_store *s, uint64_t *restored, uint64_t *finished) {
    if (!s) return -EINVAL;
    struct Pending {
        wfs_id id = 0;
        int is_snapshot = 0;
        String home;    // where the tree lives when the rename did not happen
        String trash;   // where it lives when it did
    };
    Vec<Pending> rows;
    {
        Guard g(s->mu);
        {
            Stmt q(s->db, "SELECT id, path, trash_path, owner_pid, owner_start FROM worlds WHERE state=4");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                // PR #1 review (8th round): its owner is still running, so this is a `discard`
                // or a `restore` in flight -- not a crash. Deciding for it would flip the row
                // under an operation that is about to do its own rename.
                if (wfs::producer_alive(q.col_i64(3), q.col_i64(4))) continue;
                Pending p;
                p.id = (wfs_id)q.col_i64(0);
                p.home.assign(q.col_text(1));
                p.trash.assign(q.col_text(2));
                rows.emplace_back(p);
            }
            // 32nd round, P2: a scan that stopped half way leaves TRASHING rows unresolved and
            // their trees looking like orphans to the sweep below. Nothing is resolved from a
            // list that is not the whole list.
            if (!q.done()) return -EIO;
        }
        {
            Stmt q(s->db, "SELECT id, path, trash_path, owner_pid, owner_start FROM snapshots WHERE state=4");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                if (wfs::producer_alive(q.col_i64(3), q.col_i64(4))) continue;
                Pending p;
                p.id = (wfs_id)q.col_i64(0);
                p.is_snapshot = 1;
                // The row records <store>/snapshots/S<n>/root; what a discard moves is the
                // directory above it, which holds the manifest `verify` needs.
                dirname_of(q.col_text(1), p.home);
                p.trash.assign(q.col_text(2));
                rows.emplace_back(p);
            }
            if (!q.done()) return -EIO;   // 32nd round, P2
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const Pending &p = rows[i];
        // PR #1 review (12th round): "the rename did not happen" is what puts the row back to
        // ACTIVE with its trash_path cleared, and it was a bool over lstat(2). An EACCES or an
        // EIO on <store>/trash therefore resurrected a row whose tree is in the trash and whose
        // home path holds nothing -- an ACTIVE world that is dangling by construction, which
        // the next `gc --reconcile` would then bury. A row whose tree cannot be located is left
        // TRASHING for the next pass instead; it is nobody's work in progress either way.
        int trc = p.trash.size() ? wfs::fs_probe(p.trash.c_str()) : -ENOENT;
        if (trc && !wfs::fs_gone(trc)) continue;
        bool in_trash = trc == 0;
        if (in_trash && p.is_snapshot) {
            SnapRefs refs;
            bool needed = false;
            {
                Guard g(s->mu);
                // PR #1 review (32nd round, P2): this decides whether a baseline in the trash is
                // put back or left there for the collector, and its failure used to read as "no
                // references". A row whose references cannot be counted stays TRASHING for the
                // next pass -- nobody's work in progress, exactly like a tree that cannot be
                // located two lines above.
                if (int rrc = snapshot_refs_locked(s, p.id, refs)) { (void)rrc; continue; }
                needed = refs.active_worlds || refs.creating_worlds || refs.trashing_worlds ||
                         refs.pool_entries;
            }
            // Somebody's baseline. Put it back before anything else can call it due.
            if (needed) {
                if (exists(p.home.c_str())) continue;               // cannot: leave it TRASHING
                if (wfs::fs_rename(p.trash.c_str(), p.home.c_str())) continue;
                in_trash = false;
            }
        }
        Guard g(s->mu);
        Txn t(s->db);
        if (!t.ok()) return t.err();   // 34th round, P1
        // PR #1 review (9th round), P18: the verdict above was reached by lstat'ing the very
        // trash_path this row recorded when the scan read it, so the write carries that path as
        // well as the state. Two recoveries running at once then agree by construction, and a
        // row whose path changed under us is left for the next pass rather than resolved from a
        // name nobody uses any more.
        Stmt u(s->db,
               in_trash
                   ? (p.is_snapshot
                          ? "UPDATE snapshots SET state=2, owner_pid=0, owner_start=0"
                            " WHERE id=? AND state=4 AND trash_path=?"
                          : "UPDATE worlds SET state=2, owner_pid=0, owner_start=0"
                            " WHERE id=? AND state=4 AND trash_path=?")
                   : (p.is_snapshot
                          ? "UPDATE snapshots SET state=1, trash_path='', trashed_at=0, owner_pid=0,"
                            " owner_start=0 WHERE id=? AND state=4 AND trash_path=?"
                          : "UPDATE worlds SET state=1, trash_path='', trashed_at=0, owner_pid=0,"
                            " owner_start=0 WHERE id=? AND state=4 AND trash_path=?"));
        if (!u.ok()) return -EIO;
        u.i64(1, (int64_t)p.id);
        u.text(2, p.trash.c_str());
        if (u.step() != SQLITE_DONE) return -EIO;
        int changed = sqlite3_changes(s->db);
        if (int crc = t.commit()) return crc;
        if (!changed) continue;
        if (in_trash) { if (finished) (*finished)++; }
        else if (restored) (*restored)++;
    }
    return 0;
}

} // namespace wfs

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

extern "C" int wfs_world_marker_store(wfs_store *s, const char *world_root, wfs_marker_store *out) {
    if (!s || !world_root || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    String real;
    if (int rc = wfs::fs_realpath(world_root, real)) return rc;
    MarkerData m;
    if (int rc = marker_read(real.c_str(), m)) return rc;
    out->same_store = ::strcmp(m.store_id, s->store_id.c_str()) == 0 ? 1 : 0;
    if (!m.store_path[0]) return 0;   // written before T2.3: the extension's fallback is real
    out->has_path = 1;
    copy_str(out->path, sizeof out->path, m.store_path);
    // Resolved, not compared as text: the extension will open(2) this path, so the question is
    // which directory it reaches. One that resolves nowhere is not this store's directory.
    String mreal;
    if (wfs::fs_realpath(m.store_path, mreal) == 0 && !::strcmp(mreal.c_str(), s->dir.c_str()))
        out->same_path = 1;
    return 0;
}

extern "C" int wfs_world_marker_refresh(wfs_store *s, const char *world_root) {
    if (!s || !world_root) return -EINVAL;
    wfs_identity id;
    // P1/P2 decide who may: the marker's store id has to be this store's (anything else is
    // WFS_E_FOREIGN_STORE from here), the row has to exist and the inode has to match it
    // (WFS_E_UNREGISTERED otherwise). Both of those are `adopt`'s business, not a refresh's.
    if (int rc = wfs_world_verify_identity(s, world_root, &id)) return rc;
    if (!id.registered) return WFS_E_UNREGISTERED;
    // P12: rewriting the marker is a world-level operation, so it serialises with the fork, the
    // checkpoint and the discard that read it.
    WorldLock lock;
    if (int rc = lock.take(id.path)) return rc;
    MarkerData m;
    if (int rc = marker_read(id.path, m)) return rc;
    // Everything but the path stays exactly as it was; the file is written beside the marker and
    // renamed over it, so a crash leaves the old marker whole rather than a truncated one.
    String text;
    marker_text(text, s->store_id.c_str(), s->dir.c_str(), m.world, m.name, m.snapshot, m.parent,
                m.created);
    String dst = joinp(id.path, WFS_MARKER_NAME);
    String tmp;
    int fd = -1;
    for (int t = 0; t < 8 && fd < 0; ++t) {
        char leaf[96];
        ::snprintf(leaf, sizeof leaf, ".world.wfs-%d-%d", (int)::getpid(), t);
        tmp = joinp(id.path, leaf);
        // O_EXCL is the claim: nothing that is not ours is ever written to or unlinked here.
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST) return -errno;
    }
    if (fd < 0) return -EEXIST;
    ssize_t n = ::write(fd, text.c_str(), text.size());
    int rc = (n == (ssize_t)text.size()) ? 0 : -EIO;
    ::close(fd);
    if (!rc) rc = wfs::fs_rename(tmp.c_str(), dst.c_str());
    if (rc) ::unlink(tmp.c_str());
    return rc;
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
        if (!t.ok()) return t.err();   // 34th round, P1
        Stmt u(s->db, "UPDATE worlds SET path=? WHERE id=?");
        if (!u.ok()) return -EIO;
        u.text(1, real.c_str());
        u.i64(2, (int64_t)m.world);
        if (u.step() != SQLITE_DONE) return -EIO;
        if (int crc = t.commit()) return crc;
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
    // PR #1 review (12th round): WFS_E_WORLD_MISSING is what sends an operator to
    // `gc --reconcile`, so it may only be said of a tree that is really not there. An EACCES or
    // an EIO comes back as itself.
    if (int prc = wfs::fs_probe(where)) {
        if (!wfs::fs_gone(prc)) return prc;
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
        if (!t.ok()) return t.err();   // 34th round, P1
        // PR #1 review (6th round): the baseline has to still be a baseline, and the question has
        // to be asked under the same write lock `discard S<n>` decides under (BEGIN IMMEDIATE).
        // An adopt registers a world from its marker alone -- nothing on disk is consulted about
        // the snapshot -- so it was the one way an ACTIVE world could appear for a snapshot that
        // was already on its way out: discard the only world, discard the snapshot (its reference
        // count is zero), then adopt a copy, and the copy's `diff` comes back WFS_E_SOURCE_GONE
        // for the rest of its life. Inside this transaction the two orders are the only two:
        // either the snapshot is still ACTIVE here and this row makes the copy a reference the
        // discard's own check will see, or the discard got there first and the adoption fails.
        //
        // Anything but ACTIVE is refused, TRASHING included: a TRASHING row is a discard that is
        // either in flight (its next step renames the tree away, and with --now unlinks it) or
        // was killed half way, and from here the two look exactly alike.
        if (parent && m.snapshot) {
            Stmt q(s->db, "SELECT state FROM snapshots WHERE id=?");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)m.snapshot);
            if (!q.row()) return q.done() ? WFS_E_SOURCE_GONE : -EIO;   // 32nd round, P2
            if (q.col_i64(0) != WFS_ST_ACTIVE) return WFS_E_SOURCE_GONE;
        }
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
        // The marker below is written for a row that exists; a commit that failed has none
        // (34th round, P1).
        if (int crc = t.commit()) return crc;
    }
    if (int rc = marker_write(real.c_str(), s->store_id.c_str(), s->dir.c_str(), id, nm, parent ? m.snapshot : 0, parent,
                              created)) {
        Guard g(s->mu);
        Txn t(s->db);
        // 34th round, P1: no transaction, no undo. The row is left ACTIVE for a directory with
        // no marker, which is what `gc --reconcile` reports and buries, and the caller gets the
        // marker's own error.
        if (t.ok()) {
            Stmt del(s->db, "DELETE FROM worlds WHERE id=?");
            if (del.ok()) { del.i64(1, (int64_t)id); del.step(); }
            (void)t.commit();   // best effort: reconciliation is the backstop either way
        }
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

// Linux sandbox preflight. This deliberately does not use wfs_path_check(): that public helper
// rejects a path carrying its own marker, while the selected world root is exactly the one marker
// that is allowed here. The identity check comes first so a moved registered world keeps the
// normal P1 row-repair semantics.
namespace {

struct SandboxLink {
    uint64_t dev, ino, nlink;
};

struct SandboxScan {
    Mutex mu;
    Vec<SandboxLink> links;
#ifdef __linux__
    uint64_t root_mount_id = 0;
#endif
};

#ifdef __linux__
// Return the kernel mount identity for one path. This deliberately asks for no attributes other
// than STATX_MNT_ID and does not follow a final symlink: every object reported by the tree walk,
// including a symlink itself, must belong to the same mount as the world root.
int sandbox_mount_id(const char *path, uint64_t *out) {
    struct statx sx;
    ::memset(&sx, 0, sizeof sx);
    if (::syscall(SYS_statx, AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &sx) != 0)
        return WFS_E_SANDBOX_MOUNT;
    if (!(sx.stx_mask & STATX_MNT_ID)) return WFS_E_SANDBOX_MOUNT;
    *out = (uint64_t)sx.stx_mnt_id;
    return 0;
}
#endif

int sandbox_scan_cb(void *opaque, const char *path, const char *rel, const struct stat &st, bool is_dir) {
    SandboxScan &scan = *(SandboxScan *)opaque;
#ifdef __linux__
    // st_dev is shared by bind mounts. statx's mount ID is the kernel identity that matters to
    // the recursive bind used by the namespace sandbox. Check every callback, including the
    // root, before FS_DIRS_PRE queues a directory for traversal. A failed syscall or a kernel
    // that does not return STATX_MNT_ID is unsafe to treat as an ordinary walk error: fail closed.
    uint64_t mount_id = 0;
    if (int rc = sandbox_mount_id(path, &mount_id)) return rc;
    if (mount_id != scan.root_mount_id)
        return WFS_E_SANDBOX_MOUNT;
#endif
    if (rel && *rel) {
        const char *leaf = ::strrchr(rel, '/');
        leaf = leaf ? leaf + 1 : rel;
        if (!::strcmp(leaf, WFS_MARKER_NAME) && ::strcmp(rel, WFS_MARKER_NAME) != 0)
            return WFS_E_PATH_REFUSED;
    }
    if (!is_dir && st.st_nlink > 1) {
        SandboxLink link{(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_nlink};
        Guard g(scan.mu);
        scan.links.emplace_back(link);
    }
    return 0;
}

int sandbox_link_cmp(const void *a, const void *b) {
    const SandboxLink &x = *(const SandboxLink *)a;
    const SandboxLink &y = *(const SandboxLink *)b;
    if (x.dev != y.dev) return x.dev < y.dev ? -1 : 1;
    if (x.ino != y.ino) return x.ino < y.ino ? -1 : 1;
    return 0;
}

} // namespace

extern "C" int wfs_world_check_sandbox(wfs_store *s, wfs_id id) {
    if (!s || !id) return -EINVAL;
    wfs_world_rec row;
    {
        Guard g(s->mu);
        if (int rc = world_row(s, id, row)) return rc;
    }
    if (row.state != WFS_ST_ACTIVE) return -ESTALE;

    wfs_identity identity;
    if (int rc = wfs_world_verify(s, id, &identity)) return rc;
    if (!identity.registered) return WFS_E_UNREGISTERED;

    String root;
    bool marker = false;
    if (int rc = check_path(s, identity.path, PATH_SOURCE, root, &marker)) return rc;
    if (!marker) return WFS_E_NOT_A_WORLD;

    SandboxScan scan;
#ifdef __linux__
    // Capture the root mount once. The callback compares every entry against this immutable
    // value, so bind mounts on the same device are rejected as well as mounts on another device.
    if (int rc = sandbox_mount_id(root.c_str(), &scan.root_mount_id)) return rc;
#endif
    if (int rc = wfs::fs_walk_tree(root.c_str(), 4, wfs::FS_DIRS_PRE, &scan, sandbox_scan_cb)) return rc;
    if (scan.links.empty()) return 0;
    ::qsort(scan.links.data(), scan.links.size(), sizeof(SandboxLink), sandbox_link_cmp);
    for (size_t i = 0; i < scan.links.size();) {
        size_t j = i + 1;
        while (j < scan.links.size() && scan.links[j].dev == scan.links[i].dev &&
               scan.links[j].ino == scan.links[i].ino)
            ++j;
        uint64_t observed = (uint64_t)(j - i);
        for (size_t k = i; k < j; ++k)
            if (scan.links[k].nlink != observed) return WFS_E_SANDBOX_UNSAFE;
        i = j;
    }
    return 0;
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

// The manifest's escaping in reverse (hardlinks.cpp does the same thing). `\\r` since the 12th
// round of the PR #1 review: an older manifest cannot hold the two-byte sequence backslash-r,
// because a literal backslash was always written `\\\\` and is consumed as a pair here.
void unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '\\' && r[1]) {
            ++r;
            *w++ = (*r == 'n') ? '\n' : (*r == 'r') ? '\r' : *r;
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
        // PR #1 review (12th round): the terminator only. A trailing CR belongs to the name --
        // the writer escapes it now -- and eating it made `verify` report a file that is
        // perfectly intact as modified, and its neighbour as extra.
        size_t n = ::strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (!n) continue;
        char kind = line[0];
        char *p = line + 1;
        if (*p != ' ') continue;
        unsigned mode = 0;
        unsigned long long size = 0, nlink = 0;
        long long sec = 0;
        long nsec = 0;
        int consumed = 0;
        // PR #1 review (19th round): the head ends at the nlink, and the byte after it is the
        // one separator Manifest::line wrote (`"%c %o %llu %lld.%ld %llu "`, one space, then the
        // name verbatim). A trailing whitespace directive here ate that space AND every space or
        // tab the name itself begins with, so ` a` was checked as `a`: a file this snapshot does
        // not have, or -- worse -- a neighbour of the same name whose size and mtime decide the
        // verdict. The root's line is the one with nothing after the separator, and it still
        // reads as the empty rel it always did. hardlinks.cpp's reader had the same directive.
        if (::sscanf(p, " %o %llu %lld.%ld %llu%n", &mode, &size, &sec, &nsec, &nlink, &consumed) != 5
            || !consumed || p[consumed] != ' ')
            continue;
        char *rel = p + consumed + 1;
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

    // T2.5, and PR #1 review (6th round): the row says how many hardlink groups this snapshot
    // has, and the manifest's own section is where every fork and every pool filler reads them
    // back from. A manifest that has lost that section -- or part of it -- is a snapshot that
    // cannot be cloned faithfully any more, and both of those now refuse it outright, so this is
    // the command that has to be able to say why. (A manifest that is missing altogether was
    // already an error above: the fopen fails.)
    if (r.hl_groups) {
        wfs::HardlinkSet hl;
        int hrc = wfs::hardlinks_manifest_read(mp.c_str(), hl);
        // PR #1 review (13th round): and the groups are asked of the tree, not only of the
        // header. A manifest whose members were exchanged between two groups passes every count
        // -- group total, name total, each group's nlink, no name twice -- and describes a tree
        // nobody has: this is the command that has to be able to say so, and it is already
        // inside the gate, which is where the lstats have to happen.
        if (hrc || hl.groups.size() != (size_t)r.hl_groups ||
            wfs::hardlinks_verify_groups(r.path, hl)) {
            out->modified++;
            if (!out->first_bad[0]) copy_str(out->first_bad, sizeof out->first_bad, mp.c_str());
        }
    }

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
//   cheap    half-built rows and the trees they RECORD (P8), stale seatbelt profiles, pool
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

// A suffix sweep, and therefore a guess: every `*.wfs-tmp` in `dir` goes. That is only ever
// allowable inside the store, where every name was made by us -- `<store>/snapshots/S<n>.wfs-tmp`
// is derived from a row id and nothing else can be called that. It used to be run over the
// parent directory of every world as well, i.e. over the user's own directories, where it
// destroyed `~/w/notes.wfs-tmp` on a routine `world fs gc`. A fork's leftovers are now found
// from the path its CREATING row recorded (gc_creating_tmp below), never from a name pattern.
// PR #1 review (7th round): under the wake's deadline, like everything else in the cheap half
// that removes a whole tree. What this sweeps are half-built snapshots -- clones of a source
// tree, not a handful of files -- and it used to run flat out right after the CREATING loop,
// which made that loop's own deadline pointless for exactly the trees it had just stopped on.
//
// PR #1 review (10th round), P18: "nothing names this tree" is a verdict, and a verdict about
// live rows is re-asked of the live rows under the store mutex immediately before the deletion
// (docs/M1_DESIGN.md §3). The sweep asked nothing at all: it removed every `S<n>.wfs-tmp` on
// sight, including the one `wfs_snapshot_create()` had just made and was cloning into. The
// CREATING pass above is careful about exactly that -- it skips a row whose producer is alive --
// and the sweep then deleted the very tree that pass had spared, so the create failed with the
// clone half gone. So each entry's `S<n>` is parsed back into a row id and asked of the
// snapshots table: see snap_tmp_named_by_row().
//
// PR #1 review (10th round): and only over <store>/snapshots. The claim rule below IS the
// snapshots table, so a second call site sweeping some other store directory by suffix would
// inherit a rule that does not describe it; it has to bring its own (as pool.cpp's sweep does).
bool snap_tmp_named_by_row(wfs_store *s, const char *leaf, size_t sl) {
    // `S<digits>.wfs-tmp` and nothing else. Anything that does not parse is not a name this
    // store ever wrote under <store>/snapshots, so it keeps the old behaviour and goes.
    size_t n = ::strlen(leaf);
    if (leaf[0] != 'S' || n <= sl + 1) return false;
    uint64_t id = 0;
    for (size_t i = 1; i + sl < n; ++i) {
        if (leaf[i] < '0' || leaf[i] > '9') return false;
        if (id > (UINT64_MAX - (uint64_t)(leaf[i] - '0')) / 10) return false;   // not a row id
        id = id * 10 + (uint64_t)(leaf[i] - '0');
    }
    if (!id) return false;
    Guard g(s->mu);
    // Any state but DEAD counts as naming it, and deliberately more than just CREATING:
    //   * CREATING with a live producer is a snapshot being built right now -- the tree under
    //     the sweep's hand is its clone;
    //   * CREATING with a dead producer is the CREATING pass's job, not the sweep's. That pass
    //     removes both trees and only then deletes the row; when it cannot (EPERM, a deadline),
    //     it keeps the row on purpose so the next wake retries under the failure cap. The sweep
    //     racing it would remove the tree without ever clearing the row.
    //   * ACTIVE/TRASHING/TRASHED means `S<n>` itself is a real snapshot. A `.wfs-tmp` beside it
    //     should not exist at all, and if one ever does, leaving a stray directory in the store
    //     is the cheap mistake -- wfs_snapshot_create() clears it before reusing that id, and
    //     `gc --status` counts nothing it names.
    // A row that cannot be read at all is likewise "do not delete": the sweep is the last and
    // least informed of gc's passes, and its guess is never allowed to beat a row.
    Stmt q(s->db, "SELECT 1 FROM snapshots WHERE id=? AND state<>?");
    if (!q.ok()) return true;
    q.i64(1, (int64_t)id);
    q.i64(2, (int64_t)WFS_ST_DEAD);
    // PR #1 review (32nd round, P2): and a step that failed is a row that could not be read,
    // not a row that is not there. Same answer: do not delete.
    return q.row() || !q.done();
}

// This sweep's key in the store's shared gc retry counter (internal.h). The path, exactly as
// pool.cpp keys its own row-less trees: there is no row to key on -- that is what makes this
// tree the sweep's business in the first place.
String snap_tmp_fail_key(const String &path) {
    String k("gcfail:snaptmp:");
    k.append(path.c_str());
    return k;
}

// PR #1 review (11th round): a tree here that will not go is reported, counted and retried, the
// same as everything else the collector cannot remove. The result used to be dropped: a
// row-less `S<n>.wfs-tmp` under an ACL, an EPERM or a transient EIO was neither counted in the
// report nor allowed to set work_remains, and wfs_gc_pending() only ever classifies the trash,
// so nothing came back for it -- the tree sat in the store until somebody ran gc by hand. It has
// no row (that is the whole reason the sweep owns it), so the counter is keyed on its path.
//
// PR #1 review (28th round, P2): and a <store>/snapshots that cannot be READ is not an empty
// one. `if (!d) return 0;` -- an EACCES on the directory, an EIO -- reported the sweep as having
// run and found nothing, and readdir(3), which reports its own failure through errno alone, was
// never asked, so a scan that stopped halfway was an end of directory. Either way a row-less
// `S<n>.wfs-tmp` was neither removed, nor counted, nor retried. Only ENOENT is absence now; the
// rest goes to wfs::gc_note_unreadable (internal.h), which keys the shared retry counter on the
// DIRECTORY and sets work_remains under the cap, so the worker chain comes back for it.
int rm_tmp_in_store_dir(wfs_store *s, const char *dir, uint64_t *removed, int64_t deadline_us,
                        int *work_remains, uint64_t *failed, wfs::DirUnreadable *unreadable) {
    if (!under_dir(dir, s->dir.c_str())) return -EINVAL;   // not ours to sweep
    if (::strcmp(dir, joinp(s->dir.c_str(), "snapshots").c_str()) != 0) return -EINVAL;
    DIR *d = ::opendir(dir);
    if (!d) {
        if (errno == ENOENT) return 0;   // no such directory: there is nothing in it
        wfs::gc_note_unreadable(s, unreadable, dir, errno, true, work_remains);
        return 0;
    }
    bool read_through = true;
    size_t sl = ::strlen(WFS_TMP_SUFFIX);
    for (;;) {
        errno = 0;
        struct dirent *e = ::readdir(d);
        if (!e) {
            if (errno) {
                wfs::gc_note_unreadable(s, unreadable, dir, errno, true, work_remains);
                read_through = false;
            }
            break;
        }
        size_t n = ::strlen(e->d_name);
        if (n <= sl || ::strcmp(e->d_name + n - sl, WFS_TMP_SUFFIX) != 0) continue;
        // Out of time. The directory was not read to its end, so its retry counter is left
        // exactly as it was -- this wake proved nothing about what is further down it.
        if (deadline_us && now_us() >= deadline_us) {
            if (work_remains) *work_remains = 1;
            read_through = false;
            break;
        }
        if (snap_tmp_named_by_row(s, e->d_name, sl)) continue;
        String p = joinp(dir, e->d_name);
        int partial = 0;
        int rc = wfs::fs_remove_tree(p.c_str(), deadline_us, &partial);
        // Out of time, not stuck: the tree is still there and is still row-less next wake, so
        // nothing is counted as a failure and the successor carries on from here.
        if (partial) { if (work_remains) *work_remains = 1; read_through = false; break; }
        // PR #1 review (12th round): "it went" has to be proven, not assumed -- an lstat(2) that
        // fails for EACCES or EIO is not evidence that the tree is gone, and this row-less tree
        // has nothing else in the store that remembers it.
        if (!proven_gone(p.c_str())) {
            if (failed) (*failed)++;
            if (wfs::gc_fail_bump(s, snap_tmp_fail_key(p).c_str()) < wfs::kGcFailCap && work_remains)
                *work_remains = 1;
            continue;
        }
        // Gone -- by this call or by somebody else's. Either way it is not waiting any more.
        wfs::gc_fail_clear(s, snap_tmp_fail_key(p).c_str());
        if (rc == 0) (*removed)++;
    }
    ::closedir(d);
    // Read right through: whatever a previous wake could not read here it can read now, so the
    // directory's retry counter goes back to zero (the same thing a tree that finally went does).
    wfs::gc_note_readable(s, dir, read_through);
    return 0;
}

// Is `p` still the half-built clone that CREATING row `id` recorded? The row is evidence about
// the past and this is a directory in somebody's workspace, so it is re-checked against the
// present before anything is removed:
//   * it is a directory that is there (gone, or not a directory: nothing to do);
//   * no live row claims its inode -- that would mean the tree reached its target and was
//     registered, and the path we are looking at is a second name for a real world;
//   * a `.world` marker, if there is one, names this very world in this very store. The publish
//     order writes the marker before the rename, so our own half-published clone has one; a
//     marker naming anything else is somebody else's world and is left alone.
// Anything unreadable or unrecognised is left alone too.
//
// PR #1 review (12th round): and "unreadable" is now told apart from "not ours". Both used to
// be a bare `false`, on which the caller marks the row DEAD and clears its tmp_path -- fine for
// a tree that is gone or that belongs to somebody else, and exactly the permanent stranding the
// 5th round fixed for a tree that is still there and still ours. `*undecided` says which: with
// it set, the caller keeps the CREATING row, counts the tree and comes back for it.
//
// PR #1 review (26th round, P1): and the answer is a descriptor, for the same reason the trash
// claim is one. Everything here used to be a fresh lookup of the same name -- lstat the tree,
// lstat its marker, read its marker, and then, back in the caller, remove the tree -- and that
// name is a path in the *user's own* target directory (the 5th round: a fork's temporary is
// deliberately never found by a suffix sweep, it is named by the row alone). A `mv` between the
// marker read and the removal and gc deleted whatever had taken the name. So the tree is opened
// once, the marker is read through that descriptor (marker_read_at), the inode the database is
// asked about is that descriptor's, and the caller removes the tree through it as well. `c` is
// the claim the caller then deletes with; it is only filled when this returns true.
bool gc_tmp_is_removable(wfs_store *s, wfs_id id, const char *p, bool *undecided, TrashClaim &c) {
    if (undecided) *undecided = false;
    if (!p || !*p) return false;
    // No chmod on the way in, unlike the trash claim: there the row's dev/ino prove the tree is
    // ours before anything else happens, and here the proof is *inside* the tree (its marker).
    // A directory in the user's workspace that we cannot open is one we have not proved
    // anything about yet, so it keeps its bits and its row -- which is exactly what the 12th
    // round's `undecided` was for, when the same case was an EACCES on the marker's lstat.
    int fd = ::open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = -errno;
        // fs_gone(): nothing is there, or what is there is not a directory -- nothing to do,
        // exactly as before. Anything else (an EACCES, an EIO, a symlink O_NOFOLLOW refused) is
        // a tree we could not ask about, and the caller keeps the row rather than bury it.
        if (!wfs::fs_gone(e) && undecided) *undecided = true;
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        if (undecided) *undecided = true;
        return false;
    }
    c.fd = fd;
    c.dev = (uint64_t)st.st_dev;
    c.ino = (uint64_t)st.st_ino;
    // The name the final rmdir will use, and the descriptor of the directory it is in.
    if (trash_claim_parent(c, p)) {
        if (undecided) *undecided = true;
        return false;
    }
    MarkerData m;
    // A marker that is there and cannot be read is a not-an-answer: it may yet say this tree is
    // ours, or that it is not. Only a marker that is genuinely absent lets the checks below
    // decide by themselves (the publish order writes the marker before the rename, so a tree
    // that never got one is a clone that never got that far).
    int mrc = marker_read_at(c.fd, m);
    if (mrc == 0) {
        if (m.world != id || ::strcmp(m.store_id, s->store_id.c_str()) != 0) return false;
    } else if (!wfs::fs_gone(mrc)) {
        if (undecided) *undecided = true;
        return false;
    }
    Guard g(s->mu);
    // PR #1 review (9th round), P18: and the row still has to be the row this job was made from
    // -- CREATING, still naming this tree. The scan is a snapshot; a fork whose producer this
    // wake judged dead may have published in the meantime, and the published world's tree is
    // not gc's to remove.
    {
        Stmt r(s->db, "SELECT 1 FROM worlds WHERE id=? AND state=0 AND tmp_path=?");
        if (!r.ok()) return false;
        r.i64(1, (int64_t)id);
        r.text(2, p);
        // 32nd round, P2: no row and a failed read both come out false here, and false is
        // "leave it alone" -- the one answer that removes nothing.
        if (!r.row()) return false;
    }
    Stmt q(s->db, "SELECT COUNT(*) FROM worlds WHERE dir_dev=? AND dir_ino=? AND state<>3 AND id<>?");
    if (!q.ok()) return false;
    q.i64(1, (int64_t)st.st_dev);
    q.i64(2, (int64_t)st.st_ino);
    q.i64(3, (int64_t)id);
    // 32nd round, P2: likewise. A count that could not be read is not a count of zero, and
    // only a count of zero lets this tree go.
    return q.row() && q.col_i64(0) == 0;
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
    String row_path;  // and what the row said when this job was made (PR #1 review, 8th round):
                      // `path` follows the collector's own `.deleting` rename, the row does not,
                      // and every write the collector makes to that row is conditional on it
                      // still saying this.
    wfs_id row = 0;   // 0 = an orphan: a directory in the trash that no row claims
    int is_snapshot = 0;
    // PR #1 review (25th round, P1): what the row says its tree IS, not only where it is.
    // Worlds only -- a snapshot's trash is always inside the store (see trash_scan) and its row
    // has no dev/ino columns to compare with. 0 means "no identity recorded": don't compare.
    uint64_t dir_dev = 0, dir_ino = 0;
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
    // PR #1 review (28th round, P2): and what this scan could not look at. <store>/trash is
    // read by every one of trash_scan's three callers -- the collector, `gc --status` and
    // wfs_gc_pending() -- and an opendir/readdir that failed there used to leave all three of
    // them saying "no row-less orphans": nothing was collected, nothing was counted, and the
    // spawn decision that is the head of the worker chain said there was nothing to do.
    wfs::DirUnreadable unreadable;
};

// PR #1 review (5th round): a directory in the trash is row-less only when NO row names it, in
// ANY state. The collector used to build this list from the TRASHED rows alone, so a row in the
// middle of its discard (TRASHING) -- or one whose state the same wake was about to change --
// left its tree looking like an orphan, and an orphan is deleted immediately, retention and
// restorability and "this is somebody's baseline" all skipped. The `.deleting` spelling counts
// as the same name: the collector renames the tree first and records it second, so between the
// two the row still names the tree by its old name.
//
// PR #1 review (32nd round, P2): and the list is either the whole list or no answer at all. A
// prepare that failed was skipped with `continue` and a step that failed ended the loop, and
// either one drops rows out of `claimed` -- which is precisely how a tree that a row does name
// becomes an orphan, and an orphan is deleted on sight. It returns its verdict now, and
// trash_scan() hands the caller -EIO rather than a classification built on half the rows.
int claim_trash_paths(wfs_store *s, Vec<String> &claimed) {
    for (int table = 0; table < 2; ++table) {
        Stmt q(s->db, table ? "SELECT trash_path FROM snapshots WHERE trash_path<>''"
                            : "SELECT trash_path FROM worlds WHERE trash_path<>''");
        if (!q.ok()) return -EIO;
        while (q.row()) {
            String tp(q.col_text(0));
            if (!tp.size()) continue;
            if (!ends_with(tp.c_str(), WFS_DELETING_SUFFIX)) {
                String d(tp);
                d.append(WFS_DELETING_SUFFIX);
                claimed.emplace_back(d);
            }
            claimed.emplace_back(tp);
        }
        if (!q.done()) return -EIO;
    }
    return 0;
}

// PR #1 review (9th round), P18: the same question claim_trash_paths answers from a snapshot,
// asked of the live rows instead -- does ANY world or snapshot row, in ANY state, name this
// trash path right now? "Names" has claim_trash_paths' meaning: a row's trash_path claims both
// that name and that name with `.deleting` appended, because the collector renames the tree
// first and records the new name second. The caller holds s->mu.
bool trash_path_claimed_locked(wfs_store *s, const char *p) {
    String base(p);
    size_t n = base.size(), sl = ::strlen(WFS_DELETING_SUFFIX);
    if (n > sl && !::strcmp(base.c_str() + n - sl, WFS_DELETING_SUFFIX)) base.resize(n - sl);
    for (int table = 0; table < 2; ++table) {
        Stmt q(s->db, table ? "SELECT 1 FROM snapshots WHERE trash_path=? OR trash_path=?"
                            : "SELECT 1 FROM worlds WHERE trash_path=? OR trash_path=?");
        if (!q.ok()) return true;   // cannot tell, and "cannot tell" is never "delete it"
        q.text(1, p);
        q.text(2, base.c_str());
        // PR #1 review (32nd round, P2): a step that failed is "cannot tell" too. It used to
        // fall through as "no row names this tree", which is the verdict that deletes it.
        if (q.row() || !q.done()) return true;
    }
    return false;
}

// `collector` is the one caller that may write: it bumps the shared retry counter for a trash
// directory it could not read (and clears it for one it read right through) and sets
// *work_remains under the cap. `gc --status` and wfs_gc_pending() pass false and only count.
int trash_scan(wfs_store *s, int64_t cutoff, TrashView &v, bool collector = false,
               int *work_remains = nullptr) {
    Vec<String> claimed;   // trash paths a row points at, whatever state that row is in
    {
        Guard g(s->mu);
        if (int crc = claim_trash_paths(s, claimed)) return crc;
        {
            Stmt q(s->db, "SELECT id, trash_path, trashed_at, entries, dir_dev, dir_ino"
                          " FROM worlds WHERE state=2");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                String tp(q.col_text(1));
                if (!tp.size()) continue;
                v.worlds++;
                v.tree_entries += (uint64_t)q.col_i64(3);
                TrashJob j;
                j.dir_dev = (uint64_t)q.col_i64(4);
                j.dir_ino = (uint64_t)q.col_i64(5);
                j.row_path = tp;
                trash_follow_deleting(tp);
                j.path = tp;
                j.row = (wfs_id)q.col_i64(0);
                if (ends_with(tp.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
                else if (q.col_i64(2) <= cutoff) v.due.emplace_back(j);
                else v.waiting++;
            }
            // PR #1 review (32nd round, P2): every row of it, or nothing. A row this loop never
            // reached is a row whose tree the orphan pass below then finds unclaimed.
            if (!q.done()) return -EIO;
        }
        {
            Stmt q(s->db, "SELECT id, trash_path, trashed_at, entries FROM snapshots WHERE state=2");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                String tp(q.col_text(1));
                if (!tp.size()) continue;
                v.snapshots++;
                v.tree_entries += (uint64_t)q.col_i64(3);
                TrashJob j;
                j.row_path = tp;
                trash_follow_deleting(tp);
                j.path = tp;
                j.row = (wfs_id)q.col_i64(0);
                j.is_snapshot = 1;
                if (ends_with(tp.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
                else if (q.col_i64(2) <= cutoff) v.due.emplace_back(j);
                else v.waiting++;
            }
            if (!q.done()) return -EIO;   // 32nd round, P2
        }
        // A discard that was killed in the middle. Its tree is in the trash and it is nobody's
        // orphan; it is also not collectable until trashing_recover() has said which way it
        // goes, so it is counted and never queued.
        {
            Stmt q(s->db, "SELECT id, trash_path, entries, 0 FROM worlds WHERE state=4"
                          " UNION ALL SELECT id, trash_path, entries, 1 FROM snapshots WHERE state=4");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                // PR #1 review (12th round): counted unless its absence is proven. A tree that
                // cannot be lstat'ed is still waiting for the collector, and `gc --status` must
                // not answer "nothing is waiting" because it could not look.
                if (!*q.col_text(1) || proven_gone(q.col_text(1))) continue;   // the rename never happened
                if (q.col_i64(3)) v.snapshots++; else v.worlds++;
                v.tree_entries += (uint64_t)q.col_i64(2);
                v.waiting++;
            }
            if (!q.done()) return -EIO;   // 32nd round, P2
        }
    }
    if (wfs_test_before_trash_orphans) wfs_test_before_trash_orphans(wfs_test_before_trash_orphans_ctx);
    // Directories in <store>/trash that no row claims: a store restored from a backup, or a
    // *.deleting tree whose row was already marked DEAD. They go immediately -- there is nothing
    // left that could restore them.
    //
    // PR #1 review (28th round, P2): and a trash that cannot be READ is not an empty trash. The
    // `if (DIR *d = opendir(...))` below swallowed every failure -- an EACCES on <store>/trash,
    // an EIO -- and readdir(3), which reports its own failure through errno alone (13th round),
    // was never asked, so a scan that stopped halfway looked like the end of the directory. The
    // orphan in there was then not collected, not counted by `gc --status`, and not seen by
    // wfs_gc_pending(), which is what decides whether a collector runs at all. Only ENOENT is
    // absence; everything else is recorded and, on the collector's pass, retried under the cap.
    String trashdir = joinp(s->dir.c_str(), "trash");
    Vec<String> maybe_orphans;
    DIR *d = ::opendir(trashdir.c_str());
    if (!d) {
        if (errno != ENOENT)
            wfs::gc_note_unreadable(s, &v.unreadable, trashdir.c_str(), errno, collector,
                                    work_remains);
    } else {
        bool read_through = true;
        for (;;) {
            errno = 0;
            struct dirent *e = ::readdir(d);
            if (!e) {
                if (errno) {
                    wfs::gc_note_unreadable(s, &v.unreadable, trashdir.c_str(), errno, collector,
                                            work_remains);
                    read_through = false;
                }
                break;
            }
            if (e->d_name[0] == '.') continue;
            String p = joinp(trashdir.c_str(), e->d_name);
            bool wanted = false;
            for (size_t i = 0; i < claimed.size(); ++i)
                if (!::strcmp(claimed[i].c_str(), p.c_str())) { wanted = true; break; }
            if (wanted) continue;
            maybe_orphans.emplace_back(p);
        }
        ::closedir(d);
        wfs::gc_note_readable(s, trashdir.c_str(), collector && read_through);
    }
    // PR #1 review (9th round), P18: `claimed` is a snapshot, and the readdir above is not.
    // A `discard` that commits its TRASHING row and renames its tree into the trash between the
    // two produces a directory `claimed` has never heard of -- and a row-less orphan is deleted
    // on sight, retention, restorability and "this is somebody's baseline" all skipped, right
    // beside a discard that is still running. So the verdict is re-asked of the live rows, under
    // the store mutex, immediately before the tree is queued; gc_delete_one asks again under the
    // same mutex immediately before it starts deleting.
    if (maybe_orphans.size()) {
        Guard g(s->mu);
        for (size_t i = 0; i < maybe_orphans.size(); ++i) {
            if (trash_path_claimed_locked(s, maybe_orphans[i].c_str())) continue;
            TrashJob j;
            j.path = maybe_orphans[i];
            if (ends_with(j.path.c_str(), WFS_DELETING_SUFFIX)) v.deleting.emplace_back(j);
            else v.due.emplace_back(j);
        }
    }
    return 0;
}

// ---- PR #1 review (8th round): every write the collector makes to a row is conditional -------
//
// The collector queues a TRASHED row, and by the time it gets to it the row may not be that row
// any more. `restore W<n>` takes the row to TRASHING under BEGIN IMMEDIATE and renames the tree
// out of the trash and back home; the collector's rename to `<name>.deleting` then answers
// -ENOENT, which used to mean "somebody deleted it already" and buried the row -- a DEAD row for
// a world that is back at its home path and ACTIVE, with no way back.
//
// So the two UPDATEs below carry the row's whole identity in their WHERE clause: still TRASHED,
// and still naming the tree this job was made from. Zero rows changed means the job is not the
// collector's any more, and the caller then counts nothing and reports nothing.
//
// Marks a row DEAD once its tree is gone. Worlds and snapshots keep their row: the DAG is
// history, and a dangling reference has to be explainable afterwards.
bool mark_dead(wfs_store *s, const TrashJob &j) {
    if (!j.row) return true;   // an orphan has no row to bury
    Guard g(s->mu);
    Txn t(s->db);
    if (!t.ok()) return false;   // 34th round, P1: false is "not buried", which is this
    Stmt u(s->db,                //          collector's safe answer -- nothing is deleted for it
           j.is_snapshot
               ? "UPDATE snapshots SET state=?, trash_path='' WHERE id=? AND state=2 AND trash_path=?"
               : "UPDATE worlds SET state=?, trash_path='' WHERE id=? AND state=2 AND trash_path=?");
    if (!u.ok()) return false;
    u.i64(1, WFS_ST_DEAD);
    u.i64(2, (int64_t)j.row);
    u.text(3, j.row_path.c_str());
    if (u.step() != SQLITE_DONE) return false;
    bool changed = sqlite3_changes(s->db) != 0;
    if (t.commit()) return false;
    return changed;
}

// PR #1 review (15th round, P1): the rename to `<name>.deleting` IS the collector's claim on the
// entry, so it happens inside the transaction that checks the claim -- not after it.
//
// It used to be three separate steps: trash_row_still_ours() read the row, the rename followed,
// and a conditional UPDATE recorded the new name. A `restore W<n>` landing between the first two
// took the row to TRASHING under BEGIN IMMEDIATE and had not yet renamed the tree home, so the
// collector's rename found the tree exactly where its job said it was and succeeded; the UPDATE
// behind it then matched nothing (the row is not TRASHED any more) and the unlink went ahead all
// the same. The restore's own rename then failed -- the tree is at the `.deleting` name -- and it
// fell back to TRASHED: a restorable world, deleted, with a row still offering to restore it.
//
// Both sides now take the SQLite write lock: restore's step (a) is BEGIN IMMEDIATE and so is
// this, so the two serialise. Either the restore is first, and this re-read sees TRASHING and
// claims nothing; or this is first, and the restore sees the row naming `.deleting`
// (trash_is_deleting -> WFS_E_TRASH_DELETING) with its tree untouched. What is held across the
// rename is one rename(2) inside one transaction -- microseconds, and no tree removal at all.
// (Until the 20th round the caller first folded a `<name>.deleting` leftover into this job out
// here, because that removal could take minutes. Nothing folds anything now: this transaction
// is why such a leftover can never be ours -- see trash_mark_deleting().)
//
// A crash between the rename and the commit leaves the row naming the old name with the tree at
// the new one, which is the state trash_follow_deleting() already resolves: both names belong to
// the row.
//
// Returns 0 with `deleting` set to the name the tree now has, 1 if the entry is not the
// collector's any more, -ENOENT if it is at neither name, and any other errno as itself.
int gc_claim_deleting(wfs_store *s, TrashJob &j, String &deleting, TrashClaim &claim) {
    Guard g(s->mu);
    Txn t(s->db);
    // 34th round, P1: the rename to `.deleting` below and the row that records it are one write
    // or they are nothing. Without the write lock the claim proves nothing -- a restore could
    // take the row between the check and the rename -- so the entry is left exactly as it is.
    if (!t.ok()) return t.err();
    if (j.row) {
        Stmt q(s->db,
               j.is_snapshot ? "SELECT state, trash_path, 0, 0 FROM snapshots WHERE id=?"
                             : "SELECT state, trash_path, dir_dev, dir_ino FROM worlds WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)j.row);
        // 32nd round, P2: 1 is "not the collector's any more", which is a safe thing to say
        // about a row we could read. A row we could NOT read is a different answer and gets one.
        if (!q.row()) return q.done() ? 1 : -EIO;
        if (q.col_i64(0) != WFS_ST_TRASHED || ::strcmp(q.col_text(1), j.row_path.c_str()))
            return 1;
        // PR #1 review (25th round, P1): and the last question before the rename is whether the
        // thing at that path IS this row's tree. The row naming it is not enough -- the
        // cross-volume entry sits at `<parent>/.wfs-trash/W<id>-<timestamp>`, which is the
        // user's directory and a name they can guess, so during the retention period they can
        // move the tree away and leave a directory of their own there; the collector renamed it
        // to `.deleting` and deleted it whole. The row's dev/ino are read here, inside the
        // transaction that makes the claim, so identity and claim cannot come apart. A tree
        // that is genuinely gone still goes the way it always did (-ENOENT out of
        // trash_mark_deleting below); anything else that cannot be proven ours is left exactly
        // as it is, counted and named.
        // This covers the `.deleting` spelling too: j.path is whichever of the two names
        // trash_follow_deleting() found the tree at, and a same-volume rename keeps the inode,
        // so our own half-deleted tree matches and somebody else's directory at that name does
        // not.
        // PR #1 review (26th round, P1): and the answer is a descriptor, because everything
        // after this question is a fresh name lookup -- see TrashClaim above. A world row with
        // an identity recorded (every one this store writes) is claimed through the fd from
        // here on; a snapshot row has no dev/ino to check, and neither has a row older than the
        // columns, so those two keep the name-based protocol they have always had.
        uint64_t rdev = (uint64_t)q.col_i64(2), rino = (uint64_t)q.col_i64(3);
        if (rino) {
            int irc = trash_claim_open(j.path.c_str(), rdev, rino, claim);
            if (irc && !wfs::fs_gone(irc)) return irc;
        }
        trash_claim_seam(j.is_snapshot, j.row, j.path.c_str());
    } else if (trash_path_claimed_locked(s, j.path.c_str())) {
        // P18: an orphan has no row to claim, so what it re-asks is "does any row name this
        // tree now?" -- and it asks it in here, under the same write lock, because a `discard`
        // commits its TRASHING row before it renames its tree into the trash. Asking outside
        // left the window this whole helper exists to close, one step earlier in the protocol.
        return 1;
    }
    int rc = trash_mark_deleting(j.path.c_str(), deleting);
    if (rc) return rc;   // including -ENOENT: the Txn destructor rolls back, no row was written
    if (claim.held()) {
        // Step 3: the rename moved *a* directory to the `.deleting` name. This is what says it
        // moved ours -- and if it did not, it is put straight back and nothing is deleted.
        if (int vrc = trash_claim_verify_rename(claim, deleting.c_str(),
                                                ::strcmp(deleting.c_str(), j.path.c_str()) != 0))
            return vrc;
    }
    if (j.row && ::strcmp(deleting.c_str(), j.row_path.c_str())) {
        // The row's own name for the tree, kept current. Unconditional on purpose: the SELECT
        // above read this row in this transaction, so there is nothing left that could have
        // changed it. If the write itself fails the rollback leaves the row naming the old name,
        // which trash_follow_deleting() resolves on the next wake.
        Stmt u(s->db,
               j.is_snapshot ? "UPDATE snapshots SET trash_path=? WHERE id=?"
                             : "UPDATE worlds SET trash_path=? WHERE id=?");
        if (!u.ok()) return -EIO;
        u.text(1, deleting.c_str());
        u.i64(2, (int64_t)j.row);
        if (u.step() != SQLITE_DONE) return -EIO;
        j.row_path.assign(deleting.c_str());
    }
    return t.commit();
}

// Is this job still the collector's to do? Read under the store mutex immediately before the
// rename that starts the deletion, so the ordinary case never touches a tree whose row has moved
// on. A TRASHING row is somebody else's operation in flight, an ACTIVE one has been restored,
// and a DEAD one has been collected by somebody else -- none of them is ours.
//
// PR #1 review (9th round), P18: and an orphan is re-asked too. "Nothing names this tree" was
// decided from trash_scan's snapshot of the rows; by the time the job comes up a discard may
// have committed its TRASHING row over exactly this name, and the tree is then the newest thing
// in the store rather than the oldest.
bool trash_row_still_ours(wfs_store *s, const TrashJob &j) {
    if (!j.row) {
        Guard g(s->mu);
        return !trash_path_claimed_locked(s, j.path.c_str());
    }
    Guard g(s->mu);
    Stmt q(s->db, j.is_snapshot ? "SELECT state, trash_path FROM snapshots WHERE id=?"
                                : "SELECT state, trash_path FROM worlds WHERE id=?");
    if (!q.ok()) return false;
    q.i64(1, (int64_t)j.row);
    // 32nd round, P2: no row and a failed read both come out false, and false here stops the
    // deletion -- the one answer that cannot lose a tree.
    return q.row() && q.col_i64(0) == WFS_ST_TRASHED &&
           !::strcmp(q.col_text(1), j.row_path.c_str());
}

// PR #1 review: how many wakes in a row have failed to delete one trash entry, keyed by the
// entry's name with any `.deleting` suffix stripped (the name changes under us the first time).
// The counter itself is the store's (wfs::gc_fail_bump, internal.h); since the 8th round the
// pool's own stale entries share it.
void gc_fail_key(const TrashJob &j, String &out) {
    String name(basename_of(j.path.c_str()));
    size_t n = name.size(), sl = ::strlen(WFS_DELETING_SUFFIX);
    if (n > sl && !::strcmp(name.c_str() + n - sl, WFS_DELETING_SUFFIX)) name.resize(n - sl);
    out.assign("gcfail:");
    out.append(name.c_str());
}

int64_t gc_fail_bump(wfs_store *s, const TrashJob &j) {
    String k;
    gc_fail_key(j, k);
    return wfs::gc_fail_bump(s, k.c_str());
}

void gc_fail_clear(wfs_store *s, const TrashJob &j) {
    String k;
    gc_fail_key(j, k);
    wfs::gc_fail_clear(s, k.c_str());
}

// One entry, the crash-safe way: rename first, record the new name, then unlink. `*partial`
// comes back 1 when the deadline stopped the unlink half-way: the row stays TRASHED and the
// tree stays `.deleting`, which is exactly the state the next wake resumes from.
// A positive return (PR #1 review, 8th round) means the job was resolved by somebody else while
// this collector had it queued -- a `restore` that won the rename, or another collector. Nothing
// was done to it and nothing may be counted or reported for it.
// WFS_E_TRASH_BLOCKED (PR #1 review, 20th round) means a directory that is not ours is sitting
// at the `.deleting` name: nothing was done to it either, and the entry is counted apart from
// the ones that were tried and failed, because the remedy is the operator's.
// WFS_E_TRASH_FOREIGN (PR #1 review, 25th round) is the same answer one step earlier: the
// directory at the entry's OWN path is not the tree this row was written for.
int gc_delete_one(wfs_store *s, TrashJob &j, int threads, uint64_t *entries_freed,
                  int64_t deadline_us, int *partial) {
    // The cheap question first, on a read: most jobs that are not the collector's any more are
    // settled here, without taking the store's write lock at all. It is not the one that
    // decides -- gc_claim_deleting() asks it again inside the transaction that renames.
    if (!trash_row_still_ours(s, j)) return 1;
    if (wfs_test_before_trash_delete)
        wfs_test_before_trash_delete(wfs_test_before_trash_delete_ctx, j.is_snapshot, j.row,
                                     j.path.c_str());
    String deleting;
    TrashClaim claim;
    int rc = gc_claim_deleting(s, j, deleting, claim);
    if (rc == 1) return 1;
    // Not at either name. Somebody deleted it -- or a restore renamed it home before the claim,
    // which is why burying the row is conditional on the row still being the one that was queued.
    if (rc == -ENOENT) return mark_dead(s, j) ? 0 : 1;
    if (rc) return rc;
    // PR #1 review (26th round, P1): through the descriptor the claim proved, when there was an
    // identity to prove (a world row). A snapshot entry, a row-less orphan and a row older than
    // the dir_dev/dir_ino columns have none, and keep the 4-thread path-based deleter.
    rc = claim.held() ? trash_unlink_claim(claim, entries_freed, deadline_us, partial)
                      : trash_unlink(deleting.c_str(), threads, entries_freed, deadline_us, partial);
    if (rc) return rc;
    if (partial && *partial) return 0;
    return mark_dead(s, j) ? 0 : 1;
}

} // namespace

extern "C" int wfs_gc_status(wfs_store *s, int64_t retention_secs, wfs_trash_stat *out) {
    if (!s || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    int64_t retention = retention_secs < 0 ? kDefaultRetention : retention_secs;
    TrashView v;
    if (int rc = trash_scan(s, now_sec() - retention, v)) return rc;
    // PR #1 review (27th/28th/29th rounds, P2): every directory this report's counts are made
    // from, and whether it could be read at all. One count for all of them -- <store>/trash, the
    // <store>/snapshots sweep, <store>/pool and its S<n>s, <store>/tmp -- because what the
    // reader has to know is the same in each case: a number below is missing what is in there, and here is which
    // directory and why. The first one's path and errno, since only its owner can do anything
    // about it. Nothing here writes: a counting pass that bumped the retry counter would be a
    // `gc --status` that decides how often the collector comes back.
    auto note = [&](const wfs::DirUnreadable &u) {
        if (!u.count) return;
        out->dirs_unreadable += u.count;
        if (!out->dirs_unreadable_path[0]) {
            out->dirs_unreadable_errno = u.err;
            copy_str(out->dirs_unreadable_path, sizeof out->dirs_unreadable_path, u.path.c_str());
        }
    };
    note(v.unreadable);
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
    // PR #1 review (20th round, P1): and the due entries the collector will not start on --
    // something that is not ours is sitting at the `<entry>.deleting` name it renames to. It
    // used to remove that directory recursively, so this state was invisible by construction;
    // now nothing touches it, which means somebody has to be told it is there and where. Only
    // entries a row claims are counted: a row-less orphan's stray is a row-less directory in
    // <store>/trash too, and the collector takes that one on its own (5th/9th rounds).
    for (size_t i = 0; i < v.due.size(); ++i) {
        if (!v.due[i].row) continue;
        String blocked;
        if (!trash_blocked_by(v.due[i].path, blocked)) continue;
        out->trash_blocked++;
        if (!out->blocked_path[0])
            copy_str(out->blocked_path, sizeof out->blocked_path, blocked.c_str());
    }
    // PR #1 review (25th round, P1): and the entries the collector will not start on because
    // what is at their path is not their tree. Same audience and the same reason to name the
    // path: for a world discarded across volumes it is a directory in the user's own
    // `.wfs-trash`, and the only way anything moves again is for them to look at it. Both
    // passes the collector makes are covered -- an entry already at its `.deleting` name has
    // the same identity, because the rename that gave it that name was same-volume. Orphans are
    // skipped: a row-less directory has no row to be the tree of, and it is a directory under
    // <store>/trash, which the store allocated (5th/9th/20th rounds).
    for (size_t pass = 0; pass < 2; ++pass) {
        const Vec<TrashJob> &jobs = pass == 0 ? v.deleting : v.due;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (!jobs[i].row) continue;
            if (trash_identity(jobs[i].path.c_str(), jobs[i].dir_dev, jobs[i].dir_ino) !=
                WFS_E_TRASH_FOREIGN)
                continue;
            out->trash_foreign++;
            if (!out->foreign_path[0])
                copy_str(out->foreign_path, sizeof out->foreign_path, jobs[i].path.c_str());
        }
    }
    // PR #1 review (33rd round, P2): and the volume's own numbers, whose rc was dropped while
    // `wfs_store_status()` two files away has propagated the identical call since it was written.
    // A statvfs(2) that fails leaves both fields 0, and `gc --status` printed `volume: 0 B free`
    // -- a number, in the line an operator reads to decide whether to collect now.
    if (int rc = wfs::fs_free_space(s->dir.c_str(), &out->volume_free_bytes,
                                    &out->volume_total_bytes))
        return rc;
    // PR #1 review (5th round): and the abandoned fork trees that are still on disk. They are not
    // in the trash -- they are in the user's directory, named only by their CREATING row -- but
    // an operator asking what is waiting for the collector has to be told about them too.
    {
        int64_t reap_before = now_sec() - wfs::creating_min_age_secs();
        Guard g(s->mu);
        Stmt q(s->db, "SELECT tmp_path, owner_pid, owner_start, created_at FROM worlds"
                      " WHERE state=0 AND tmp_path<>''");
        // 32nd round, P2: a count that stopped half way is not the count. `gc --status` is
        // what an operator reads before deciding the store is clean.
        if (!q.ok()) return -EIO;
        while (q.row()) {
            if (wfs::producer_alive(q.col_i64(1), q.col_i64(2))) continue;
            if (q.col_i64(3) > reap_before) continue;
            if (!proven_gone(q.col_text(0))) out->creating_stranded++;
        }
        if (!q.done()) return -EIO;
        // PR #1 review (7th round): and the half-built snapshots, whose two possible trees
        // (<store>/snapshots/S<n> and its .wfs-tmp) are named by nothing but the CREATING row
        // once the producer is gone. gc keeps that row when it cannot remove them, so an
        // operator asking what is waiting has to be told about those as well.
        String snapd = joinp(s->dir.c_str(), "snapshots");
        Stmt sq(s->db, "SELECT id, owner_pid, owner_start, created_at FROM snapshots WHERE state=0");
        if (!sq.ok()) return -EIO;
        while (sq.row()) {
            if (wfs::producer_alive(sq.col_i64(1), sq.col_i64(2))) continue;
            if (sq.col_i64(3) > reap_before) continue;
            wfs_id sid = (wfs_id)sq.col_i64(0);
            String stmp = numbered(snapd.c_str(), 'S', sid, WFS_TMP_SUFFIX);
            String sdir = numbered(snapd.c_str(), 'S', sid, nullptr);
            if (!proven_gone(stmp.c_str()) || !proven_gone(sdir.c_str())) out->creating_stranded++;
        }
        if (!sq.done()) return -EIO;   // 32nd round, P2
    }
    // PR #1 review (11th round): and the `*.wfs-tmp` under <store>/snapshots that no row names
    // at all. Those belong to the suffix sweep rather than to the CREATING pass -- half-built
    // clones whose row is already gone, or a name nothing in this store ever wrote -- and one
    // the sweep cannot remove is waiting for the collector exactly as the two kinds above are.
    // They were counted nowhere, so `gc --status` said the store was clean while the tree sat
    // in it. Disjoint from the loop above by construction: snap_tmp_named_by_row() is what
    // decides, and any row in any state but DEAD keeps its tree out of here.
    //
    // PR #1 review (28th round, P2): and, as for the sweep itself, a <store>/snapshots that
    // cannot be read is not an empty one -- this pass answered an opendir failure with "nothing
    // to count" and its readdir was never asked about its own errno, so the report called the
    // store clean because it could not look at it. It records the failure and, being a counting
    // pass, writes nothing.
    {
        String snapdir = joinp(s->dir.c_str(), "snapshots");
        wfs::DirUnreadable su;
        DIR *d = ::opendir(snapdir.c_str());
        if (!d) {
            if (errno != ENOENT)
                wfs::gc_note_unreadable(s, &su, snapdir.c_str(), errno, false, nullptr);
        } else {
            size_t sl = ::strlen(WFS_TMP_SUFFIX);
            for (;;) {
                errno = 0;
                struct dirent *e = ::readdir(d);
                if (!e) {
                    if (errno)
                        wfs::gc_note_unreadable(s, &su, snapdir.c_str(), errno, false, nullptr);
                    break;
                }
                size_t n = ::strlen(e->d_name);
                if (n <= sl || ::strcmp(e->d_name + n - sl, WFS_TMP_SUFFIX) != 0) continue;
                if (!snap_tmp_named_by_row(s, e->d_name, sl)) out->creating_stranded++;
            }
            ::closedir(d);
        }
        note(su);
    }
    // T1.5, PR #1 review (6th round): and the stale pool entries. Like the abandoned fork trees
    // above they are not in the trash -- they are clones under <store>/pool -- but they are
    // space waiting for the same collector, and a wake that ran out of time leaves them there.
    // PR #1 review (27th round, P2): and what the count above could not look at. A pool root
    // or an S<n> that answers EACCES/EIO is not an empty pool, and this line is what stops the
    // report from saying it is.
    // PR #1 review (33rd round, P2): and the count is the count, or this report does not come
    // back. pool_stranded() answers -EIO when the query that classifies pool work could not be
    // stepped (the 32nd round put that answer there), and its rc was dropped -- so the one case
    // in which `gc --status` printed no `pool:` line at all was the case in which it had no idea
    // whether there was one to print. That is the opposite of what this command is for: it is a
    // diagnostic an operator reads before deciding a store is clean, and a wrong "clean" is
    // worse than an error they can act on. There is nothing partial to salvage either -- the
    // failure is in the classification, not in one directory of many, so the honest report is
    // the errno.
    {
        wfs::DirUnreadable pu;
        if (int rc = wfs::pool_stranded(s, &out->pool_stranded, &pu)) return rc;
        note(pu);
    }
    // PR #1 review (29th round, P2): and <store>/tmp, the fourth and last directory the
    // collector scans. The 28th round taught the collector's own pass there -- the seatbelt
    // profiles `world exec` leaves behind -- to record an unreadable directory and retry it,
    // but this report walked trash, snapshots and pool and not that one, so an EACCES on
    // <store>/tmp produced an `unread:` line from `gc` and none at all from `gc --status`; and
    // once the retry cap stopped the worker chain, the run that would have printed it was the
    // run that no longer happens. `gc --status` is then the only thing left that can say it,
    // which is exactly why it is the thing that has to.
    //
    // A direct scan, not the persisted `gcfail:dir:` record: status answers for the present, and
    // a counter says only that some wake once failed here. It costs nothing to be exact --
    // every directory that counter can name is a directory this report now opens for itself
    // (<store>/trash by trash_scan, <store>/snapshots and <store>/pool and its S<n>s by the two
    // passes above, <store>/tmp by this one), so folding the counter in would add no directory
    // and would risk reporting one that has been readable for a week.
    //
    // Nothing is counted per entry -- what the trash stat says about <store>/tmp is only whether
    // it could be read, since the profiles themselves are the collector's business (tmp_removed)
    // and no field here holds them. The loop still runs to the end of the directory, because
    // readdir(3) reports its own failure through errno alone: a stream that stopped halfway is
    // the silence this whole round is about. And it writes nothing, like every counting pass.
    {
        String tmpd = joinp(s->dir.c_str(), "tmp");
        wfs::DirUnreadable tu;
        DIR *d = ::opendir(tmpd.c_str());
        if (!d) {
            if (errno != ENOENT)
                wfs::gc_note_unreadable(s, &tu, tmpd.c_str(), errno, false, nullptr);
        } else {
            for (;;) {
                errno = 0;
                struct dirent *e = ::readdir(d);
                if (!e) {
                    if (errno)
                        wfs::gc_note_unreadable(s, &tu, tmpd.c_str(), errno, false, nullptr);
                    break;
                }
            }
            ::closedir(d);
        }
        note(tu);
    }
    gc_worker_probe(s, &out->worker_pid, &out->worker_started_at, &out->worker_done,
                    &out->worker_remaining);
    return 0;
}

// PR #1 review (20th round, P1): which directory is in the way of this row's collection. The
// caller is a `discard --now` that has just been answered WFS_E_TRASH_BLOCKED, and what it owes
// the user is the name of the thing to look at -- in the EXDEV case that is a directory in
// their own `<parent>/.wfs-trash`, put there by something that is not this store.
extern "C" int wfs_trash_blocked_path(wfs_store *s, wfs_id id, int is_snapshot, char *buf,
                                      size_t cap) {
    if (!s || !id || !buf || !cap) return -EINVAL;
    String tp;
    {
        Guard g(s->mu);
        if (int rc = is_snapshot ? snapshot_trash_path(s, id, tp) : world_trash_path(s, id, tp))
            return rc;
    }
    trash_follow_deleting(tp);
    String blocked;
    if (!trash_blocked_by(tp, blocked)) return -ENOENT;
    if (blocked.size() + 1 > cap) return -ENAMETOOLONG;
    copy_str(buf, cap, blocked.c_str());
    return 0;
}

// PR #1 review (25th round, P1): which directory this row's collection refused to touch. The
// caller is a `discard --now` (or a `restore`) answered WFS_E_TRASH_FOREIGN, and what it owes
// the user is the path where their world's tree should be and something else is -- in the EXDEV
// case a directory in their own `<parent>/.wfs-trash`.
extern "C" int wfs_trash_entry_path(wfs_store *s, wfs_id id, int is_snapshot, char *buf,
                                    size_t cap) {
    if (!s || !id || !buf || !cap) return -EINVAL;
    String tp;
    {
        Guard g(s->mu);
        if (int rc = is_snapshot ? snapshot_trash_path(s, id, tp) : world_trash_path(s, id, tp))
            return rc;
    }
    if (!tp.size()) return -ENOENT;
    trash_follow_deleting(tp);
    if (tp.size() + 1 > cap) return -ENAMETOOLONG;
    copy_str(buf, cap, tp.c_str());
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
    // PR #1 review (3rd round): the collector's own classification, not an approximation of it.
    // This used to be two indexed counts plus a readdir looking for `*.deleting`, which missed
    // the third class trash_scan knows about: an ordinary `W<n>-<t>` or `S<n>-<t>` directory
    // that no row claims, left by a discard killed between the rename and the commit. trash_scan
    // calls such an orphan due immediately and `gc` sets work_remains for it, but the spawn
    // decision said "nothing waiting" -- so the CLI announced a background collection and
    // started nothing, every fork and discard made the same call, and the orphan sat there until
    // somebody ran `gc --now` by hand. Same cost as before: one readdir and the two row tables.
    // PR #1 review (33rd round, P2): and a scan that failed answers 1, not 0. This function has
    // no error channel -- it is a bool, "is there work" -- so the rc cannot be propagated and
    // has to be turned into whichever answer does nothing irreversible, the rule the 32nd round
    // wrote for every helper that returns a verdict instead of an errno. Here the two answers
    // are "start a collector" and "there is nothing to collect", and it is the second that
    // throws something away: the work the failed scan could not see stops waking the chain and
    // sits in the trash until somebody runs `gc --now` by hand (the 3rd round's bug, reached
    // through an EIO instead of through a classification gap). A collector started on a scan
    // that turns out to fail again costs one process that returns the errno and exits -- and it
    // is not a spin: this is only ever asked after a command that read the same database
    // successfully, so a read that fails here is the transient, not the steady state.
    TrashView v;
    if (trash_scan(s, cutoff, v) != 0) return 1;
    if (v.deleting.size() || v.due.size()) return 1;
    // PR #1 review (28th round, P2): and a directory the collector could not READ is work too --
    // it is the one kind of work whose size nobody knows, which is exactly why the chain has to
    // come back for it. The trash's own failure is in `v` (this scan just found it, and on a
    // store where no collector has run yet that is the only record there is); the counter covers
    // the directories this cheap pass does not walk -- <store>/snapshots, <store>/pool and its
    // S<n>s -- because every gc wake is a new process and the counter is where a wake that could
    // not read one of them left that fact. Both stop at kGcFailCap: past it the thing is still
    // reported by every run and by `gc --status`, it just no longer wakes a worker every two
    // seconds. Nothing here writes; the spawn decision is not allowed to spend the retries.
    if (v.unreadable.count &&
        wfs::gc_fail_get(s, wfs::gc_dir_fail_key(v.unreadable.path.c_str()).c_str()) <
            wfs::kGcFailCap)
        return 1;
    return wfs::gc_dirs_unreadable_pending(s) ? 1 : 0;
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

    // Before anything classifies anything: a discard that was killed between its rename and its
    // commit (PR #1 review, 5th round). wfs_store_open() has already done this once, but a
    // worker's wake can be minutes long and the store handle older still, so the row a *different*
    // process left behind since is resolved here rather than mistaken for something else below.
    //
    // PR #1 review (33rd round, P2): the same discarded rc as wfs_store_open's, and here it is
    // the collector that carries on over it. "Before anything classifies anything" is the whole
    // reason this line exists: a TRASHING row the recovery could not read is a row whose tree
    // the orphan pass below then finds unclaimed, and an unclaimed tree in <store>/trash is
    // deleted on sight -- no retention, no `restore`, no "this is somebody's baseline". A run
    // that cannot establish its own premise does not get to act on it, so the wake ends with the
    // errno and the next one starts over.
    if (int rc = wfs::trashing_recover(s, nullptr, nullptr)) return rc;

    // ---- the cheap half, always run in full ------------------------------------------------
    //
    // Worlds whose fork never finished (P8). The tree, if there is one, is at the exact path the
    // row recorded before it started cloning -- nowhere else, and certainly not "whatever in the
    // user's directory happens to end in .wfs-tmp". A row whose tmp_path is empty (a pool-backed
    // fork: its tree is a store-internal pool entry, collected by pool_collect below) or whose
    // tmp_path is no longer there is just marked DEAD.
    //
    // And only rows whose producer is gone (PR #1 review, 3rd round): a CREATING row means
    // "somebody is building this", and gc used to read every one of them as "somebody was". A
    // clone of a big tree outlives the two-second pause before an auto-spawned worker runs its
    // cheap pass, so the worker deleted a live fork's tree and its row, and the fork then
    // "succeeded" with an UPDATE that matched nothing -- an unregistered directory at --to.
    int64_t reap_before = now_sec() - wfs::creating_min_age_secs();
    {
        Vec<wfs_id> creating;
        Vec<String> cpaths;
        {
            Guard g(s->mu);
            Stmt q(s->db, "SELECT id, tmp_path, owner_pid, owner_start, created_at"
                          " FROM worlds WHERE state=0");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                if (wfs::producer_alive(q.col_i64(2), q.col_i64(3))) continue;
                if (q.col_i64(4) > reap_before) continue;
                creating.emplace_back((wfs_id)q.col_i64(0));
                cpaths.emplace_back(q.col_text(1));
            }
            if (!q.done()) return -EIO;   // 32nd round, P2
        }
        // PR #1 review (7th round): under this wake's deadline. An abandoned fork tree is a
        // half-built clone of a whole workspace, not the handful of stat(2)s the rest of gc's
        // cheap half is made of, and this ran flat out in front of the deadline-controlled trash
        // loop -- so a two-second worker wake, or an interactive `gc`, could spend minutes here.
        // Running out of time is not a failure: the row stays CREATING with its tmp_path, the
        // failure counter is left alone, work_remains is set, and the loop stops rather than
        // start another tree it also cannot finish (the same shape pool_collect has).
        for (size_t i = 0; i < creating.size(); ++i) {
            if (deadline_us && now_us() >= deadline_us) { rep.work_remains = 1; break; }
            TrashJob j;
            j.path = cpaths[i];
            j.row = creating[i];
            bool undecided = false;
            // PR #1 review (26th round, P1): the claim is the descriptor the marker was read
            // through, and the removal goes through it -- contents by fd, and the tree's own
            // name re-checked against that fd before the one rmdir that still uses a name.
            TrashClaim claim;
            if (gc_tmp_is_removable(s, creating[i], cpaths[i].c_str(), &undecided, claim)) {
                int partial = 0;
                int trc = trash_unlink_claim(claim, nullptr, deadline_us, &partial);
                if (partial) { rep.work_remains = 1; break; }
                if (trc == 0) {
                    rep.tmp_removed++;
                } else if (!proven_gone(cpaths[i].c_str())) {
                    // PR #1 review (5th round): the tree is still there and this row is the only
                    // thing in the world that knows its name -- it lives in the user's own target
                    // directory, under a name drawn at random, and is deliberately never found by
                    // a suffix sweep (rm_tmp_in_store_dir). Marking the row DEAD and clearing
                    // tmp_path here stranded the whole clone permanently. So the row stays
                    // CREATING with its tmp_path, the tree is counted, and a later wake tries
                    // again -- under the same failure cap as a trash entry, so a tree that will
                    // never budge stops waking a worker every two seconds without ever being
                    // reported as gone.
                    rep.tmp_failed++;
                    if (gc_fail_bump(s, j) < wfs::kGcFailCap) rep.work_remains = 1;
                    continue;
                }
                // Neither removed nor still there: somebody else got to it. Bury the row.
            } else if (undecided) {
                // PR #1 review (12th round): the tree could not be asked about -- an EACCES or
                // an EIO on it or on its marker. Burying the row here would strand the clone
                // exactly as the 5th round's failed removal did, because this row is the only
                // name that tree has. Keep it, count it, come back for it under the same cap.
                rep.tmp_failed++;
                if (gc_fail_bump(s, j) < wfs::kGcFailCap) rep.work_remains = 1;
                continue;
            }
            gc_fail_clear(s, j);
            Guard g(s->mu);
            Txn t(s->db);
            // 34th round, P1: the tree is gone either way; a row that could not be buried is
            // work for the next wake rather than a silent no-op.
            if (!t.ok()) { rep.work_remains = 1; continue; }
            // P18: still CREATING, and still naming the tree this job was made from.
            Stmt d(s->db,
                   "UPDATE worlds SET state=?, tmp_path='' WHERE id=? AND state=0 AND tmp_path=?");
            if (d.ok()) {
                d.i64(1, WFS_ST_DEAD);
                d.i64(2, (int64_t)creating[i]);
                d.text(3, cpaths[i].c_str());
                d.step();
            }
            if (t.commit()) rep.work_remains = 1;
        }
    }
    // Half-built snapshots: the row is the only trace of the tmp tree's name.
    String snaps = joinp(s->dir.c_str(), "snapshots");
    {
        Vec<wfs_id> snap_gone;
        {
            Guard g(s->mu);
            Stmt q(s->db, "SELECT id, owner_pid, owner_start, created_at FROM snapshots WHERE state=0");
            if (!q.ok()) return -EIO;
            while (q.row()) {
                if (wfs::producer_alive(q.col_i64(1), q.col_i64(2))) continue;   // still cloning
                if (q.col_i64(3) > reap_before) continue;
                snap_gone.emplace_back((wfs_id)q.col_i64(0));
            }
            if (!q.done()) return -EIO;   // 32nd round, P2
        }
        // The same deadline as the fork trees above, and for the same reason: a half-built
        // snapshot is a clone of the source tree (PR #1 review, 7th round).
        for (size_t i = 0; i < snap_gone.size(); ++i) {
            if (deadline_us && now_us() >= deadline_us) { rep.work_remains = 1; break; }
            String tmp = numbered(snaps.c_str(), 'S', snap_gone[i], WFS_TMP_SUFFIX);
            String dir = numbered(snaps.c_str(), 'S', snap_gone[i], nullptr);
            int partial = 0;
            wfs::fs_remove_tree(tmp.c_str(), deadline_us, &partial);
            int partial2 = 0;
            if (!partial) wfs::fs_remove_tree(dir.c_str(), deadline_us, &partial2);
            // Out of time, not stuck: the row keeps its CREATING state and its trees, nothing
            // is counted as a failure, and the successor carries on from here.
            if (partial || partial2) { rep.work_remains = 1; break; }
            // PR #1 review (7th round): the row goes only when both trees are confirmed gone.
            // The results used to be thrown away and the row deleted regardless, and an S<n>
            // that would not budge (EPERM, an ACL, a transient EIO) then leaked for ever: the
            // suffix sweep below only ever looks at `*.wfs-tmp`, so nothing left in the store
            // knew that directory was rubbish. The CREATING row is kept instead -- exactly as
            // the abandoned fork trees above keep theirs -- so it is counted, reported by
            // `gc --status`, and retried under the same failure cap.
            TrashJob j;
            j.path = dir;
            j.row = snap_gone[i];
            j.is_snapshot = 1;
            // PR #1 review (12th round): and the row is deleted only when both trees are
            // *proven* gone. An lstat(2) that fails for EACCES or EIO used to count as gone,
            // and the row -- the only name S<n> has left -- went with it.
            if (!proven_gone(tmp.c_str()) || !proven_gone(dir.c_str())) {
                rep.tmp_failed++;
                if (gc_fail_bump(s, j) < wfs::kGcFailCap) rep.work_remains = 1;
                continue;
            }
            gc_fail_clear(s, j);
            rep.snapshots_deleted++;
            Guard g(s->mu);
            Txn t(s->db);
            if (!t.ok()) { rep.work_remains = 1; continue; }   // 34th round, P1: as above
            Stmt d(s->db, "DELETE FROM snapshots WHERE id=? AND state=0");
            if (d.ok()) { d.i64(1, (int64_t)snap_gone[i]); d.step(); }
            if (t.commit()) rep.work_remains = 1;
        }
    }
    // <store>/snapshots only: a name under the store is one we made. The parent directories of
    // the worlds are the user's and are never swept (see rm_tmp_in_store_dir).
    {
        // 28th round: and the sweep says whether it could read <store>/snapshots at all. What it
        // could not look at is not a clean directory, so it is reported and retried rather than
        // passed over -- the same rule the pool got in the 27th.
        // 33rd round, P2: and the sweep's own rc is this run's rc. It answers non-zero only for
        // "this is not the directory I sweep" -- which would mean the collector had been asked
        // to apply <store>/snapshots' claim rule somewhere else -- and a run that silently did
        // no sweep at all is a run that reports a clean store it never looked at.
        wfs::DirUnreadable tu;
        int src = rm_tmp_in_store_dir(s, snaps.c_str(), &rep.tmp_removed, deadline_us,
                                      &rep.work_remains, &rep.tmp_failed, &tu);
        rep.dirs_unreadable += tu.count;
        if (src) return src;
    }

    // ---- T2.2: reconciliation ----------------------------------------------------------------
    //
    // A row whose tree is not on disk. Detection is free and unconditional; acting on it is not,
    // because a World that was merely moved looks exactly the same from here until someone runs
    // `world fs verify <its new path>` (P1). So the default is to report, and WFS_GC_RECONCILE
    // is the operator saying "yes, those are gone".
    {
        Vec<wfs_id> dead_snaps, dead_worlds;
        Vec<String> dead_snap_paths, dead_world_paths;
        {
            Guard g(s->mu);
            struct stat st;
            // PR #1 review (12th round): and "the tree is not there" has to be *proven*.
            // This was a bool over stat(2), so every reason stat(2) can fail read as absence:
            // EACCES on a parent directory, an EIO, a volume that is not mounted this minute,
            // an ENAMETOOLONG. --reconcile then marked the row DEAD -- and a DEAD world is not
            // repairable by `verify` and not adoptable, so one transient error unregistered a
            // live world permanently. Only ENOENT/ENOTDIR is an absence. Anything else, and
            // that includes a path which holds something that is *not* a directory (a damaged
            // world, not a missing one: burying it would throw away the row that says what
            // belongs there), is counted as unreadable, reported, and left exactly as it is.
            {
                Stmt q(s->db, "SELECT id, path FROM snapshots WHERE state=1");
                if (!q.ok()) return -EIO;
                while (q.row()) {
                    const char *p = q.col_text(1);
                    int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
                    if (prc == 0 && S_ISDIR(st.st_mode)) continue;
                    if (!wfs::fs_gone(prc)) { rep.snapshots_unreadable++; continue; }
                    rep.snapshots_dangling++;
                    dead_snaps.emplace_back((wfs_id)q.col_i64(0));
                    dead_snap_paths.emplace_back(p);
                }
                if (!q.done()) return -EIO;   // 32nd round, P2
            }
            {
                Stmt q(s->db, "SELECT id, path FROM worlds WHERE state=1");
                if (!q.ok()) return -EIO;
                while (q.row()) {
                    const char *p = q.col_text(1);
                    int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
                    if (prc == 0 && S_ISDIR(st.st_mode)) continue;
                    if (!wfs::fs_gone(prc)) { rep.worlds_unreadable++; continue; }
                    rep.worlds_dangling++;
                    dead_worlds.emplace_back((wfs_id)q.col_i64(0));
                    dead_world_paths.emplace_back(p);
                }
                if (!q.done()) return -EIO;   // 32nd round, P2
            }
        }
        if (wfs_test_before_reconcile) wfs_test_before_reconcile(wfs_test_before_reconcile_ctx);
        // PR #1 review (9th round), P18: the verdict above is "there is no tree at the path this
        // row records", and it was acted on with an unconditional UPDATE. A World that was merely
        // moved looks exactly like that until somebody runs `world fs verify <its new path>`,
        // which relocates the row by inode -- and a verify landing between the scan and the
        // update had its work buried: an ACTIVE world at a path it had just been taught, marked
        // DEAD by a reconcile that was reading a path nobody uses any more. So each update
        // carries the state and the path the scan observed, and a row is counted as reconciled
        // only when it was this update that changed it.
        if (o.flags & WFS_GC_RECONCILE) {
            for (size_t i = 0; i < dead_snaps.size(); ++i) {
                // The directory the row named is gone, but <store>/snapshots/S<n> may still
                // hold the manifest or a stump; it goes with the row.
                //
                // PR #1 review (12th round): it goes *before* the row, and the row is buried
                // only once it is confirmed gone -- the same rule as the 5th, 7th and 8th
                // rounds. The removal used to run after the UPDATE with its result thrown
                // away, and an S<n> that would not budge (an ACL, an EPERM, a transient EIO)
                // then leaked for ever: reconciliation scans ACTIVE rows and the suffix sweep
                // only `*.wfs-tmp`, so the DEAD row left nothing in the store that knew the
                // directory was rubbish, nothing counted it, and nothing ever came back. When
                // it will not go the row stays ACTIVE -- it is already reported as dangling,
                // which is exactly what it is -- and the tree is counted in tmp_failed and
                // retried under the shared per-path failure cap.
                String dir = numbered(snaps.c_str(), 'S', dead_snaps[i], nullptr);
                TrashJob j;
                j.path = dir;
                j.row = dead_snaps[i];
                j.is_snapshot = 1;
                int partial = 0;
                wfs::fs_remove_tree(dir.c_str(), deadline_us, &partial);
                // Out of time, not stuck: nothing is counted as a failure and the successor
                // carries on from here, with the row exactly as it found it.
                if (partial) { rep.work_remains = 1; break; }
                if (!proven_gone(dir.c_str())) {
                    rep.tmp_failed++;
                    if (gc_fail_bump(s, j) < wfs::kGcFailCap) rep.work_remains = 1;
                    continue;
                }
                gc_fail_clear(s, j);
                bool changed = false;
                {
                    Guard g(s->mu);
                    Txn t(s->db);
                    if (!t.ok()) continue;   // 34th round, P1
                    Stmt u(s->db, "UPDATE snapshots SET state=?, trash_path=''"
                                  " WHERE id=? AND state=1 AND path=?");
                    if (!u.ok()) continue;
                    u.i64(1, WFS_ST_DEAD);
                    u.i64(2, (int64_t)dead_snaps[i]);
                    u.text(3, dead_snap_paths[i].c_str());
                    if (u.step() != SQLITE_DONE) continue;
                    changed = sqlite3_changes(s->db) != 0;
                    if (t.commit()) changed = false;
                }
                if (!changed) continue;   // the row moved on: not this collector's to bury
                rep.snapshots_reconciled++;
            }
            for (size_t i = 0; i < dead_worlds.size(); ++i) {
                Guard g(s->mu);
                Txn t(s->db);
                if (!t.ok()) continue;   // 34th round, P1
                Stmt u(s->db, "UPDATE worlds SET state=? WHERE id=? AND state=1 AND path=?");
                if (!u.ok()) continue;
                u.i64(1, WFS_ST_DEAD);
                u.i64(2, (int64_t)dead_worlds[i]);
                u.text(3, dead_world_paths[i].c_str());
                if (u.step() != SQLITE_DONE) continue;
                bool changed = sqlite3_changes(s->db) != 0;
                if (t.commit()) continue;
                if (changed) rep.worlds_reconciled++;
            }
        }
    }

    // T1.5: pool entries whose snapshot is gone or is a different snapshot now, rows whose
    // filler was killed mid-clone, and trees under <store>/pool that no row claims. After the
    // snapshot sweeps above, so a snapshot that died in this same run takes its pool with it.
    // PR #1 review (6th round): under this wake's deadline, and not in front of it. A pool entry
    // is a whole clone of a snapshot -- a stale 120k-entry one is seconds of unlink(2), which is
    // what the batch limit exists to bound -- and this used to run flat out before the deadline
    // was ever consulted, so a two-second worker wake could spend minutes here. What the
    // deadline cuts short keeps the shape its successor rediscovers (the row, or the row-less
    // directory) and sets work_remains, so the worker chain comes back for it.
    {
        // 27th round: a pool the collector could not read is reported, not passed over. The
        // sweep sets work_remains under the shared cap, so the worker chain comes back for it.
        // 33rd round, P2: and its rc. pool_collect() answers -EIO when the classification it
        // removes trees on the strength of could not be read (the 32nd round), and the run used
        // to report `pool_removed 0` -- "there was nothing stale in the pool" -- for it. The
        // classification is the first thing it does and the only thing that can fail, so a
        // non-zero rc here means nothing was touched, let alone left half counted.
        wfs::DirUnreadable pu;
        int prc = wfs::pool_collect(s, &rep.pool_removed, deadline_us, &rep.work_remains,
                                    &rep.pool_failed, &pu);
        rep.dirs_unreadable += pu.count;
        if (prc) return prc;
    }

    // <store>/tmp holds the seatbelt profiles `world exec` generates. They are removed when the
    // command ends; one that survives an hour belongs to a process that was killed.
    //
    // PR #1 review (28th round, P2): the third directory the collector scans, and it was silent
    // in the same way -- `if (DIR *d = opendir(...))` and a readdir never asked about its errno,
    // so a profile nobody could see went on sitting there while the run reported a clean sweep.
    // Only ENOENT is absence; the rest is recorded and retried under the shared cap.
    {
        String tmpd = joinp(s->dir.c_str(), "tmp");
        wfs::DirUnreadable tu;
        DIR *d = ::opendir(tmpd.c_str());
        if (!d) {
            if (errno != ENOENT)
                wfs::gc_note_unreadable(s, &tu, tmpd.c_str(), errno, true, &rep.work_remains);
        } else {
            bool read_through = true;
            int64_t cut = now_sec() - 3600;
            for (;;) {
                errno = 0;
                struct dirent *e = ::readdir(d);
                if (!e) {
                    if (errno) {
                        wfs::gc_note_unreadable(s, &tu, tmpd.c_str(), errno, true,
                                                &rep.work_remains);
                        read_through = false;
                    }
                    break;
                }
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
            wfs::gc_note_readable(s, tmpd.c_str(), read_through);
        }
        rep.dirs_unreadable += tu.count;
    }

    // ---- the expensive half: the trash -------------------------------------------------------
    if (o.flags & WFS_GC_NO_TRASH) {
        // The collector's pass all the same, although this one deletes nothing: it is the run
        // that finds a trash it cannot read, and the finding has to be recorded and retried
        // (28th round) exactly as the full pass below records it.
        // 33rd round, P2: and a scan that failed is not a scan that found an empty trash. The
        // rc was tested and then dropped on the floor -- `if (... == 0)` with no else -- so a
        // `gc --no-trash` whose scan could not be read returned 0 with `work_remains` unset,
        // which is the one thing that stops the worker chain from coming back.
        TrashView left;
        if (int lrc = trash_scan(s, cutoff, left, true, &rep.work_remains)) return lrc;
        if (left.deleting.size() || left.due.size()) rep.work_remains = 1;
        rep.dirs_unreadable += left.unreadable.count;
        if (out) *out = rep;
        return 0;
    }
    TrashView v;
    if (int rc = trash_scan(s, cutoff, v, true, &rep.work_remains)) return rc;
    rep.dirs_unreadable += v.unreadable.count;
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
            int partial = 0;
            int drc = gc_delete_one(s, jobs[i], threads, &freed, deadline_us, &partial);
            if (drc > 0) {
                // Not this collector's entry any more: a `restore` took it back, or another
                // collector finished it. Nothing to count, nothing to report, nothing to retry
                // (PR #1 review, 8th round).
                rep.entries_freed += freed;
                continue;
            }
            if (drc == WFS_E_TRASH_BLOCKED) {
                // PR #1 review (20th round, P1): a directory nothing in this store named is at
                // the `.deleting` name this entry has to be renamed to. It is not removed and
                // not renamed over -- the fold that used to do exactly that is gone -- so the
                // entry stays in the trash and is counted, with `gc --status` naming the
                // directory. Same retry cap as a failure: the first few wakes come back for it
                // (the stray may be somebody else's temporary), after that the collector stops
                // waking itself for something only the operator can clear.
                rep.trash_blocked++;
                rep.entries_freed += freed;
                if (gc_fail_bump(s, jobs[i]) < wfs::kGcFailCap) rep.work_remains = 1;
                continue;
            }
            if (drc == WFS_E_TRASH_FOREIGN) {
                // PR #1 review (25th round, P1): the directory at this entry's path is not the
                // tree the row was written for -- its dev/ino do not match what the row has
                // carried since the world was published. Until now the row NAMING the path was
                // the whole of the check, and the cross-volume entry's name is the user's own
                // to occupy, so what got renamed to `.deleting` and deleted could be a
                // directory of theirs. Nothing is touched. Counted apart from trash_failed for
                // the same reason as trash_blocked: no number of retries clears it, only
                // putting the world's own tree back does, and `gc --status` names the path.
                rep.trash_foreign++;
                rep.entries_freed += freed;
                if (gc_fail_bump(s, jobs[i]) < wfs::kGcFailCap) rep.work_remains = 1;
                continue;
            }
            if (drc != 0) {
                // The tree is still there, and the loop must not end up reporting an empty
                // trash because of it (PR #1 review). Count it, keep the chain alive for the
                // first few wakes so a transient error is retried, and after that leave the
                // entry where it is -- reported by every run and by `gc --status` -- instead of
                // waking a worker every two seconds for something that will not budge.
                rep.trash_failed++;
                rep.entries_freed += freed;
                if (gc_fail_bump(s, jobs[i]) < wfs::kGcFailCap) rep.work_remains = 1;
                continue;
            }
            gc_fail_clear(s, jobs[i]);
            rep.entries_freed += freed;
            if (partial) {
                // The deadline landed in the middle of this tree. Stop here rather than finish
                // it: the tree is `.deleting`, the successor carries on from where this wake
                // stopped, and the foreground gets its disk back on time (P16).
                rep.work_remains = 1;
                goto finished;
            }
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
