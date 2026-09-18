// Metadata store: SQLite in WAL mode (arch.md §12, §20, §27). The only third-party
// dependency of the core; isolated to this file so it can be swapped later.
#include "internal.h"

#include <errno.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

using wfs::Guard;
using wfs::String;

namespace {

const char *kSchema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS worlds("
    "  world_id INTEGER PRIMARY KEY AUTOINCREMENT, parent_world_id INTEGER,"
    "  generation INTEGER NOT NULL DEFAULT 0, state INTEGER NOT NULL DEFAULT 0,"
    "  base_dir TEXT, created_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS worlds_base ON worlds(base_dir) WHERE parent_world_id IS NULL;"
    "CREATE TABLE IF NOT EXISTS entries("
    "  world_id INTEGER NOT NULL, parent_inode INTEGER NOT NULL, name BLOB NOT NULL,"
    "  logical_inode INTEGER, operation INTEGER NOT NULL,"
    "  PRIMARY KEY(world_id, parent_inode, name));"
    "CREATE TABLE IF NOT EXISTS inodes("
    "  logical_inode INTEGER PRIMARY KEY, backing_object INTEGER, type INTEGER NOT NULL,"
    "  mode INTEGER, uid INTEGER, gid INTEGER, size INTEGER,"
    "  metadata_generation INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS changed("
    "  world_id INTEGER NOT NULL, logical_inode INTEGER NOT NULL, change_type INTEGER NOT NULL,"
    "  PRIMARY KEY(world_id, logical_inode));"
    "CREATE TABLE IF NOT EXISTS objects("
    "  object_id INTEGER PRIMARY KEY AUTOINCREMENT, backing_path TEXT NOT NULL,"
    "  immutable INTEGER NOT NULL DEFAULT 0, state INTEGER NOT NULL DEFAULT 0);";

int map_sqlite(int rc) {
    switch (rc) {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW: return 0;
    case SQLITE_BUSY: case SQLITE_LOCKED: return -EBUSY;
    case SQLITE_NOMEM: return -ENOMEM;
    case SQLITE_CONSTRAINT: return -EEXIST;
    default: return -EIO;
    }
}

struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    bool ok() const { return s != nullptr; }
};

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void exec(sqlite3 *db, const char *sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

} // namespace

extern "C" const char *wfs_version(void) { return "0.0.3-m0"; }

extern "C" int wfs_store_open(const char *store_dir, wfs_store **out) {
    if (!store_dir || !out) return -EINVAL;
    if (int rc = wfs::fs_mkdir_p(store_dir)) return rc;
    wfs_store *s = new wfs_store();
    s->dir.assign(store_dir);
    String dbp(store_dir);
    dbp.append("/metadata.db");
    int rc = sqlite3_open_v2(dbp.c_str(), &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) { wfs_store_close(s); return -EIO; }
    sqlite3_busy_timeout(s->db, 5000);
    if (sqlite3_exec(s->db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) { wfs_store_close(s); return -EIO; }
    *out = s;
    return 0;
}

extern "C" void wfs_store_close(wfs_store *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    delete s;
}

extern "C" int wfs_world_init(wfs_store *s, const char *base_dir, wfs_world *out) {
    if (!s || !base_dir || !out) return -EINVAL;
    String real;
    if (int rc = wfs::fs_realpath(base_dir, real)) return rc;
    wfs_attr a;
    if (int rc = wfs::fs_lstat(real.c_str(), a)) return rc;
    if (a.type != WFS_T_DIR) return -ENOTDIR;

    Guard g(s->mu);
    {
        Stmt q(s->db, "SELECT world_id FROM worlds WHERE parent_world_id IS NULL AND base_dir=? AND state=0");
        if (!q.ok()) return -EIO;
        sqlite3_bind_text(q.s, 1, real.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q.s) == SQLITE_ROW) { *out = (wfs_world)sqlite3_column_int64(q.s, 0); return 0; }
    }
    Stmt ins(s->db, "INSERT INTO worlds(parent_world_id, generation, state, base_dir, created_at) VALUES(NULL,0,0,?,?)");
    if (!ins.ok()) return -EIO;
    sqlite3_bind_text(ins.s, 1, real.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.s, 2, now_ns());
    int rc = sqlite3_step(ins.s);
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    *out = (wfs_world)sqlite3_last_insert_rowid(s->db);
    return 0;
}

extern "C" int wfs_world_fork(wfs_store *s, wfs_world parent, wfs_world *out) {
    if (!s || !out || parent == 0) return -EINVAL;
    Guard g(s->mu);
    // One transaction, one row: fork cost is independent of workspace size (arch.md §20).
    exec(s->db, "BEGIN IMMEDIATE");
    int rc = -EIO;
    {
        Stmt chk(s->db, "SELECT generation, state FROM worlds WHERE world_id=?");
        if (!chk.ok()) goto fail;
        sqlite3_bind_int64(chk.s, 1, (sqlite3_int64)parent);
        if (sqlite3_step(chk.s) != SQLITE_ROW) { rc = -ENOENT; goto fail; }
        if (sqlite3_column_int(chk.s, 1) != WFS_W_ACTIVE) { rc = -ESTALE; goto fail; }
        int64_t gen = sqlite3_column_int64(chk.s, 0);
        // Parent's currently visible backing versions become immutable/shared (arch.md §10).
        Stmt bump(s->db, "UPDATE worlds SET generation=generation+1 WHERE world_id=?");
        if (!bump.ok()) goto fail;
        sqlite3_bind_int64(bump.s, 1, (sqlite3_int64)parent);
        sqlite3_step(bump.s);
        Stmt ins(s->db, "INSERT INTO worlds(parent_world_id, generation, state, base_dir, created_at) VALUES(?,?,0,NULL,?)");
        if (!ins.ok()) goto fail;
        sqlite3_bind_int64(ins.s, 1, (sqlite3_int64)parent);
        sqlite3_bind_int64(ins.s, 2, gen + 1);
        sqlite3_bind_int64(ins.s, 3, now_ns());
        int src = sqlite3_step(ins.s);
        if (src != SQLITE_DONE) { rc = map_sqlite(src); goto fail; }
        *out = (wfs_world)sqlite3_last_insert_rowid(s->db);
    }
    exec(s->db, "COMMIT");
    return 0;
fail:
    exec(s->db, "ROLLBACK");
    return rc;
}

extern "C" int wfs_world_discard(wfs_store *s, wfs_world w) {
    if (!s || w == 0) return -EINVAL;
    Guard g(s->mu);
    // Mark only; physical cleanup is background GC (arch.md §17, §27).
    Stmt u(s->db, "UPDATE worlds SET state=? WHERE world_id=? AND state=0");
    if (!u.ok()) return -EIO;
    sqlite3_bind_int(u.s, 1, WFS_W_DISCARDED);
    sqlite3_bind_int64(u.s, 2, (sqlite3_int64)w);
    int rc = sqlite3_step(u.s);
    if (rc != SQLITE_DONE) return map_sqlite(rc);
    return sqlite3_changes(s->db) == 1 ? 0 : -ENOENT;
}

extern "C" int wfs_world_list(wfs_store *s, wfs_world *buf, size_t cap, size_t *count) {
    if (!s || !count) return -EINVAL;
    Guard g(s->mu);
    Stmt q(s->db, "SELECT world_id FROM worlds WHERE state=0 ORDER BY world_id");
    if (!q.ok()) return -EIO;
    size_t n = 0;
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        if (buf && n < cap) buf[n] = (wfs_world)sqlite3_column_int64(q.s, 0);
        ++n;
    }
    *count = n;
    return 0;
}

extern "C" int wfs_world_info(wfs_store *s, wfs_world w, wfs_world *parent, wfs_world_state *state,
                              char *base_dir, size_t cap) {
    if (!s || w == 0) return -EINVAL;
    wfs_world p = 0;
    int st = 0;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT parent_world_id, state FROM worlds WHERE world_id=?");
        if (!q.ok()) return -EIO;
        sqlite3_bind_int64(q.s, 1, (sqlite3_int64)w);
        if (sqlite3_step(q.s) != SQLITE_ROW) return -ENOENT;
        p = sqlite3_column_type(q.s, 0) == SQLITE_NULL ? 0 : (wfs_world)sqlite3_column_int64(q.s, 0);
        st = sqlite3_column_int(q.s, 1);
    }
    if (parent) *parent = p;
    if (state) *state = (wfs_world_state)st;
    if (base_dir && cap) {
        String base;
        if (int rc = wfs::store_world_base_dir(s, w, base)) return rc;
        if (base.size() + 1 > cap) return -ENAMETOOLONG;
        memcpy(base_dir, base.c_str(), base.size() + 1);
    }
    return 0;
}

extern "C" int wfs_diff(wfs_store *s, wfs_world w, wfs_change_cb cb, void *ctx) {
    (void)s; (void)w; (void)cb; (void)ctx;
    return -ENOTSUP; // M2: driven by the `changed` table
}

namespace wfs {

// Walk parents until the base world and return its base_dir. Bounded by branch
// depth; runs once per view open, never on the lookup path.
int store_world_base_dir(wfs_store *s, wfs_world w, String &base_dir) {
    Guard g(s->mu);
    Stmt q(s->db, "SELECT parent_world_id, base_dir FROM worlds WHERE world_id=?");
    if (!q.ok()) return -EIO;
    for (int depth = 0; depth < 4096; ++depth) {
        sqlite3_reset(q.s);
        sqlite3_bind_int64(q.s, 1, (sqlite3_int64)w);
        if (sqlite3_step(q.s) != SQLITE_ROW) return -ENOENT;
        if (sqlite3_column_type(q.s, 0) == SQLITE_NULL) {
            const unsigned char *t = sqlite3_column_text(q.s, 1);
            if (!t) return -EIO;
            base_dir.assign((const char *)t);
            return 0;
        }
        w = (wfs_world)sqlite3_column_int64(q.s, 0);
    }
    return -ELOOP;
}

} // namespace wfs
