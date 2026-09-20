// The store: a directory with a VERSION file, a SQLite metadata3.db (schema v3), the snapshot
// trees and the trash. Everything that is not snapshot/world lifecycle lives here.
//
// The database's name is part of the schema (PR #1 review, 35th round): schema 2 -- M1's -- kept
// it at `metadata.db`, schema 3 keeps it at `metadata3.db`, and the old name becomes an empty
// directory that no sqlite3_open_v2() can open. See store_layout() below for why.
#include "db.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>

using wfs::Guard;
using wfs::String;
using wfs::Stmt;

// The 2 -> 3 upgrade's rename window (PR #1 review, 31st round). NULL in every run that is not
// core_test; nothing in the library ever assigns it. Declared in worldfs.h.
extern "C" void (*wfs_test_before_version_rename)(void *ctx, const char *dir) = nullptr;
extern "C" void *wfs_test_before_version_rename_ctx = nullptr;

// The window between the VERSION bump and the database migration (PR #1 review, 32nd round).
// Same rules: NULL in every run that is not core_test. Declared in worldfs.h.
extern "C" void (*wfs_test_after_version_bump)(void *ctx, const char *dir) = nullptr;
extern "C" void *wfs_test_after_version_bump_ctx = nullptr;

// One step(2), failed on demand (PR #1 review, 32nd round). NULL in every run that is not a
// test; nothing in the library ever assigns it a value. Read by Stmt::row() in db.h.
extern "C" const char *wfs_test_stmt_fail_sql = nullptr;

// One BEGIN IMMEDIATE, failed on demand (PR #1 review, 34th round). Same rules: 0 in every run
// that is not a test, and nothing in the library ever assigns it. Read by wfs::Txn in db.h.
extern "C" int wfs_test_txn_fail_once = 0;

// The gap between the database's move and its migration (PR #1 review, 35th round): the stub is
// in place, the holder gate has passed, nothing has been migrated yet. NULL in every run that is
// not core_test; nothing in the library ever assigns it. Declared in worldfs.h.
extern "C" void (*wfs_test_after_db_move)(void *ctx, const char *dir) = nullptr;
extern "C" void *wfs_test_after_db_move_ctx = nullptr;

// The three steps the database's move is made of (PR #1 review, 36th round): 1 = after the
// link, 2 = after the exchange, 3 = after the extra link is dropped. NULL in every run that is
// not core_test; nothing in the library ever assigns it. Declared in worldfs.h.
extern "C" void (*wfs_test_between_db_steps)(void *ctx, const char *dir, int phase) = nullptr;
extern "C" void *wfs_test_between_db_steps_ctx = nullptr;

namespace {

// ---- PR #1 review (35th round, P1): the database's name is part of the schema ----------------
//
// The 32nd round put the VERSION bump before the migration and the 34th round added the holder
// gate, and between them they cover every M1 process except one: the process that read
// `VERSION` as 2, was admitted, and then paused BEFORE its sqlite3_open_v2(). It holds no
// descriptor, so proc_listpidspath(3) cannot see it; it has already read the file, so the bump
// cannot refuse it. When it resumes it opens the database this core has just migrated, stamps
// `user_version` back down to 2 (M1's open does exactly that for any stamp that is not 2) and
// runs M1's collector over M2 trash.
//
// There is no file M1 reads later that could exclude it -- M1 reads VERSION once and then opens
// the database. So the database itself has to be the exclusion: a schema-3 store keeps it under
// a name M1 does not know, and the name M1 does know is left as something M1 cannot open.
// Measured here (scratchpad probe, SQLite 3.54.0): sqlite3_open_v2() on a DIRECTORY returns
// SQLITE_CANTOPEN (14) with SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, with READWRITE alone and
// with READONLY alone -- every mode M1 could use -- so an empty directory at `metadata.db` fails
// M1's open outright, and M1's open failing ends M1's command before its collector runs.
const char *kDbLeaf = "/metadata3.db";     // schema 3: this core's database
const char *kDbLeafM1 = "/metadata.db";    // schema 2: M1's, and afterwards the stub directory

void db_path(const char *dir, String &out, bool m1_name) {
    out.assign(dir);
    out.append(m1_name ? kDbLeafM1 : kDbLeaf);
}

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
//
// PR #1 review (32nd round, P2): 1 = yes, 0 = no, negative = the schema could not be asked. It
// used to answer a bool, so a prepare that failed and a step that failed both came back "the
// column is not there" -- which is the answer that runs the ALTER (it then fails with duplicate
// column name and the whole migration is rolled back) and, in the verification pass below, the
// answer that calls a perfectly migrated store un-migrated. Neither is a lie the migration is
// allowed to tell about the schema.
int has_column(sqlite3 *db, const char *table, const char *column) {
    String sql("PRAGMA table_info(");
    sql.append(table);
    sql.append(")");
    Stmt q(db, sql.c_str());
    if (!q.ok()) return -EIO;
    while (q.row())
        if (!::strcmp(q.col_text(1), column)) return 1;   // 1 = name
    return q.done() ? 0 : -EIO;
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
    if (!t.ok()) return t.err();
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    for (const Migration &m : kMigrations) {
        int hc = has_column(db, m.table, m.column);
        if (hc < 0) return -EIO;
        if (hc) continue;
        if (sqlite3_exec(db, m.ddl, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    }
    // The verdict, from the schema rather than from the fact that the statements returned OK.
    for (const Migration &m : kMigrations)
        if (has_column(db, m.table, m.column) != 1) return -EIO;
    char pragma[64];
    ::snprintf(pragma, sizeof pragma, "PRAGMA user_version=%d", user_version_want());
    if (sqlite3_exec(db, pragma, nullptr, nullptr, nullptr) != SQLITE_OK) return -EIO;
    return t.commit();
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

// What the VERSION file says right now, or a negative errno. Unlike check_version() this one
// never writes: it is the second look, after a rename that did not land, at a file somebody
// else may have finished writing in the meantime.
int version_read(const char *dir, long *out) {
    String p(dir);
    p.append("/VERSION");
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return -errno;
    char buf[64] = {0};
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return -EIO;
    *out = ::strtol(buf, nullptr, 10);
    return 0;
}

// The leftovers of an upgrader that was killed between creating its temporary and renaming it.
//
// P18 / PR #1 review (20th round): a sweep is only ever allowed to delete names this core
// itself allocated. These are: the name is inside `<store>` (never a subdirectory, never
// followed through a symlink -- unlink(2) does not follow), and it begins with `VERSION.tmp`,
// which is the whole of the namespace this file has ever written there -- `VERSION.tmp.<pid>.<hex>`
// now, and the single shared `VERSION.tmp` that older builds of this core used. Nothing else
// writes a name under that prefix in a store directory, and a user's own file there is not a
// name they can reach by accident. Called only from the upgrade below, i.e. only by a process
// that is itself the upgrader, so the readdir is not on any other open's path; failures are
// ignored, because a temporary left behind is 2 bytes and never read by anything.
void version_tmp_sweep(const char *dir) {
    DIR *d = ::opendir(dir);
    if (!d) return;
    while (struct dirent *e = ::readdir(d)) {
        if (::strncmp(e->d_name, "VERSION.tmp", 11) != 0) continue;
        String p(dir);
        p.append("/");
        p.append(e->d_name);
        ::unlink(p.c_str());
    }
    ::closedir(d);
}

// The 2 -> 3 rewrite of that file. Through a temporary and a rename, because a VERSION that was
// half overwritten is a store NOTHING can open any more (a short read is -EIO above, in this
// core and in M1's alike), and the whole value of the file is that a binary older than the one
// that wrote it can still read it. Called only after the database migration has committed: a
// store we failed to migrate has not become a schema-3 store, and locking M1 out of it would
// buy nothing -- it is still the schema-2 store M1's collector can handle.
//
// ---- PR #1 review (31st round, P2): and two of us may be doing it at once -----------------
//
// The first open of a schema-2 store is not serialised by anything. wfs_store_open() takes no
// lock of its own; the only mutual exclusion in the whole path is the migration's own BEGIN
// IMMEDIATE, which is enough for the database -- one transaction wins and the other finds every
// column already there -- and is no help at all for the file. So both processes read VERSION as
// 2, both migrate, and both arrive here. This used to write one shared `<store>/VERSION.tmp`:
// the two of them truncated the same file, and after the first rename consumed it the second
// process renamed a name that was no longer there and returned -ENOENT out of an open that had
// done nothing wrong -- a valid `world` invocation failing on a perfectly healthy store, and
// only ever on its very first open, which is the one nobody is watching.
//
// The temporary is private now: O_CREAT|O_EXCL on `VERSION.tmp.<pid>.<hex>`, hex from the same
// getentropy() the store id is drawn with, so no two upgraders can be holding the same one. The
// rename that lands second then simply replaces a VERSION that already says 3 with a VERSION
// that says 3, which is the idempotent write it always was. A rename that fails anyway asks the
// file itself: if it reads 3, the upgrade is done -- by us or by whoever got there first -- and
// this returns 0, because what the caller needs is a schema-3 store on disk, not the authorship
// of it. Only an errno with VERSION still at 2 is a failure, and it is returned as one.
//
// PR #1 review (34th round, P1): `value`, because the same write is now made in both directions.
// The bump to 3 is one of them; the other is the revert back to 2 that undoes it when the
// holder check below finds the door was shut on somebody who was already inside. `seam` is the
// 31st round's test window, which belongs to the upgrade only.
int version_put(const char *dir, int value, bool seam) {
    char rnd[33];
    hex_id(rnd, sizeof rnd);
    char name[80];
    ::snprintf(name, sizeof name, "VERSION.tmp.%lld.%s", (long long)::getpid(), rnd);
    String tmp(dir);
    tmp.append("/");
    tmp.append(name);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -errno;
    char line[64];
    int n = ::snprintf(line, sizeof line, "%d\n", value);
    ssize_t w = ::write(fd, line, (size_t)n);
    int frc = ::fsync(fd);
    ::close(fd);
    if (w != n || frc != 0) { ::unlink(tmp.c_str()); return -EIO; }
    String dst(dir);
    dst.append("/VERSION");
    if (seam && wfs_test_before_version_rename)
        wfs_test_before_version_rename(wfs_test_before_version_rename_ctx, dir);
    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        int e = -errno;
        ::unlink(tmp.c_str());
        return e;
    }
    return 0;
}

int version_upgrade(const char *dir) {
    if (int rc = version_put(dir, WFS_STORE_SCHEMA, true)) {
        long v = 0;
        // Somebody else's rename may have landed while ours was failing -- including a sweep
        // like the one below, run by a concurrent upgrader, that took our temporary out from
        // under us. The store is schema 3 either way, so this open has nothing to refuse.
        if (version_read(dir, &v) == 0 && v == WFS_STORE_SCHEMA) { version_tmp_sweep(dir); return 0; }
        return rc;
    }
    version_tmp_sweep(dir);
    return 0;
}

// ---- PR #1 review (35th round, P1): moving the database out from under the old name ----------
//
// Step (b) of the upgrade. The rename itself is one syscall; what takes care is what SQLite
// leaves NEXT to the file. `main`'s M1 core runs
//     "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=OFF;"
// on every open, so a live M1 store carries `metadata.db-wal` and `metadata.db-shm`, and one
// that was interrupted before WAL was set can carry a rollback `metadata.db-journal`. Renaming
// the database alone would strand all three beside a DIRECTORY of that name -- a hot journal
// whose database is no longer there is the one shape of this that loses data.
//
// So the old file is opened with this core's own connection first. That open is what recovers a
// hot rollback journal (SQLite replays it on the first read), and `PRAGMA journal_mode=DELETE`
// is what checkpoints the WAL back into the database and removes the `-wal` and the `-shm`. The
// pragma answers with the mode that is now in force, so it is READ rather than assumed: another
// connection holding the WAL makes the change fail, and SQLite reports that by answering "wal",
// not by an error code. Anything that is not "delete" is a store somebody else is in --
// WFS_E_STORE_BUSY, with the file still where M1 expects it.
//
// After the close, the three sidecar names are unlinked unconditionally. They are ours by the
// same P18 rule the VERSION temporaries follow -- inside `<store>`, never a subdirectory, and
// under a prefix nothing but SQLite writes there -- and everything they held is in the database
// we have just checkpointed and closed, so what is unlinked is a file with nothing in it.
//
// ---- PR #1 review (36th round, P1): and `metadata.db` is never absent, not for an instant ---
//
// The 35th round moved the database with one rename and made the stub with one mkdir, and
// between those two syscalls the name M1 opens is NOT THERE. An M1 process that read VERSION as
// 2, was admitted, and resumes in exactly that window opens `<store>/metadata.db` with
// SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE -- and CREATE is what it then does: SQLite makes a
// fresh, empty database under the name, M1 stamps it 2 and finds no rows at all, so its
// collector walks the store and deletes every snapshot tree and every trash entry in it as an
// orphan. This core, arriving a moment later, finds a regular file where its stub should be and
// reports the store damaged. Measured on the parent commit with a seam in that window: the
// M1-shaped open returned SQLITE_OK, `SELECT count(*) FROM worlds` failed with "no such table"
// on a database that had two rows a moment earlier, `PRAGMA user_version` read 0, and
// wfs_store_open() then returned WFS_E_STORE_DAMAGED.
//
// So the move and the stub are ONE step, and the name is occupied throughout it:
//
//   1. mkdir <store>/metadata.db.stub.<pid>.<hex>    0500 and empty -- the stub, built under a
//                                                    private name, so nothing at `metadata.db`
//                                                    has moved yet
//   2. link  metadata.db -> metadata3.db             the database gains its new name while
//                                                    keeping the old one: one inode, two names
//   3. EXCHANGE stub_tmp with metadata.db            fs_rename_swap(): the two entries change
//                                                    places in one atomic step, so
//                                                    `metadata.db` goes from being the database
//                                                    to being the directory with no instant in
//                                                    between, and `stub_tmp` becomes the
//                                                    database's second link
//   4. unlink stub_tmp                               the extra link goes; the database is at
//                                                    `metadata3.db` and nowhere else
//
// Step 2 hard-links a CLOSED SQLite database, which is safe precisely because it is closed:
// step (b) above ran `PRAGMA journal_mode=DELETE` through this core's own connection and then
// unlinked the three sidecar names, so what is linked is a plain file with nothing open on it
// and nothing beside it. A hard link is a second NAME for one inode, not a copy -- there is no
// second database, so there is nothing that can diverge.
//
// Step 3 was measured before it was written (scratchpad probe, APFS, this machine):
// renameatx_np(AT_FDCWD, <directory>, AT_FDCWD, <regular file>, RENAME_SWAP) returns 0 and the
// entries change places -- the directory keeps its 0500 mode, the file keeps its inode, its
// size and its link count -- and the reverse exchange the revert makes was measured the same
// way. The project already uses renameatx_np(RENAME_EXCL) for every publish; this is the same
// call with the other flag, behind fs_rename_swap() so that the one non-portable syscall stays
// in the platform layer.
//
// What an M1 process sees, at every instant of the four steps:
//   * before 3 (including between 2 and 3): `metadata.db` IS the database -- one of its two
//     names. That open SUCCEEDS and gets the real database with all of its rows, which is the
//     case the 34th round's holder gate exists for, and the gate finds it: proc_listpidspath(3)
//     resolves `metadata3.db` to the same vnode. A holder there is a full revert and
//     WFS_E_STORE_BUSY.
//   * after 3: `metadata.db` is the directory. SQLITE_CANTOPEN in every open mode M1 could use
//     (the 35th round's measurement), so M1's command ends before its collector runs.
// There is no third instant, and in neither of them does an M1 open create a database.
//
// The private name the stub is built under. The other prefix this core sweeps inside a store
// directory, by the same P18 rule the VERSION temporaries follow: ours, never in a
// subdirectory, and nothing but the code below ever writes it.
const char *kStubPrefix = "metadata.db.stub.";

void db_stub_tmp(const char *dir, String &out) {
    char rnd[33];
    hex_id(rnd, sizeof rnd);
    char name[96];
    ::snprintf(name, sizeof name, "/%s%lld.%s", kStubPrefix, (long long)::getpid(), rnd);
    out.assign(dir);
    out.append(name);
}

// What an interrupted move left under that prefix, and nothing else:
//   * a DIRECTORY is a stub that was never exchanged (a crash before step 3). It is empty by
//     construction, so rmdir(2) is the whole of removing it.
//   * a REGULAR FILE whose inode is `metadata3.db`'s is the extra link of step 4 (a crash
//     between 3 and 4). Unlinking it IS step 4, done late.
// Anything else under the prefix stays exactly where it is: this removes only what it can prove
// is ours. A concurrent upgrader's stub can be taken out from under it, and then that
// upgrader's exchange fails and its open retries -- the same trade the 31st round's VERSION.tmp
// sweep makes, for the same reason: there is no lock on this path and there does not need to be.
void db_stub_sweep(const char *dir, uint64_t db_ino) {
    DIR *d = ::opendir(dir);
    if (!d) return;
    size_t pl = ::strlen(kStubPrefix);
    while (struct dirent *e = ::readdir(d)) {
        if (::strncmp(e->d_name, kStubPrefix, pl) != 0) continue;
        String p(dir);
        p.append("/");
        p.append(e->d_name);
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) ::rmdir(p.c_str());
        else if (S_ISREG(st.st_mode) && db_ino != 0 && (uint64_t)st.st_ino == db_ino)
            ::unlink(p.c_str());
    }
    ::closedir(d);
}

// 0, or the error the upgrade ends with. Every failure leaves the store in one of the states
// store_layout() below resumes from, and in all of them `metadata.db` is still there.
int db_move_to_schema3(const char *dir) {
    String olddb, newdb;
    db_path(dir, olddb, true);
    db_path(dir, newdb, false);
    // A sidecar at the SCHEMA-3 name. Only a resumed run reaches this function with
    // `metadata3.db` already present (a crash between steps 2 and 3), and a `-wal` or a hot
    // `-journal` under that name belongs to an opener this core knows nothing about: the
    // checkpoint below goes through the OLD name, so SQLite would not even look at it, and
    // carrying on would strand it. That is the one shape of this that loses data, so it is
    // refused with the store left exactly as it is.
    for (const char *sfx : {"-wal", "-shm", "-journal"}) {
        String p(newdb);
        p.append(sfx);
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) return WFS_E_STORE_BUSY;
    }
    sqlite3 *h = nullptr;
    // No SQLITE_OPEN_CREATE: this is called only when `metadata.db` is a regular file, and a
    // database that has gone missing under us is not one to create here.
    if (sqlite3_open_v2(olddb.c_str(), &h, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (h) sqlite3_close(h);
        return WFS_E_STORE_DAMAGED;
    }
    sqlite3_busy_timeout(h, 10000);
    int rc = 0;
    {
        Stmt q(h, "PRAGMA journal_mode=DELETE");
        // A file that is not a database comes back here as SQLITE_NOTADB, from the step -- the
        // same verdict the 13th round gave every other shape of that.
        if (!q.ok() || !q.row()) rc = WFS_E_STORE_DAMAGED;
        else {
            const char *mode = q.col_text(0);
            if (!mode || ::strcasecmp(mode, "delete") != 0) rc = WFS_E_STORE_BUSY;
        }
    }
    sqlite3_close(h);
    if (rc) return rc;
    for (const char *sfx : {"-wal", "-shm", "-journal"}) {
        String p(olddb);
        p.append(sfx);
        ::unlink(p.c_str());
    }
    // ---- 1. the stub, under a name nothing else can be holding ------------------------------
    String stub;
    db_stub_tmp(dir, stub);
    if (::mkdir(stub.c_str(), 0500) != 0) return -errno;
    // ---- 2. the second name -----------------------------------------------------------------
    if (::link(olddb.c_str(), newdb.c_str()) != 0) {
        int e = errno;
        struct stat a, b;
        if (e != EEXIST) rc = -e;
        else if (::stat(olddb.c_str(), &a) != 0 || ::stat(newdb.c_str(), &b) != 0) rc = -errno;
        // The same inode under both names is a run that crashed between 2 and 3: the link we
        // were about to make is already there, so this step is simply done. Two DIFFERENT
        // inodes is a shape the protocol cannot produce, and is never written over.
        else if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) rc = WFS_E_STORE_DAMAGED;
    }
    if (!rc && wfs_test_between_db_steps)
        wfs_test_between_db_steps(wfs_test_between_db_steps_ctx, dir, 1);
    // ---- 3. the exchange --------------------------------------------------------------------
    if (!rc) {
        if (int src = wfs::fs_rename_swap(stub.c_str(), olddb.c_str())) rc = src;
    }
    if (rc) {
        // Nothing left behind here is a state the next open cannot pick up: `metadata.db` is
        // either the database alone, or the database with `metadata3.db` as its second name.
        // Our own stub goes -- it is empty and nothing was ever put in it.
        ::rmdir(stub.c_str());
        return rc;
    }
    // The invariant this round is about, checked once, where it is cheapest to check: from here
    // on `metadata.db` is a DIRECTORY, and an instant ago it was the database. It is never
    // absent in between, and an M1 open can therefore never create one here.
    struct stat ost;
    if (::stat(olddb.c_str(), &ost) != 0 || !S_ISDIR(ost.st_mode)) return WFS_E_STORE_DAMAGED;
    if (wfs_test_between_db_steps)
        wfs_test_between_db_steps(wfs_test_between_db_steps_ctx, dir, 2);
    // ---- 4. the extra link ------------------------------------------------------------------
    // A DIRECTORY at our own stub name is another upgrader that exchanged between our 3 and our
    // 4: its stub and ours changed places. Both are empty and `metadata.db` is a stub either
    // way, so the only difference is which call removes this one.
    if (::unlink(stub.c_str()) != 0 && (errno == EPERM || errno == EISDIR)) ::rmdir(stub.c_str());
    if (wfs_test_between_db_steps)
        wfs_test_between_db_steps(wfs_test_between_db_steps_ctx, dir, 3);
    return 0;
}

// The database's own stamp, read from the file at `path` without writing a byte of it. Used on
// the upgrade path, where the stamp has to be read from the database where it still LIES -- a
// store from a schema we do not know comes back exactly as it was found (24th round), and after
// (b) it would already have been moved. READWRITE and not READONLY: a schema-2 store is in WAL
// mode, and a read-only connection to a WAL database needs the `-shm` it may not be allowed to
// make. Reading a pragma writes nothing to the database file either way.
int db_user_version_at(const char *path, int *out) {
    sqlite3 *h = nullptr;
    if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (h) sqlite3_close(h);
        return WFS_E_STORE_DAMAGED;
    }
    sqlite3_busy_timeout(h, 10000);
    int rc = 0;
    {
        Stmt q(h, "PRAGMA user_version");
        if (!q.ok() || !q.row()) rc = WFS_E_STORE_DAMAGED;
        else *out = (int)q.col_i64(0);
    }
    sqlite3_close(h);
    return rc;
}

// The stub on its own: an empty directory, mode 0500, at the name M1 opens. Idempotent, because
// every crash state of the upgrade is resumed by re-running the steps that are still owed.
//
// PR #1 review (36th round, P1): this is no longer how the stub arrives at the end of a MOVE --
// there it is exchanged into place with the database, in one step, so that the name is never
// free (db_move_to_schema3 above). What is left for this are the two states where there is
// nothing at the M1 name to exchange with: a brand new store, whose database this open created
// itself a moment ago, and a schema-3 store whose stub somebody removed. In neither is there a
// database at `metadata.db` for an M1 open to find, so a plain mkdir is the whole of it.
int db_stub_make(const char *dir) {
    String p;
    db_path(dir, p, true);
    if (::mkdir(p.c_str(), 0500) == 0) return 0;
    if (errno != EEXIST) return -errno;
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return -errno;
    // Something that is not our directory took the name while we were not looking. That is not
    // a store to carry on migrating.
    return S_ISDIR(st.st_mode) ? 0 : WFS_E_STORE_DAMAGED;
}

// ---- PR #1 review (34th round, P1): the handle that was already inside -----------------------
//
// The 32nd round put the VERSION bump before the migration, so that from the instant anything
// migrated exists in this store, every M1 binary has already been refused it by the one file it
// checks first. That shuts the door on M1 processes that START after the rename. It can do
// nothing about one that completed wfs_store_open() a millisecond BEFORE it: that process holds
// an open sqlite3 handle on a database that is about to become M2's, and M1 takes no store-wide
// lock of any kind, so the exclusion cannot be made to need M1's cooperation. What such a
// process does with the handle is the whole of the 24th round's finding -- it stamps
// `user_version` back down to 2, and then its wfs_gc() deletes M2 trash: a snapshot still inside
// its retention window, the tree of a world whose row says TRASHING.
//
// So the question is asked of the operating system, in this order:
//
//     bump VERSION to 3  ->  move the database  ->  list who has it open  ->  holders ? revert
//                                                                                     : migrate
//
// and the order is what makes the answer complete. A holder that appears AFTER the listing was
// admitted after the rename, and the file already refused it (32nd round). A holder the listing
// names was admitted before the bump -- exactly the set this exists for. Nothing can slip
// between the two, because there is no third case.
//
// PR #1 review (35th round, P1): the question is asked of the NEW name, and it still finds the
// handles that were opened under the old one. proc_listpidspath(3) resolves the path it is given
// to a vnode and compares vnodes, not names, so a process holding the renamed inode is reported
// against `metadata3.db`. Verified in a scratchpad probe: a child opens `<dir>/metadata.db`, the
// parent renames it to `<dir>/metadata3.db`, and proc_listpidspath on the new name returns that
// child's pid -- before the rename, after the rename, and with the stub directory created at the
// old name; asking about the old name after the rename returns ENOENT, which is why the question
// has to be asked about the new one.
//
// Every foreign holder is treated the same. Another M2 upgrader is indistinguishable from an M1
// one by pid, and guessing wrong in that direction is the expensive mistake; two upgraders that
// refuse each other is a retry on the first open of a store, which is a cost nobody notices.
// "Cannot tell" -- an errno from the platform, or the -ENOSYS of a platform that cannot ask
// (Linux is deferred: docs/M1_DESIGN.md P13) -- is a holder for this purpose, because the whole
// point is that admitting one is not recoverable.
//
// The revert undoes the three steps in reverse: the stub directory, the rename, the VERSION
// file. A store that is refused is a store an M1 binary may still open and collect normally --
// which is what it was a moment ago. A revert that cannot finish stops where it is and leaves
// VERSION at 3 over a database M1 cannot reach under either name: refused by M1, finished by the
// next M2 open, which is the safe half of the same trade the 32nd round made.
int legacy_holders_gate(const char *dir) {
    String newdb, olddb;
    db_path(dir, newdb, false);
    db_path(dir, olddb, true);
    wfs::Vec<int64_t> holders;
    int rc = wfs::fs_other_holders(newdb.c_str(), holders);
    if (rc == 0 && holders.size() == 0) return 0;
    // The database goes back alone or not at all: a `-wal` left at the schema-3 name beside a
    // database at the schema-2 one is an M1 open that silently reads a database missing every
    // committed page in that WAL, which is worse than any refusal. db_move_to_schema3() leaves
    // none behind and the gate runs before this core's own journal-mode pragma, so on the
    // ordinary path there are none to find.
    for (const char *sfx : {"-wal", "-shm", "-journal"}) {
        String p(newdb);
        p.append(sfx);
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) { version_tmp_sweep(dir); return WFS_E_STORE_BUSY; }
    }
    // PR #1 review (36th round, P1): and backwards through the same four steps, so that the
    // revert does not open the hole the move just closed. `metadata.db` is the stub directory
    // at this point; an rmdir of it followed by a rename of the database back to it would leave
    // that name ABSENT in between -- the very window an admitted M1 process turns into a fresh
    // empty database. So the database is linked under a private name first, and that name is
    // EXCHANGED with the stub: the directory leaves for the private name and the database
    // arrives at `metadata.db` in one step. Only then does the schema-3 name go, and only then
    // is what is now an empty directory at the private name removed.
    //
    // A step that fails stops the revert where it is: what is on disk then is one of the crash
    // states store_layout() below resumes from, and pressing on would make it one that is not.
    // The stub left over in that case is swept by the next open (db_stub_sweep above).
    String stub;
    db_stub_tmp(dir, stub);
    if (::link(newdb.c_str(), stub.c_str()) == 0) {
        if (wfs::fs_rename_swap(stub.c_str(), olddb.c_str()) == 0) {
            // `metadata.db` is the database again -- its second link -- and `stub` is the stub
            // directory. The schema-3 name can go now, and not one instant earlier.
            if (::unlink(newdb.c_str()) == 0) {
                ::rmdir(stub.c_str());
                version_put(dir, WFS_STORE_SCHEMA_M1, false);   // best effort; see above
            }
        } else {
            ::unlink(stub.c_str());   // the extra link, and nothing else, goes back
        }
    }
    version_tmp_sweep(dir);
    return WFS_E_STORE_BUSY;
}

int meta_get(sqlite3 *db, const char *key, String &out) {
    Stmt q(db, "SELECT value FROM meta WHERE key=?");
    if (!q.ok()) return -EIO;
    q.text(1, key);
    if (!q.row()) return q.done() ? -ENOENT : -EIO;   // 32nd round, P2
    out.assign(q.col_text(0));
    return 0;
}

// P17: does this store directory still hold trees? One readdir of each of the three places a
// tree can be, stopping at the first entry that is not "." or "..". Cheap enough to do on every
// open of a store whose database is not there.
//
// 1 = yes (and `what` names the first one found), 0 = no, a negative errno = the question could
// not be answered.
//
// PR #1 review (13th round, P1): that third answer is the whole point. This used to be a bool
// over three opendir(2)s whose failures were skipped in silence -- `if (!d) continue;` -- so a
// `snapshots/` that came back EACCES, EIO, or ENOENT-because-the-volume-is-not-mounted read as
// "nothing in there", the guard called the store empty, and the open built a fresh database
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

// ---- PR #1 review (35th round, P1): what is on disk, before anything is opened ---------------
//
// Two names and one VERSION file, and between them they say which step of the upgrade this store
// is owed. The protocol is
//
//     (a) VERSION := 3
//     (b) checkpoint the sidecars away and rename  metadata.db -> metadata3.db
//     (c) mkdir the stub at  metadata.db  (0500, empty)
//     (d) list who holds metadata3.db open; holders ? revert : migrate and stamp 3xx
//
// in that order, and every prefix of it is a state a crash can leave.
//
// PR #1 review (36th round, P1): (b) and (c) are one step now -- link, exchange, unlink -- so
// that `metadata.db` is never free for an M1 open to create a database at. That adds two
// intermediate shapes, and both are resumable:
//
//   metadata.db  metadata3.db   what it is                          what this open does
//   -----------  ------------   ---------------------------------   ---------------------------
//   absent       absent         a store with no database at all     trees ? DAMAGED : create
//   regular      absent         schema 2, or a crash before 2       (a) if VERSION 2, then 1-4, (d)
//   regular      SAME inode     a crash between 2 and 3             the same, and link() finds
//                               (one file, two names)               its own work already done
//   regular      other inode    a state the protocol cannot reach   DAMAGED
//   absent       present        a store whose stub was removed      (a) if VERSION 2, then the
//                               from under it                       stub, then (d)
//   directory    present        schema 3 -- or a crash before (d),  (a) if VERSION 2, then (d);
//                               or one between 3 and 4, which the   nlink > 1 also sweeps the
//                               link count gives away               extra link away
//   directory    absent         the stub with no database           DAMAGED
//   anything else at either name                                    DAMAGED
//
// The invariant, stated once: in every state this protocol can produce from a store that HAS a
// database, `metadata.db` EXISTS -- as the database, as a second link to it, or as the stub
// directory. The two rows above where it is absent are the store that has no database yet (a
// brand new one, where an M1 open would create nothing anybody wants either) and one whose stub
// somebody removed; neither is a window this core opens.
//
// VERSION is not in the table because it does not decide anything: the LAYOUT decides, and the
// file only says whether the bump in (a) is still owed. That settles the one direction the
// upgrade could otherwise be read two ways -- VERSION still 2 with `metadata3.db` already there,
// which is a revert that did not finish. It is taken FORWARD, always: the database has already
// moved, moving it back would be a second unsynchronised rename for no gain, and the bump is
// what makes the store's two halves agree again. The revert direction is only ever taken by the
// process that is holding the upgrade in its hand (legacy_holders_gate above), never by a later
// open reading these names.
//
// `*trees_checked` is the 13th round's guard, folded in: "the metadata is missing" now means
// neither a readable `metadata3.db` nor a readable `metadata.db` to upgrade, and a store that
// still has trees in it is refused rather than given a fresh database and a fresh store id.
struct StoreLayout {
    bool move_needed = false;    // the four steps of the move are still owed
    bool stub_needed = false;    // the stub is owed on its own: there is nothing to exchange it
                                 // with, because nothing at all is at the M1 name
    bool db_existed = false;     // metadata3.db was there before this open touched anything
    bool sweep_needed = false;   // an interrupted move may have left something under the stub
                                 // prefix: a stub it never exchanged, or a link it never dropped
    uint64_t db_ino = 0;         // metadata3.db's inode, so the sweep can tell that extra link
                                 // from a stranger's file under the same prefix
};

int store_layout(const char *dir, StoreLayout &lay) {
    String olddb, newdb;
    db_path(dir, olddb, true);
    db_path(dir, newdb, false);
    struct stat ost, nst;
    int orc = ::stat(olddb.c_str(), &ost) == 0 ? 0 : -errno;
    int nrc = ::stat(newdb.c_str(), &nst) == 0 ? 0 : -errno;
    // "I could not look" is neither presence nor absence (13th round): it ends the open with the
    // errno that caused it, before anything at all is created.
    if (orc && orc != -ENOENT) return orc;
    if (nrc && nrc != -ENOENT) return nrc;
    bool old_reg = orc == 0 && S_ISREG(ost.st_mode);
    bool old_dir = orc == 0 && S_ISDIR(ost.st_mode);
    if (orc == 0 && !old_reg && !old_dir) return WFS_E_STORE_DAMAGED;
    if (nrc == 0 && !S_ISREG(nst.st_mode)) return WFS_E_STORE_DAMAGED;
    // Both names on a regular file: the same inode under both is the crash between steps 2 and
    // 3 of the move and is resumed; two different inodes is a shape nothing in this protocol
    // writes, and is never written over (36th round).
    if (old_reg && nrc == 0 && (ost.st_dev != nst.st_dev || ost.st_ino != nst.st_ino))
        return WFS_E_STORE_DAMAGED;
    if (old_dir && nrc != 0) return WFS_E_STORE_DAMAGED;
    lay.move_needed = old_reg;
    lay.stub_needed = orc != 0;   // nothing at the M1 name at all: a mkdir is the whole of it
    lay.db_existed = nrc == 0;
    lay.db_ino = nrc == 0 ? (uint64_t)nst.st_ino : 0;
    // A second link to the database is one an interrupted move left under the stub prefix --
    // st_nlink is what says so without a readdir on the ordinary path, where there is none.
    lay.sweep_needed = old_reg || (nrc == 0 && nst.st_nlink > 1);

    // P17, over whichever of the two names the database is under at this moment. "Unreadable"
    // counts as missing, exactly as it did when there was one name: a database we cannot open is
    // one whose ids we do not know, and a zero-length file is a database SQLite would happily
    // start counting from 1 again.
    bool readable = (nrc == 0 && nst.st_size > 0 && ::access(newdb.c_str(), R_OK | W_OK) == 0) ||
                    (old_reg && ost.st_size > 0 && ::access(olddb.c_str(), R_OK | W_OK) == 0);
    if (!readable) {
        String what;
        int trees = store_has_trees(dir, what);
        if (trees < 0) return trees;
        if (trees) return WFS_E_STORE_DAMAGED;
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
    if (!t.ok()) return kGcFailCap;   // 34th round, P1: cannot count, so do not spin
    int64_t n = 0;
    {
        Stmt q(s->db, "SELECT value FROM meta WHERE key=?");
        if (!q.ok()) return kGcFailCap;
        q.text(1, key);
        // 32nd round, P2: a counter that could not be read is not a counter at zero -- that
        // would reset the cap on every failed read and put the collector back in a loop.
        if (q.row()) n = ::strtoll(q.col_text(0), nullptr, 10);
        else if (!q.done()) return kGcFailCap;
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
    if (t.commit()) return kGcFailCap;
    return n;
}

int64_t gc_fail_get(wfs_store *s, const char *key) {
    if (!s || !key || !*key) return kGcFailCap;   // cannot tell: treat it as spent, never spin
    Guard g(s->mu);
    Stmt q(s->db, "SELECT value FROM meta WHERE key=?");
    if (!q.ok()) return kGcFailCap;
    q.text(1, key);
    if (q.row()) return ::strtoll(q.col_text(0), nullptr, 10);
    return q.done() ? 0 : kGcFailCap;   // 32nd round, P2: cannot tell, so treat it as spent
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
        if (!q.row()) return;   // 32nd round, P2: no row and no answer both mean "clear nothing"
    }
    Txn t(s->db);
    if (!t.ok()) return;   // 34th round, P1: a counter that stayed costs one more wake
    Stmt d(s->db, "DELETE FROM meta WHERE key=?");
    if (d.ok()) { d.text(1, key); d.step(); }
    (void)t.commit();   // best effort: the same, one wake later
}

// ---- PR #1 review (28th round, P2): the directories the collector could not read -------------
//
// The 27th round put this on <store>/pool alone; it lives here now because the trash and the
// snapshots directory need the identical thing, and because the counter it keys on is the
// store's. See internal.h for what the silence used to cost.
String gc_dir_fail_key(const char *dir) {
    String k("gcfail:dir:");
    k.append(dir);
    return k;
}

void gc_note_unreadable(wfs_store *s, DirUnreadable *u, const char *dir, int err, bool collector,
                        int *work_remains) {
    if (!dir || !*dir) return;
    if (u) {
        u->count++;
        if (!u->err) {
            u->err = err;
            u->path.assign(dir);
        }
    }
    // `gc --status` and wfs_gc_pending() are counting passes: they report the failure and do not
    // write a byte to the database for it.
    if (!collector) return;
    if (gc_fail_bump(s, gc_dir_fail_key(dir).c_str()) < kGcFailCap && work_remains)
        *work_remains = 1;
}

void gc_note_readable(wfs_store *s, const char *dir, bool collector) {
    if (!collector || !dir || !*dir) return;
    gc_fail_clear(s, gc_dir_fail_key(dir).c_str());
}

bool gc_dirs_unreadable_pending(wfs_store *s) {
    if (!s) return false;
    Guard g(s->mu);
    // GLOB, not LIKE: it is the one of the two SQLite turns into a range over the primary key,
    // and this runs on the fork path. `value` is the failure count the cap is compared against,
    // so a directory that has failed kGcFailCap times stops waking workers -- what it is still
    // doing is being reported by every run and by `gc --status`, which is the whole contract of
    // the shared cap (internal.h).
    Stmt q(s->db, "SELECT 1 FROM meta WHERE key GLOB 'gcfail:dir:*'"
                  " AND CAST(value AS INTEGER) < ? LIMIT 1");
    if (!q.ok()) return false;   // cannot tell, and "cannot tell" never spawns a worker chain
    q.i64(1, kGcFailCap);
    // 32nd round, P2: same for a step that failed. Both answers are false here, and false is
    // the one that does nothing.
    return q.row();
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
    case WFS_E_STORE_DAMAGED: return "the store has trees in it but no readable metadata3.db";
    case WFS_E_STORE_BUSY:
        return "older clients still have the store open; stop them and retry";
    case WFS_E_TRASH_BLOCKED: return "a directory is in the way of this trash entry's deletion";
    case WFS_E_TRASH_FOREIGN:
        return "the directory at this trash entry's path is not the tree this record was written for";
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

// PR #1 review (34th round, P1): who else has this store's database open. The one caller is the
// CLI, after a WFS_E_STORE_BUSY refusal, so that the operator is told what to stop rather than
// left to find it. Read-only and stateless: it opens nothing and writes nothing.
//
// PR #1 review (35th round, P1): under whichever of the two names the database is at right now.
// The refusal this explains has already put the store back the way M1 left it -- the database is
// at `metadata.db` again -- so asking only about the schema-3 name would name nobody at all.
extern "C" int wfs_store_holders(const char *store_dir, wfs_store_holder *buf, size_t cap,
                                 size_t *count) {
    if (!store_dir || !count) return -EINVAL;
    *count = 0;
    String real;
    if (int rc = wfs::fs_realpath(store_dir, real)) return rc;
    String dbp;
    db_path(real.c_str(), dbp, false);
    struct stat dbst;
    if (::stat(dbp.c_str(), &dbst) != 0) db_path(real.c_str(), dbp, true);
    wfs::Vec<int64_t> pids;
    if (int rc = wfs::fs_other_holders(dbp.c_str(), pids)) return rc;
    *count = pids.size();
    for (size_t i = 0; buf && i < pids.size() && i < cap; ++i) {
        buf[i].pid = pids[i];
        String exe;
        wfs::fs_pid_exe(pids[i], exe);
        wfs::copy_str(buf[i].exe, sizeof buf[i].exe, exe.c_str());
    }
    return 0;
}

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
    // PR #1 review (13th round, P1): and "I could not look" counts as neither. A stat(2) that
    // fails for a reason other than ENOENT says nothing about whether the file is there, and a
    // subtree scan that could not run says nothing about whether the store is empty -- so both
    // of those end the open with the errno that caused them, before anything at all is created.
    // The file itself is never replaced either way: a database that is there but cannot be
    // opened fails in sqlite3_open_v2 below (SQLITE_OPEN_CREATE creates a database that is not
    // there, it does not truncate one that is).
    //
    // PR #1 review (35th round, P1): both of those questions are now asked of two names, and
    // the answer also says which step of the 2 -> 3 upgrade this store is owed. See
    // store_layout() above for the table.
    StoreLayout lay;
    if (int rc = store_layout(real.c_str(), lay)) return rc;

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
    // ---- PR #1 review (36th round, P1): the leftovers of an interrupted move ---------------
    //
    // A stub that was never exchanged, or an extra link that was never dropped. Both are ours,
    // both are under a prefix nothing else writes, and neither is in the way of anything --
    // they are swept here, before the upgrade, so that what the steps below find is the shape
    // the table above describes. On the ordinary path this does not even open the directory:
    // `sweep_needed` is false unless the M1 name is still a regular file or the database is
    // carrying a second link.
    if (lay.sweep_needed) db_stub_sweep(s->dir.c_str(), lay.db_ino);

    // ---- PR #1 review (32nd/34th/35th/36th rounds): the 2 -> 3 upgrade, in order ------------
    //
    // (a) VERSION := 3, (b) move the database -- link, exchange, unlink, which makes the stub
    // in the same step (36th round) -- (d) the holder gate, and only then is anything opened.
    // The 32nd round's reasoning for putting the file first is below; the 35th round's for
    // moving the database at all, and the 36th's for moving it this way, are on store_layout()
    // and db_move_to_schema3() above. The stamp is read here, from the database where it still
    // lies, because a store from a schema we do not know has to come back untouched and step
    // (b) would already have moved it.
    bool gated = false;
    if (lay.move_needed) {
        String olddb;
        db_path(s->dir.c_str(), olddb, true);
        int old_uv = 0;
        int mrc = db_user_version_at(olddb.c_str(), &old_uv);
        if (!mrc && old_uv / 100 > WFS_STORE_SCHEMA) { wfs_store_close(s); return WFS_E_SCHEMA; }
        if (!mrc && legacy_schema) {
            if (int vrc = version_upgrade(s->dir.c_str())) { wfs_store_close(s); return vrc; }
            if (wfs_test_after_version_bump)
                wfs_test_after_version_bump(wfs_test_after_version_bump_ctx, s->dir.c_str());
        }
        if (!mrc) mrc = db_move_to_schema3(s->dir.c_str());
        if (mrc) {
            // PR #1 review (31st round, P2, one step further along): the first open of a
            // schema-2 store is serialised by nothing, so two of them can be here at once and
            // only one rename can land. The loser finds a DIRECTORY where the database was and
            // fails every way there is to fail -- which is not an error in either process, it
            // is the move having already been made. So the layout is asked again, and only a
            // layout that still owes the move ends this open.
            StoreLayout again;
            if (store_layout(s->dir.c_str(), again) || again.move_needed) {
                wfs_store_close(s);
                return mrc;
            }
            lay = again;
        } else {
            // No db_stub_make() here any more: the exchange in step 3 put the stub at
            // `metadata.db` itself, which is the whole of the 36th round's finding.
            // ... and the door is only shut for those who were not already through it. See
            // legacy_holders_gate() above: this is the one open in a store's life that pays
            // for it.
            if (int hrc = legacy_holders_gate(s->dir.c_str())) { wfs_store_close(s); return hrc; }
            gated = true;
            lay.stub_needed = false;
            lay.db_existed = true;
            if (wfs_test_after_db_move)
                wfs_test_after_db_move(wfs_test_after_db_move_ctx, s->dir.c_str());
        }
    } else if (legacy_schema) {
        // The layout says the move has already happened -- or that there was never a database
        // to move -- so all this store is owed is the bump. Forward, always (store_layout()).
        if (int vrc = version_upgrade(s->dir.c_str())) { wfs_store_close(s); return vrc; }
        if (wfs_test_after_version_bump)
            wfs_test_after_version_bump(wfs_test_after_version_bump_ctx, s->dir.c_str());
    }

    String dbp;
    db_path(s->dir.c_str(), dbp, false);
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
    // PR #1 review (32nd round, P2): and it is READ, or this open ends. A PRAGMA that could
    // not be stepped used to leave `user_version` at 0 -- which is neither the refusal the
    // hundreds check owes a future schema nor a stamp this core wrote, and it would have sent
    // an unreadable database straight into the migration below.
    int user_version = 0;
    {
        Stmt q(s->db, "PRAGMA user_version");
        // A stamp that cannot be read is a database that cannot be read -- SQLITE_NOTADB comes
        // back here, from the step, for a file that is not one -- so it gets the verdict the
        // 13th round gave every other shape of that: WFS_E_STORE_DAMAGED, and nothing written.
        if (!q.ok() || !q.row()) { wfs_store_close(s); return WFS_E_STORE_DAMAGED; }
        user_version = (int)q.col_i64(0);
    }
    if (user_version / 100 > WFS_STORE_SCHEMA) { wfs_store_close(s); return WFS_E_SCHEMA; }

    // ---- PR #1 review (32nd round, P1): the file first, and only then the database ----------
    //
    // The 24th round bumped the schema so that an M1 binary would be refused this store; the
    // order in which the bump was applied gave it a window in which it was not. The upgrade ran
    //     check_version() -> migrate_schema() (committed, user_version 3xx) -> VERSION := 3
    // and M1 gates on the file alone, so an M1 process starting anywhere between the commit and
    // the rename read `2`, was admitted, and kept the handle it opened there for the rest of its
    // command -- against a database that is already M2's. What it does with it is worse than
    // reading it: M1's own open is
    //     if (user_version != WFS_STORE_SCHEMA) { ...kSchema...; PRAGMA user_version=2; }
    // with WFS_STORE_SCHEMA == 2, so the first thing it does to a store stamped 3xx is stamp it
    // back down to 2 -- and then it runs M1's wfs_gc() over M2 trash semantics: a `state=2`
    // snapshot row's tree is not one of the paths it protects, so a snapshot still inside its
    // retention window is deleted whole.
    //
    // So the bump comes first. From the instant the rename lands, M1's check_version() refuses
    // the store, and everything the migration then writes is written behind a closed door. The
    // two orders fail differently, which is the point:
    //   * old order, crash after the commit: VERSION says 2, the database says 3xx -- a store
    //     that every M1 binary may open and no M2 binary can tell from an M1 store;
    //   * new order, crash after the rename: VERSION says 3, the database says 2xx -- refused
    //     by M1, and finished by the next M2 open, because a 2xx stamp under a 3 file is read
    //     right here as "an upgrade that got half-way", not as a schema to refuse. The hundreds
    //     check above is what has to be careful about that, and is: only a stamp HIGHER than
    //     ours is refused, so 2xx-under-3 falls through to the migration below, which has been
    //     idempotent by column presence since the 11th round.
    // Nothing else changes: a store that is already 3 does not come through here at all, and a
    // migration that fails leaves a VERSION the older binary is refused by -- which costs it an
    // M1 store it could have collected, and is the safe half of that trade.
    //
    // PR #1 review (35th round, P1): the steps above ran before the open, and what is left here
    // is the crash states they can be resumed from. The stub is made good whenever there is
    // nothing at all at the M1 name -- which is also how a brand new store gets one, right
    // after its database is created (36th round: a store whose database is still at that name
    // gets its stub from the exchange instead, never from a mkdir into a name that was just
    // freed) -- and the holder gate is owed by exactly the stores whose database has moved but
    // whose migration has not committed: `user_version` still in the 2xx, on a file that was
    // already on disk when this open started. A store that is stamped 3xx does not pay
    // proc_listpidspath(3)'s ~100 ms, and a database this open created itself has no holder
    // that predates it.
    if (lay.stub_needed) {
        if (int src = db_stub_make(s->dir.c_str())) { wfs_store_close(s); return src; }
    }
    if (!gated && lay.db_existed && user_version / 100 <= WFS_STORE_SCHEMA_M1) {
        if (int hrc = legacy_holders_gate(s->dir.c_str())) { wfs_store_close(s); return hrc; }
    }
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
        // Belt and braces: the door this open migrated behind is asked, once, whether it is
        // really shut. The rename above said so; a VERSION that says anything else by now is a
        // store somebody is rewriting under us, and the one thing this open must not do is hand
        // back a migrated database that an M1 binary is still allowed to open.
        long v = 0;
        if (int vrc = version_read(s->dir.c_str(), &v)) { wfs_store_close(s); return vrc; }
        if (v != WFS_STORE_SCHEMA) { wfs_store_close(s); return WFS_E_SCHEMA; }
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
    //
    // PR #1 review (33rd round, P2): "has to happen" is a requirement, and a requirement whose
    // result is thrown away is a wish. The recovery's own queries fail the way every other read
    // in this core does (the 32nd round: a step that comes back SQLITE_IOERR, SQLITE_CORRUPT, a
    // SQLITE_BUSY that outlived the busy timeout), and it now reports that as -EIO instead of
    // resolving what it could not read. Dropping that rc handed the caller an open store with
    // TRASHING rows still in it -- and every reader that follows is entitled to assume there are
    // none: `gc --status` counts such a row's tree as a trash entry nothing may collect, the
    // orphan rule treats a tree whose row was never resolved as unclaimed, and `restore` reads a
    // state that is about to change. So the open ends here with the errno that ended the
    // recovery, the handle is closed rather than returned, and the user's next command runs the
    // recovery again -- the one thing it must not do is carry on over a store it could not put
    // back in order.
    //
    // SQLITE_BUSY is not a special case here: sqlite3_busy_timeout() is set to 10 s on this very
    // handle above, before anything is read, so another process's write lock is waited out
    // rather than returned -- by these SELECTs and by the BEGIN IMMEDIATE the resolution writes
    // under alike. What survives ten seconds of contention is not the transient a retry would
    // paper over, and a retry loop here would be a second busy timeout on top of the one SQLite
    // already runs. A store open is also the cheapest thing in this core to repeat.
    if (int rc = wfs::trashing_recover(s, nullptr, nullptr)) { wfs_store_close(s); return rc; }
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
        // PR #1 review (32nd round, P2): a count that could not be read is not a count of zero.
        // `status` is what an operator reads before deciding what to collect.
        if (!q.row()) return -EIO;
        out->snapshots = (uint64_t)q.col_i64(0);
        out->snapshot_entries = (uint64_t)q.col_i64(1);
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
        if (!q.done()) return -EIO;   // 32nd round, P2
    }
    {
        Stmt q(s->db, "SELECT COUNT(*), COALESCE(SUM(entries),0) FROM pool WHERE state=1");
        if (!q.ok() || !q.row()) return -EIO;   // 32nd round, P2
        out->pool_ready = (uint64_t)q.col_i64(0);
        out->pool_entries = (uint64_t)q.col_i64(1);
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
        if (!q.ok()) return -EIO;   // 32nd round, P2
        q.i64(1, WFS_ST_ACTIVE);
        struct stat st;
        while (q.row()) {
            const char *p = q.col_text(0);
            int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
            if (prc == 0 && S_ISDIR(st.st_mode)) continue;
            if (wfs::fs_gone(prc)) out->snapshots_dangling++;
            else out->snapshots_unreadable++;
        }
        if (!q.done()) return -EIO;
    }
    {
        Stmt q(s->db, "SELECT COUNT(*) FROM snapshots WHERE state=?");
        if (!q.ok()) return -EIO;   // 32nd round, P2
        q.i64(1, WFS_ST_TRASHED);
        if (!q.row()) return -EIO;
        out->snapshots_trashed = (uint64_t)q.col_i64(0);
    }
    {
        Stmt q(s->db, "SELECT path, dir_ino FROM worlds WHERE state=?");
        if (!q.ok()) return -EIO;   // 32nd round, P2
        q.i64(1, WFS_ST_ACTIVE);
        struct stat st;
        while (q.row()) {
            const char *p = q.col_text(0);
            int prc = *p ? wfs::fs_probe(p, &st, true) : -ENOENT;
            if (prc == 0 && S_ISDIR(st.st_mode)) continue;
            if (wfs::fs_gone(prc)) out->worlds_dangling++;
            else out->worlds_unreadable++;
        }
        if (!q.done()) return -EIO;
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
