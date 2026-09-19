// The store: a directory with a VERSION file, a SQLite metadata.db (schema v2), the snapshot
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

// Schema v2 (docs/M1_DESIGN.md §2). Snapshot ids and world ids are separate sequences, so
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
    "  state INTEGER NOT NULL DEFAULT 0);"   /* 0 = being cloned, 1 = ready */
    "CREATE INDEX IF NOT EXISTS pool_snap ON pool(snapshot_id, state);";

// Columns added after the first schema-2 stores were written. They are additive and carry
// defaults, so an older core reading such a store still works and VERSION does not change
// (P13 is about incompatible schemas, not about new columns). Run one statement per exec:
// each one fails harmlessly with "duplicate column name" once it has been applied, and a
// combined script would stop at the first of those.
const char *kMigrations[] = {
    "ALTER TABLE snapshots ADD COLUMN hard INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE snapshots ADD COLUMN root_mode INTEGER NOT NULL DEFAULT 0",
    // T2.2
    "ALTER TABLE snapshots ADD COLUMN trash_path TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE snapshots ADD COLUMN trashed_at INTEGER NOT NULL DEFAULT 0",
    // T2.5 (P9): the hardlink groups recorded in the snapshot's manifest. hl_groups is what a
    // fork checks to decide whether the manifest has to be read at all.
    "ALTER TABLE snapshots ADD COLUMN hl_groups INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE snapshots ADD COLUMN hl_external INTEGER NOT NULL DEFAULT 0",
    // PR #1 review, third round: where a fork in flight is building its clone. The name is
    // drawn, not derived, so the row is the only record of it.
    "ALTER TABLE worlds ADD COLUMN tmp_path TEXT NOT NULL DEFAULT ''",
};

// Additive revision of schema v2. `PRAGMA user_version` carries SCHEMA*100 + REV, so a store
// written before a column was added still gets the migrations run exactly once: the old code
// compared user_version against the schema number alone, which meant a store already stamped
// with 2 never saw a later ALTER TABLE. VERSION (and therefore P13) is untouched -- an older
// core opens such a store and simply does not use the new columns.
const int kSchemaRev = 3;
inline int user_version_want(void) { return WFS_STORE_SCHEMA * 100 + kSchemaRev; }

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
int check_version(const char *dir) {
    String p(dir);
    p.append("/VERSION");
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd >= 0) {
        char buf[64] = {0};
        ssize_t n = ::read(fd, buf, sizeof buf - 1);
        ::close(fd);
        if (n <= 0) return -EIO;
        long v = ::strtol(buf, nullptr, 10);
        return v == WFS_STORE_SCHEMA ? 0 : WFS_E_SCHEMA;
    }
    if (errno != ENOENT) return -errno;
    fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return errno == EEXIST ? check_version(dir) : -errno;
    char line[64];
    int n = ::snprintf(line, sizeof line, "%d\n", WFS_STORE_SCHEMA);
    ssize_t w = ::write(fd, line, (size_t)n);
    ::close(fd);
    return w == n ? 0 : -EIO;
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
bool store_has_trees(const char *dir, String &what) {
    for (const char *sub : {"/snapshots", "/trash", "/pool"}) {
        String p(dir);
        p.append(sub);
        DIR *d = ::opendir(p.c_str());
        if (!d) continue;
        bool found = false;
        while (struct dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
                continue;
            what.assign(sub + 1);
            what.append("/");
            what.append(e->d_name);
            found = true;
            break;
        }
        ::closedir(d);
        if (found) return true;
    }
    return false;
}

int meta_set(sqlite3 *db, const char *key, const char *value) {
    Stmt u(db, "INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!u.ok()) return -EIO;
    u.text(1, key);
    u.text(2, value);
    return u.step() == SQLITE_DONE ? 0 : -EIO;
}

} // namespace

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
    if (int rc = check_version(real.c_str())) return rc;

    // P17: never build a fresh database next to trees that the old one was the index of. Ids
    // restart at 1 when the database does, and the first `init` would then be handed S1 with
    // `snapshots/S1` already on disk -- so this is refused, loudly, before anything is created.
    // "Unreadable" counts as missing: a database we cannot open is one whose ids we do not know.
    {
        String dbp(real);
        dbp.append("/metadata.db");
        struct stat dbst;
        bool readable = ::stat(dbp.c_str(), &dbst) == 0 && S_ISREG(dbst.st_mode) &&
                        dbst.st_size > 0 && ::access(dbp.c_str(), R_OK | W_OK) == 0;
        String what;
        if (!readable && store_has_trees(real.c_str(), what)) return WFS_E_STORE_DAMAGED;
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
    if (sqlite3_exec(s->db, kPragmas, nullptr, nullptr, nullptr) != SQLITE_OK) {
        wfs_store_close(s);
        return WFS_E_STORE_DAMAGED;
    }
    int user_version = 0;
    {
        Stmt q(s->db, "PRAGMA user_version");
        if (q.ok() && q.row()) user_version = (int)q.col_i64(0);
    }
    if (user_version != user_version_want()) {
        if (sqlite3_exec(s->db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) { wfs_store_close(s); return -EIO; }
        for (const char *m : kMigrations) sqlite3_exec(s->db, m, nullptr, nullptr, nullptr);
        char pragma[64];
        ::snprintf(pragma, sizeof pragma, "PRAGMA user_version=%d", user_version_want());
        sqlite3_exec(s->db, pragma, nullptr, nullptr, nullptr);
    }
    if (meta_get(s->db, "store_id", s->store_id) != 0) {
        char id[33];
        hex_id(id, sizeof id);
        if (meta_set(s->db, "store_id", id) != 0) { wfs_store_close(s); return -EIO; }
        s->store_id.assign(id);
    }
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
    {
        Stmt q(s->db, "SELECT path FROM snapshots WHERE state=?");
        if (q.ok()) {
            q.i64(1, WFS_ST_ACTIVE);
            struct stat st;
            while (q.row())
                if (::stat(q.col_text(0), &st) != 0 || !S_ISDIR(st.st_mode)) out->snapshots_dangling++;
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
            while (q.row())
                if (::stat(q.col_text(0), &st) != 0 || !S_ISDIR(st.st_mode)) out->worlds_dangling++;
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
