// The store: a directory with a VERSION file, a SQLite metadata.db (schema v3), the snapshot
// trees and the trash. Everything that is not snapshot/world lifecycle lives here.
#include "db.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>

using wfs::Guard;
using wfs::String;
using wfs::Stmt;

namespace {

// Schema v3 (docs/M1_DESIGN.md §2). Snapshot ids and world ids are separate sequences, so
// S1 and W1 can both exist; the CLI prints the prefix.
// Run on every open: journal_mode is persistent in the file, but `synchronous` and
// `foreign_keys` are per-connection.
const char *kPragmas =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA foreign_keys=OFF;";

// Run only when `PRAGMA user_version` says this store has not seen this schema yet. Parsing and
// executing a dozen DDL statements is ~1 ms, which is a lot next to a pool-served fork's 9 ms
// (T1.7), and every `world` invocation opens the store exactly once.
const char *kSchema =
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS snapshots("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL DEFAULT '',"
    "  path TEXT NOT NULL DEFAULT '',"
    "  src_path TEXT NOT NULL DEFAULT '',"
    "  from_world INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  entries INTEGER NOT NULL DEFAULT 0,"
    "  hardlinks INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  hard INTEGER NOT NULL DEFAULT 0,"
    "  root_mode INTEGER NOT NULL DEFAULT 0,"
    /* T2.2: snapshots go through the same trash as worlds do (P4). */
    "  trash_path TEXT NOT NULL DEFAULT '',"
    "  owner_pid INTEGER NOT NULL DEFAULT 0,"
    "  owner_start INTEGER NOT NULL DEFAULT 0,"
    "  trashed_at INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS worlds("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  kind INTEGER NOT NULL DEFAULT 1,"
    "  parent_world INTEGER NOT NULL DEFAULT 0,"
    "  snapshot_id INTEGER NOT NULL DEFAULT 0,"
    "  name TEXT NOT NULL DEFAULT '',"
    "  path TEXT NOT NULL DEFAULT '',"
    "  trash_path TEXT NOT NULL DEFAULT '',"
    "  dir_dev INTEGER NOT NULL DEFAULT 0,"
    "  dir_ino INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0,"
    "  fsevents_id INTEGER NOT NULL DEFAULT 0,"
    "  entries INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    /* The exact path this fork is building its clone at, written before the clone starts and
     * cleared when the tree is published. A CREATING row is the only thing that names it, and
     * gc removes that path and nothing else: the store never guesses in a user's directory. */
    "  tmp_path TEXT NOT NULL DEFAULT '',"
    /* Who is building it: the pid, and that process's own start time so a reused pid is not
     * mistaken for the producer. gc leaves a CREATING row alone while its producer is alive. */
    "  owner_pid INTEGER NOT NULL DEFAULT 0,"
    "  owner_start INTEGER NOT NULL DEFAULT 0,"
    "  trashed_at INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS worlds_ino ON worlds(dir_ino);"
    "CREATE INDEX IF NOT EXISTS worlds_path ON worlds(path);"
    // T1.5, the pre-clone pool. A row is a finished clone of `snapshot_id` with no marker and
    // no world: <store>/pool/S<n>/<uuid>. `snap_created_at` is the snapshot's created_at as it
    // was when the entry was cloned -- snapshot rows are immutable, so a mismatch means the id
    // now belongs to a different snapshot and the entry must never be handed out.
    "CREATE TABLE IF NOT EXISTS pool("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  snapshot_id INTEGER NOT NULL,"
    "  snap_created_at INTEGER NOT NULL,"
    "  uuid TEXT NOT NULL DEFAULT '',"
    "  path TEXT NOT NULL DEFAULT '',"
    "  entries INTEGER NOT NULL DEFAULT 0,"
    "  root_mode INTEGER NOT NULL DEFAULT 0,"
    "  root_mtime INTEGER NOT NULL DEFAULT 0,"   /* nanoseconds */
    "  dir_dev INTEGER NOT NULL DEFAULT 0,"
    "  dir_ino INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  owner_pid INTEGER NOT NULL DEFAULT 0,"
    "  owner_start INTEGER NOT NULL DEFAULT 0,"
    "  state INTEGER NOT NULL DEFAULT 0);"   /* 0 = being cloned, 1 = ready */
    "CREATE INDEX IF NOT EXISTS pool_snap ON pool(snapshot_id, state);";

// Columns added after the first schema-2 stores were written. They are additive and carry
// defaults, so an older core reading such a store still works and VERSION does not change on
// their account (P13 is about incompatible schemas, not about new columns). What did move
// VERSION, in the end, is not a column at all -- see the 24th-round note above check_version().
// One statement per exec: a combined script would stop at the first column that is already
// there.
//
// PR #1 review (11th round): each one carries the table and the column it adds, because "is
// this migration still to be run?" and "did it work?" are questions about the schema, and the
// schema is what PRAGMA table_info answers. They used to be bare SQL whose result was thrown
// away, on the assumption that the only way an ALTER can fail is "duplicate column name" --
// see migrate_schema() below for what that assumption cost.
struct Migration {
    const char *table;
    const char *column;
    const char *ddl;
};

const Migration kMigrations[] = {
    {"snapshots", "hard",
     "ALTER TABLE snapshots ADD COLUMN hard INTEGER NOT NULL DEFAULT 0"},
    {"snapshots", "root_mode",
     "ALTER TABLE snapshots ADD COLUMN root_mode INTEGER NOT NULL DEFAULT 0"},
    // T2.2
    {"snapshots", "trash_path",
     "ALTER TABLE snapshots ADD COLUMN trash_path TEXT NOT NULL DEFAULT ''"},
    {"snapshots", "trashed_at",
     "ALTER TABLE snapshots ADD COLUMN trashed_at INTEGER NOT NULL DEFAULT 0"},
    // T2.5 (P9): the hardlink groups recorded in the snapshot's manifest. hl_groups is what a
    // fork checks to decide whether the manifest has to be read at all.
    {"snapshots", "hl_groups",
     "ALTER TABLE snapshots ADD COLUMN hl_groups INTEGER NOT NULL DEFAULT 0"},
    {"snapshots", "hl_external",
     "ALTER TABLE snapshots ADD COLUMN hl_external INTEGER NOT NULL DEFAULT 0"},
    // PR #1 review, third round: where a fork in flight is building its clone. The name is
    // drawn, not derived, so the row is the only record of it.
    {"worlds", "tmp_path",
     "ALTER TABLE worlds ADD COLUMN tmp_path TEXT NOT NULL DEFAULT ''"},
    // PR #1 review, third round: who is building a CREATING row, so that gc can tell a crashed
    // producer from one that is simply still cloning. owner_start is that process's own start
    // time: a pid that has been reused since is not the producer.
    {"worlds", "owner_pid",
     "ALTER TABLE worlds ADD COLUMN owner_pid INTEGER NOT NULL DEFAULT 0"},
    {"worlds", "owner_start",
     "ALTER TABLE worlds ADD COLUMN owner_start INTEGER NOT NULL DEFAULT 0"},
    {"snapshots", "owner_pid",
     "ALTER TABLE snapshots ADD COLUMN owner_pid INTEGER NOT NULL DEFAULT 0"},
    {"snapshots", "owner_start",
     "ALTER TABLE snapshots ADD COLUMN owner_start INTEGER NOT NULL DEFAULT 0"},
    {"pool", "owner_pid",
     "ALTER TABLE pool ADD COLUMN owner_pid INTEGER NOT NULL DEFAULT 0"},
    {"pool", "owner_start",
     "ALTER TABLE pool ADD COLUMN owner_start INTEGER NOT NULL DEFAULT 0"},
};

// Additive revision of the schema. `PRAGMA user_version` carries SCHEMA*100 + REV, so a store
// written before a column was added still gets the migrations run exactly once: the old code
// compared user_version against the schema number alone, which meant a store already stamped
// with 2 never saw a later ALTER TABLE. The revision counter keeps counting across the 2 -> 3
// bump: it numbers additive steps, and never resetting it means no two stamps this core has
// ever written collide.
const int kSchemaRev = 4;
inline int user_version_want(void) { return WFS_STORE_SCHEMA * 100 + kSchemaRev; }

// Does `table` have a column called `column`, right now, in this database? The table names are
// this file's own string literals, so the interpolation is not a place a name can come from.
bool has_column(sqlite3 *db, const char *table, const char *column) {
    String sql("PRAGMA table_info(");
    sql.append(table);
    sql.append(")");
    Stmt q(db, sql.c_str());
    if (!q.ok()) return false;
    while (q.row())
        if (!::strcmp(q.col_text(1), column)) return true;   // 1 = name
    return false;
}

// ---- PR #1 review (11th round): migrating is a transaction, and its verdict is the schema ----
//
// Every ALTER's result used to be discarded and `user_version` stamped with the current revision
// regardless. That is only safe if the one thing an ALTER can do is fail with "duplicate column
// name", and it is not: an EIO, a full disk, a SQLITE_BUSY that outlives the busy timeout, or a
// database SQLite could only open read-only all fail the same call. The store was then stamped
// as migrated with a column missing -- and since the stamp is the only thing that decides
// whether the migrations run at all, no later open ever tried again. Every prepare that names
// the missing column fails from then on, for ever, and nothing in the store says why.
//
// So: one transaction around the whole thing; each step run only when PRAGMA table_info says the
// column is not there yet (no error-string matching, and a column that is already there is not a
// failure to tolerate but a step with nothing to do); every step's result checked; and the stamp
// written only after the schema has been asked, again, whether every column this core needs is
// present. Anything else rolls the whole thing back and fails the open with -EIO, leaving the
// store exactly as it was -- un-stamped, so the next open retries it.
int migrate_schema(sqlite3 *db) {
    wfs::Txn t(db);
    if (t.begin_rc != SQLITE_OK) return -EIO;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    for (const Migration &m : kMigrations) {
        if (has_column(db, m.table, m.column)) continue;
        if (sqlite3_exec(db, m.ddl, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    }
    // The verdict, from the schema rather than from the fact that the statements returned OK.
    for (const Migration &m : kMigrations)
        if (!has_column(db, m.table, m.column)) return -EIO;
    char pragma[64];
    ::snprintf(pragma, sizeof pragma, "PRAGMA user_version=%d", user_version_want());
    if (sqlite3_exec(db, pragma, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    return t.commit_rc() == SQLITE_OK ? 0 : -EIO;
}

void hex_id(char *out, size_t n) { // n = 33 for 32 hex digits + NUL
    unsigned char raw[16];
    if (::getentropy(raw, sizeof raw) != 0) {
        // Never fails on Darwin/Linux for <= 256 bytes; keep a deterministic fallback anyway.
        for (size_t i = 0; i < sizeof raw; ++i) raw[i] = (unsigned char)(::getpid() + i * 31 + (int)::time(nullptr));
    }
    static const char h[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; i < sizeof raw && j + 2 < n; ++i) {
        out[j++] = h[raw[i] >> 4];
        out[j++] = h[raw[i] & 15];
    }
    out[j] = 0;
}

// P13: the VERSION file is the first thing read and the first thing written. A store from a
// different schema is refused before the database is even opened, so an old CLI cannot
// migrate a new store by accident.
//
// ---- PR #1 review (24th round, P1): what a collector may delete is part of the schema -------
//
// M2 left the store looking, to an M1 binary, exactly like an M1 store -- every column it added
// was additive, so VERSION stayed at 2 and `main`'s build was still allowed to open it. But M2
// did not only add columns; it changed what is on disk and what may be done to it. Snapshots go
// through the trash now (T2.2), a discard in flight commits a row in WFS_ST_TRASHING with the
// tree at one of two names, the collector renames an entry to `.deleting` before it unlinks it,
// a pool row can sit in DRAINING, rows carry an owner, and a snapshot carries a hardlink
// manifest. M1's wfs_gc() knows none of it: it protects `state=2` world trash paths and treats
// everything else under trash/ and snapshots/ as an orphan to sweep. Run it on an M2 store and
// it recursively deletes a snapshot still inside its retention window, or the tree of a world
// whose row says TRASHING, and every M2 row then points at nothing. So: schema 3.
//
// That makes "different schema" asymmetric. A store still stamped 2 is one THIS core has never
// opened -- nothing above exists in it yet -- so it is taken over rather than refused:
// `*legacy` says so here, and version_upgrade() below rewrites the file to 3 once the database
// migration has committed. Anything that is neither 2 nor 3 is refused exactly as before.
int check_version(const char *dir, bool *legacy) {
    *legacy = false;
    String p(dir);
    p.append("/VERSION");
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd >= 0) {
        char buf[64] = {0};
        ssize_t n = ::read(fd, buf, sizeof buf - 1);
        ::close(fd);
        if (n <= 0) return -EIO;
        long v = ::strtol(buf, nullptr, 10);
        if (v == WFS_STORE_SCHEMA) return 0;
        if (v == WFS_STORE_SCHEMA_M1) { *legacy = true; return 0; }
        return WFS_E_SCHEMA;
    }
    if (errno != ENOENT) return -errno;
    fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return errno == EEXIST ? check_version(dir, legacy) : -errno;
    char line[64];
    int n = ::snprintf(line, sizeof line, "%d\n", WFS_STORE_SCHEMA);
    ssize_t w = ::write(fd, line, (size_t)n);
    ::close(fd);
    return w == n ? 0 : -EIO;
}

// The 2 -> 3 rewrite of that file. Through a temporary and a rename, because a VERSION that was
// half overwritten is a store NOTHING can open any more (a short read is -EIO above, in this
// core and in M1's alike), and the whole value of the file is that a binary older than the one
// that wrote it can still read it. Called only after the database migration has committed: a
// store we failed to migrate has not become a schema-3 store, and locking M1 out of it would
// buy nothing -- it is still the schema-2 store M1's collector can handle.
int version_upgrade(const char *dir) {
    String tmp(dir);
    tmp.append("/VERSION.tmp");
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    char line[64];
    int n = ::snprintf(line, sizeof line, "%d\n", WFS_STORE_SCHEMA);
    ssize_t w = ::write(fd, line, (size_t)n);
    int frc = ::fsync(fd);
    ::close(fd);
    if (w != n || frc != 0) { ::unlink(tmp.c_str()); return -EIO; }
    String dst(dir);
    dst.append("/VERSION");
    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        int e = -errno;
        ::unlink(tmp.c_str());
        return e;
    }
    return 0;
}

int meta_get(sqlite3 *db, const char *key, String &out) {
    Stmt q(db, "SELECT value FROM meta WHERE key=?");
    if (!q.ok()) return -EIO;
    q.text(1, key);
    if (!q.row()) return -ENOENT;
    out.assign(q.col_text(0));
    return 0;
}

// P17: does this store directory still hold trees? One readdir of each of the three places a
// tree can be, stopping at the first entry that is not "." or "..". Cheap enough to do on every
// open of a store whose metadata.db is not there.
//
// 1 = yes (and `what` names the first one found), 0 = no, a negative errno = the question could
// not be answered.
//
// PR #1 review (13th round, P1): that third answer is the whole point. This used to be a bool
// over three opendir(2)s whose failures were skipped in silence -- `if (!d) continue;` -- so a
// `snapshots/` that came back EACCES, EIO, or ENOENT-because-the-volume-is-not-mounted read as
// "nothing in there", the guard called the store empty, and the open built a fresh metadata.db
// and a fresh store id beside trees the old database was the index of. Worse than the missing
// guard: from then on the database IS there, so no later open ever asks again. Only ENOENT is
// evidence of absence -- the subtree is genuinely not there, so nothing can be in it -- and
// every other errno is the caller's to refuse on. The same rule the 12th round put on
// exists(): presence is assumed unless absence is proven.
int store_has_trees(const char *dir, String &what) {
    for (const char *sub : {"/snapshots", "/trash", "/pool"}) {
        String p(dir);
        p.append(sub);
        DIR *d = ::opendir(p.c_str());
        if (!d) {
            if (errno == ENOENT) continue;   // no such subtree: there is nothing in it
            return -errno;                   // and anything else is not an answer at all
        }
        bool found = false;
        errno = 0;   // readdir(3) reports its own failure only through errno
        while (struct dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
                continue;
            what.assign(sub + 1);
            what.append("/");
            what.append(e->d_name);
            found = true;
            break;
        }
        int rderr = found ? 0 : errno;   // a NULL return with errno set is a read that failed
        ::closedir(d);
        if (found) return 1;
        if (rderr) return -rderr;
    }
    return 0;
}

int meta_set(sqlite3 *db, const char *key, const char *value) {
    Stmt u(db, "INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!u.ok()) return -EIO;
    u.text(1, key);
    u.text(2, value);
    return u.step() == SQLITE_DONE ? 0 : -EIO;
}

} // namespace

// ---- PR #1 review: the shared gc retry counter (internal.h) ----------------------------------
//
// It lives here because it is one row of the store's meta table: read, increment, write, in one
// transaction. World.cpp keys it on a trash entry's name and pool.cpp (8th round) on a pool
// entry's path; neither has any business owning the counter itself.
namespace wfs {

const int64_t kGcFailCap = 5;

int64_t gc_fail_bump(wfs_store *s, const char *key) {
    if (!s || !key || !*key) return kGcFailCap;   // cannot count: do not spin
    Guard g(s->mu);
    Txn t(s->db);
    int64_t n = 0;
    {
        Stmt q(s->db, "SELECT value FROM meta WHERE key=?");
        if (!q.ok()) return kGcFailCap;
        q.text(1, key);
        if (q.row()) n = ::strtoll(q.col_text(0), nullptr, 10);
    }
    ++n;
    char v[32];
    ::snprintf(v, sizeof v, "%lld", (long long)n);
    Stmt u(s->db,
           "INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!u.ok()) return kGcFailCap;
    u.text(1, key);
    u.text(2, v);
    if (u.step() != SQLITE_DONE) return kGcFailCap;
    t.commit();
    return n;
}

// The read comes first so that the ordinary case -- something that never failed -- costs one
// indexed lookup instead of a transaction per removed tree.
void gc_fail_clear(wfs_store *s, const char *key) {
    if (!s || !key || !*key) return;
    Guard g(s->mu);
    {
        Stmt q(s->db, "SELECT 1 FROM meta WHERE key=?");
        if (!q.ok()) return;
        q.text(1, key);
        if (!q.row()) return;
    }
    Txn t(s->db);
    Stmt d(s->db, "DELETE FROM meta WHERE key=?");
    if (d.ok()) { d.text(1, key); d.step(); }
    t.commit();
}

} // namespace wfs

extern "C" const char *wfs_version(void) { return "0.1.0-m1"; }

extern "C" const char *wfs_strerror(int rc) {
    switch (rc) {
    case 0: return "ok";
    case WFS_E_CROSS_VOLUME: return "clonefile across volumes (EXDEV)";
    case WFS_E_UNREGISTERED: return "unregistered copy of a world";
    case WFS_E_NOT_A_WORLD: return "not a world (no .world marker)";
    case WFS_E_PATH_REFUSED: return "path refused by a safety rule";
    case WFS_E_LOW_SPACE: return "not enough free space";
    case WFS_E_SCHEMA: return "store schema mismatch";
    case WFS_E_WORLD_BUSY: return "world is locked by another command";
    case WFS_E_SNAPSHOT_DIRTY: return "snapshot no longer matches its manifest";
    case WFS_E_FOREIGN_STORE: return "marker belongs to a different store";
    case WFS_E_WORLD_MISSING: return "world is not at its recorded path";
    case WFS_E_SOURCE_GONE: return "the snapshot this world was forked from is gone";
    case WFS_E_POOL_BUSY: return "another pool fill is running";
    case WFS_E_TRASH_DELETING: return "this trash entry is already being deleted";
    case WFS_E_SNAPSHOT_IN_USE: return "a live world still needs this snapshot";
    case WFS_E_GC_BUSY: return "another gc worker is running";
    case WFS_E_STORE_UNREACHABLE: return "that store cannot be opened from here";
    case WFS_E_STORE_DAMAGED: return "the store has trees in it but no readable metadata.db";
    case WFS_E_TRASH_BLOCKED: return "a directory is in the way of this trash entry's deletion";
    default: return ::strerror(rc < 0 ? -rc : rc);
    }
}

extern "C" int wfs_store_default_dir(char *buf, size_t cap) {
    if (!buf || cap == 0) return -EINVAL;
    const char *home = ::getenv("HOME");
    if (!home || !*home) return -ENOENT;
#ifdef __APPLE__
    int n = ::snprintf(buf, cap, "%s/Library/Application Support/World/fs", home);
#else
    const char *x = ::getenv("XDG_DATA_HOME");
    int n = (x && *x) ? ::snprintf(buf, cap, "%s/world/fs", x)
                      : ::snprintf(buf, cap, "%s/.local/share/world/fs", home);
#endif
    return (n < 0 || (size_t)n >= cap) ? -ENAMETOOLONG : 0;
}

extern "C" const char *wfs_store_dir(const wfs_store *s) { return s ? s->dir.c_str() : ""; }

extern "C" int wfs_store_open(const char *store_dir, wfs_store **out) {
    if (!store_dir || !out) return -EINVAL;
    if (int rc = wfs::fs_mkdir_p(store_dir)) return rc;
    String real;
    if (int rc = wfs::fs_realpath(store_dir, real)) return rc;
    bool legacy_schema = false;
    if (int rc = check_version(real.c_str(), &legacy_schema)) return rc;

    // P17: never build a fresh database next to trees that the old one was the index of. Ids
    // restart at 1 when the database does, and the first `init` would then be handed S1 with
    // `snapshots/S1` already on disk -- so this is refused, loudly, before anything is created.
    // "Unreadable" counts as missing: a database we cannot open is one whose ids we do not know.
    //
    // PR #1 review (13th round, P1): and "I could not look" counts as neither. A stat(2) on
    // metadata.db that fails for a reason other than ENOENT says nothing about whether the file
    // is there, and a subtree scan that could not run says nothing about whether the store is
    // empty -- so both of those end the open with the errno that caused them, before anything
    // at all is created. The file itself is never replaced either way: a metadata.db that is
    // there but cannot be opened fails in sqlite3_open_v2 below (SQLITE_OPEN_CREATE creates a
    // database that is not there, it does not truncate one that is).
    {
        String dbp(real);
        dbp.append("/metadata.db");
        struct stat dbst;
        int srt = ::stat(dbp.c_str(), &dbst) == 0 ? 0 : -errno;
        if (srt && srt != -ENOENT) return srt;
        bool readable = srt == 0 && S_ISREG(dbst.st_mode) && dbst.st_size > 0 &&
                        ::access(dbp.c_str(), R_OK | W_OK) == 0;
        if (!readable) {
            String what;
            int trees = store_has_trees(real.c_str(), what);
            if (trees < 0) return trees;
            if (trees) return WFS_E_STORE_DAMAGED;
        }
    }

    wfs_store *s = new wfs_store();
    s->dir.assign(real.c_str());
    for (const char *sub : {"/snapshots", "/trash", "/locks", "/tmp", "/pool", "/logs"}) {
        String p(s->dir);
        p.append(sub);
        if (int rc = wfs::fs_mkdir_p(p.c_str())) { wfs_store_close(s); return rc; }
    }
    // Keep Spotlight out of the store: indexing a snapshot tree is pure waste and it was the
    // only reproducible outlier source in the benchmarks (CLONE_MODEL_MACOS27 §0).
    {
        String p(s->dir);
        p.append("/.metadata_never_index");
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) ::close(fd);
    }
    String dbp(s->dir);
    dbp.append("/metadata.db");
    // A file that is there but is not a database (truncated, or something else entirely) is the
    // same danger as one that is missing, and by here we know the store is not empty.
    int rc = sqlite3_open_v2(dbp.c_str(), &s->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) { wfs_store_close(s); return WFS_E_STORE_DAMAGED; }
    sqlite3_busy_timeout(s->db, 10000);
    // PR #1 review (24th round, P1): the database's own stamp is read before ANYTHING is written
    // to it -- before even the journal-mode pragma, which rewrites the file header -- because a
    // store from a schema we do not know has to come back untouched. `user_version` carries
    // SCHEMA*100 + REV, so the schema is its hundreds: a store from a future major is refused
    // with the same P13 code the VERSION file above would have refused it with. That file is
    // the line of defence that normally speaks; this one is for a database whose stamp and
    // whose VERSION file disagree, and it is the one thing an older core could not have written
    // by accident.
    int user_version = 0;
    {
        Stmt q(s->db, "PRAGMA user_version");
        if (q.ok() && q.row()) user_version = (int)q.col_i64(0);
    }
    if (user_version / 100 > WFS_STORE_SCHEMA) { wfs_store_close(s); return WFS_E_SCHEMA; }
    if (sqlite3_exec(s->db, kPragmas, nullptr, nullptr, nullptr) != SQLITE_OK) {
        wfs_store_close(s);
        return WFS_E_STORE_DAMAGED;
    }
    // All of it or none of it, and the stamp last (migrate_schema above). A store this fails on
    // is left un-stamped and untouched, so the next open is the retry. A schema-2 store gets its
    // columns from the same table-driven pass -- they were all there already, additively -- and
    // the 3 in `user_version_want()` is the whole of what the bump costs it.
    if (user_version != user_version_want()) {
        if (int mrc = migrate_schema(s->db)) { wfs_store_close(s); return mrc; }
    }
    // Only now the file an older binary reads first. M1 (`main`) does, verbatim:
    //     return v == WFS_STORE_SCHEMA ? 0 : WFS_E_SCHEMA;   // with WFS_STORE_SCHEMA == 2
    // so this single line is what keeps its collector off a store that is no longer its own.
    if (legacy_schema) {
        if (int vrc = version_upgrade(s->dir.c_str())) { wfs_store_close(s); return vrc; }
    }
    if (meta_get(s->db, "store_id", s->store_id) != 0) {
        char id[33];
        hex_id(id, sizeof id);
        if (meta_set(s->db, "store_id", id) != 0) { wfs_store_close(s); return -EIO; }
        s->store_id.assign(id);
    }
    // PR #1 review (5th round): a discard killed between its rename and its commit leaves one
    // row in WFS_ST_TRASHING and a tree at one of two names. Resolving it is two indexed
    // SELECTs that normally return nothing, and it has to happen before anything in this process
    // reads a state or classifies the trash -- `gc --status` and the orphan rule both do.
    wfs::trashing_recover(s, nullptr, nullptr);
    *out = s;
    return 0;
}

extern "C" void wfs_store_close(wfs_store *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    delete s;
}

extern "C" int wfs_store_status(wfs_store *s, wfs_store_stat *out) {
    if (!s || !out) return -EINVAL;
    memset(out, 0, sizeof *out);
    wfs::copy_str(out->dir, sizeof out->dir, s->dir.c_str());
    wfs::copy_str(out->store_id, sizeof out->store_id, s->store_id.c_str());
    out->schema = WFS_STORE_SCHEMA;
    Guard g(s->mu);
    {
        Stmt q(s->db, "SELECT COUNT(*), COALESCE(SUM(entries),0) FROM snapshots WHERE state=?");
        if (!q.ok()) return -EIO;
        q.i64(1, WFS_ST_ACTIVE);
        if (q.row()) { out->snapshots = (uint64_t)q.col_i64(0); out->snapshot_entries = (uint64_t)q.col_i64(1); }
    }
    {
        Stmt q(s->db, "SELECT state, COUNT(*), COALESCE(SUM(entries),0) FROM worlds GROUP BY state");
        if (!q.ok()) return -EIO;
        while (q.row()) {
            uint64_t n = (uint64_t)q.col_i64(1);
            switch ((int)q.col_i64(0)) {
            case WFS_ST_ACTIVE: out->worlds_active = n; out->world_entries = (uint64_t)q.col_i64(2); break;
            case WFS_ST_TRASHED: out->worlds_trashed = n; break;
            case WFS_ST_DEAD: out->worlds_dead = n; break;
            default: break;
            }
        }
    }
    {
        Stmt q(s->db, "SELECT COUNT(*), COALESCE(SUM(entries),0) FROM pool WHERE state=1");
        if (q.ok() && q.row()) {
            out->pool_ready = (uint64_t)q.col_i64(0);
            out->pool_entries = (uint64_t)q.col_i64(1);
        }
    }
    // T2.2 reconciliation: a row whose tree is not there. One stat(2) per row, and a store has
    // tens of snapshots and (measured) a thousand worlds at most, so `status` stays a 7 ms
    // command. A world that was merely moved shows up here too until someone runs
    // `world fs verify <its new path>`; that is why `gc --reconcile` is explicit.
    // PR #1 review (12th round): the same rule the collector reconciles by (world.cpp). A
    // stat(2) that fails for a reason other than absence -- EACCES on a parent, EIO, a volume
    // that is not mounted -- is not evidence that anything is gone, and neither is a path that
    // holds something which is not a directory. `status` is the number an operator reads before
    // running `gc --reconcile`, so it must not call either of those dangling.
    {
        Stmt q(s->db, "SELECT path FROM snapshots WHERE state=?");
        if (q.ok()) {
            q.i64(1, WFS_ST_ACTIVE);
            struct stat st;
            while (q.row()) {
                const char *p = q.col_text(0);
                int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
                if (prc == 0 && S_ISDIR(st.st_mode)) continue;
                if (wfs::fs_gone(prc)) out->snapshots_dangling++;
                else out->snapshots_unreadable++;
            }
        }
    }
    {
        Stmt q(s->db, "SELECT COUNT(*) FROM snapshots WHERE state=?");
        if (q.ok()) { q.i64(1, WFS_ST_TRASHED); if (q.row()) out->snapshots_trashed = (uint64_t)q.col_i64(0); }
    }
    {
        Stmt q(s->db, "SELECT path, dir_ino FROM worlds WHERE state=?");
        if (q.ok()) {
            q.i64(1, WFS_ST_ACTIVE);
            struct stat st;
            while (q.row()) {
                const char *p = q.col_text(0);
                int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
                if (prc == 0 && S_ISDIR(st.st_mode)) continue;
                if (wfs::fs_gone(prc)) out->worlds_dangling++;
                else out->worlds_unreadable++;
            }
        }
    }
    uint64_t avail = 0, total = 0;
    if (int rc = wfs::fs_free_space(s->dir.c_str(), &avail, &total)) return rc;
    out->volume_free_bytes = avail;
    out->volume_total_bytes = total;
    // 308 B/entry, measured on 27.0 for a 50k-file clone (CLONE_MODEL_MACOS27 §4).
    out->metadata_estimate_bytes =
        (out->snapshot_entries + out->world_entries + out->pool_entries) * 308;
    return 0;
}

extern "C" int wfs_store_clone_probe(wfs_store *s, const char *src_dir) {
    if (!s || !src_dir) return -EINVAL;
    int rc = wfs::fs_clone_probe(s->dir.c_str(), src_dir);
    if (rc == -EXDEV || rc == -ENOTSUP) return WFS_E_CROSS_VOLUME;
    return rc;
}
