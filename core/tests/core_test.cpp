// M1 core tests: snapshots, worlds and every safety rule the core owns.
// Plain asserts, libc only, so this runs anywhere with a C++ compiler (arch.md §39).
//
// Each block is labelled with the rule from docs/M1_DESIGN.md §3 that it pins down.
#include "worldfs/worldfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
// PR #1 review (11th round): the schema-migration block at the end of main() has to build a
// database as schema 2 first wrote it -- older than anything this library can produce any more.
// worldfs_core links SQLite publicly, so this is the same library the core itself opens.
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
// PR #1 review (20th round, P2): a tree the remover genuinely cannot remove. Mode bits are no
// use -- rm_rec() chmods its way in and fs_unprotect_tree() clears chflags, because it is
// deleting the thing -- so this is the ACL scripts/tests/safety.sh uses for the same purpose,
// applied through the API instead of through chmod(1).
#include <membership.h>
#include <sys/acl.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CHECK_OK(x) do { int _rc = (x); if (_rc != 0) { fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc)); exit(1); } } while (0)
#define CHECK_RC(x, want) do { int _rc = (x); if (_rc != (want)) { fprintf(stderr, "%s:%d: %s -> %d (%s), wanted %d\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc), (want)); exit(1); } } while (0)

static void write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    CHECK(f);
    fputs(s, f);
    fclose(f);
}

static int read_file(const char *p, char *buf, size_t cap) {
    FILE *f = fopen(p, "r");
    if (!f) return -errno;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return 0;
}

static void join(char *out, size_t cap, const char *a, const char *b) { snprintf(out, cap, "%s/%s", a, b); }

// PR #1 review (6th round): a snapshot's manifest, kept aside and put back. Byte for byte, so
// the restored snapshot is the one that was made, not one this test rewrote.
static void copy_file(const char *from, const char *to) {
    FILE *i = fopen(from, "r");
    CHECK(i);
    FILE *o = fopen(to, "w");
    CHECK(o);
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof b, i)) > 0) CHECK(fwrite(b, 1, n, o) == n);
    fclose(i);
    CHECK(fclose(o) == 0);
}

// The damage: every `hl ` line out of a manifest, nothing else touched. That is a manifest that
// reads fine and holds fewer hardlink groups than the snapshot row claims -- what a truncated
// write, or a manifest from a store somebody has been editing, looks like.
static void strip_hl_lines(const char *manifest) {
    FILE *i = fopen(manifest, "r");
    CHECK(i);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.stripped", manifest);
    FILE *o = fopen(tmp, "w");
    CHECK(o);
    char line[8192];
    while (fgets(line, sizeof line, i))
        if (strncmp(line, "hl ", 3) != 0) fputs(line, o);
    fclose(i);
    CHECK(fclose(o) == 0);
    CHECK(rename(tmp, manifest) == 0);
}

// PR #1 review (8th round): three ways a manifest's hardlink section stops describing itself.
// The first is what a truncated write leaves -- the last member never reached the disk -- and it
// is the one that used to fork fine, with two independent files where the snapshot records one
// inode under two names.
static void rewrite_manifest(const char *manifest, int drop_last, int drop_header,
                             unsigned long long header_groups) {
    char lines[256][8192];
    size_t n = 0;
    FILE *i = fopen(manifest, "r");
    CHECK(i);
    while (n < 256 && fgets(lines[n], sizeof lines[n], i)) n++;
    fclose(i);
    if (drop_last) { CHECK(n > 0); n--; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.edit", manifest);
    FILE *o = fopen(tmp, "w");
    CHECK(o);
    for (size_t k = 0; k < n; ++k) {
        if (!strncmp(lines[k], "#hl ", 4)) {
            if (drop_header) continue;
            if (header_groups) {
                unsigned long long ver = 0, gs = 0, ns = 0, eg = 0, en = 0;
                CHECK(sscanf(lines[k] + 3, " %llu %llu %llu %llu %llu", &ver, &gs, &ns, &eg, &en) == 5);
                fprintf(o, "#hl %llu %llu %llu %llu %llu\n", ver, header_groups, ns, eg, en);
                continue;
            }
        }
        fputs(lines[k], o);
    }
    CHECK(fclose(o) == 0);
    CHECK(rename(tmp, manifest) == 0);
}

// PR #1 review (9th round): one `hl` line moved into the neighbouring group. The group count and
// the name count are untouched, so the `#hl` header still agrees with the section -- what does
// not agree any more is each group's member count with the nlink it declares. `which` is the
// index among the `hl ` lines; `gid` is the group id to write on it.
static void retag_hl_line(const char *manifest, size_t which, unsigned long long gid) {
    char lines[256][8192];
    size_t n = 0, seen = 0;
    FILE *i = fopen(manifest, "r");
    CHECK(i);
    while (n < 256 && fgets(lines[n], sizeof lines[n], i)) n++;
    fclose(i);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.retag", manifest);
    FILE *o = fopen(tmp, "w");
    CHECK(o);
    int done = 0;
    for (size_t k = 0; k < n; ++k) {
        if (!strncmp(lines[k], "hl ", 3)) {
            if (seen++ == which) {
                unsigned long long g = 0, nl = 0;
                int used = 0;
                // PR #1 review (19th round): `%llu%n` and one explicit separator, the same way
                // the reader now parses these lines -- a trailing whitespace directive would
                // move a name that begins with a space, not just re-tag it.
                CHECK(sscanf(lines[k] + 2, " %llu %llu%n", &g, &nl, &used) == 2 && used);
                CHECK(lines[k][2 + used] == ' ');
                fprintf(o, "hl %llu %llu%s", gid, nl, lines[k] + 2 + used);
                done = 1;
                continue;
            }
        }
        fputs(lines[k], o);
    }
    CHECK(done);
    CHECK(fclose(o) == 0);
    CHECK(rename(tmp, manifest) == 0);
}

// PR #1 review (11th round): the path of the `which`-th `hl` line, replaced. Group number,
// nlink and the number of lines all stay exactly as they were, so every count the manifest
// checks look at still agrees -- which is the whole point of this damage shape.
static void repath_hl_line(const char *manifest, size_t which, const char *path) {
    char lines[256][8192];
    size_t n = 0, seen = 0;
    FILE *i = fopen(manifest, "r");
    CHECK(i);
    while (n < 256 && fgets(lines[n], sizeof lines[n], i)) n++;
    fclose(i);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.repath", manifest);
    FILE *o = fopen(tmp, "w");
    CHECK(o);
    int done = 0;
    for (size_t k = 0; k < n; ++k) {
        if (!strncmp(lines[k], "hl ", 3) && seen++ == which) {
            unsigned long long g = 0, nl = 0;
            int used = 0;
            CHECK(sscanf(lines[k] + 2, " %llu %llu%n", &g, &nl, &used) == 2 && used);
            fprintf(o, "hl %llu %llu %s\n", g, nl, path);
            done = 1;
            continue;
        }
        fputs(lines[k], o);
    }
    CHECK(done);
    CHECK(fclose(o) == 0);
    CHECK(rename(tmp, manifest) == 0);
}

// PR #1 review (13th round): the paths of two `hl` lines exchanged. Nothing else moves -- the
// group ids, the nlinks, the line order and the header are all exactly as the snapshot wrote
// them -- so when the two lines belong to different groups, every structural check the reader
// makes still passes and each group now names members of two different inodes.
static void hl_line_path(const char *manifest, size_t which, char *out, size_t cap) {
    char line[8192];
    size_t seen = 0;
    FILE *f = fopen(manifest, "r");
    CHECK(f);
    out[0] = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "hl ", 3) || seen++ != which) continue;
        unsigned long long g = 0, nl = 0;
        int used = 0;
        CHECK(sscanf(line + 2, " %llu %llu%n", &g, &nl, &used) == 2 && used);
        CHECK(line[2 + used] == ' ');
        snprintf(out, cap, "%s", line + 2 + used + 1);
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
        break;
    }
    fclose(f);
    CHECK(out[0]);
}

static void swap_hl_paths(const char *manifest, size_t i, size_t j) {
    char pi[4096], pj[4096];
    hl_line_path(manifest, i, pi, sizeof pi);
    hl_line_path(manifest, j, pj, sizeof pj);
    repath_hl_line(manifest, i, pj);
    repath_hl_line(manifest, j, pi);
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

// PR #1 review (17th round, P2): one string value of a world's `.world` marker, rewritten. The
// marker is the small JSON marker_text() writes, and what a relocated store or a stale marker
// leaves behind is exactly this: one key with another store's path in it.
static void marker_set_str(const char *world_root, const char *key, const char *value) {
    char mp[4096], buf[8192], out[8192], pat[128];
    join(mp, sizeof mp, world_root, ".world");
    CHECK_OK(read_file(mp, buf, sizeof buf));
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    char *at = strstr(buf, pat);
    CHECK(at);
    char *val = at + strlen(pat);
    char *end = strchr(val, '"');
    CHECK(end);
    size_t head = (size_t)(val - buf);
    memcpy(out, buf, head);
    snprintf(out + head, sizeof out - head, "%s%s", value, end);
    write_file(mp, out);
}

// ---- PR #1 review (11th round): a database of the shape schema 2 had before the ALTERs -------
static void db_exec(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "db_exec(%s): %s\n", path, err ? err : "?");
    CHECK(rc == SQLITE_OK);
    sqlite3_close(db);
}

static int db_user_version(const char *path) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    int v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

// PR #1 review (21st round, P1): what the store's one pool row really says its state is.
// POOL_DRAINING is internal to the core (core/src/pool.h), so the row itself is the evidence
// that a drain which could not remove a tree left it in a state no claim can match.
static int db_pool_state(const char *store_dir) {
    char dbp[4096];
    snprintf(dbp, sizeof dbp, "%s/metadata.db", store_dir);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open_v2(dbp, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT state FROM pool ORDER BY id", -1, &st, NULL) == SQLITE_OK);
    int v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

static int db_has_column(const char *path, const char *table, const char *column) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    int found = 0;
    while (!found && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *n = sqlite3_column_text(st, 1);
        if (n && !strcmp((const char *)n, column)) found = 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

// PR #1 review (18th round, P2): one text column of the store's own database. `tmp_path` is a
// column no public struct carries, and it is the whole of what the fix writes down.
static int db_query_text(const char *path, const char *sql, char *out, size_t cap) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    int rows = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        snprintf(out, cap, "%s", t ? (const char *)t : "");
        rows = 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rows;
}

// The v2 tables as they were before any of the ALTER TABLE migrations: no `hard`, `root_mode`,
// `trash_path`, `trashed_at`, `hl_groups`, `hl_external` on snapshots, no `tmp_path` and no
// owner columns anywhere. `snapshots_as_view` builds the same thing with `snapshots` as a view
// over a real table: `CREATE TABLE IF NOT EXISTS snapshots` is then a silent no-op and the very
// first migration comes back "Cannot add a column to a view" while the `PRAGMA user_version`
// write after it succeeds -- which is exactly the shape an EIO, a full disk or a SQLITE_BUSY
// that outlives the busy timeout has, and the only one of them a test can produce on demand.
static void make_v2_db(const char *path, int snapshots_as_view) {
    char wal[4096], shm[4096];
    snprintf(wal, sizeof wal, "%s-wal", path);
    snprintf(shm, sizeof shm, "%s-shm", path);
    unlink(path);
    unlink(wal);
    unlink(shm);
    static const char *kSnapCols =
        "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL DEFAULT '',"
        " path TEXT NOT NULL DEFAULT '', src_path TEXT NOT NULL DEFAULT '',"
        " from_world INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL,"
        " entries INTEGER NOT NULL DEFAULT 0, hardlinks INTEGER NOT NULL DEFAULT 0,"
        " state INTEGER NOT NULL DEFAULT 0";
    char sql[4096];
    snprintf(sql, sizeof sql,
             "PRAGMA journal_mode=WAL;"
             "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
             "CREATE TABLE %s(%s);"
             "%s"
             "CREATE TABLE worlds(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " kind INTEGER NOT NULL DEFAULT 1, parent_world INTEGER NOT NULL DEFAULT 0,"
             " snapshot_id INTEGER NOT NULL DEFAULT 0, name TEXT NOT NULL DEFAULT '',"
             " path TEXT NOT NULL DEFAULT '', trash_path TEXT NOT NULL DEFAULT '',"
             " dir_dev INTEGER NOT NULL DEFAULT 0, dir_ino INTEGER NOT NULL DEFAULT 0,"
             " state INTEGER NOT NULL DEFAULT 0, fsevents_id INTEGER NOT NULL DEFAULT 0,"
             " entries INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL,"
             " trashed_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE TABLE pool(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " snapshot_id INTEGER NOT NULL, snap_created_at INTEGER NOT NULL,"
             " uuid TEXT NOT NULL DEFAULT '', path TEXT NOT NULL DEFAULT '',"
             " entries INTEGER NOT NULL DEFAULT 0, root_mode INTEGER NOT NULL DEFAULT 0,"
             " root_mtime INTEGER NOT NULL DEFAULT 0, dir_dev INTEGER NOT NULL DEFAULT 0,"
             " dir_ino INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL,"
             " state INTEGER NOT NULL DEFAULT 0);"
             "PRAGMA user_version=%d;",
             snapshots_as_view ? "snapshots_real" : "snapshots", kSnapCols,
             snapshots_as_view ? "CREATE VIEW snapshots AS SELECT * FROM snapshots_real;" : "",
             WFS_STORE_SCHEMA_M1 * 100);
    db_exec(path, sql);
}

// The 13 columns the migrations add. PR #1 review (24th round): hoisted out of the 11th-round
// block, because the 24th-round block below asks the same question of the same store -- a
// schema-2 database is exactly an M1 store, and what the bump must NOT cost it is its columns.
static const char *kAdded[][2] = {
    {"snapshots", "hard"},       {"snapshots", "root_mode"},
    {"snapshots", "trash_path"}, {"snapshots", "trashed_at"},
    {"snapshots", "hl_groups"},  {"snapshots", "hl_external"},
    {"snapshots", "owner_pid"},  {"snapshots", "owner_start"},
    {"worlds", "tmp_path"},      {"worlds", "owner_pid"},
    {"worlds", "owner_start"},   {"pool", "owner_pid"},
    {"pool", "owner_start"},
};
static const size_t kAddedN = sizeof kAdded / sizeof kAdded[0];

// P9 (T2.5): two names are the same file when they are the same inode, and a group of n names
// shows n links on every one of them.
static uint64_t ino_of(const char *p) { struct stat st; CHECK(lstat(p, &st) == 0); return (uint64_t)st.st_ino; }
static uint64_t nlink_of(const char *p) { struct stat st; CHECK(lstat(p, &st) == 0); return (uint64_t)st.st_nlink; }

// PR #1 review (P1): how many entries of a directory start with `prefix`. Used to assert that
// the hardlink replay left none of its own temporaries behind.
static size_t n_with_prefix(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0, pl = strlen(prefix);
    while (struct dirent *e = readdir(d))
        if (!strncmp(e->d_name, prefix, pl)) n++;
    closedir(d);
    return n;
}

// Only ever called on paths under this test's own mkdtemp root.
static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    lchflags(path, 0);
    if (S_ISDIR(st.st_mode)) {
        chmod(path, 0755);
        DIR *d = opendir(path);
        if (d) {
            while (struct dirent *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[4096];
                join(child, sizeof child, path, e->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

// Lists the entries of a pool directory: names and inodes, so a hand-out can be checked to be
// the very tree that was waiting there (T1.5).
static size_t list_dir(const char *dir, char names[][256], uint64_t *inos, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (n >= cap) break;
        snprintf(names[n], 256, "%s", e->d_name);
        char full[4096];
        join(full, sizeof full, dir, e->d_name);
        struct stat st;
        inos[n] = (stat(full, &st) == 0) ? (uint64_t)st.st_ino : 0;
        ++n;
    }
    closedir(d);
    return n;
}

// PR #1 review (23rd round, P2): the seam that makes one directory of a fork's clone unreadable
// in the instant before the hardlink replay walks it -- a member lookup that fails without the
// member being absent, which is the one thing the replay must not read as absence.
static void hl_shut_dir(void *ctx, wfs_id world, const char *tmp_path) {
    (void)ctx;
    (void)world;
    char p[4096];
    snprintf(p, sizeof p, "%s/d", tmp_path);
    CHECK(chmod(p, 0000) == 0);
}

// PR #1 review (P1): the test seam that puts this test inside a pool-backed fork, between the
// claim and the moment the world becomes visible. `wfs_test_after_pool_claim` calls this there.
static wfs_store *g_race_store;
static wfs_id g_race_snap;
static int g_race_rc, g_race_ran;
static void race_discard(void *ctx, wfs_id world) {
    (void)ctx;
    (void)world;
    g_race_ran = 1;
    g_race_rc = wfs_snapshot_discard(g_race_store, g_race_snap, 0, 0);
}

// PR #1 review (3rd round): the seam that stops a fork dead between the recorded clone and the
// publish rename, and remembers what it was about to publish. Returning non-zero unwinds
// nothing, so the CREATING row and the tree it names survive exactly as a `kill -9` leaves them.
struct CrashSeen {
    wfs_id world;
    char tmp[4096];
};
static CrashSeen crash_seen;
static int crash_before_publish(void *ctx, wfs_id world, const char *tmp_path) {
    CrashSeen *c = (CrashSeen *)ctx;
    c->world = world;
    snprintf(c->tmp, sizeof c->tmp, "%s", tmp_path);
    return -EINTR;
}

// PR #1 review (20th round, P2): an owner-deny ACL on a directory -- delete and delete_child --
// which survives every chmod and chflags the remover does on its way in. Undone by dropping the
// extended ACL again.
static void deny_delete(const char *path) {
    acl_t acl = acl_init(1);
    CHECK(acl != NULL);
    acl_entry_t e;
    CHECK(acl_create_entry(&acl, &e) == 0);
    CHECK(acl_set_tag_type(e, ACL_EXTENDED_DENY) == 0);
    acl_permset_t ps;
    CHECK(acl_get_permset(e, &ps) == 0);
    CHECK(acl_add_perm(ps, ACL_DELETE) == 0);
    CHECK(acl_add_perm(ps, ACL_DELETE_CHILD) == 0);
    CHECK(acl_set_permset(e, ps) == 0);
    uuid_t uu;
    CHECK(mbr_uid_to_uuid(getuid(), uu) == 0);
    CHECK(acl_set_qualifier(e, uu) == 0);
    CHECK(acl_set_link_np(path, ACL_TYPE_EXTENDED, acl) == 0);
    acl_free(acl);
}
static void allow_delete(const char *path) {
    acl_t empty = acl_init(0);   // an ACL with no entries removes the extended ACL
    CHECK(empty != NULL);
    CHECK(acl_set_link_np(path, ACL_TYPE_EXTENDED, empty) == 0);
    acl_free(empty);
}

// ... and the fork failure that runs the ORDINARY path's unwind with the temp tree on disk. The
// seam fires with the clone made, recorded and marked, so this locks a directory inside the
// clone (the unwind's removal will fail on it) and then makes the publish rename fail the way
// P7 says it must: a directory that appeared at --to since check_path looked. Returning 0 --
// unlike crash_before_publish above, which is a `kill -9` and unwinds nothing.
static char u20_target[4096];
static char u20_tmp[4096];
static char u20_locked[4096];
static wfs_id u20_world;
static int fork_fail_at_publish(void *ctx, wfs_id world, const char *tmp_path) {
    (void)ctx;
    u20_world = world;
    snprintf(u20_tmp, sizeof u20_tmp, "%s", tmp_path);
    snprintf(u20_locked, sizeof u20_locked, "%s/keep", tmp_path);
    CHECK(mkdir(u20_locked, 0755) == 0);
    char f[4096];
    snprintf(f, sizeof f, "%s/f.txt", u20_locked);
    write_file(f, "half a clone\n");
    deny_delete(u20_locked);
    CHECK(mkdir(u20_target, 0755) == 0);
    return 0;
}

// PR #1 review (16th round, P2): the two halves of "a pool-backed fork that fails after the
// claim". The first makes the hand-out fail the way P7 says it must -- a directory appears at
// --to after check_path looked, so the publish rename is EEXIST -- and the second runs a whole
// `discard S<n>` inside the unwind, at the instant the entry is out of the pool.
static char unwind_target[4096];
static void unwind_block_target(void *ctx, wfs_id world) {
    (void)ctx;
    (void)world;
    mkdir(unwind_target, 0755);
}
static wfs_store *g_unwind_store;
static wfs_id g_unwind_snap;
static int g_unwind_rc, g_unwind_ran;
static void unwind_discard(void *ctx) {
    (void)ctx;
    g_unwind_ran = 1;
    g_unwind_rc = wfs_snapshot_discard(g_unwind_store, g_unwind_snap, 0, 0);
}

// PR #1 review (18th round, P2): the seam that fires with a pool entry's clone already at the
// user's --to and no row owning it there yet. Moving it away makes the stat that follows fail,
// and -- the point of the whole thing -- makes the unwind's rollback rename fail too.
static char publish_target[4096];
static char publish_moved[4096];
static wfs_id g_publish_world;
static int g_publish_ran;
static void publish_move_away(void *ctx, wfs_id world, const char *target) {
    (void)ctx;
    (void)target;
    g_publish_ran = 1;
    g_publish_world = world;
    CHECK(rename(publish_target, publish_moved) == 0);
}

// And the same failure with the tree left where the hand-out put it: the directory --to lives in
// is shut, so the stat of the published tree and the rollback rename both come back EACCES.
static char publish_shut[4096];
static void publish_shut_parent(void *ctx, wfs_id world, const char *target) {
    (void)ctx;
    (void)target;
    g_publish_ran = 1;
    g_publish_world = world;
    CHECK(chmod(publish_shut, 0) == 0);
}

// PR #1 review (5th round): the source of a snapshot is a live directory, and this is what a
// user writing to it in the window between the walk and the clone looks like at its worst -- one
// member of a hardlink group replaced by a different file of exactly the same size and exactly
// the same mtime, which is the pair restore_group() uses to tell "still the file the scan saw"
// from "not any more".
static const char *g_swap_path;
static const char *g_swap_peer;
static int g_swap_hits;
static void swap_member(void *ctx, const char *src_dir) {
    (void)ctx;
    (void)src_dir;
    if (!g_swap_path) return;
    struct stat st;
    CHECK(lstat(g_swap_peer, &st) == 0);
    CHECK(unlink(g_swap_path) == 0);
    write_file(g_swap_path, "BBBB\n");
    struct timespec ts[2];
#ifdef __APPLE__
    ts[0] = st.st_atimespec;
    ts[1] = st.st_mtimespec;
#else
    ts[0] = st.st_atim;
    ts[1] = st.st_mtim;
#endif
    CHECK(utimensat(AT_FDCWD, g_swap_path, ts, 0) == 0);
    g_swap_hits++;
}

// PR #1 review (5th round): the two halves of a discard. Phase 0 is the row committed in
// TRASHING with the tree still at home, phase 1 is the tree renamed with the row not yet
// TRASHED. Returning non-zero is a `kill -9` right there: nothing is unwound.
static int g_trash_crash_phase = -1;
static int g_trash_crash_hits;
static char g_trash_crash_path[4096];
static int trash_crash(void *ctx, int phase, int is_snapshot, wfs_id id, const char *trash_path) {
    (void)ctx;
    (void)is_snapshot;
    (void)id;
    if (phase != g_trash_crash_phase) return 0;
    g_trash_crash_hits++;
    snprintf(g_trash_crash_path, sizeof g_trash_crash_path, "%s", trash_path ? trash_path : "");
    return -EINTR;
}

// PR #1 review (8th round): a TRASHING row records the process that is moving the tree -- the
// pid and that process's own start time -- and the recovery leaves such a row alone for as long
// as that process is alive: it is an operation in flight, not a crash. The seam's `kill -9`
// happens inside a test process that goes on running, so the crash cases have to record an owner
// that really is gone. wfs_test_fork_owner_pid is what puts one there: the same pid that does
// not exist as every other abandoned-producer case in this file uses.
static void crash_at(int phase) {
    g_trash_crash_phase = phase;
    g_trash_crash_hits = 0;
    wfs_test_fork_owner_pid = phase < 0 ? 0 : 2147480000;
}

// PR #1 review (7th round): the window `restore W<n>` has between the row it commits (phase 2,
// the world TRASHING, the tree still in the trash) and the rename that brings the tree home.
// What has to happen inside that window is a whole `discard S<n>`, run to completion -- and then
// the restore carries on, which is why this returns 0 rather than the seam's usual -EINTR.
static int restore_race(void *ctx, int phase, int is_snapshot, wfs_id id, const char *trash_path) {
    (void)ctx;
    (void)is_snapshot;
    (void)id;
    if (phase != 2) return 0;
    g_race_ran = 1;
    snprintf(g_trash_crash_path, sizeof g_trash_crash_path, "%s", trash_path ? trash_path : "");
    g_race_rc = wfs_snapshot_discard(g_race_store, g_race_snap, 0, 0);
    return 0;
}

// PR #1 review (8th round, P1): a second process, opening the store in the middle of a live
// `discard` or `restore`. Every `world fs ...` invocation opens the store, and the open runs the
// TRASHING recovery; this seam is that other process, running at the exact point where the row
// is committed and the tree has not moved yet. It must change nothing: the row belongs to an
// operation that is still running. Returning 0 lets our own operation carry on afterwards.
static char g_own_dir[4096];
static int g_own_phase = -1;
static int g_own_ran;
static int g_own_state = -1;
static int owner_race(void *ctx, int phase, int is_snapshot, wfs_id id, const char *trash_path) {
    (void)ctx;
    if (phase != g_own_phase) return 0;
    g_own_ran++;
    snprintf(g_trash_crash_path, sizeof g_trash_crash_path, "%s", trash_path ? trash_path : "");
    wfs_store *b = NULL;
    CHECK_OK(wfs_store_open(g_own_dir, &b));       // the open recovers
    wfs_gc_report brep;
    memset(&brep, 0, sizeof brep);
    CHECK_OK(wfs_gc(b, 0, &brep));                 // and so does gc, with nothing held back
    if (is_snapshot) {
        wfs_snapshot_rec sr;
        CHECK_OK(wfs_snapshot_info(b, id, &sr));
        g_own_state = sr.state;
    } else {
        wfs_world_rec wr2;
        CHECK_OK(wfs_world_info(b, id, &wr2));
        g_own_state = wr2.state;
    }
    wfs_store_close(b);
    return 0;
}

// PR #1 review (8th round, P1): the trash collector's own race. The seam runs between the scan
// that queued this entry and the rename that starts deleting it, and what it does in there is a
// whole `restore` of the row the collector is holding -- on a different store handle, the way a
// different process would.
static wfs_store *g_del_store = NULL;
static wfs_id g_del_world;
static int g_del_ran;
static int g_del_rc = -1;
static void restore_before_delete(void *ctx, int is_snapshot, wfs_id row, const char *path) {
    (void)ctx;
    (void)path;
    if (is_snapshot || row != g_del_world) return;
    g_del_ran++;
    g_del_rc = wfs_world_restore(g_del_store, g_del_world);
}

// PR #1 review (15th round, P1): the same window, entered by a restore that stops in the middle
// of itself -- row committed in WFS_ST_TRASHING, tree still sitting in the trash under its own
// name. That is the state the collector's claim has to lose to: the rename it is about to make
// would succeed (the tree is exactly where the job says it is) while the row that authorised it
// has stopped being TRASHED. The restoring process dies there (crash_at(2) records an owner that
// is not running), because the disk state between a restore's row and its rename is the same
// whether that process carries on or not, and both endings need the tree.
static wfs_store *g_half_store = NULL;
static wfs_id g_half_world;
static int g_half_ran;
static int g_half_rc;
static char g_half_path[4096];
static void restore_half_before_delete(void *ctx, int is_snapshot, wfs_id row, const char *path) {
    (void)ctx;
    if (is_snapshot || row != g_half_world) return;
    g_half_ran++;
    snprintf(g_half_path, sizeof g_half_path, "%s", path ? path : "");
    wfs_test_trash_crash = trash_crash;
    crash_at(2);
    g_half_rc = wfs_world_restore(g_half_store, g_half_world);
    crash_at(-1);
    wfs_test_trash_crash = NULL;
}

// PR #1 review (15th round, P1): and the mirror order -- the restore's row first, the collector's
// claim second. A whole `wfs_gc` on a second handle, run inside the restore's window; it must
// leave the tree exactly where it is, and the restore then carries on to its end, which is why
// this returns 0 rather than the seam's usual -EINTR.
static wfs_store *g_mirror_store = NULL;
static int g_mirror_ran;
static int g_mirror_rc = -1;
static wfs_gc_report g_mirror_rep;
static int gc_inside_restore(void *ctx, int phase, int is_snapshot, wfs_id id,
                             const char *trash_path) {
    (void)ctx;
    (void)is_snapshot;
    (void)id;
    if (phase != 2) return 0;
    g_mirror_ran++;
    snprintf(g_trash_crash_path, sizeof g_trash_crash_path, "%s", trash_path ? trash_path : "");
    memset(&g_mirror_rep, 0, sizeof g_mirror_rep);
    g_mirror_rc = wfs_gc(g_mirror_store, 0, &g_mirror_rep);
    return 0;
}

// PR #1 review (9th round, P1): reconciliation's window. The seam runs between the scan that
// found an ACTIVE row with no tree at its recorded path and the update that buries it; what it
// does in there is `world fs verify <the new path>`, which is how a world that was merely moved
// teaches its row where it went.
static wfs_store *g_rec_store = NULL;
static const char *g_rec_path;
static int g_rec_ran;
static int g_rec_rc = -1;
static void verify_before_reconcile(void *ctx) {
    (void)ctx;
    if (g_rec_ran) return;
    g_rec_ran++;
    wfs_identity id;
    g_rec_rc = wfs_world_verify_identity(g_rec_store, g_rec_path, &id);
    if (!g_rec_rc && !id.moved) g_rec_rc = -EINVAL;
}

// PR #1 review (9th round, P1): the pool collector's scan window. The seam runs after the pool
// rows have been read and before anything is removed; what it does in there is a whole
// `pool fill` and a whole pool-backed `fork`, both on a second handle. The fill's entry is a
// tree the row snapshot has never heard of, which is exactly what the orphan sweep used to
// delete; the fork's is a tree that changes hands in the same window.
static wfs_store *g_pool_store = NULL;
static wfs_id g_pool_snap;
static const char *g_pool_target;
static int g_pool_ran;
static int g_pool_rc = -1;
static wfs_fork_result g_pool_fr;
static void fill_before_sweep(void *ctx) {
    (void)ctx;
    if (g_pool_ran) return;
    g_pool_ran++;
    wfs_fork_opts fo;
    memset(&fo, 0, sizeof fo);
    fo.name = "poolrace";
    memset(&g_pool_fr, 0, sizeof g_pool_fr);
    g_pool_rc = wfs_world_create_ex(g_pool_store, (wfs_ref){WFS_K_SNAPSHOT, g_pool_snap},
                                    g_pool_target, &fo, &g_pool_fr);
    if (g_pool_rc) return;
    uint64_t made = 0;
    g_pool_rc = wfs_pool_fill(g_pool_store, g_pool_snap, 1, &made);
    if (!g_pool_rc && made != 1) g_pool_rc = -EINVAL;
}

// PR #1 review (15th round, P2): the filler's own window. The seam runs after wfs_pool_fill has
// read the snapshot row and before the transaction that inserts an entry's CREATING pool row;
// what it does in there is a `discard S<n>` on a second handle, stopped (phase 0) at the point
// where the row is committed in TRASHING and the tree has not moved yet -- so the clone that
// follows would still succeed, and the entry would be published READY for a snapshot on its way
// to the trash.
static wfs_store *g_pins_store = NULL;
static wfs_id g_pins_snap;
static int g_pins_ran;
static int g_pins_rc;
static void discard_before_pool_insert(void *ctx) {
    (void)ctx;
    if (g_pins_ran) return;
    g_pins_ran++;
    wfs_test_trash_crash = trash_crash;
    crash_at(0);
    g_pins_rc = wfs_snapshot_discard(g_pins_store, g_pins_snap, 0, 0);
    crash_at(-1);
    wfs_test_trash_crash = NULL;
}

// PR #1 review (21st round, P1): the drain's own window. Phase 0 is before the transaction that
// takes the row out of the hand-out set, phase 1 is after that commit, with the tree still whole
// on disk and the removal about to start. What runs in there is a pool-backed fork on a handle
// of its own -- the one thing the drain cannot lock out, because pool_claim() must never wait on
// a filler. At phase 1 the hook first takes one file out of the entry, because fs_remove_tree()
// is a walk and not one atomic step: a fork that lands in the middle of it finds exactly that,
// a tree some of whose entries have already been unlinked.
static wfs_store *g_drain_store = NULL;
static wfs_id g_drain_snap;
static const char *g_drain_target;
static const char *g_drain_pooldir;
static int g_drain_phase, g_drain_ran, g_drain_rc = -1;
static wfs_fork_result g_drain_fr;
static void fork_in_drain(void *ctx, int phase) {
    (void)ctx;
    if (g_drain_ran || phase != g_drain_phase) return;
    g_drain_ran = 1;
    if (phase == 1) {
        char names[8][256];
        uint64_t inos[8];
        CHECK(list_dir(g_drain_pooldir, names, inos, 8) == 1);
        char victim[4096];
        snprintf(victim, sizeof victim, "%s/%s/b.txt", g_drain_pooldir, names[0]);
        CHECK(unlink(victim) == 0);
    }
    wfs_fork_opts fo;
    memset(&fo, 0, sizeof fo);
    fo.name = "drainrace";
    memset(&g_drain_fr, 0, sizeof g_drain_fr);
    g_drain_rc = wfs_world_create_ex(g_drain_store, (wfs_ref){WFS_K_SNAPSHOT, g_drain_snap},
                                     g_drain_target, &fo, &g_drain_fr);
}

// PR #1 review (9th round, P1): `discard W<n> --now`'s own window. Phase 4 is inside the helper
// that deletes a trash entry here and now, after it has followed the tree to whatever name it
// has and before it marks it `.deleting`. What runs in there is a whole `restore` on a second
// handle -- the tree goes home, the row goes ACTIVE -- and what must happen next is that `--now`
// notices the row is not its row any more instead of burying it.
static wfs_store *g_now_store = NULL;
static wfs_id g_now_world;
static int g_now_ran;
static int g_now_rc = -1;
static int restore_before_now(void *ctx, int phase, int is_snapshot, wfs_id id,
                              const char *trash_path) {
    (void)ctx;
    (void)is_snapshot;
    (void)id;
    (void)trash_path;
    if (phase != 4 || g_now_ran) return 0;
    g_now_ran++;
    g_now_rc = wfs_world_restore(g_now_store, g_now_world);
    return 0;
}

// PR #1 review (15th round, P1): the same window, entered by a restore that stops in the middle
// of itself -- row in WFS_ST_TRASHING, tree still in the trash. `--now` used to rename it to
// `.deleting` out here, discover on the UPDATE behind the rename that the row was not its row
// any more, and return -ESTALE having already made the world unrestorable. The claim is one
// transaction with the row now, so the loser does nothing at all.
static wfs_store *g_nowhalf_store = NULL;
static wfs_id g_nowhalf_world;
static int g_nowhalf_ran;
static int g_nowhalf_rc;
static char g_nowhalf_path[4096];
static int restore_half_before_now(void *ctx, int phase, int is_snapshot, wfs_id id,
                                   const char *trash_path) {
    (void)ctx;
    (void)is_snapshot;
    (void)id;
    if (phase != 4 || g_nowhalf_ran) return 0;
    g_nowhalf_ran++;
    snprintf(g_nowhalf_path, sizeof g_nowhalf_path, "%s", trash_path ? trash_path : "");
    wfs_test_trash_crash = trash_crash;
    crash_at(2);
    g_nowhalf_rc = wfs_world_restore(g_nowhalf_store, g_nowhalf_world);
    crash_at(-1);
    wfs_test_trash_crash = NULL;   // and `--now` carries on into its claim
    return 0;
}

// PR #1 review (9th round, P1): the collector's own *scan* window. The seam runs after the scan
// has read which trash paths the rows claim and before the readdir that decides what nothing
// claims; what it does in there is a whole `discard` on a second handle, so the tree lands in
// the trash after the claim list was built and the readdir sees a directory that list has never
// heard of. That is the one shape "row-less orphan" must not be read as.
static wfs_store *g_orph_store = NULL;
static wfs_id g_orph_world;
static int g_orph_ran;
static int g_orph_rc = -1;
static void discard_before_orphans(void *ctx) {
    (void)ctx;
    if (g_orph_ran) return;
    g_orph_ran++;
    g_orph_rc = wfs_world_discard(g_orph_store, g_orph_world, 0, 0);
}

// PR #1 review (10th round, P1): the snapshot tmp sweep's window. The seam runs inside
// wfs_snapshot_create(), with <store>/snapshots/S<n>.wfs-tmp already on disk, the row CREATING
// with this very process as its producer, and the clone not started yet; what it does in there
// is a whole `gc` on a second handle, retention 0 and no deadline -- the suffix sweep at its
// most eager. It must take nothing: that tree is a create that is still running.
static wfs_store *g_sweep_store = NULL;
static int g_sweep_ran;
static int g_sweep_rc = -1;
static wfs_gc_report g_sweep_rep;
static void gc_before_snapshot_clone(void *ctx, const char *src_dir) {
    (void)ctx;
    (void)src_dir;
    if (g_sweep_ran) return;
    g_sweep_ran++;
    wfs_gc_opts go;
    memset(&go, 0, sizeof go);
    go.retention_secs = 0;
    memset(&g_sweep_rep, 0, sizeof g_sweep_rep);
    g_sweep_rc = wfs_gc_ex(g_sweep_store, &go, &g_sweep_rep);
}

// PR #1 review (26th round, P1): the user's own `mv`, run in the one window no transaction of
// ours can serialise -- after the entry's identity has been checked against the row and before
// the rename that claims it. The world's tree goes aside and a directory of somebody else's,
// with a file in it, takes the name the row still holds. Three callers enter that window (the
// collector, `--now` and `restore`) and none of them may touch what it leaves behind.
static char g_swap_aside[4096];
static char g_swap_file[4096];
static char g_swap_entry[4096];
static wfs_id g_swap_world;
static int g_swap_ran;
static void swap_between_claim(void *ctx, int is_snapshot, wfs_id row, const char *path) {
    (void)ctx;
    if (is_snapshot || row != g_swap_world || g_swap_ran) return;
    g_swap_ran++;
    snprintf(g_swap_entry, sizeof g_swap_entry, "%s", path);
    CHECK(rename(path, g_swap_aside) == 0);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(g_swap_file, sizeof g_swap_file, "%s/user.txt", path);
    write_file(g_swap_file, "not the world\n");
}

// Puts it all back: the stranger removed, the world's own tree returned to the entry's name.
static void swap_undo(const char *entry) {
    rm_rf(entry);
    CHECK(rename(g_swap_aside, entry) == 0);
}

// A copy in the sense of `cp -R`: same bytes, same marker, different inode (P2).
static void copy_dir(const char *src, const char *dst) {
    CHECK(mkdir(dst, 0755) == 0);
    DIR *d = opendir(src);
    CHECK(d);
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[4096], t[4096];
        join(s, sizeof s, src, e->d_name);
        join(t, sizeof t, dst, e->d_name);
        struct stat st;
        CHECK(lstat(s, &st) == 0);
        if (S_ISDIR(st.st_mode)) { copy_dir(s, t); continue; }
        char buf[65536];
        CHECK_OK(read_file(s, buf, sizeof buf));
        write_file(t, buf);
    }
    closedir(d);
}

int main() {
    const char *tmp = getenv("TMPDIR");
    char tpl[4096];
    snprintf(tpl, sizeof tpl, "%swfs-m1-test.XXXXXX", (tmp && *tmp) ? tmp : "/tmp/");
    CHECK(mkdtemp(tpl));
    char root[4096];
    // The world is identified by its inode, but the test compares paths too, so work from the
    // resolved root (/tmp is a symlink to /private/tmp on Darwin).
    CHECK(realpath(tpl, root));

    char base[4096], store[4096], worlds[4096], p[4096], q[4096], buf[4096];
    join(base, sizeof base, root, "project");
    join(store, sizeof store, root, "store");
    join(worlds, sizeof worlds, root, "worlds");
    CHECK(mkdir(base, 0755) == 0);
    CHECK(mkdir(worlds, 0755) == 0);
    join(p, sizeof p, base, "src");
    CHECK(mkdir(p, 0755) == 0);
    join(p, sizeof p, base, "hello.txt");
    write_file(p, "hello\n");
    join(q, sizeof q, base, "src/a.c");
    write_file(q, "int main(){}\n");
    // A hardlink pair: clonefile breaks these, and init must say so (P9).
    join(p, sizeof p, base, "src/a-link.c");
    CHECK(link(q, p) == 0);

    int lockfd2 = -1;
    wfs_id sbusy = 0, wbusy = 0;
    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    CHECK(strlen(wfs_version()) > 0);
    CHECK(strcmp(wfs_strerror(WFS_E_CROSS_VOLUME), wfs_strerror(-EINVAL)) != 0);

    // ---- P6: a cross-volume clone is detected by really cloning, not by comparing st_dev ----
    // /System and the data volume report the same st_dev on 27.0 and clonefile still returns
    // EXDEV (docs/CLONE_MODEL_MACOS27.md §7).
#ifdef __APPLE__
    CHECK_RC(wfs_store_clone_probe(s, "/System/Library/CoreServices"), WFS_E_CROSS_VOLUME);
#endif
    CHECK_OK(wfs_store_clone_probe(s, base));

    // ---- init: a gate-protected snapshot (T1.1b, the default) ----
    wfs_id s1 = 0;
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "proj";
    CHECK_OK(wfs_snapshot_create(s, base, &sopts, &s1));
    wfs_snapshot_rec sr;
    CHECK_OK(wfs_snapshot_info(s, s1, &sr));
    CHECK(sr.state == WFS_ST_ACTIVE);
    CHECK(sr.entries == 4);          // src, hello.txt, src/a.c, src/a-link.c
    CHECK(sr.hardlinks == 2);        // P9: both ends of the pair are reported
    CHECK(!strcmp(sr.name, "proj"));
    CHECK(!strcmp(sr.src_path, base));
    CHECK(sr.from_world == 0);
    CHECK(sr.hard == 0);
    CHECK((sr.root_mode & 0700) == 0700);   // the source root's own mode, kept for the fork

    // P3: the gate is the snapshot root itself. Nothing below it can be reached at all —
    // not listed, not opened, not stat'ed — and nothing inside was touched to achieve that.
    struct stat st;
    CHECK(stat(sr.path, &st) == 0 && (st.st_mode & 07777) == 0);
    CHECK(opendir(sr.path) == NULL && errno == EACCES);
    join(p, sizeof p, sr.path, "hello.txt");
    CHECK(open(p, O_RDONLY) < 0 && errno == EACCES);
    CHECK(open(p, O_WRONLY) < 0 && errno == EACCES);
    CHECK(lstat(p, &st) != 0 && errno == EACCES);
    CHECK(unlink(p) != 0 && errno == EACCES);
    join(q, sizeof q, sr.path, "new.txt");
    CHECK(open(q, O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES);
    // The entries themselves carry no flags: that is exactly what makes the fork cheap.
    CHECK(chmod(sr.path, 0700) == 0);
    CHECK(lstat(p, &st) == 0 && (st.st_flags & UF_IMMUTABLE) == 0);
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "hello\n"));
    CHECK(chmod(sr.path, 0) == 0);

    // P3: verify agrees with the manifest it wrote, and opens the gate itself to do so.
    wfs_verify_report vr;
    CHECK_OK(wfs_snapshot_verify(s, s1, &vr));
    CHECK(vr.checked == sr.entries + 1);   // + the root itself
    CHECK(vr.missing == 0 && vr.modified == 0 && vr.unprotected == 0 && vr.extra == 0);
    CHECK(stat(sr.path, &st) == 0 && (st.st_mode & 07777) == 0);   // and closes it again
    // A gate left open is a finding of its own.
    CHECK(chmod(sr.path, 0755) == 0);
    CHECK_RC(wfs_snapshot_verify(s, s1, &vr), WFS_E_SNAPSHOT_DIRTY);
    CHECK(vr.unprotected == 1 && strstr(vr.first_bad, "root") != NULL);
    CHECK(chmod(sr.path, 0) == 0);
    CHECK_OK(wfs_snapshot_verify(s, s1, &vr));

    // ---- P7: which paths may be initialised ----
    CHECK_RC(wfs_path_check(s, "/", 0), WFS_E_PATH_REFUSED);
    CHECK_RC(wfs_path_check(s, store, 0), WFS_E_PATH_REFUSED);
    CHECK_RC(wfs_path_check(s, sr.path, 0), WFS_E_PATH_REFUSED);       // a snapshot root
    join(p, sizeof p, sr.path, "src");
    CHECK_RC(wfs_path_check(s, p, 0), WFS_E_PATH_REFUSED);             // inside a snapshot
    CHECK_OK(wfs_path_check(s, base, 0));

    // ---- fork: a writable world with identical content ----
    char w1path[4096];
    join(w1path, sizeof w1path, worlds, "w1");
    wfs_id next = 0;
    CHECK_OK(wfs_world_next_id(s, &next));
    CHECK(next == 1);
    wfs_ref from = {WFS_K_SNAPSHOT, s1};
    wfs_fork_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.name = "w1";
    wfs_id w1 = 0;
    CHECK_OK(wfs_world_create(s, from, w1path, &opts, &w1));
    CHECK(w1 == next);

    join(p, sizeof p, w1path, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "hello\n"));
    write_file(p, "changed\n");                                   // writable, unlike its source
    join(q, sizeof q, w1path, "src/b.c");
    write_file(q, "new\n");
    CHECK(exists(q));
    join(p, sizeof p, sr.path, "hello.txt");                       // the snapshot did not move
    CHECK(chmod(sr.path, 0700) == 0);                              // (look behind the gate)
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "hello\n"));
    CHECK(chmod(sr.path, 0) == 0);
    join(p, sizeof p, w1path, ".world");
    CHECK(exists(p));
    join(p, sizeof p, w1path, "src");
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0200) != 0);          // directories writable again
    // T1.1b: a fork from a gated snapshot copies nothing but the tree. The world root has the
    // source's own mode back (never the 0500 of the open gate, never 0000), and no entry
    // anywhere carries UF_IMMUTABLE, because none ever did.
    CHECK(stat(w1path, &st) == 0 && (st.st_mode & 0700) == 0700 && (st.st_mode & 07777) != 0500);
    CHECK(lstat(w1path, &st) == 0 && (st.st_flags & UF_IMMUTABLE) == 0);
    join(p, sizeof p, w1path, "hello.txt");
    CHECK(lstat(p, &st) == 0 && (st.st_flags & UF_IMMUTABLE) == 0);
    join(p, sizeof p, w1path, "src/a.c");
    CHECK(lstat(p, &st) == 0 && (st.st_flags & UF_IMMUTABLE) == 0);

    wfs_world_rec wr;
    CHECK_OK(wfs_world_info(s, w1, &wr));
    CHECK(wr.state == WFS_ST_ACTIVE && wr.present == 1);
    CHECK(wr.snapshot_id == s1 && wr.parent_world == 0 && wr.origin == WFS_O_SNAPSHOT);
    CHECK(!strcmp(wr.path, w1path));
    CHECK(wr.dir_ino != 0);
    CHECK(wr.entries == sr.entries);

    // P7 again: a world root is not a legal init target, and neither is anything inside it.
    CHECK_RC(wfs_path_check(s, w1path, 0), WFS_E_PATH_REFUSED);
    join(p, sizeof p, w1path, "src");
    CHECK_RC(wfs_path_check(s, p, 0), WFS_E_PATH_REFUSED);
    join(p, sizeof p, w1path, "src/deeper");
    CHECK_RC(wfs_path_check(s, p, 1), WFS_E_PATH_REFUSED);         // as a fork target, too

    // ---- P1: identity survives a move ----
    char moved[4096];
    join(moved, sizeof moved, worlds, "renamed");
    CHECK(rename(w1path, moved) == 0);
    wfs_identity id;
    CHECK_OK(wfs_world_verify_identity(s, moved, &id));
    CHECK(id.registered == 1 && id.is_copy == 0 && id.moved == 1);
    CHECK(id.world_id == w1 && id.snapshot_id == s1);
    CHECK(!strcmp(id.path, moved));
    CHECK_OK(wfs_world_info(s, w1, &wr));
    CHECK(!strcmp(wr.path, moved) && wr.present == 1);             // the row followed the tree
    CHECK_OK(wfs_world_verify_identity(s, moved, &id));
    CHECK(id.moved == 0);                                          // ... and only reports it once
    CHECK(rename(moved, w1path) == 0);
    CHECK_OK(wfs_world_verify_identity(s, w1path, &id));

    // ---- P2: a copy is refused ----
    char copy[4096];
    join(copy, sizeof copy, worlds, "w1-copy");
    copy_dir(w1path, copy);
    wfs_identity cid;
    CHECK_RC(wfs_world_verify_identity(s, copy, &cid), WFS_E_UNREGISTERED);
    CHECK(cid.has_marker == 1 && cid.is_copy == 1 && cid.registered == 0);
    CHECK(cid.world_id == w1);
    CHECK(cid.ino != wr.dir_ino);
    // `adopt` is the remedy: the copy becomes a world of its own, with the original as parent.
    wfs_id w2 = 0;
    CHECK_OK(wfs_world_adopt(s, copy, "adopted", &w2));
    CHECK(w2 != w1);
    CHECK_OK(wfs_world_verify_identity(s, copy, &cid));
    CHECK(cid.registered == 1 && cid.world_id == w2);
    CHECK_OK(wfs_world_info(s, w2, &wr));
    CHECK(wr.origin == WFS_O_ADOPTED && wr.parent_world == w1);
    CHECK_RC(wfs_world_adopt(s, copy, NULL, &w2), -EEXIST);
    CHECK_OK(wfs_world_info(s, w1, &wr));                          // and the original is untouched
    CHECK(wr.present == 1);

    // ---- checkpoint: the same call as init, from a live world. --hard this time, so the
    // ---- per-entry UF_IMMUTABLE variant is exercised end to end. ----
    wfs_id s2 = 0;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "after-edit";
    sopts.hard = 1;
    CHECK_OK(wfs_snapshot_create(s, w1path, &sopts, &s2));
    CHECK_OK(wfs_snapshot_info(s, s2, &sr));
    CHECK(sr.from_world == w1);
    CHECK(sr.hard == 1);
    CHECK(sr.entries == 5);                                        // b.c was added in the world
    join(p, sizeof p, sr.path, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "changed\n"));
    join(p, sizeof p, sr.path, ".world");
    CHECK(!exists(p));            // the source world's marker is not part of the snapshot
    // --hard: every entry is chflagged and no gate is needed, so the tree stays traversable.
    CHECK(stat(sr.path, &st) == 0 && (st.st_mode & 07777) != 0 && (st.st_mode & 0222) == 0);
    join(p, sizeof p, sr.path, "hello.txt");
    CHECK(lstat(p, &st) == 0 && (st.st_flags & UF_IMMUTABLE) != 0);
    CHECK(open(p, O_WRONLY) < 0 && errno == EPERM);
    CHECK(unlink(p) != 0 && errno == EPERM);
    join(q, sizeof q, sr.path, "new.txt");
    CHECK(open(q, O_WRONLY | O_CREAT, 0644) < 0 && (errno == EPERM || errno == EACCES));
    CHECK(!exists(q));
    CHECK_OK(wfs_snapshot_verify(s, s2, &vr));
    CHECK(vr.missing == 0 && vr.modified == 0 && vr.unprotected == 0 && vr.extra == 0);
    // A fork from a --hard snapshot has to undo all of that on the clone.
    char whardpath[4096];
    join(whardpath, sizeof whardpath, worlds, "w-hard");
    wfs_ref fromhard = {WFS_K_SNAPSHOT, s2};
    wfs_id whard = 0;
    memset(&opts, 0, sizeof opts);
    CHECK_OK(wfs_world_create(s, fromhard, whardpath, &opts, &whard));
    join(p, sizeof p, whardpath, "hello.txt");
    CHECK(lstat(p, &st) == 0 && (st.st_flags & UF_IMMUTABLE) == 0);
    write_file(p, "writable\n");
    CHECK_OK(wfs_world_discard(s, whard, 1, 0));
    // The world it came from is still writable.
    join(p, sizeof p, w1path, "hello.txt");
    write_file(p, "changed again\n");

    // fork from a world, not a snapshot
    char w3path[4096];
    join(w3path, sizeof w3path, worlds, "w3");
    wfs_ref fw = {WFS_K_WORLD, w1};
    wfs_id w3 = 0;
    memset(&opts, 0, sizeof opts);
    CHECK_OK(wfs_world_create(s, fw, w3path, &opts, &w3));
    CHECK_OK(wfs_world_info(s, w3, &wr));
    CHECK(wr.parent_world == w1 && wr.origin == WFS_O_WORLD);
    join(p, sizeof p, w3path, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "changed again\n"));

    // A fork target that already exists is refused before anything is cloned.
    CHECK_RC(wfs_world_create(s, from, w3path, &opts, &w2), -EEXIST);

    // ---- P4: discard to the trash, then restore ----
    CHECK_OK(wfs_world_discard(s, w3, 0, 0));
    CHECK(!exists(w3path));
    CHECK_OK(wfs_world_info(s, w3, &wr));
    CHECK(wr.state == WFS_ST_TRASHED && wr.trashed_at > 0);
    CHECK(wr.present == 1);                                        // it is intact, just elsewhere
    size_t n = 0;
    CHECK_OK(wfs_world_list(s, 0, NULL, 0, &n));
    size_t active_without_w3 = n;
    CHECK_OK(wfs_world_list(s, 1, NULL, 0, &n));
    CHECK(n == active_without_w3 + 1);
    CHECK_OK(wfs_world_restore(s, w3));
    CHECK(exists(w3path));
    join(p, sizeof p, w3path, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "changed again\n"));
    CHECK_OK(wfs_world_info(s, w3, &wr));
    CHECK(wr.state == WFS_ST_ACTIVE && wr.present == 1);
    CHECK_OK(wfs_world_verify_identity(s, w3path, &id));
    CHECK(id.registered == 1);
    CHECK_RC(wfs_world_restore(s, w3), -ESTALE);                   // not trashed any more

    // gc must not touch a world that is still inside its retention window.
    wfs_gc_report gc;
    CHECK_OK(wfs_world_discard(s, w3, 0, 0));
    CHECK_OK(wfs_gc(s, 3600, &gc));
    CHECK(gc.worlds_deleted == 0);
    CHECK_OK(wfs_world_restore(s, w3));

    // --now deletes immediately.
    char w4path[4096];
    join(w4path, sizeof w4path, worlds, "w4");
    wfs_id w4 = 0;
    CHECK_OK(wfs_world_create(s, from, w4path, &opts, &w4));
    CHECK_OK(wfs_world_discard(s, w4, 1, 0));
    CHECK(!exists(w4path));
    CHECK_OK(wfs_world_info(s, w4, &wr));
    CHECK(wr.state == WFS_ST_DEAD);

    // ---- P5: the exec lock ----
    // w1 is the world under test; the lock lives in the store, not in the world tree.
    int lockfd = -1;
    CHECK_OK(wfs_world_lock_exec(s, w1, "sleep 30", &lockfd));
    CHECK(lockfd >= 0);
    char lockpath[4096];
    snprintf(lockpath, sizeof lockpath, "%s/locks/W%llu.lock", store, (unsigned long long)w1);
    CHECK(exists(lockpath));
    wfs_lock_info li;
    CHECK_OK(wfs_world_lock_check(s, w1, &li));
    CHECK(li.held == 1 && li.pid == (int64_t)getpid() && li.started_at > 0);
    CHECK(!strcmp(li.cmd, "sleep 30"));
    // Nothing destructive may touch a world somebody is working in...
    CHECK_RC(wfs_world_lock_exec(s, w1, "another", &lockfd2), WFS_E_WORLD_BUSY);
    CHECK_RC(wfs_world_discard(s, w1, 0, 0), WFS_E_WORLD_BUSY);
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "busy";
    CHECK_RC(wfs_snapshot_create(s, w1path, &sopts, &sbusy), WFS_E_WORLD_BUSY);
    char wbusypath[4096];
    join(wbusypath, sizeof wbusypath, worlds, "w-busy");
    wfs_ref fromw1 = {WFS_K_WORLD, w1};
    memset(&opts, 0, sizeof opts);
    CHECK_RC(wfs_world_create(s, fromw1, wbusypath, &opts, &wbusy), WFS_E_WORLD_BUSY);
    // ... unless the caller insists.
    opts.force = 1;
    CHECK_OK(wfs_world_create(s, fromw1, wbusypath, &opts, &wbusy));
    CHECK_OK(wfs_world_discard(s, wbusy, 1, 0));
    // A world that is not locked is not affected by W1's lock.
    CHECK_OK(wfs_world_lock_check(s, w3, &li));
    CHECK(li.held == 0);
    wfs_world_unlock_exec(s, w1, lockfd);
    CHECK(!exists(lockpath));
    CHECK_OK(wfs_world_discard(s, w1, 0, 0));
    CHECK_OK(wfs_world_restore(s, w1));

    // A lock left behind by a process that is gone is stale: it is removed, not obeyed. pid 1
    // is alive but cannot be holding the flock, which is the case that a bare kill(pid, 0)
    // check would get wrong.
    write_file(lockpath, "pid 1\nstart 1\ncmd ghost\n");
    CHECK_OK(wfs_world_lock_check(s, w1, &li));
    CHECK(li.held == 0);
    CHECK(!exists(lockpath));
    write_file(lockpath, "pid 2147480000\nstart 1\ncmd ghost\n");   // a pid that does not exist
    CHECK_OK(wfs_world_discard(s, w1, 0, 0));
    CHECK(!exists(lockpath));
    CHECK_OK(wfs_world_restore(s, w1));

    // ---- P8 + PR #1 review (3rd round): a half-built tree is collected, by name, not by guess --
    //
    // The fork's temporary used to be `<target>.wfs-tmp`, removed on sight before cloning, and
    // gc swept every `*.wfs-tmp` out of the parent directory of every world -- both of which are
    // the user's directory. `~/w/a.wfs-tmp` and `~/w/notes.wfs-tmp` are somebody's own file and
    // somebody's own directory; `fork --to ~/w/a` and a routine `gc` destroyed them.
    char mine_file[4096], mine_dir[4096], mine_inner[4096], tmptgt[4096];
    join(mine_file, sizeof mine_file, worlds, "wtmp.wfs-tmp");     // == <target>.wfs-tmp below
    write_file(mine_file, "mine, not a temporary\n");
    join(mine_dir, sizeof mine_dir, worlds, "notes.wfs-tmp");
    CHECK(mkdir(mine_dir, 0755) == 0);
    join(mine_inner, sizeof mine_inner, mine_dir, "keep");
    write_file(mine_inner, "keep me\n");

    join(tmptgt, sizeof tmptgt, worlds, "wtmp");
    wfs_id wtmp = 0;
    memset(&opts, 0, sizeof opts);
    opts.name = "wtmp";
    CHECK_OK(wfs_world_create(s, from, tmptgt, &opts, &wtmp));
    CHECK_OK(read_file(mine_file, buf, sizeof buf));
    CHECK(!strcmp(buf, "mine, not a temporary\n"));                // the fork did not eat it
    CHECK_OK(read_file(mine_inner, buf, sizeof buf));
    CHECK(!strcmp(buf, "keep me\n"));
    CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);               // and left nothing of its own

    // The crash the temp path exists for: the clone is made and recorded, the publish rename
    // never happens. gc removes exactly the recorded path and marks the row DEAD.
    //
    // The row has to look abandoned for that, which from inside one process means writing a pid
    // that is not running (PR #1 review, 3rd round: a CREATING row whose producer is alive is
    // work in progress, not litter) and switching off the minimum age that backs that rule up.
    char crashtgt[4096];
    join(crashtgt, sizeof crashtgt, worlds, "wcrash");
    crash_seen.world = 0;
    crash_seen.tmp[0] = 0;
    wfs_test_before_fork_publish = crash_before_publish;
    wfs_test_before_fork_publish_ctx = &crash_seen;
    wfs_id wcrash = 0;

    // First: the same crash with THIS process recorded as the producer -- alive, by definition.
    // gc must not touch it, which is the overlapping-worker case: a clone that outlives the
    // pause before an auto-spawned worker starts used to have its tree and its row deleted.
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    CHECK(crash_seen.world != 0 && exists(crash_seen.tmp));
    setenv("WORLD_GC_CREATING_MIN_AGE", "0", 1);        // age is not what protects it here
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(exists(crash_seen.tmp));                      // still building, as far as gc knows
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_CREATING);
    rm_rf(crash_seen.tmp);                              // clean up after the live-producer case

    // Now the producer is gone. A pid that does not exist, the same one the lock tests use.
    wfs_test_fork_owner_pid = 2147480000;
    join(crashtgt, sizeof crashtgt, worlds, "wcrash2");
    crash_seen.world = 0;
    crash_seen.tmp[0] = 0;
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    wfs_test_before_fork_publish = NULL;
    wfs_test_fork_owner_pid = 0;
    CHECK(crash_seen.world != 0 && crash_seen.tmp[0]);
    CHECK(!exists(crashtgt));                                      // never published
    CHECK(exists(crash_seen.tmp));                                 // the clone is still there
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_CREATING);
    // ... and it is inside the user's directory, under a name of ours.
    CHECK(!strncmp(crash_seen.tmp + strlen(worlds) + 1, ".wfs-fork-", 10));

    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(!exists(crash_seen.tmp));
    CHECK(gc.tmp_removed >= 1);
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_DEAD);
    CHECK(exists(w1path) && exists(w3path) && exists(tmptgt));     // nothing else went with it
    CHECK_OK(read_file(mine_file, buf, sizeof buf));
    CHECK(!strcmp(buf, "mine, not a temporary\n"));                // nor did gc
    CHECK(exists(mine_dir) && exists(mine_inner));

    // A CREATING row whose recorded tree is gone already (somebody deleted it by hand): the row
    // is buried, and gc counts nothing removed for it.
    crash_seen.world = 0;
    crash_seen.tmp[0] = 0;
    wfs_test_before_fork_publish = crash_before_publish;
    wfs_test_fork_owner_pid = 2147480000;
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    wfs_test_before_fork_publish = NULL;
    wfs_test_fork_owner_pid = 0;
    CHECK(exists(crash_seen.tmp));
    rm_rf(crash_seen.tmp);
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(gc.tmp_removed == 0);
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_DEAD);
    CHECK(exists(mine_dir) && exists(mine_file));

    // ---- PR #1 review (12th round), the exists() audit: a tree gc cannot ask about ------------
    //
    // gc_tmp_is_removable() promised in its own comment that "anything unreadable is left alone
    // too", and the marker's lstat(2) did not keep it: an EACCES or an EIO read as "there is no
    // marker", and the clone -- which lives in the *user's* target directory -- was removed
    // without the one check that says it is ours. Answering "not removable" is only half the
    // fix, because the caller then marks the row DEAD and clears its tmp_path, which is the
    // permanent stranding the 5th round fixed. So "I cannot tell" is now its own answer: the
    // row stays CREATING, the tree is counted, and a later wake asks again.
    crash_seen.world = 0;
    crash_seen.tmp[0] = 0;
    wfs_test_before_fork_publish = crash_before_publish;
    wfs_test_fork_owner_pid = 2147480000;
    join(crashtgt, sizeof crashtgt, worlds, "wcrash-unreadable");
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    wfs_test_before_fork_publish = NULL;
    wfs_test_fork_owner_pid = 0;
    CHECK(exists(crash_seen.tmp));
    CHECK(chmod(crash_seen.tmp, 0) == 0);      // the marker inside cannot be lstat'ed any more
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(exists(crash_seen.tmp));             // not removed: nobody proved it was ours
    CHECK(gc.tmp_failed >= 1);                 // ... and not silently dropped either
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_CREATING);        // the row is still the tree's only name
    CHECK(chmod(crash_seen.tmp, 0755) == 0);
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(!exists(crash_seen.tmp));            // and the next wake finishes it
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_DEAD);

    // ---- PR #1 review (20th round, P2): a fork unwind that could not remove its tmp tree -----
    //
    // A fork that fails after its clone exists removes the tree and then deletes the CREATING
    // row. The removal's result was thrown away, and the row is the ONLY record of that tree's
    // name anywhere: it lives in the user's own target directory under a name drawn from 64
    // random bits, and gc deliberately never sweeps a user directory by suffix (that is what
    // the wtmp case above pins down). So one EPERM left the whole half-built clone on disk with
    // nothing in the store naming it -- not counted by `gc --status`, never retried, and not
    // adoptable either. Same rule as the 5th, 7th, 8th, 12th, 16th and 18th rounds: a tree that
    // could not be removed is not a tree that was removed, so the row stays CREATING with its
    // tmp_path and the caller gets the error that started it.
    {
        char u20tgt[4096];
        join(u20tgt, sizeof u20tgt, worlds, "w20unwind");
        snprintf(u20_target, sizeof u20_target, "%s", u20tgt);
        u20_tmp[0] = 0;
        u20_locked[0] = 0;
        u20_world = 0;
        wfs_test_before_fork_publish = fork_fail_at_publish;
        wfs_test_before_fork_publish_ctx = NULL;
        wfs_test_fork_owner_pid = 2147480000;    // the producer is gone once the call returns
        memset(&opts, 0, sizeof opts);
        opts.name = "w20unwind";
        wfs_id w20 = 0;
        CHECK_RC(wfs_world_create(s, from, u20tgt, &opts, &w20), -EEXIST);
        wfs_test_before_fork_publish = NULL;
        wfs_test_before_fork_publish_ctx = &crash_seen;
        wfs_test_fork_owner_pid = 0;
        CHECK(u20_world != 0 && u20_tmp[0]);
        CHECK(exists(u20_tmp));                        // the clone could not be removed ...
        CHECK_OK(wfs_world_info(s, u20_world, &wr));   // ... so the row that names it is kept
        CHECK(wr.state == WFS_ST_CREATING);
        // And it is counted: `gc --status` is where an operator finds out that the space is
        // still out there, which needs the row to exist (the count comes off the CREATING rows).
        setenv("WORLD_GC_CREATING_MIN_AGE", "0", 1);
        wfs_trash_stat u20st;
        CHECK_OK(wfs_gc_status(s, 0, &u20st));
        CHECK(u20st.creating_stranded >= 1);
        // gc cannot remove it either while the ACL is on, and answers the same way: row kept,
        // tree counted, retried under the failure cap.
        memset(&gc, 0, sizeof gc);
        CHECK_OK(wfs_gc(s, 0, &gc));
        CHECK(exists(u20_tmp) && gc.tmp_failed >= 1);
        CHECK_OK(wfs_world_info(s, u20_world, &wr));
        CHECK(wr.state == WFS_ST_CREATING);
        // Take the ACL away and the next wake finishes it: tree gone, row buried.
        allow_delete(u20_locked);
        memset(&gc, 0, sizeof gc);
        CHECK_OK(wfs_gc(s, 0, &gc));
        CHECK(!exists(u20_tmp));
        CHECK_OK(wfs_world_info(s, u20_world, &wr));
        CHECK(wr.state == WFS_ST_DEAD);
        CHECK(exists(u20tgt));                         // the directory that was in the way, intact
        CHECK(rmdir(u20tgt) == 0);
    }

    // ---- PR #1 review (3rd round): "is there work for a collector?" must ask what the ---------
    // collector asks. A discard killed between the rename into <store>/trash and the commit of
    // its row leaves an ordinary `W<n>-<t>` directory that no row claims and that does not wear
    // the `.deleting` suffix. trash_scan() calls that due immediately, so `gc` reports work --
    // but the pending check looked only at the two row tables and for a `.deleting` name, said
    // "nothing waiting", and no worker was ever started for it. The CLI announced a background
    // collection and spawned nothing, and every later fork and discard decided the same.
    char orphandir[4096];
    snprintf(orphandir, sizeof orphandir, "%s/trash/W9999-1", store);
    CHECK(mkdir(orphandir, 0755) == 0);
    join(p, sizeof p, orphandir, "leftover");
    write_file(p, "x");
    int gc_worker = 0;
    CHECK(wfs_gc_pending(s, 7 * 24 * 3600, &gc_worker) == 1);
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 7 * 24 * 3600, &gc));   // a 7-day retention: nothing else is due
    CHECK(!exists(orphandir));
    CHECK(gc.trash_orphans >= 1);
    CHECK(exists(w1path) && exists(w3path));

    // ---- P3 again: tampering with a --hard snapshot is detected ----
    CHECK_OK(wfs_snapshot_info(s, s2, &sr));
    join(p, sizeof p, sr.path, "hello.txt");                       // sr is S2 here
    CHECK(lchflags(p, 0) == 0);
    write_file(p, "tampered\n");
    CHECK_RC(wfs_snapshot_verify(s, s2, &vr), WFS_E_SNAPSHOT_DIRTY);
    CHECK(vr.modified == 1 && vr.first_bad[0]);
    CHECK(strstr(vr.first_bad, "hello.txt") != NULL);
    // An entry nobody recorded is caught by the count, not by the manifest.
    CHECK(lchflags(sr.path, 0) == 0 && chmod(sr.path, 0755) == 0);
    join(p, sizeof p, sr.path, "smuggled.txt");
    write_file(p, "x");
    CHECK_RC(wfs_snapshot_verify(s, s2, &vr), WFS_E_SNAPSHOT_DIRTY);
    CHECK(vr.extra == 1);

    // ---- store status ----
    wfs_store_stat ss;
    CHECK_OK(wfs_store_status(s, &ss));
    CHECK(ss.schema == WFS_STORE_SCHEMA);
    CHECK(ss.snapshots == 2);
    CHECK(ss.worlds_active >= 2);
    CHECK(ss.volume_total_bytes > 0 && ss.volume_free_bytes > 0);
    CHECK(ss.metadata_estimate_bytes > 0);
    CHECK(!strcmp(ss.dir, store));

    // ---- T1.5: the pre-clone pool ----
    // An entry is a finished clone of the snapshot with no marker and no world row. Filling is
    // a top-up to a target, handing one out is a rename, and everything that can go wrong with
    // it (a stale snapshot identity, a touched entry, a killed filler) is caught here.
    wfs_snapshot_rec s1rec;
    CHECK_OK(wfs_snapshot_info(s, s1, &s1rec));
    uint64_t made = 0, ready = 0, removed = 0;
    CHECK_OK(wfs_pool_fill(s, s1, 2, &made));
    CHECK(made == 2);
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 2);
    // `fill` is a top-up, not an addition: asking for 2 again makes nothing.
    CHECK_OK(wfs_pool_fill(s, s1, 2, &made));
    CHECK(made == 0);
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 2);

    wfs_pool_stat ps[4];
    size_t pn = 0;
    CHECK_OK(wfs_pool_status(s, ps, 4, &pn));
    CHECK(pn == 1);
    CHECK(ps[0].snapshot == s1 && ps[0].ready == 2 && ps[0].building == 0 && ps[0].stale == 0);
    CHECK(ps[0].entries == s1rec.entries && !strcmp(ps[0].snapshot_name, s1rec.name));

    // P7: the entries live in the store, so they are neither a legal source nor a legal target.
    char pooldir[4096], poolpath[4096];
    snprintf(pooldir, sizeof pooldir, "%s/pool/S%llu", store, (unsigned long long)s1);
    CHECK_RC(wfs_path_check(s, pooldir, 0), WFS_E_PATH_REFUSED);
    snprintf(poolpath, sizeof poolpath, "%s/taken", pooldir);
    CHECK_RC(wfs_path_check(s, poolpath, 1), WFS_E_PATH_REFUSED);

    char pnames[8][256];
    uint64_t pinos[8];
    CHECK(list_dir(pooldir, pnames, pinos, 8) == 2);

    // The hit: the world IS one of those trees, renamed. Same inode, one entry fewer, and a
    // marker where there was none.
    char wpool[4096];
    join(wpool, sizeof wpool, worlds, "w-pool");
    memset(&opts, 0, sizeof opts);
    opts.name = "w-pool";
    wfs_fork_result fr;
    memset(&fr, 0, sizeof fr);
    wfs_ref from_s1 = {WFS_K_SNAPSHOT, s1};
    CHECK_OK(wfs_world_create_ex(s, from_s1, wpool, &opts, &fr));
    CHECK(fr.from_pool == 1 && fr.world != 0 && fr.pool_left == 1);
    CHECK(stat(wpool, &st) == 0);
    CHECK((uint64_t)st.st_ino == pinos[0] || (uint64_t)st.st_ino == pinos[1]);
    char gone[4096];
    join(gone, sizeof gone, pooldir, ((uint64_t)st.st_ino == pinos[0]) ? pnames[0] : pnames[1]);
    CHECK(!exists(gone));
    join(p, sizeof p, wpool, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "hello\n"));
    join(p, sizeof p, wpool, ".world");
    CHECK(exists(p));
    CHECK_OK(wfs_world_verify_identity(s, wpool, &id));
    CHECK(id.registered && id.world_id == fr.world);
    CHECK_OK(wfs_world_info(s, fr.world, &wr));
    CHECK(wr.origin == WFS_O_SNAPSHOT && wr.snapshot_id == s1 && wr.state == WFS_ST_ACTIVE);
    CHECK(wr.entries == s1rec.entries && wr.dir_ino == (uint64_t)st.st_ino);
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 1);

    // --no-pool clones here and now even with a warm pool.
    char wnop[4096];
    join(wnop, sizeof wnop, worlds, "w-nopool");
    memset(&opts, 0, sizeof opts);
    opts.name = "w-nopool";
    opts.no_pool = 1;
    memset(&fr, 0, sizeof fr);
    CHECK_OK(wfs_world_create_ex(s, from_s1, wnop, &opts, &fr));
    CHECK(fr.from_pool == 0);
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 1);

    // The miss path: an empty pool is not an error, it is a clonefile.
    CHECK_OK(wfs_pool_drain(s, s1, &removed));
    CHECK(removed == 1);
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 0);
    char wmiss[4096];
    join(wmiss, sizeof wmiss, worlds, "w-miss");
    memset(&opts, 0, sizeof opts);
    opts.name = "w-miss";
    memset(&fr, 0, sizeof fr);
    CHECK_OK(wfs_world_create_ex(s, from_s1, wmiss, &opts, &fr));
    CHECK(fr.from_pool == 0 && fr.world != 0);
    join(p, sizeof p, wmiss, "hello.txt");
    CHECK_OK(read_file(p, buf, sizeof buf));
    CHECK(!strcmp(buf, "hello\n"));

    // verify S<n> also looks at what is waiting: an entry somebody wrote to is a finding.
    CHECK_OK(wfs_pool_fill(s, s1, 1, &made));
    CHECK(made == 1);
    CHECK_OK(wfs_snapshot_verify(s, s1, &vr));
    CHECK(vr.pool_checked == 1 && vr.pool_dirty == 0);
    CHECK(list_dir(pooldir, pnames, pinos, 8) == 1);
    join(p, sizeof p, pooldir, pnames[0]);
    join(q, sizeof q, p, "smuggled.txt");
    write_file(q, "x");
    CHECK_RC(wfs_snapshot_verify(s, s1, &vr), WFS_E_SNAPSHOT_DIRTY);
    CHECK(vr.pool_dirty == 1 && strstr(vr.first_bad, pnames[0]) != NULL);
    CHECK_OK(wfs_pool_drain(s, s1, &removed));
    CHECK(removed == 1);
    CHECK_OK(wfs_snapshot_verify(s, s1, &vr));
    CHECK(vr.pool_dirty == 0);

    // gc: a filler that was killed leaves a *.wfs-tmp tree, and a fork that died between the
    // claim and the rename leaves a directory no row points at. Both go; what is ready stays.
    CHECK_OK(wfs_pool_fill(s, s1, 1, &made));
    CHECK(list_dir(pooldir, pnames, pinos, 8) == 1);
    char half[4096], orphan[4096];
    snprintf(half, sizeof half, "%s/deadbeef%s", pooldir, WFS_TMP_SUFFIX);
    CHECK(mkdir(half, 0755) == 0);
    join(p, sizeof p, half, "partial");
    write_file(p, "x");
    snprintf(orphan, sizeof orphan, "%s/0123456789abcdef", pooldir);
    CHECK(mkdir(orphan, 0755) == 0);
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(gc.pool_removed >= 2);
    CHECK(!exists(half) && !exists(orphan));
    CHECK_OK(wfs_pool_ready(s, s1, &ready));
    CHECK(ready == 1);
    CHECK(list_dir(pooldir, pnames, pinos, 8) == 1);

    // An entry whose snapshot is no longer the snapshot it was cloned from is never handed out.
    // (Snapshot rows are immutable, so this is only reachable by a store surviving an id reuse;
    // the pool keys on created_at to make it impossible.)
    CHECK_OK(wfs_pool_status(s, ps, 4, &pn));
    CHECK(pn == 1 && ps[0].ready == 1 && ps[0].stale == 0);
    CHECK_OK(wfs_pool_drain(s, 0, &removed));
    CHECK(removed == 1);
    CHECK_OK(wfs_pool_status(s, ps, 4, &pn));
    CHECK(pn == 0);

    // ---- P9 (T2.5): hardlinks inside a tree are rebuilt inside every clone of it ----
    // clonefile breaks them all (CLONE_MODEL_MACOS27 §11). Three groups live entirely inside
    // the tree (2, 3 and 5 names) and one has a fifth name outside it, which no clone can be
    // given: that one is counted and left as independent copies.
    char hlsrc[4096];
    join(hlsrc, sizeof hlsrc, root, "hlproj");
    CHECK(mkdir(hlsrc, 0755) == 0);
    join(p, sizeof p, hlsrc, "sub");
    CHECK(mkdir(p, 0755) == 0);
    join(p, sizeof p, hlsrc, "g2");
    write_file(p, "two\n");
    join(q, sizeof q, hlsrc, "sub/g2-b");
    CHECK(link(p, q) == 0);
    join(p, sizeof p, hlsrc, "g3");
    write_file(p, "three\n");
    for (int i = 0; i < 2; ++i) {
        snprintf(q, sizeof q, "%s/sub/g3-%d", hlsrc, i);
        CHECK(link(p, q) == 0);
    }
    join(p, sizeof p, hlsrc, "g5");
    write_file(p, "five\n");
    for (int i = 0; i < 4; ++i) {
        snprintf(q, sizeof q, "%s/g5-%d", hlsrc, i);
        CHECK(link(p, q) == 0);
    }
    join(p, sizeof p, hlsrc, "ext");
    write_file(p, "ext\n");
    join(q, sizeof q, root, "ext-outside");
    CHECK(link(p, q) == 0);
    // PR #1 review (P1): `.wfs-tmp` is not a name reserved to us inside somebody's workspace.
    // The replay used to make its temporary link at `<name>.wfs-tmp` and, on EEXIST, *unlink*
    // whatever was already there -- so these two ordinary files, sitting next to the two names
    // of a hardlink group, were silently destroyed by every fork. Both of them, because which
    // name of the group is the canonical one and which gets relinked is the scan's business.
    join(p, sizeof p, hlsrc, "g2.wfs-tmp");
    write_file(p, "not a temporary, mine\n");
    join(p, sizeof p, hlsrc, "sub/g2-b.wfs-tmp");
    write_file(p, "nor is this one\n");

    wfs_id shl = 0;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "hl";
    CHECK_OK(wfs_snapshot_create(s, hlsrc, &sopts, &shl));
    CHECK_OK(wfs_snapshot_info(s, shl, &sr));
    CHECK(sr.hardlinks == 11);    // 2 + 3 + 5 names inside, plus the one that reaches outside
    CHECK(sr.hl_groups == 3);     // only the groups that are entirely inside the tree
    CHECK(sr.hl_external == 1);   // and the name whose inode also lives outside it
    CHECK_OK(wfs_snapshot_verify(s, shl, &vr));   // the manifest's new section is skipped, not read as an entry
    CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0);

    char whl[4096];
    join(whl, sizeof whl, worlds, "w-hl");
    wfs_ref from_hl = {WFS_K_SNAPSHOT, shl};
    memset(&opts, 0, sizeof opts);
    opts.name = "w-hl";
    opts.no_pool = 1;
    wfs_fork_result hfr;
    memset(&hfr, 0, sizeof hfr);
    CHECK_OK(wfs_world_create_ex(s, from_hl, whl, &opts, &hfr));
    CHECK(hfr.from_pool == 0);
    CHECK(hfr.hardlinks == 1 + 2 + 4);   // names relinked to their group's canonical file

    char hn[4096];
    join(p, sizeof p, whl, "g2");
    join(q, sizeof q, whl, "sub/g2-b");
    CHECK(ino_of(p) == ino_of(q));
    CHECK(nlink_of(p) == 2 && nlink_of(q) == 2);
    join(p, sizeof p, whl, "g3");
    CHECK(nlink_of(p) == 3);
    for (int i = 0; i < 2; ++i) {
        snprintf(hn, sizeof hn, "%s/sub/g3-%d", whl, i);
        CHECK(ino_of(hn) == ino_of(p) && nlink_of(hn) == 3);
    }
    join(p, sizeof p, whl, "g5");
    CHECK(nlink_of(p) == 5);
    for (int i = 0; i < 4; ++i) {
        snprintf(hn, sizeof hn, "%s/g5-%d", whl, i);
        CHECK(ino_of(hn) == ino_of(p) && nlink_of(hn) == 5);
    }
    // PR #1 review (P1): all three of `g2`, `sub/g2-b` and their `.wfs-tmp` namesakes are in
    // the fork; the two hardlinked names share an inode and the two ordinary files are exactly
    // the bytes the snapshot had. And the replay left no temporary of its own behind.
    join(p, sizeof p, whl, "g2.wfs-tmp");
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "not a temporary, mine\n"));
    CHECK(nlink_of(p) == 1);
    join(p, sizeof p, whl, "sub/g2-b.wfs-tmp");
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "nor is this one\n"));
    CHECK(nlink_of(p) == 1);
    CHECK(n_with_prefix(whl, ".wfs-hl-") == 0);
    join(p, sizeof p, whl, "sub");
    CHECK(n_with_prefix(p, ".wfs-hl-") == 0);

    // The group with a name outside the tree is exactly what it was before T2.5: same bytes,
    // separate inodes.
    join(p, sizeof p, whl, "ext");
    CHECK(nlink_of(p) == 1);
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "ext\n"));
    join(q, sizeof q, root, "ext-outside");
    CHECK(ino_of(p) != ino_of(q));
    // Mode and content come from the canonical file, as they do on the source.
    join(p, sizeof p, whl, "g5");
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "five\n"));
    CHECK(lstat(p, &st) == 0 && (st.st_mode & 07777) == 0644);

    // A write through one name is a write to the file, so the other names see it -- which is
    // the whole point of keeping the links (a pnpm store, a git pack, a cargo cache).
    join(p, sizeof p, whl, "sub/g2-b");
    write_file(p, "written\n");
    join(q, sizeof q, whl, "g2");
    CHECK(read_file(q, buf, sizeof buf) == 0 && !strcmp(buf, "written\n"));
    CHECK(nlink_of(q) == 2 && ino_of(p) == ino_of(q));
    join(p, sizeof p, hlsrc, "g2");   // and the source tree is untouched by any of it
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "two\n"));

    // checkpoint: the same rebuild on the snapshot's own clone, from a live world this time.
    // The world has no name outside itself any more, so the external group is gone.
    wfs_id shl2 = 0;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "hl-after";
    CHECK_OK(wfs_snapshot_create(s, whl, &sopts, &shl2));
    CHECK_OK(wfs_snapshot_info(s, shl2, &sr));
    CHECK(sr.hl_groups == 3 && sr.hl_external == 0 && sr.hardlinks == 10);
    CHECK(chmod(sr.path, 0700) == 0);   // look behind the gate
    join(p, sizeof p, sr.path, "g5");
    join(q, sizeof q, sr.path, "g5-3");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 5);
    join(p, sizeof p, sr.path, "ext");
    CHECK(nlink_of(p) == 1);
    CHECK(chmod(sr.path, 0) == 0);

    // T1.5: a pool entry is a clone as well, so the groups are replayed when it is filled --
    // not when it is handed out, which stays a marker plus a rename.
    CHECK_OK(wfs_pool_fill(s, shl, 1, &made));
    CHECK(made == 1);
    // The replay happens before the entry is published, so `verify` (which checks that nobody
    // has written to a waiting entry since it was cloned) sees nothing out of place.
    CHECK_OK(wfs_snapshot_verify(s, shl, &vr));
    CHECK(vr.pool_checked == 1 && vr.pool_dirty == 0);
    char wplhl[4096];
    join(wplhl, sizeof wplhl, worlds, "w-hl-pool");
    memset(&opts, 0, sizeof opts);
    opts.name = "w-hl-pool";
    memset(&hfr, 0, sizeof hfr);
    CHECK_OK(wfs_world_create_ex(s, from_hl, wplhl, &opts, &hfr));
    CHECK(hfr.from_pool == 1 && hfr.hardlinks == 0);
    join(p, sizeof p, wplhl, "g3");
    join(q, sizeof q, wplhl, "sub/g3-1");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 3);
    join(p, sizeof p, wplhl, "g5");
    CHECK(nlink_of(p) == 5);
    // The pool entry was cloned and replayed at fill time, so the same two files have to have
    // come through that path untouched as well.
    join(p, sizeof p, wplhl, "sub/g2-b.wfs-tmp");
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "nor is this one\n"));
    join(p, sizeof p, wplhl, "g2.wfs-tmp");
    CHECK(read_file(p, buf, sizeof buf) == 0 && !strcmp(buf, "not a temporary, mine\n"));
    CHECK_OK(wfs_pool_drain(s, shl, &removed));

    // A fork of a live world carries the groups too: they are the origin snapshot's, checked
    // name by name against the world before anything in the clone is touched (one lstat per
    // name, never a walk). A group the agent broke in the world stays broken in the fork.
    join(p, sizeof p, whl, "sub/g3-1");
    CHECK(unlink(p) == 0);            // 3 names become 2: the group no longer matches
    char whl2[4096];
    join(whl2, sizeof whl2, worlds, "w-hl-child");
    CHECK_OK(wfs_world_verify_identity(s, whl, &id));
    wfs_ref from_whl = {WFS_K_WORLD, id.world_id};
    memset(&opts, 0, sizeof opts);
    opts.name = "w-hl-child";
    memset(&hfr, 0, sizeof hfr);
    CHECK_OK(wfs_world_create_ex(s, from_whl, whl2, &opts, &hfr));
    CHECK(hfr.hardlinks == 1 + 4);    // g2 and g5; g3 no longer matches the world
    join(p, sizeof p, whl2, "g2");
    join(q, sizeof q, whl2, "sub/g2-b");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
    join(p, sizeof p, whl2, "g5");
    CHECK(nlink_of(p) == 5);
    join(p, sizeof p, whl2, "g3");
    join(q, sizeof q, whl2, "sub/g3-0");
    CHECK(ino_of(p) != ino_of(q) && nlink_of(p) == 1);

    // ---- PR #1 review (4th round, P2): a hardlink group under a read-only directory ----
    // 0555 is an ordinary mode for a vendored tree, a generated fixture, a `chmod -R a-w`
    // release directory. The clone wears it too, so link(2)/rename(2) inside it come back
    // EACCES -- and the replay's result was ignored, so the snapshot was published with a
    // manifest and a row advertising a group its tree did not have, and every fork and pool
    // entry inherited the lie. Now the directory is lent owner write for the two calls and
    // gets its exact mode back.
    char rosrc[4096], snapdir[4096];
    join(snapdir, sizeof snapdir, store, "snapshots");
    join(rosrc, sizeof rosrc, root, "roproj");
    CHECK(mkdir(rosrc, 0755) == 0);
    join(p, sizeof p, rosrc, "ro");
    CHECK(mkdir(p, 0755) == 0);
    join(q, sizeof q, rosrc, "ro/x");
    write_file(q, "read only\n");
    join(p, sizeof p, rosrc, "ro/y");
    CHECK(link(q, p) == 0);
    join(p, sizeof p, rosrc, "ro");
    CHECK(chmod(p, 0555) == 0);           // no owner write, and that is how it must stay

    wfs_id sro = 0;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "ro";
    CHECK_OK(wfs_snapshot_create(s, rosrc, &sopts, &sro));
    CHECK_OK(wfs_snapshot_info(s, sro, &sr));
    CHECK(sr.hl_groups == 1 && sr.hl_external == 0 && sr.hardlinks == 2);
    CHECK_OK(wfs_snapshot_verify(s, sro, &vr));   // the lent mode was given back before this
    CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0);
    CHECK(chmod(sr.path, 0700) == 0);     // look behind the gate
    join(p, sizeof p, sr.path, "ro/x");
    join(q, sizeof q, sr.path, "ro/y");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
    join(p, sizeof p, sr.path, "ro");
    CHECK(lstat(p, &st) == 0 && (st.st_mode & 07777) == 0555);   // exactly the mode it had
    CHECK(n_with_prefix(p, ".wfs-hl-") == 0);
    CHECK(chmod(sr.path, 0) == 0);

    char wro[4096];
    join(wro, sizeof wro, worlds, "w-ro");
    wfs_ref from_ro = {WFS_K_SNAPSHOT, sro};
    memset(&opts, 0, sizeof opts);
    opts.name = "w-ro";
    opts.no_pool = 1;
    memset(&hfr, 0, sizeof hfr);
    CHECK_OK(wfs_world_create_ex(s, from_ro, wro, &opts, &hfr));
    CHECK(hfr.hardlinks == 1);            // the one name relinked to its canonical file
    join(p, sizeof p, wro, "ro/x");
    join(q, sizeof q, wro, "ro/y");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
    join(p, sizeof p, wro, "ro");
    CHECK(lstat(p, &st) == 0 && (st.st_mode & 07777) == 0555);   // and in the world as well
    CHECK(n_with_prefix(p, ".wfs-hl-") == 0);

    // The same through the pool, whose filler does the replay on the entry it clones.
    CHECK_OK(wfs_pool_fill(s, sro, 1, &made));
    CHECK(made == 1);
    char wropool[4096];
    join(wropool, sizeof wropool, worlds, "w-ro-pool");
    memset(&opts, 0, sizeof opts);
    opts.name = "w-ro-pool";
    memset(&hfr, 0, sizeof hfr);
    CHECK_OK(wfs_world_create_ex(s, from_ro, wropool, &opts, &hfr));
    CHECK(hfr.from_pool == 1);
    join(p, sizeof p, wropool, "ro/x");
    join(q, sizeof q, wropool, "ro/y");
    CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
    join(p, sizeof p, wropool, "ro");
    CHECK(lstat(p, &st) == 0 && (st.st_mode & 07777) == 0555);

    // And the other half of the fix: a replay the file system really refuses must not be
    // published at all. Nothing on a developer's disk makes link(2) fail that way, so the seam
    // says it did -- what is being pinned down is the unwinding, not the errno.
    size_t snaps_before = 0, trees_before = n_with_prefix(snapdir, "S");
    CHECK_OK(wfs_snapshot_list(s, NULL, 0, &snaps_before));
    wfs_test_hardlink_restore_err = -EIO;
    {
        wfs_id sfail = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ro-fail";
        CHECK_RC(wfs_snapshot_create(s, rosrc, &sopts, &sfail), -EIO);
        CHECK(sfail == 0);
        size_t snaps_after = 0;
        CHECK_OK(wfs_snapshot_list(s, NULL, 0, &snaps_after));
        CHECK(snaps_after == snaps_before);                      // no row
        CHECK(n_with_prefix(snapdir, "S") == trees_before);      // no S<n>, no S<n>.wfs-tmp

        // A fork unwinds the same way: no tree at --to and no world row left behind.
        char wfail[4096];
        join(wfail, sizeof wfail, worlds, "w-ro-fail");
        memset(&opts, 0, sizeof opts);
        opts.name = "w-ro-fail";
        opts.no_pool = 1;
        wfs_id wfailid = 0;
        CHECK_RC(wfs_world_create(s, from_ro, wfail, &opts, &wfailid), -EIO);
        CHECK(!exists(wfail));
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);

        // And a pool fill drops the entry rather than parking a tree that is not a faithful
        // clone of the snapshot it claims to be one of.
        CHECK_OK(wfs_pool_drain(s, sro, &removed));
        CHECK_RC(wfs_pool_fill(s, sro, 1, &made), -EIO);
        CHECK(made == 0);
        CHECK_OK(wfs_pool_status(s, ps, 4, &pn));
        CHECK(pn == 0);
    }
    wfs_test_hardlink_restore_err = 0;
    // With the seam cleared the very same snapshot succeeds: nothing was poisoned on the way.
    {
        wfs_id sagain = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ro-again";
        CHECK_OK(wfs_snapshot_create(s, rosrc, &sopts, &sagain));
        CHECK_OK(wfs_snapshot_verify(s, sagain, &vr));
        CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0);
        CHECK_OK(wfs_snapshot_discard(s, sagain, 1, 0));
    }

    // ---- PR #1 review (18th round, P2): a hardlink group whose inode the source locked ----
    //
    // UF_IMMUTABLE and UF_APPEND are USER flags -- `chflags uchg` on a vendored tree, a release
    // directory, a fixture somebody froze -- and clonefile(2) copies them onto every name of the
    // clone. link(2) refuses an immutable or append-only source with EPERM, and rename(2)
    // refuses to replace an immutable target with EPERM, so neither half of the replay's
    // link+rename could run: the 4th round's lend unlocks the parent DIRECTORY and nothing else,
    // and since that round a replay error is fatal. `snapshot create`, `pool fill` and `fork`
    // therefore all refused a tree that is perfectly valid, with the errno of a flag the user
    // set on purpose. The file's own flags come off for exactly those two calls now and go back
    // on the way out -- once, because by then every name of the group is one inode.
    for (int pass = 0; pass < 2; ++pass) {
        const unsigned int uflag = (pass == 0) ? UF_IMMUTABLE : UF_APPEND;
        const char *tag = (pass == 0) ? "uchg" : "uappnd";
        char flsrc[4096], fldir[4096], flw[4096];
        snprintf(flsrc, sizeof flsrc, "%s/flag-%s", root, tag);
        CHECK(mkdir(flsrc, 0755) == 0);
        snprintf(fldir, sizeof fldir, "%s/d", flsrc);
        CHECK(mkdir(fldir, 0755) == 0);
        join(p, sizeof p, fldir, "a");
        write_file(p, "locked\n");
        join(q, sizeof q, fldir, "b");
        CHECK(link(p, q) == 0);
        CHECK(chflags(p, uflag) == 0);     // one inode, two names, and the flag is on the inode

        wfs_id sfl = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = tag;
        CHECK_OK(wfs_snapshot_create(s, flsrc, &sopts, &sfl));
        CHECK_OK(wfs_snapshot_info(s, sfl, &sr));
        CHECK(sr.hl_groups == 1 && sr.hl_external == 0 && sr.hardlinks == 2);
        CHECK_OK(wfs_snapshot_verify(s, sfl, &vr));
        CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0);
        CHECK(chmod(sr.path, 0700) == 0);             // look behind the gate
        join(p, sizeof p, sr.path, "d/a");
        join(q, sizeof q, sr.path, "d/b");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK(lstat(p, &st) == 0 && (st.st_flags & uflag) == uflag);   // exactly as it was
        CHECK(lstat(q, &st) == 0 && (st.st_flags & uflag) == uflag);
        join(p, sizeof p, sr.path, "d");
        CHECK(n_with_prefix(p, ".wfs-hl-") == 0);
        CHECK(chmod(sr.path, 0) == 0);

        // A fork off that snapshot rebuilds the pair, flag and all.
        snprintf(flw, sizeof flw, "%s/w-%s", worlds, tag);
        wfs_ref from_fl = {WFS_K_SNAPSHOT, sfl};
        memset(&opts, 0, sizeof opts);
        opts.name = tag;
        opts.no_pool = 1;
        memset(&hfr, 0, sizeof hfr);
        CHECK_OK(wfs_world_create_ex(s, from_fl, flw, &opts, &hfr));
        CHECK(hfr.hardlinks == 1);         // the one name relinked to its canonical file
        join(p, sizeof p, flw, "d/a");
        join(q, sizeof q, flw, "d/b");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK(lstat(p, &st) == 0 && (st.st_flags & uflag) == uflag);
        CHECK(lstat(q, &st) == 0 && (st.st_flags & uflag) == uflag);
        join(p, sizeof p, flw, "d");
        CHECK(n_with_prefix(p, ".wfs-hl-") == 0);

        // And so does the pool filler, which replays on the entry it clones.
        CHECK_OK(wfs_pool_fill(s, sfl, 1, &made));
        CHECK(made == 1);
        snprintf(flw, sizeof flw, "%s/w-%s-pool", worlds, tag);
        memset(&opts, 0, sizeof opts);
        opts.name = tag;
        memset(&hfr, 0, sizeof hfr);
        CHECK_OK(wfs_world_create_ex(s, from_fl, flw, &opts, &hfr));
        CHECK(hfr.from_pool == 1);
        join(p, sizeof p, flw, "d/a");
        join(q, sizeof q, flw, "d/b");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK(lstat(p, &st) == 0 && (st.st_flags & uflag) == uflag);
    }

    // ---- PR #1 review (23rd round, P2): a member we could not look at is not one that is gone ----
    //
    // The replay counts a member whose fstatat/openat failed as `missing` and carries on, and
    // `missing` is tolerated on purpose: a live source may have changed between the scan and
    // the clone (5th round). But a fork from an IMMUTABLE snapshot has no such excuse, and an
    // EACCES or an EIO is not a changed source in any case -- it is a lookup that did not
    // happen. The clone was published with the group's names on separate inodes, the fork
    // returned 0, and nothing downstream ever looks at `broken`.
    //
    // The seam shuts the clone's `d` in the window between the clone and the replay, which is
    // the only way to produce that failure on a developer's disk.
    {
        char hlsrc[4096], hlw[4096];
        size_t worlds_before = 0, worlds_after = 0;
        join(hlsrc, sizeof hlsrc, root, "hl-shut");
        CHECK(mkdir(hlsrc, 0755) == 0);
        join(p, sizeof p, hlsrc, "d");
        CHECK(mkdir(p, 0755) == 0);
        join(p, sizeof p, hlsrc, "d/a");
        write_file(p, "pair\n");
        join(q, sizeof q, hlsrc, "d/b");
        CHECK(link(p, q) == 0);

        wfs_id shl = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hl-shut";
        CHECK_OK(wfs_snapshot_create(s, hlsrc, &sopts, &shl));
        CHECK_OK(wfs_snapshot_info(s, shl, &sr));
        CHECK(sr.hl_groups == 1 && sr.hl_external == 0 && sr.hardlinks == 2);

        wfs_ref from_hl = {WFS_K_SNAPSHOT, shl};
        join(hlw, sizeof hlw, worlds, "w-hl-shut");
        memset(&opts, 0, sizeof opts);
        opts.name = "w-hl-shut";
        opts.no_pool = 1;
        CHECK_OK(wfs_world_list(s, 1, NULL, 0, &worlds_before));
        wfs_test_before_hl_replay = hl_shut_dir;
        wfs_id hlid = 0;
        CHECK_RC(wfs_world_create(s, from_hl, hlw, &opts, &hlid), -EACCES);
        wfs_test_before_hl_replay = NULL;
        CHECK(hlid == 0);
        CHECK(!exists(hlw));                              // nothing at --to
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);  // and no temp tree left of it
        CHECK_OK(wfs_world_list(s, 1, NULL, 0, &worlds_after));
        CHECK(worlds_after == worlds_before);             // nor a row

        // With the directory left alone the very same fork succeeds and the pair is a pair.
        memset(&hfr, 0, sizeof hfr);
        CHECK_OK(wfs_world_create_ex(s, from_hl, hlw, &opts, &hfr));
        CHECK(hfr.hardlinks == 1);
        join(p, sizeof p, hlw, "d/a");
        join(q, sizeof q, hlw, "d/b");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
    }

    // ---- PR #1 review (P1): a fork in flight and `discard S<n>` cannot both win ----
    //
    // The window the review found: a pool-backed fork claims the last entry and pauses before it
    // has published its world; `discard` sees no active world and no pool entry, trashes the
    // snapshot, and the world that appears a moment later has no baseline to diff or verify
    // against. The claim now commits the fork's CREATING world row in its own transaction and
    // the discard counts those, under the same write lock, so one of the two has to lose -- and
    // it is never the fork that already holds the tree.
    {
        char rstore[4096], rsrc[4096], rp[4096], rw[4096];
        join(rstore, sizeof rstore, root, "race-store");
        join(rsrc, sizeof rsrc, root, "race-src");
        CHECK(mkdir(rsrc, 0755) == 0);
        join(rp, sizeof rp, rsrc, "a.txt");
        write_file(rp, "baseline\n");
        wfs_store *rs = NULL;
        CHECK_OK(wfs_store_open(rstore, &rs));
        wfs_id rsid = 0;
        wfs_snapshot_opts ropts;
        memset(&ropts, 0, sizeof ropts);
        ropts.name = "race";
        CHECK_OK(wfs_snapshot_create(rs, rsrc, &ropts, &rsid));
        uint64_t rmade = 0;
        CHECK_OK(wfs_pool_fill(rs, rsid, 1, &rmade));
        CHECK(rmade == 1);

        g_race_store = rs;
        g_race_snap = rsid;
        g_race_ran = 0;
        g_race_rc = 0;
        wfs_test_after_pool_claim = race_discard;
        join(rw, sizeof rw, worlds, "w-race");
        wfs_fork_result rfr;
        memset(&rfr, 0, sizeof rfr);
        memset(&opts, 0, sizeof opts);
        opts.name = "w-race";
        wfs_ref from_rs = {WFS_K_SNAPSHOT, rsid};
        CHECK_OK(wfs_world_create_ex(rs, from_rs, rw, &opts, &rfr));
        wfs_test_after_pool_claim = NULL;
        // The fork did come out of the pool (so the claim really was the last thing between the
        // pool and the world), and the discard really did run in the middle of it.
        CHECK(g_race_ran == 1 && rfr.from_pool == 1 && rfr.world != 0);
        CHECK_RC(g_race_rc, WFS_E_SNAPSHOT_IN_USE);
        // The snapshot is untouched, so the world that was being forked has its baseline.
        wfs_snapshot_rec rsr;
        CHECK_OK(wfs_snapshot_info(rs, rsid, &rsr));
        CHECK(rsr.state == WFS_ST_ACTIVE && exists(rsr.path));
        wfs_world_rec rwr;
        CHECK_OK(wfs_world_info(rs, rfr.world, &rwr));
        CHECK(rwr.state == WFS_ST_ACTIVE && rwr.snapshot_id == rsid && rwr.present);
        join(rp, sizeof rp, rw, "a.txt");
        CHECK(exists(rp));
        // And the refusal was about the fork, not a row it left behind: once the world is gone
        // the discard goes through.
        CHECK_OK(wfs_world_discard(rs, rfr.world, 1, 0));

        // PR #1 review (P2): --now means for a snapshot what it means for a world. The tree is
        // gone when the call returns, the row is DEAD, and the trash is empty afterwards.
        char sdir[4096];
        snprintf(sdir, sizeof sdir, "%s/snapshots/S%llu", rstore, (unsigned long long)rsid);
        CHECK(exists(sdir));
        CHECK_OK(wfs_snapshot_discard(rs, rsid, 1, 0));
        CHECK(!exists(sdir));
        CHECK_OK(wfs_snapshot_info(rs, rsid, &rsr));
        CHECK(rsr.state == WFS_ST_DEAD);
        wfs_trash_stat rts;
        memset(&rts, 0, sizeof rts);
        CHECK_OK(wfs_gc_status(rs, 0, &rts));
        CHECK(rts.entries == 0 && rts.snapshots == 0);
        // Without --now it is still a rename into the trash and nothing more.
        wfs_id rsid2 = 0;
        ropts.name = "race-later";
        CHECK_OK(wfs_snapshot_create(rs, rsrc, &ropts, &rsid2));
        CHECK_OK(wfs_snapshot_discard(rs, rsid2, 0, 0));
        CHECK_OK(wfs_snapshot_info(rs, rsid2, &rsr));
        CHECK(rsr.state == WFS_ST_TRASHED);
        memset(&rts, 0, sizeof rts);
        CHECK_OK(wfs_gc_status(rs, 0, &rts));
        CHECK(rts.entries == 1 && rts.snapshots == 1);

        // PR #1 review (3rd round): a snapshot whose tree is already gone. There is nothing to
        // move into the trash, so the row used to be committed TRASHED with an empty trash_path
        // -- invisible to trash_scan (which skips a row with no path) and to reconciliation
        // (which only looks at ACTIVE rows), i.e. trashed forever, with `status` saying so. It
        // is reconciled straight to DEAD now.
        wfs_id rsid3 = 0;
        ropts.name = "race-gone";
        CHECK_OK(wfs_snapshot_create(rs, rsrc, &ropts, &rsid3));
        char gdir[4096];
        snprintf(gdir, sizeof gdir, "%s/snapshots/S%llu", rstore, (unsigned long long)rsid3);
        CHECK(exists(gdir));
        rm_rf(gdir);                                   // somebody deleted it behind the store
        CHECK_OK(wfs_snapshot_discard(rs, rsid3, 0, 0));   // no --now: the old stuck path
        CHECK_OK(wfs_snapshot_info(rs, rsid3, &rsr));
        CHECK(rsr.state == WFS_ST_DEAD);
        wfs_store_stat rss;
        CHECK_OK(wfs_store_status(rs, &rss));
        CHECK(rss.snapshots_trashed == 1);             // rsid2's, and only rsid2's
        memset(&rts, 0, sizeof rts);
        CHECK_OK(wfs_gc_status(rs, 0, &rts));
        CHECK(rts.entries == 1 && rts.snapshots == 1);
        wfs_store_close(rs);
    }

    // ---- PR #1 review (16th round, P2): the unwind of a pool hand-out is a reference too ----
    //
    // The other half of the window the first round closed. The claim commits the fork's CREATING
    // world row, so `discard S<n>` can see a fork that has taken an entry -- but the unwind of a
    // hand-out that then failed used to delete that row first and put the entry back afterwards,
    // in two transactions. In between, the reference count saw neither the world nor the entry:
    // the discard committed the snapshot in WFS_ST_TRASHING, and pool_return() then inserted a
    // READY entry for a snapshot on its way to the trash -- a full stale clone that the next
    // fork would take as a live baseline and that pool_collect() only buries a wake later. Row
    // out and entry in are one transaction now, so there is no instant at which the snapshot has
    // no reference at all.
    {
        char ustore[4096], usrc[4096], up[4096], uw[4096];
        join(ustore, sizeof ustore, root, "unwind-store");
        join(usrc, sizeof usrc, root, "unwind-src");
        CHECK(mkdir(usrc, 0755) == 0);
        join(up, sizeof up, usrc, "a.txt");
        write_file(up, "baseline\n");
        wfs_store *us = NULL;
        CHECK_OK(wfs_store_open(ustore, &us));
        wfs_id usid = 0;
        wfs_snapshot_opts uopts;
        memset(&uopts, 0, sizeof uopts);
        uopts.name = "unwind";
        CHECK_OK(wfs_snapshot_create(us, usrc, &uopts, &usid));
        uint64_t umade = 0;
        CHECK_OK(wfs_pool_fill(us, usid, 1, &umade));
        CHECK(umade == 1);

        join(uw, sizeof uw, worlds, "w-unwind");
        snprintf(unwind_target, sizeof unwind_target, "%s", uw);
        g_unwind_store = us;
        g_unwind_snap = usid;
        g_unwind_ran = 0;
        g_unwind_rc = 0;
        wfs_test_after_pool_claim = unwind_block_target;   // the hand-out's rename now fails
        wfs_test_in_pool_unwind = unwind_discard;          // ... and the discard runs in there
        memset(&opts, 0, sizeof opts);
        opts.name = "w-unwind";
        wfs_ref from_us = {WFS_K_SNAPSHOT, usid};
        // The fork fails, as it must: the directory that appeared at --to is never renamed over.
        wfs_id uwid = 0;
        CHECK(wfs_world_create(us, from_us, uw, &opts, &uwid) != 0);
        wfs_test_after_pool_claim = NULL;
        wfs_test_in_pool_unwind = NULL;
        CHECK(g_unwind_ran == 1);
        // The fork was still holding the snapshot when the discard asked, so the discard lost.
        CHECK_RC(g_unwind_rc, WFS_E_SNAPSHOT_IN_USE);
        wfs_snapshot_rec usr;
        CHECK_OK(wfs_snapshot_info(us, usid, &usr));
        CHECK(usr.state == WFS_ST_ACTIVE && exists(usr.path));
        // And the entry is back in the pool with a row: not a stale clone of a trashed
        // snapshot, and not a row-less tree either.
        uint64_t uready = 0;
        CHECK_OK(wfs_pool_ready(us, usid, &uready));
        CHECK(uready == 1);
        char udir[4096];
        snprintf(udir, sizeof udir, "%s/pool/S%llu", ustore, (unsigned long long)usid);
        char unames[8][256];
        uint64_t uinos[8];
        CHECK(list_dir(udir, unames, uinos, 8) == 1);
        // It is an ordinary pool entry again: a refusal without --force, drained with it, and
        // nothing the failed fork left behind refusing on its own account.
        CHECK_RC(wfs_snapshot_discard(us, usid, 0, 0), WFS_E_SNAPSHOT_IN_USE);
        CHECK_OK(wfs_snapshot_discard(us, usid, 1, 1));
        CHECK(list_dir(udir, unames, uinos, 8) == 0);
        CHECK(!exists(usr.path));
        wfs_store_close(us);
        rm_rf(uw);
    }

    // ---- PR #1 review (18th round, P2): a pool hand-out whose rollback cannot run ----
    //
    // The tail of a pool-backed fork is marker, rename, stat, row UPDATE. When the stat or the
    // UPDATE fails the tree is already at the user's --to, so the unwind renames it back under
    // its pool name first -- and that rename's result was dropped on the floor. pool_return()
    // then stat'ed a pool path with nothing at it, came back -ENOENT, and the caller read that
    // as "the entry did not go back, so the row goes on its own" and deleted the CREATING row:
    // a published tree at --to, with a `.world` marker in it, that nothing in the store knew
    // about -- not gc's abandoned-fork sweep, which works from CREATING rows, and not a
    // `world fs adopt`, which needs the marker to name a world this store has. The rollback is
    // checked now: only a rollback that really happened goes on into pool_return, and one that
    // did not keeps the row and points its tmp_path at the tree it left behind.
    {
        char pstore[4096], psrc[4096], pp[4096], ptgt[4096], pmoved[4096], pdb[4096];
        join(pstore, sizeof pstore, root, "publish-store");
        join(psrc, sizeof psrc, root, "publish-src");
        CHECK(mkdir(psrc, 0755) == 0);
        join(pp, sizeof pp, psrc, "a.txt");
        write_file(pp, "baseline\n");
        wfs_store *pst = NULL;
        CHECK_OK(wfs_store_open(pstore, &pst));
        wfs_id psid = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "publish";
        CHECK_OK(wfs_snapshot_create(pst, psrc, &sopts, &psid));
        uint64_t pmade = 0;
        CHECK_OK(wfs_pool_fill(pst, psid, 1, &pmade));
        CHECK(pmade == 1);

        join(ptgt, sizeof ptgt, worlds, "w-publish");
        snprintf(pmoved, sizeof pmoved, "%s-moved", ptgt);
        snprintf(publish_target, sizeof publish_target, "%s", ptgt);
        snprintf(publish_moved, sizeof publish_moved, "%s", pmoved);
        g_publish_ran = 0;
        g_publish_world = 0;
        wfs_test_after_pool_publish = publish_move_away;
        memset(&opts, 0, sizeof opts);
        opts.name = "w-publish";
        wfs_ref from_pst = {WFS_K_SNAPSHOT, psid};
        wfs_id pwid = 0;
        // The stat of --to then answers ENOENT, and so does the rollback rename.
        CHECK(wfs_world_create(pst, from_pst, ptgt, &opts, &pwid) != 0);
        wfs_test_after_pool_publish = NULL;
        CHECK(g_publish_ran == 1 && g_publish_world != 0);
        CHECK(!exists(ptgt) && exists(pmoved));

        // The row is still there, still CREATING, and it names the tree the fork published.
        wfs_world_rec pwr;
        CHECK_OK(wfs_world_info(pst, g_publish_world, &pwr));
        CHECK(pwr.state == WFS_ST_CREATING);
        char tmp_path[4096], sql[512];
        join(pdb, sizeof pdb, pstore, "metadata.db");
        snprintf(sql, sizeof sql, "SELECT tmp_path FROM worlds WHERE id=%llu",
                 (unsigned long long)g_publish_world);
        CHECK(db_query_text(pdb, sql, tmp_path, sizeof tmp_path) == 1);
        CHECK(!strcmp(tmp_path, ptgt));        // the tree's last known name, not the pool's
        // And no entry went back into the pool: its tree is not in the pool any more.
        uint64_t pready = 0;
        CHECK_OK(wfs_pool_ready(pst, psid, &pready));
        CHECK(pready == 0);
        char pooldir[4096];
        snprintf(pooldir, sizeof pooldir, "%s/pool/S%llu", pstore, (unsigned long long)psid);
        char pnames[8][256];
        uint64_t pinos[8];
        CHECK(list_dir(pooldir, pnames, pinos, 8) == 0);
        // The snapshot is still referenced by that CREATING row, so it is still in use.
        CHECK_RC(wfs_snapshot_discard(pst, psid, 0, 0), WFS_E_SNAPSHOT_IN_USE);

        // What resolves it: the tree kept its marker, so `world fs adopt` registers it.
        wfs_id padopted = 0;
        CHECK_OK(wfs_world_adopt(pst, pmoved, "w-publish", &padopted));
        CHECK(padopted != 0 && padopted != g_publish_world);
        wfs_identity pident;
        CHECK_OK(wfs_world_verify(pst, padopted, &pident));
        CHECK(pident.registered == 1);
        rm_rf(pmoved);

        // The same failure with the tree still AT the target, which is the shape gc has to
        // recognise: the rollback rename cannot run because its whole parent directory is shut
        // (so is the stat that fails the hand-out in the first place), and what is left is a
        // published tree that only this CREATING row names. gc's abandoned-fork sweep asks the
        // marker who owns the tree and the row whether it still says CREATING with that
        // tmp_path -- both true here -- and removes it once its producer is gone.
        CHECK_OK(wfs_pool_fill(pst, psid, 1, &pmade));
        CHECK(pmade == 1);
        char ptgt2[4096];
        join(ptgt2, sizeof ptgt2, worlds, "w-publish-2");
        snprintf(publish_target, sizeof publish_target, "%s", ptgt2);
        snprintf(publish_shut, sizeof publish_shut, "%s", worlds);   // shut the parent instead
        g_publish_ran = 0;
        g_publish_world = 0;
        wfs_test_after_pool_publish = publish_shut_parent;
        wfs_test_fork_owner_pid = 2147480000;    // ... and the fork's producer is gone with it
        memset(&opts, 0, sizeof opts);
        opts.name = "w-publish-2";
        pwid = 0;
        CHECK(wfs_world_create(pst, from_pst, ptgt2, &opts, &pwid) != 0);
        wfs_test_after_pool_publish = NULL;
        wfs_test_fork_owner_pid = 0;
        CHECK(chmod(worlds, 0755) == 0);
        CHECK(g_publish_ran == 1 && g_publish_world != 0);
        CHECK(exists(ptgt2));                    // published, and never rolled back
        CHECK_OK(wfs_world_info(pst, g_publish_world, &pwr));
        CHECK(pwr.state == WFS_ST_CREATING);
        snprintf(sql, sizeof sql, "SELECT tmp_path FROM worlds WHERE id=%llu",
                 (unsigned long long)g_publish_world);
        CHECK(db_query_text(pdb, sql, tmp_path, sizeof tmp_path) == 1);
        CHECK(!strcmp(tmp_path, ptgt2));
        setenv("WORLD_GC_CREATING_MIN_AGE", "0", 1);
        wfs_gc_report prep;
        memset(&prep, 0, sizeof prep);
        CHECK_OK(wfs_gc(pst, 0, &prep));
        CHECK(prep.tmp_removed >= 1);
        CHECK(!exists(ptgt2));
        CHECK_OK(wfs_world_info(pst, g_publish_world, &pwr));
        CHECK(pwr.state == WFS_ST_DEAD);
        wfs_store_close(pst);
    }

    // ---- P13: a store from another schema is refused before anything is read ----
    char other[4096], ver[4096];
    join(other, sizeof other, root, "other-store");
    CHECK(mkdir(other, 0755) == 0);
    join(ver, sizeof ver, other, "VERSION");
    write_file(ver, "99\n");
    wfs_store *bad = NULL;
    CHECK_RC(wfs_store_open(other, &bad), WFS_E_SCHEMA);
    CHECK(bad == NULL);

    // ---- P17: trees in the store, no database -> refuse, never rebuild ----
    // A store whose metadata.db is gone still has its snapshot trees, and those trees are what
    // the database was the index of. Making a fresh one would hand out id 1 again and the next
    // `init` would write S1 over the `snapshots/S1` that is still there, so the open is refused.
    {
        char dstore[4096], dsrc[4096], dp[4096];
        join(dstore, sizeof dstore, root, "damaged-store");
        join(dsrc, sizeof dsrc, root, "damaged-src");
        CHECK(mkdir(dsrc, 0755) == 0);
        join(dp, sizeof dp, dsrc, "a.txt");
        write_file(dp, "one file is enough\n");
        wfs_store *d = NULL;
        CHECK_OK(wfs_store_open(dstore, &d));
        wfs_id dsid = 0;
        wfs_snapshot_opts dopts;
        memset(&dopts, 0, sizeof dopts);
        dopts.name = "damaged";
        CHECK_OK(wfs_snapshot_create(d, dsrc, &dopts, &dsid));
        wfs_store_close(d);

        // The snapshot root keeps its gate (0000) through all of this: finding the tree is a
        // readdir of snapshots/, which never has to step inside it.
        static const char *dbfiles[] = {"/metadata.db", "/metadata.db-wal", "/metadata.db-shm"};
        for (size_t i = 0; i < sizeof dbfiles / sizeof dbfiles[0]; ++i) {
            char q2[4096];
            snprintf(q2, sizeof q2, "%s%s", dstore, dbfiles[i]);
            unlink(q2);
        }
        d = NULL;
        CHECK_RC(wfs_store_open(dstore, &d), WFS_E_STORE_DAMAGED);
        CHECK(d == NULL);
        // ... and it did not quietly leave a new one behind.
        char dbp[4096];
        join(dbp, sizeof dbp, dstore, "metadata.db");
        CHECK(!exists(dbp));
        // The same verdict for a database that is there but is not one.
        write_file(dbp, "this is not a database\n");
        CHECK_RC(wfs_store_open(dstore, &d), WFS_E_STORE_DAMAGED);
        CHECK(d == NULL);
        // An empty store with no trees in it is not damaged, it is new.
        char fresh[4096];
        join(fresh, sizeof fresh, root, "fresh-store");
        wfs_store *f2 = NULL;
        CHECK_OK(wfs_store_open(fresh, &f2));
        wfs_store_close(f2);

        // ---- PR #1 review (13th round, P1): a scan that could not look is not an empty store --
        //
        // The guard's whole evidence is three readdirs. An opendir(2) that failed for any reason
        // other than ENOENT -- EACCES, EIO, a volume that is not mounted any more -- was skipped
        // in silence, so a store whose `snapshots/` could not be read came out "empty": the open
        // then created a fresh metadata.db and a fresh store id right beside the trees, and
        // every later open found that database and never ran the guard again. Only ENOENT means
        // "no such subtree" now; anything else fails the open with that errno.
        CHECK(unlink(dbp) == 0);
        char dsnaps[4096];
        join(dsnaps, sizeof dsnaps, dstore, "snapshots");
        CHECK(chmod(dsnaps, 0000) == 0);
        d = NULL;
        CHECK_RC(wfs_store_open(dstore, &d), -EACCES);
        CHECK(d == NULL);
        CHECK(!exists(dbp));   // and nothing was created beside the trees
        // With the mode back, the same store reports the damage it has, exactly as above.
        CHECK(chmod(dsnaps, 0755) == 0);
        CHECK_RC(wfs_store_open(dstore, &d), WFS_E_STORE_DAMAGED);
        CHECK(d == NULL);
        CHECK(!exists(dbp));

        char snap[4096];
        snprintf(snap, sizeof snap, "%s/snapshots/S%llu", dstore, (unsigned long long)dsid);
        CHECK(chmod(snap, 0700) == 0); // so the test's own rm_rf can clear it
    }

    // ---- PR #1 review (3rd round): a pthread_create that fails for one slot and not the next --
    //
    // Both parallel loops in the core wrote the handle to th[i] while `started` merely counted,
    // so a refused slot 0 followed by three successes made the join loop join th[0] (never
    // written) and never join the live worker in th[3] -- which is then free to keep reading a
    // RestoreJob on a stack frame that has already returned. They both go through
    // threads_start() now, which writes the handles it really started contiguously. With slot 0
    // refused, everything still has to come out exactly right, one worker fewer.
    {
        // The helper's own contract first: slot 0 refused, slots 1..3 taken. Three workers
        // start, three handles are written contiguously, all three join. The old loop wrote
        // th[1..3] and joined th[0..2]: one uninitialised join, one worker never joined.
        int started = 0, joined = 0;
        CHECK_OK(wfs_test_threads_start(4, 0x1u, &started, &joined));
        CHECK(started == 3 && joined == 3);
        CHECK_OK(wfs_test_threads_start(4, 0x5u, &started, &joined));   // slots 0 and 2
        CHECK(started == 2 && joined == 2);
        CHECK_OK(wfs_test_threads_start(4, 0u, &started, &joined));
        CHECK(started == 4 && joined == 4);

        char tsrc[4096], wth[4096];
        join(tsrc, sizeof tsrc, root, "threads");
        CHECK(mkdir(tsrc, 0755) == 0);
        for (int i = 0; i < 12; ++i) {   // 12 groups: past hardlinks.cpp's parallel threshold
            snprintf(p, sizeof p, "%s/g%d", tsrc, i);
            write_file(p, "linked\n");
            snprintf(q, sizeof q, "%s/g%d-b", tsrc, i);
            CHECK(link(p, q) == 0);
        }
        wfs_test_thread_fail_mask = 1;                 // slot 0 never starts
        wfs_id sth = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "threads";
        CHECK_OK(wfs_snapshot_create(s, tsrc, &sopts, &sth));   // the scan walk is 4 threads
        wfs_ref fth = {WFS_K_SNAPSHOT, sth};
        join(wth, sizeof wth, worlds, "wthreads");
        memset(&opts, 0, sizeof opts);
        wfs_id wthid = 0;
        CHECK_OK(wfs_world_create(s, fth, wth, &opts, &wthid));  // and the hardlink replay
        wfs_test_thread_fail_mask = 0;
        CHECK_OK(wfs_snapshot_verify(s, sth, &vr));
        for (int i = 0; i < 12; ++i) {
            snprintf(p, sizeof p, "%s/g%d", wth, i);
            snprintf(q, sizeof q, "%s/g%d-b", wth, i);
            CHECK(ino_of(p) == ino_of(q));
            CHECK(nlink_of(p) == 2);
        }
        CHECK(n_with_prefix(wth, ".wfs-hl-") == 0);
    }

    // ---- PR #1 review (5th round, P2): the source changing between the walk and the clone ----
    //
    // hardlinks_restore() was handed nullptr as its verify root here, which turns off the only
    // check that looks at the source at all. A group member replaced in that window by a
    // different file of the same size and the same mtime was therefore linked to the canonical
    // file in the clone -- the snapshot came out holding one name's contents under both names,
    // and said nothing about it. The live source is the verify root now, and the group the
    // source broke stays broken in the clone and is dropped from what the snapshot claims.
    {
        char hsrc[4096], ha[4096], hb[4096], hw[4096];
        join(hsrc, sizeof hsrc, root, "hl-race");
        CHECK(mkdir(hsrc, 0755) == 0);
        join(ha, sizeof ha, hsrc, "a.txt");
        join(hb, sizeof hb, hsrc, "b.txt");
        write_file(ha, "AAAA\n");
        CHECK(link(ha, hb) == 0);
        CHECK(ino_of(ha) == ino_of(hb));

        g_swap_path = hb;
        g_swap_peer = ha;
        g_swap_hits = 0;
        wfs_test_before_snapshot_clone = swap_member;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlrace";
        wfs_id hs = 0;
        CHECK_OK(wfs_snapshot_create(s, hsrc, &sopts, &hs));
        wfs_test_before_snapshot_clone = NULL;
        g_swap_path = NULL;
        CHECK(g_swap_hits == 1);
        // The source, as the seam left it: two files, same size, same mtime, different bytes.
        CHECK(ino_of(ha) != ino_of(hb));
        {
            struct stat sa, sb;
            CHECK(lstat(ha, &sa) == 0 && lstat(hb, &sb) == 0);
            CHECK(sa.st_size == sb.st_size);
        }
        // The snapshot: the two names are still two files, and it does not advertise a group it
        // does not have -- a fork replays the manifest without a verify root.
        wfs_snapshot_rec hr;
        CHECK_OK(wfs_snapshot_info(s, hs, &hr));
        CHECK(hr.hardlinks == 2);     // what the source had when it was walked
        CHECK(hr.hl_groups == 0);     // what this tree actually has
        CHECK(chmod(hr.path, 0700) == 0);
        join(p, sizeof p, hr.path, "a.txt");
        join(q, sizeof q, hr.path, "b.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "AAAA\n"));
        CHECK_OK(read_file(q, buf, sizeof buf));
        CHECK(!strcmp(buf, "BBBB\n"));
        CHECK(ino_of(p) != ino_of(q));
        CHECK(nlink_of(p) == 1 && nlink_of(q) == 1);
        CHECK(chmod(hr.path, 0000) == 0);
        CHECK_OK(wfs_snapshot_verify(s, hs, &vr));
        CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0 && vr.unprotected == 0);
        // And a fork from it inherits two files, not one file under two names.
        wfs_ref hf = {WFS_K_SNAPSHOT, hs};
        memset(&opts, 0, sizeof opts);
        join(hw, sizeof hw, worlds, "hlrace-w");
        wfs_id hwid = 0;
        CHECK_OK(wfs_world_create(s, hf, hw, &opts, &hwid));
        join(p, sizeof p, hw, "a.txt");
        join(q, sizeof q, hw, "b.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "AAAA\n"));
        CHECK_OK(read_file(q, buf, sizeof buf));
        CHECK(!strcmp(buf, "BBBB\n"));
        CHECK(ino_of(p) != ino_of(q));

        // The control: the same tree, nothing touching it, still gets its group back.
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlrace2";
        wfs_id hs2 = 0;
        CHECK(link(ha, hb) == -1);                     // b.txt is in the way
        CHECK(unlink(hb) == 0 && link(ha, hb) == 0);   // put the pair back
        CHECK_OK(wfs_snapshot_create(s, hsrc, &sopts, &hs2));
        CHECK_OK(wfs_snapshot_info(s, hs2, &hr));
        CHECK(hr.hl_groups == 1);
        CHECK(chmod(hr.path, 0700) == 0);
        join(p, sizeof p, hr.path, "a.txt");
        join(q, sizeof q, hr.path, "b.txt");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK(chmod(hr.path, 0000) == 0);
    }

    // ---- PR #1 review (5th round, P1): a discard killed between the rename and the commit ----
    //
    // The hole: the rename moved the tree into <store>/trash and the commit that was to name it
    // there never landed, so the row said ACTIVE (worlds) or was rolled back to ACTIVE
    // (snapshots) while the tree sat in the trash with nothing pointing at it. The collector's
    // row-less-orphan rule then deleted it on the next wake -- immediately, retention skipped,
    // restore impossible -- and for a snapshot that is the baseline every world forked from it
    // diffs and verifies against (P4/P10).
    //
    // Now the row is written first, in WFS_ST_TRASHING, with the name the tree is about to get,
    // and the recovery decides which of the two names the tree really has. Its own store, so the
    // retention-0 collections below cannot touch anything the rest of this file built.
    {
        char tstore[4096], tsrc[4096], tw[4096], twcopy[4096], sdir[4096];
        join(tstore, sizeof tstore, root, "trash-store");
        join(tsrc, sizeof tsrc, root, "trash-src");
        CHECK(mkdir(tsrc, 0755) == 0);
        join(p, sizeof p, tsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *ts = NULL;
        CHECK_OK(wfs_store_open(tstore, &ts));
        wfs_test_trash_crash = trash_crash;

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "tr";
        wfs_id t1 = 0;
        CHECK_OK(wfs_snapshot_create(ts, tsrc, &sopts, &t1));
        snprintf(sdir, sizeof sdir, "%s/snapshots/S%llu", tstore, (unsigned long long)t1);
        wfs_snapshot_rec tsr;
        wfs_trash_stat tst;
        wfs_gc_report trep;

        // (1) Killed BEFORE the rename: the discard did not happen, and the recovery says so.
        crash_at(0);
        CHECK_RC(wfs_snapshot_discard(ts, t1, 0, 0), -EINTR);
        CHECK(g_trash_crash_hits == 1);
        CHECK_OK(wfs_snapshot_info(ts, t1, &tsr));
        CHECK(tsr.state == WFS_ST_TRASHING);
        CHECK(exists(sdir));                                   // never moved
        CHECK_OK(wfs_gc_status(ts, 0, &tst));
        CHECK(tst.due == 0);                                   // and nothing to collect
        crash_at(-1);
        memset(&trep, 0, sizeof trep);
        CHECK_OK(wfs_gc(ts, 0, &trep));
        CHECK(trep.trash_orphans == 0);
        CHECK_OK(wfs_snapshot_info(ts, t1, &tsr));
        CHECK(tsr.state == WFS_ST_ACTIVE);
        CHECK(exists(sdir));
        CHECK_OK(wfs_snapshot_verify(ts, t1, &vr));            // and still a usable snapshot

        // (2) Killed AFTER the rename. `adopt` used to be the one way a reference could still
        // appear afterwards -- it registers a world from its marker alone and carries the
        // snapshot id over with it -- and since the 6th round of the review it refuses a
        // snapshot that is not ACTIVE, under the same write lock the discard decides under. So
        // the copy stays a copy, nothing references the snapshot, and the recovery finishes the
        // discard the user asked for instead of undoing it.
        wfs_ref tf = {WFS_K_SNAPSHOT, t1};
        memset(&opts, 0, sizeof opts);
        join(tw, sizeof tw, worlds, "tworld");
        join(twcopy, sizeof twcopy, worlds, "tworld-copy");
        wfs_id tw1 = 0;
        CHECK_OK(wfs_world_create(ts, tf, tw, &opts, &tw1));
        copy_dir(tw, twcopy);                                  // an unregistered copy (P2)
        CHECK_OK(wfs_world_discard(ts, tw1, 1, 0));            // no ACTIVE world left
        crash_at(1);
        CHECK_RC(wfs_snapshot_discard(ts, t1, 0, 0), -EINTR);
        CHECK(g_trash_crash_hits == 1);
        CHECK(!exists(sdir));                                  // the tree is in the trash
        CHECK(exists(g_trash_crash_path));
        CHECK_OK(wfs_snapshot_info(ts, t1, &tsr));
        CHECK(tsr.state == WFS_ST_TRASHING);
        // The regression itself: the collector used to call that directory a row-less orphan and
        // delete it on sight. It is in the trash, it is counted, and it is not due.
        CHECK_OK(wfs_gc_status(ts, 0, &tst));
        CHECK(tst.entries == 1 && tst.due == 0 && tst.snapshots == 1);
        // 6th round: a snapshot in the middle of being discarded is not a baseline anybody may
        // be given, because from here a TRASHING row that was killed and one whose next step is
        // `--now`'s unlink look exactly alike.
        wfs_id tw2 = 0;
        CHECK_RC(wfs_world_adopt(ts, twcopy, "adopted", &tw2), WFS_E_SOURCE_GONE);
        CHECK(tw2 == 0);
        wfs_world_rec twr;
        crash_at(-1);
        memset(&trep, 0, sizeof trep);
        CHECK_OK(wfs_gc(ts, 0, &trep));
        CHECK(trep.trash_orphans == 0 && trep.snapshots_deleted == 1);
        CHECK_OK(wfs_snapshot_info(ts, t1, &tsr));
        CHECK(tsr.state == WFS_ST_DEAD);
        CHECK(!exists(sdir) && !exists(g_trash_crash_path));
        // ... and the copy is no more adoptable once the snapshot is gone for good.
        CHECK_RC(wfs_world_adopt(ts, twcopy, "adopted", &tw2), WFS_E_SOURCE_GONE);

        // (3) A world, both ways round, and resolved by the store open rather than by gc.
        wfs_id t2 = 0;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "tr2";
        CHECK_OK(wfs_snapshot_create(ts, tsrc, &sopts, &t2));
        wfs_ref tf2 = {WFS_K_SNAPSHOT, t2};
        char tw3[4096];
        join(tw3, sizeof tw3, worlds, "tworld3");
        wfs_id tw3id = 0;
        CHECK_OK(wfs_world_create(ts, tf2, tw3, &opts, &tw3id));
        crash_at(0);
        CHECK_RC(wfs_world_discard(ts, tw3id, 0, 0), -EINTR);
        CHECK(exists(tw3));                                    // the rename never happened
        wfs_store_close(ts);
        CHECK_OK(wfs_store_open(tstore, &ts));                 // the open resolves it
        CHECK_OK(wfs_world_info(ts, tw3id, &twr));
        CHECK(twr.state == WFS_ST_ACTIVE && twr.present);
        // ... and after the rename it is finished, not lost: `restore` still brings it back.
        crash_at(1);
        CHECK_RC(wfs_world_discard(ts, tw3id, 0, 0), -EINTR);
        CHECK(!exists(tw3) && exists(g_trash_crash_path));
        CHECK_OK(wfs_world_info(ts, tw3id, &twr));
        CHECK(twr.state == WFS_ST_TRASHING);
        CHECK_OK(wfs_gc_status(ts, 0, &tst));
        CHECK(tst.entries == 1 && tst.due == 0 && tst.worlds == 1);
        crash_at(-1);
        wfs_store_close(ts);
        CHECK_OK(wfs_store_open(tstore, &ts));
        CHECK_OK(wfs_world_info(ts, tw3id, &twr));
        CHECK(twr.state == WFS_ST_TRASHED);
        CHECK_OK(wfs_world_restore(ts, tw3id));
        CHECK_OK(wfs_world_info(ts, tw3id, &twr));
        CHECK(twr.state == WFS_ST_ACTIVE && twr.present && exists(tw3));

        wfs_test_trash_crash = NULL;
        wfs_store_close(ts);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", tstore, (unsigned long long)t2);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (6th round, P1): `adopt` may not resurrect a discarded baseline ---------
    //
    // `wfs_world_adopt` registers a world from its `.world` marker alone: it reads the snapshot
    // id out of the marker and writes it onto a new ACTIVE row. It used to do that whatever had
    // become of that snapshot, and not under the write lock `discard S<n>` counts references
    // under. So this sequence -- discard the only world, discard its snapshot (nothing
    // references it any more), adopt a copy of the world -- produced an ACTIVE world whose
    // baseline was in the trash or already unlinked: every `diff` it would ever answer is
    // WFS_E_SOURCE_GONE, and `gc` would go on believing the snapshot was nobody's source.
    //
    // The TRASHING window of the same race is in the 5th-round block above, on the crash seam.
    {
        char astore[4096], asrc[4096], aw[4096], acopy[4096], acopy2[4096];
        join(astore, sizeof astore, root, "adopt-store");
        join(asrc, sizeof asrc, root, "adopt-src");
        CHECK(mkdir(asrc, 0755) == 0);
        join(p, sizeof p, asrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *as = NULL;
        CHECK_OK(wfs_store_open(astore, &as));

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ad";
        wfs_id a1 = 0;
        CHECK_OK(wfs_snapshot_create(as, asrc, &sopts, &a1));
        wfs_ref af = {WFS_K_SNAPSHOT, a1};
        memset(&opts, 0, sizeof opts);
        join(aw, sizeof aw, worlds, "aworld");
        join(acopy, sizeof acopy, worlds, "aworld-copy");
        join(acopy2, sizeof acopy2, worlds, "aworld-copy2");
        wfs_id aw1 = 0;
        CHECK_OK(wfs_world_create(as, af, aw, &opts, &aw1));
        copy_dir(aw, acopy);                                   // two unregistered copies (P2)
        copy_dir(aw, acopy2);

        // The control: while the snapshot is ACTIVE, adopting a copy works and the adopted world
        // really does inherit the baseline -- which is why it matters that the baseline is there.
        wfs_id ac = 0;
        wfs_world_rec awr;
        CHECK_OK(wfs_world_adopt(as, acopy, "copy-ok", &ac));
        CHECK_OK(wfs_world_info(as, ac, &awr));
        CHECK(awr.snapshot_id == a1 && awr.state == WFS_ST_ACTIVE);
        CHECK_OK(wfs_world_diff(as, ac, 0, NULL, NULL));
        // An adopted world is a reference like any other: the snapshot cannot be discarded while
        // it is alive. (That is the other half of the same invariant.)
        CHECK_RC(wfs_snapshot_discard(as, a1, 0, 0), WFS_E_SNAPSHOT_IN_USE);

        // Now take every reference away and discard the snapshot the ordinary way: the row is
        // TRASHED and the tree is waiting in <store>/trash for the collector.
        CHECK_OK(wfs_world_discard(as, ac, 1, 0));
        CHECK_OK(wfs_world_discard(as, aw1, 1, 0));
        CHECK_OK(wfs_snapshot_discard(as, a1, 0, 0));
        wfs_snapshot_rec asr;
        CHECK_OK(wfs_snapshot_info(as, a1, &asr));
        CHECK(asr.state == WFS_ST_TRASHED);
        wfs_id ax = 0;
        CHECK_RC(wfs_world_adopt(as, acopy2, "no-baseline", &ax), WFS_E_SOURCE_GONE);
        CHECK(ax == 0);
        // Nothing was written down either: no row, and the copy is still an unregistered copy.
        wfs_identity aid;
        CHECK_RC(wfs_world_verify_identity(as, acopy2, &aid), WFS_E_UNREGISTERED);
        size_t an = 0;
        CHECK_OK(wfs_world_list(as, 0, NULL, 0, &an));
        CHECK(an == 0);                                        // no ACTIVE world appeared

        // And the same from the other end of the discard: --now, which leaves the row DEAD and
        // no tree at all.
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ad2";
        wfs_id a2 = 0;
        CHECK_OK(wfs_snapshot_create(as, asrc, &sopts, &a2));
        wfs_ref af2 = {WFS_K_SNAPSHOT, a2};
        char aw2[4096], acopy3[4096];
        join(aw2, sizeof aw2, worlds, "aworld2");
        join(acopy3, sizeof acopy3, worlds, "aworld2-copy");
        wfs_id aw2id = 0;
        CHECK_OK(wfs_world_create(as, af2, aw2, &opts, &aw2id));
        copy_dir(aw2, acopy3);
        CHECK_OK(wfs_world_discard(as, aw2id, 1, 0));
        CHECK_OK(wfs_snapshot_discard(as, a2, 1, 0));
        CHECK_OK(wfs_snapshot_info(as, a2, &asr));
        CHECK(asr.state == WFS_ST_DEAD);
        CHECK_RC(wfs_world_adopt(as, acopy3, "no-baseline", &ax), WFS_E_SOURCE_GONE);
        CHECK(ax == 0);

        // A copy of a world belonging to a *different* store is not affected: there is no parent
        // row for it here, so it is adopted with no baseline at all (snapshot_id 0) rather than
        // with a dead one, exactly as before. The refusal is about a baseline this store knows
        // and has thrown away, not about every marker that mentions a snapshot id.
        char foreign[4096];
        join(foreign, sizeof foreign, worlds, "foreign-copy");
        copy_dir(w1path, foreign);
        wfs_id afid = 0;
        CHECK_OK(wfs_world_adopt(as, foreign, "foreign", &afid));
        CHECK_OK(wfs_world_info(as, afid, &awr));
        CHECK(awr.snapshot_id == 0 && awr.parent_world == 0 && awr.state == WFS_ST_ACTIVE);
        wfs_store_close(as);
    }

    // ---- PR #1 review (8th round, P2): a manifest that stops mid-group is damage --------------
    //
    // The `#hl` header carries the group and name counts of the section under it, and the reader
    // parsed them and kept only the external ones. So a manifest whose last member never reached
    // the disk -- a truncated write, a full disk, a store somebody has been editing -- read back
    // with the full group count (which is all the fork and the pool filler checked) and one group
    // holding a single name. A group of one name is replayed as nothing at all: the fork was
    // published with two independent files where the snapshot records one inode under two names,
    // silently, and nothing downstream reads the manifest again to notice.
    {
        char tstore[4096], tsrc2[4096], tman[4096], tbak[4096], tw[4096], tlk[4096];
        join(tstore, sizeof tstore, root, "hltrunc-store");
        join(tsrc2, sizeof tsrc2, root, "hltrunc-src");
        CHECK(mkdir(tsrc2, 0755) == 0);
        join(p, sizeof p, tsrc2, "a.txt");
        write_file(p, "linked\n");
        join(tlk, sizeof tlk, tsrc2, "b.txt");
        CHECK(link(p, tlk) == 0);
        wfs_store *ts2 = NULL;
        CHECK_OK(wfs_store_open(tstore, &ts2));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hltrunc";
        wfs_id t1 = 0;
        CHECK_OK(wfs_snapshot_create(ts2, tsrc2, &sopts, &t1));
        CHECK_OK(wfs_snapshot_info(ts2, t1, &sr));
        CHECK(sr.hl_groups == 1 && sr.hardlinks == 2);
        snprintf(tman, sizeof tman, "%s/snapshots/S%llu/manifest", tstore, (unsigned long long)t1);
        join(tbak, sizeof tbak, root, "hltrunc.bak");
        copy_file(tman, tbak);

        wfs_ref tf = {WFS_K_SNAPSHOT, t1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(tw, sizeof tw, worlds, "hltrunc-w");
        wfs_id tw1 = 0;
        wfs_verify_report tvr;
        uint64_t tmade = 0, tready = 0;

        // Three ways for the section to stop describing itself: the last member gone (the group
        // count still says 1), the header gone, and a header that claims a group too many.
        for (int kind = 0; kind < 3; ++kind) {
            copy_file(tbak, tman);
            rewrite_manifest(tman, kind == 0, kind == 1, kind == 2 ? 2 : 0);
            memset(&tvr, 0, sizeof tvr);
            CHECK_RC(wfs_snapshot_verify(ts2, t1, &tvr), WFS_E_SNAPSHOT_DIRTY);
            CHECK(tvr.modified == 1 && strstr(tvr.first_bad, "manifest"));
            CHECK_RC(wfs_world_create(ts2, tf, tw, &opts, &tw1), WFS_E_SNAPSHOT_DIRTY);
            CHECK(!exists(tw));                                  // nothing published at --to
            CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);     // and no clone left behind
            tmade = 1;
            CHECK_RC(wfs_pool_fill(ts2, t1, 1, &tmade), WFS_E_SNAPSHOT_DIRTY);
            CHECK(tmade == 0);
            tready = 1;
            CHECK_OK(wfs_pool_ready(ts2, t1, &tready));
            CHECK(tready == 0);
        }

        // And the manifest as the snapshot wrote it still forks, with the pair one inode again:
        // the refusal is about the damage, not about hardlinked snapshots.
        copy_file(tbak, tman);
        CHECK_OK(wfs_snapshot_verify(ts2, t1, &tvr));
        CHECK_OK(wfs_world_create(ts2, tf, tw, &opts, &tw1));
        join(p, sizeof p, tw, "a.txt");
        join(q, sizeof q, tw, "b.txt");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK_OK(wfs_world_discard(ts2, tw1, 1, 0));
        tmade = 0;
        CHECK_OK(wfs_pool_fill(ts2, t1, 1, &tmade));
        CHECK(tmade == 1);
        wfs_store_close(ts2);
    }

    // ---- PR #1 review (7th round, P1): `restore W<n>` and `discard S<n>` cannot both win -----
    //
    // `restore` used to read "the baseline is still ACTIVE" in a standalone SELECT, rename the
    // tree out of the trash, and mark the row ACTIVE in a transaction of its own. `discard S<n>`
    // counts references under BEGIN IMMEDIATE and refuses ACTIVE worlds, CREATING worlds and
    // pool entries -- and a world that is still TRASHED is none of those. So a discard landing
    // in that window was allowed, and the restore then published a live world whose baseline was
    // in the trash: `diff` and `verify` answer WFS_E_SOURCE_GONE for the rest of its life.
    //
    // Now the restore is the discard's own three-step protocol run backwards, and the world's
    // WFS_ST_TRASHING row -- the one the rename happens under -- is a hard reference. Exactly
    // one of the two operations can succeed, and which one is decided by the write lock.
    {
        char rstore[4096], rsrc[4096], rw[4096];
        join(rstore, sizeof rstore, root, "restore-store");
        join(rsrc, sizeof rsrc, root, "restore-src");
        CHECK(mkdir(rsrc, 0755) == 0);
        join(p, sizeof p, rsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *rs = NULL;
        CHECK_OK(wfs_store_open(rstore, &rs));

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "rb";
        wfs_id r1 = 0;
        CHECK_OK(wfs_snapshot_create(rs, rsrc, &sopts, &r1));
        wfs_ref rf = {WFS_K_SNAPSHOT, r1};
        memset(&opts, 0, sizeof opts);
        join(rw, sizeof rw, worlds, "rworld");
        wfs_id rw1 = 0;
        CHECK_OK(wfs_world_create(rs, rf, rw, &opts, &rw1));
        wfs_world_rec rwr;
        wfs_snapshot_rec rsnr;

        // (1) The race itself, driven from inside the window: the world is in the trash, the
        // restore commits its TRASHING row, and right there a whole `discard S<n>` runs.
        CHECK_OK(wfs_world_discard(rs, rw1, 0, 0));
        CHECK_OK(wfs_world_info(rs, rw1, &rwr));
        CHECK(rwr.state == WFS_ST_TRASHED && !exists(rw));
        g_race_store = rs;
        g_race_snap = r1;
        g_race_rc = 0;
        g_race_ran = 0;
        wfs_test_trash_crash = restore_race;
        CHECK_OK(wfs_world_restore(rs, rw1));
        wfs_test_trash_crash = NULL;
        CHECK(g_race_ran == 1);
        CHECK(g_race_rc == WFS_E_SNAPSHOT_IN_USE);   // the discard was the one that had to lose
        CHECK_OK(wfs_world_info(rs, rw1, &rwr));
        CHECK(rwr.state == WFS_ST_ACTIVE && rwr.present && exists(rw));
        CHECK_OK(wfs_snapshot_info(rs, r1, &rsnr));
        CHECK(rsnr.state == WFS_ST_ACTIVE);
        // The whole point of the baseline: the restored world can still be diffed against it.
        CHECK_OK(wfs_world_diff(rs, rw1, 0, NULL, NULL));
        CHECK_OK(wfs_snapshot_verify(rs, r1, &vr));
        CHECK(vr.missing == 0 && vr.modified == 0);

        // (2) The other order, and the other verdict: the discard gets there first, so the
        // restore is the one that is refused -- and refused without touching anything. The tree
        // stays in the trash (a TRASHED world whose tree is at trash_path reads as `present`),
        // the row stays TRASHED, and nothing has been published at the world's home path.
        CHECK_OK(wfs_world_discard(rs, rw1, 0, 0));
        CHECK_OK(wfs_snapshot_discard(rs, r1, 0, 0));
        CHECK_OK(wfs_snapshot_info(rs, r1, &rsnr));
        CHECK(rsnr.state == WFS_ST_TRASHED);
        CHECK_RC(wfs_world_restore(rs, rw1), WFS_E_SOURCE_GONE);
        CHECK_OK(wfs_world_info(rs, rw1, &rwr));
        CHECK(rwr.state == WFS_ST_TRASHED && rwr.present && !exists(rw));

        // (3) And the two kills. A fresh pair, because the one above has no baseline left.
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "rb2";
        wfs_id r2 = 0;
        CHECK_OK(wfs_snapshot_create(rs, rsrc, &sopts, &r2));
        wfs_ref rf2 = {WFS_K_SNAPSHOT, r2};
        char rw2[4096];
        join(rw2, sizeof rw2, worlds, "rworld2");
        wfs_id rw2id = 0;
        CHECK_OK(wfs_world_create(rs, rf2, rw2, &opts, &rw2id));
        CHECK_OK(wfs_world_discard(rs, rw2id, 0, 0));
        wfs_test_trash_crash = trash_crash;

        // Killed after the row and before the rename: the restore did not happen, and the
        // recovery says so -- the tree is where the discard put it and the row goes back to
        // TRASHED. A `restore` after that still works, which is the proof nothing was lost.
        crash_at(2);
        CHECK_RC(wfs_world_restore(rs, rw2id), -EINTR);
        CHECK(g_trash_crash_hits == 1);
        CHECK_OK(wfs_world_info(rs, rw2id, &rwr));
        CHECK(rwr.state == WFS_ST_TRASHING);
        CHECK(exists(g_trash_crash_path) && !exists(rw2));
        // And while it is in flight it is a reference: the baseline cannot be taken away.
        CHECK_RC(wfs_snapshot_discard(rs, r2, 0, 0), WFS_E_SNAPSHOT_IN_USE);
        crash_at(-1);
        wfs_store_close(rs);
        CHECK_OK(wfs_store_open(rstore, &rs));               // the open resolves it
        CHECK_OK(wfs_world_info(rs, rw2id, &rwr));
        CHECK(rwr.state == WFS_ST_TRASHED && rwr.present);
        CHECK(exists(g_trash_crash_path) && !exists(rw2));

        // Killed after the rename and before the commit: the restore did happen, and the
        // recovery finishes it rather than sending the tree back.
        crash_at(3);
        CHECK_RC(wfs_world_restore(rs, rw2id), -EINTR);
        CHECK(g_trash_crash_hits == 1);
        CHECK_OK(wfs_world_info(rs, rw2id, &rwr));
        CHECK(rwr.state == WFS_ST_TRASHING);
        CHECK(exists(rw2) && !exists(g_trash_crash_path));
        crash_at(-1);
        wfs_store_close(rs);
        CHECK_OK(wfs_store_open(rstore, &rs));
        CHECK_OK(wfs_world_info(rs, rw2id, &rwr));
        CHECK(rwr.state == WFS_ST_ACTIVE && rwr.present && exists(rw2));
        CHECK_OK(wfs_world_diff(rs, rw2id, 0, NULL, NULL));   // baseline intact, inode intact
        wfs_test_trash_crash = NULL;
        wfs_store_close(rs);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", rstore, (unsigned long long)r2);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (8th round, P1): a live discard is not a crashed discard ---------------
    //
    // The recovery reads one TRASHING row with lstat and decides: the tree is at home, so the
    // discard never happened -- ACTIVE, trash_path cleared. That is the right verdict for a
    // process that died there and the wrong one for a process that is simply between its step
    // (a) and its rename, and lstat cannot tell them apart. Every `world fs ...` invocation
    // opens the store and every open runs the recovery, so the second command in a script was
    // enough: it put the row back to ACTIVE, the discard then renamed the tree into the trash,
    // its step (c) matched no row, and it returned 0. What was left was an ACTIVE world (or
    // snapshot) whose tree is a row-less orphan in <store>/trash -- and a row-less orphan is
    // deleted by the next collector on sight, retention skipped, restore impossible. `restore`
    // had the mirror image: the row put back to TRASHED while the tree was renamed home.
    //
    // So a TRASHING row now carries its owner, exactly as a CREATING row carries its producer,
    // and the recovery skips a row whose owner is alive. Here the second process runs from
    // inside the window, on its own store handle, with a gc that holds nothing back.
    {
        char ostore[4096], osrc[4096], ow[4096], osdir[4096], otrash[4096];
        join(ostore, sizeof ostore, root, "owner-store");
        join(osrc, sizeof osrc, root, "owner-src");
        CHECK(mkdir(osrc, 0755) == 0);
        join(p, sizeof p, osrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *os = NULL;
        CHECK_OK(wfs_store_open(ostore, &os));
        snprintf(g_own_dir, sizeof g_own_dir, "%s", ostore);
        wfs_test_trash_crash = owner_race;
        wfs_world_rec owr;
        wfs_snapshot_rec osr;

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ow";
        wfs_id o1 = 0;
        CHECK_OK(wfs_snapshot_create(os, osrc, &sopts, &o1));
        wfs_ref ofrom = {WFS_K_SNAPSHOT, o1};
        memset(&opts, 0, sizeof opts);
        join(ow, sizeof ow, worlds, "oworld");
        wfs_id ow1 = 0;
        CHECK_OK(wfs_world_create(os, ofrom, ow, &opts, &ow1));

        // (1) `discard W<n>`, with the other process inside the window (phase 0: the row is
        // TRASHING, the tree is still at home).
        g_own_phase = 0;
        g_own_ran = 0;
        g_own_state = -1;
        CHECK_OK(wfs_world_discard(os, ow1, 0, 0));
        CHECK(g_own_ran == 1);
        CHECK(g_own_state == WFS_ST_TRASHING);   // the other process left it in flight
        CHECK_OK(wfs_world_info(os, ow1, &owr));
        CHECK(owr.state == WFS_ST_TRASHED && !exists(ow));
        // ... and the row and the tree agree: the trash entry is this row's, not an orphan, so
        // a collector that finds it inside the retention window leaves it alone.
        snprintf(otrash, sizeof otrash, "%s", g_trash_crash_path);
        wfs_store *ob = NULL;
        CHECK_OK(wfs_store_open(ostore, &ob));
        wfs_gc_report orep;
        memset(&orep, 0, sizeof orep);
        CHECK_OK(wfs_gc(ob, -1, &orep));         // the default retention: nothing is due
        CHECK(orep.trash_orphans == 0);
        wfs_store_close(ob);
        CHECK_OK(wfs_world_info(os, ow1, &owr));
        CHECK(owr.state == WFS_ST_TRASHED && owr.present);

        // ... and the world is still restorable, which is what an orphaned tree would have cost.
        g_own_phase = -1;
        CHECK_OK(wfs_world_restore(os, ow1));
        CHECK_OK(wfs_world_info(os, ow1, &owr));
        CHECK(owr.state == WFS_ST_ACTIVE && exists(ow));

        // (2) `restore W<n>`, the mirror image (phase 2: the row is TRASHING, the tree is still
        // in the trash). The other process used to put the row back to TRASHED -- and its gc
        // then deleted the tree out from under the restore.
        CHECK_OK(wfs_world_discard(os, ow1, 0, 0));
        g_own_phase = 2;
        g_own_ran = 0;
        g_own_state = -1;
        CHECK_OK(wfs_world_restore(os, ow1));
        CHECK(g_own_ran == 1);
        CHECK(g_own_state == WFS_ST_TRASHING);
        CHECK_OK(wfs_world_info(os, ow1, &owr));
        CHECK(owr.state == WFS_ST_ACTIVE && owr.present && exists(ow));
        CHECK_OK(wfs_world_diff(os, ow1, 0, NULL, NULL));

        // (3) `discard S<n>`, the same window on the snapshot side. The baseline of every world
        // forked from it, so this is the one that costs the most: an ACTIVE snapshot row whose
        // tree is an orphan in the trash.
        CHECK_OK(wfs_world_discard(os, ow1, 1, 0));   // --now: no reference left
        snprintf(osdir, sizeof osdir, "%s/snapshots/S%llu", ostore, (unsigned long long)o1);
        g_own_phase = 0;
        g_own_ran = 0;
        g_own_state = -1;
        CHECK_OK(wfs_snapshot_discard(os, o1, 0, 0));
        CHECK(g_own_ran == 1);
        CHECK(g_own_state == WFS_ST_TRASHING);
        CHECK_OK(wfs_snapshot_info(os, o1, &osr));
        CHECK(osr.state == WFS_ST_TRASHED && !exists(osdir));
        snprintf(otrash, sizeof otrash, "%s", g_trash_crash_path);
        CHECK(exists(otrash));
        CHECK_OK(wfs_store_open(ostore, &ob));
        memset(&orep, 0, sizeof orep);
        CHECK_OK(wfs_gc(ob, -1, &orep));
        CHECK(orep.trash_orphans == 0 && exists(otrash));
        wfs_store_close(ob);

        // (4) And the crash itself still recovers: the same window, with an owner that is not
        // running. This is the 5th-round case, and it has to keep working -- the ownership is
        // "somebody is on it", never "leave it alone for ever".
        wfs_test_trash_crash = trash_crash;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ow2";
        wfs_id o2 = 0;
        CHECK_OK(wfs_snapshot_create(os, osrc, &sopts, &o2));
        wfs_ref ofrom2 = {WFS_K_SNAPSHOT, o2};
        char ow2[4096];
        join(ow2, sizeof ow2, worlds, "oworld2");
        wfs_id ow2id = 0;
        CHECK_OK(wfs_world_create(os, ofrom2, ow2, &opts, &ow2id));
        crash_at(0);                                   // records a pid that does not exist
        CHECK_RC(wfs_world_discard(os, ow2id, 0, 0), -EINTR);
        crash_at(-1);
        CHECK_OK(wfs_world_info(os, ow2id, &owr));
        CHECK(owr.state == WFS_ST_TRASHING);
        CHECK_OK(wfs_store_open(ostore, &ob));
        CHECK_OK(wfs_world_info(ob, ow2id, &owr));
        CHECK(owr.state == WFS_ST_ACTIVE && exists(ow2));   // resolved, exactly as before
        wfs_store_close(ob);

        wfs_test_trash_crash = NULL;
        wfs_store_close(os);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", ostore, (unsigned long long)o2);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (8th round, P1): a restore that wins the race is not a deletion --------
    //
    // gc queues a TRASHED world; a `restore` that gets there first takes the row to TRASHING
    // under BEGIN IMMEDIATE and renames the tree out of the trash and back home. The collector's
    // rename to `<name>.deleting` then answers -ENOENT -- which means "moved home", not
    // "deleted" -- and that branch buried the row: a DEAD row for a world sitting at its home
    // path, restored a millisecond earlier and ACTIVE. Every write the collector makes to a row
    // is conditional on that row now, and the state is checked once before the rename as well,
    // so the ordinary case skips the job before touching anything.
    {
        char cstore[4096], csrc[4096], cw[4096];
        join(cstore, sizeof cstore, root, "collect-store");
        join(csrc, sizeof csrc, root, "collect-src");
        CHECK(mkdir(csrc, 0755) == 0);
        join(p, sizeof p, csrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *ca = NULL;
        CHECK_OK(wfs_store_open(cstore, &ca));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "cb";
        wfs_id c1 = 0;
        CHECK_OK(wfs_snapshot_create(ca, csrc, &sopts, &c1));
        wfs_ref cf = {WFS_K_SNAPSHOT, c1};
        memset(&opts, 0, sizeof opts);
        join(cw, sizeof cw, worlds, "cworld");
        wfs_id cw1 = 0;
        CHECK_OK(wfs_world_create(ca, cf, cw, &opts, &cw1));
        CHECK_OK(wfs_world_discard(ca, cw1, 0, 0));
        wfs_world_rec cwr;
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_TRASHED && !exists(cw));

        // The collector runs on a handle of its own, with retention 0 so the entry is due.
        wfs_store *cb = NULL;
        CHECK_OK(wfs_store_open(cstore, &cb));
        g_del_store = ca;
        g_del_world = cw1;
        g_del_ran = 0;
        g_del_rc = -1;
        wfs_test_before_trash_delete = restore_before_delete;
        wfs_gc_report crep;
        memset(&crep, 0, sizeof crep);
        CHECK_OK(wfs_gc(cb, 0, &crep));
        wfs_test_before_trash_delete = NULL;
        CHECK(g_del_ran == 1);
        CHECK_OK(g_del_rc);                       // the restore is the one that won
        // ... and gc did not touch it: not deleted, not buried, not counted, not reported as a
        // failure either -- there is nothing wrong, the entry simply stopped being trash.
        CHECK(crep.worlds_deleted == 0 && crep.trash_orphans == 0 && crep.trash_failed == 0);
        CHECK(crep.work_remains == 0);
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_ACTIVE && cwr.present && exists(cw));
        CHECK_OK(wfs_world_info(cb, cw1, &cwr));  // and the collector's own handle agrees
        CHECK(cwr.state == WFS_ST_ACTIVE);
        CHECK_OK(wfs_world_diff(ca, cw1, 0, NULL, NULL));
        join(p, sizeof p, cw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));            // the tree came home whole

        // ---- PR #1 review (15th round, P1): the claim is the rename, and it is the row's -----
        //
        // The case above is the restore that got all the way home: the collector's rename then
        // answers -ENOENT and there is nothing to lose. This is the one where it did not. The
        // restore commits its row in TRASHING and dies before the rename, so the tree is still
        // in the trash under the name the collector queued -- and the collector's rename, made
        // outside any transaction, used to succeed there: the conditional UPDATE behind it
        // matched nothing (the row is not TRASHED any anymore) and the unlink went ahead
        // regardless. A world one `restore` away from coming back, deleted.
        CHECK_OK(wfs_world_discard(ca, cw1, 0, 0));
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_TRASHED && !exists(cw));
        g_half_store = ca;
        g_half_world = cw1;
        g_half_ran = 0;
        g_half_rc = 0;
        g_half_path[0] = 0;
        wfs_test_before_trash_delete = restore_half_before_delete;
        memset(&crep, 0, sizeof crep);
        CHECK_OK(wfs_gc(cb, 0, &crep));
        wfs_test_before_trash_delete = NULL;
        CHECK(g_half_ran == 1);
        CHECK_RC(g_half_rc, -EINTR);              // the restore died between its row and its rename
        CHECK(g_half_path[0]);
        // The collector claimed nothing: no rename, no `.deleting` name, no unlink, and no row
        // write either. The entry is not trash it may touch -- it is a restore in flight.
        CHECK(exists(g_half_path));
        join(p, sizeof p, g_half_path, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        char cdel[4200];
        snprintf(cdel, sizeof cdel, "%s.deleting", g_half_path);
        CHECK(!exists(cdel));
        CHECK(crep.worlds_deleted == 0 && crep.trash_orphans == 0 && crep.trash_failed == 0);
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_TRASHING);
        // ... and because the tree is still there, the interrupted restore resolves the way it
        // always did -- back to TRASHED -- and the world comes home whole.
        wfs_store *cc = NULL;
        CHECK_OK(wfs_store_open(cstore, &cc));    // the open runs the TRASHING recovery
        CHECK_OK(wfs_world_info(cc, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_TRASHED);
        CHECK_OK(wfs_world_restore(cc, cw1));
        CHECK_OK(wfs_world_info(cc, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_ACTIVE && cwr.present && exists(cw));
        join(p, sizeof p, cw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        wfs_store_close(cc);

        // ---- and the mirror order: the restore's row first, the collector second -------------
        //
        // A whole gc, on a handle of its own, inside the restore's own window. The row is
        // TRASHING and its owner is alive, so there is no job to queue and no orphan to sweep;
        // the restore then finishes normally.
        CHECK_OK(wfs_world_discard(ca, cw1, 0, 0));
        g_mirror_store = cb;
        g_mirror_ran = 0;
        g_mirror_rc = -1;
        wfs_test_trash_crash = gc_inside_restore;
        CHECK_OK(wfs_world_restore(ca, cw1));
        wfs_test_trash_crash = NULL;
        CHECK(g_mirror_ran == 1);
        CHECK_OK(g_mirror_rc);
        CHECK(g_mirror_rep.worlds_deleted == 0 && g_mirror_rep.trash_orphans == 0 &&
              g_mirror_rep.trash_failed == 0);
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_ACTIVE && cwr.present && exists(cw));
        join(p, sizeof p, cw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));

        // And the ordinary case is untouched: discard it again and the collector takes it.
        CHECK_OK(wfs_world_discard(ca, cw1, 0, 0));
        memset(&crep, 0, sizeof crep);
        CHECK_OK(wfs_gc(cb, 0, &crep));
        CHECK(crep.worlds_deleted == 1 && crep.trash_orphans == 0);
        CHECK_OK(wfs_world_info(ca, cw1, &cwr));
        CHECK(cwr.state == WFS_ST_DEAD && !exists(cw));
        wfs_store_close(cb);
        wfs_store_close(ca);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", cstore, (unsigned long long)c1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (9th round, P1): a world that was moved is not a world that is gone ----
    //
    // `gc --reconcile` scans for ACTIVE rows whose recorded path holds no directory and buries
    // them. That verdict is true of a world that was deleted and equally true of one that was
    // merely moved -- until somebody runs `world fs verify <its new path>`, which relocates the
    // row by inode (P1). A verify landing between the scan and the update had its work buried:
    // the update named only the id, so it marked DEAD a world that was ACTIVE at a path the row
    // had just been taught. The update now carries the state and the path the scan observed, and
    // worlds_reconciled counts only rows it actually changed (docs/M1_DESIGN.md P18).
    {
        char cstore2[4096], csrc2[4096], cold[4096], cnew[4096];
        join(cstore2, sizeof cstore2, root, "recon-store");
        join(csrc2, sizeof csrc2, root, "recon-src");
        CHECK(mkdir(csrc2, 0755) == 0);
        join(p, sizeof p, csrc2, "a.txt");
        write_file(p, "one\n");
        wfs_store *ra = NULL;
        CHECK_OK(wfs_store_open(cstore2, &ra));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "rc";
        wfs_id rc1 = 0;
        CHECK_OK(wfs_snapshot_create(ra, csrc2, &sopts, &rc1));
        wfs_ref rcf = {WFS_K_SNAPSHOT, rc1};
        memset(&opts, 0, sizeof opts);
        join(cold, sizeof cold, worlds, "recon-old");
        join(cnew, sizeof cnew, worlds, "recon-new");
        wfs_id rcw = 0;
        CHECK_OK(wfs_world_create(ra, rcf, cold, &opts, &rcw));
        CHECK(rename(cold, cnew) == 0);           // the user moved it; the row still says `cold`

        wfs_store *rb = NULL;
        CHECK_OK(wfs_store_open(cstore2, &rb));
        g_rec_store = ra;
        g_rec_path = cnew;
        g_rec_ran = 0;
        g_rec_rc = -1;
        wfs_test_before_reconcile = verify_before_reconcile;
        wfs_gc_opts gopts;
        memset(&gopts, 0, sizeof gopts);
        gopts.retention_secs = 0;
        gopts.flags = WFS_GC_RECONCILE;
        wfs_gc_report rrep;
        memset(&rrep, 0, sizeof rrep);
        CHECK_OK(wfs_gc_ex(rb, &gopts, &rrep));
        wfs_test_before_reconcile = NULL;
        CHECK(g_rec_ran == 1);
        CHECK_OK(g_rec_rc);                       // the verify relocated the row
        CHECK(rrep.worlds_dangling == 1);         // the scan did see it, and says so
        CHECK(rrep.worlds_reconciled == 0);       // ... and buried nothing
        wfs_world_rec rwr2;
        CHECK_OK(wfs_world_info(ra, rcw, &rwr2));
        CHECK(rwr2.state == WFS_ST_ACTIVE && !strcmp(rwr2.path, cnew) && rwr2.present);
        CHECK_OK(wfs_world_diff(ra, rcw, 0, NULL, NULL));
        CHECK_OK(wfs_world_info(rb, rcw, &rwr2));   // and the collector's own handle agrees
        CHECK(rwr2.state == WFS_ST_ACTIVE);

        // And a world whose tree really is gone is still reconciled, which is the rule this is a
        // refinement of, not a retreat from.
        rm_rf(cnew);
        memset(&rrep, 0, sizeof rrep);
        CHECK_OK(wfs_gc_ex(rb, &gopts, &rrep));
        CHECK(rrep.worlds_dangling == 1 && rrep.worlds_reconciled == 1);
        CHECK_OK(wfs_world_info(ra, rcw, &rwr2));
        CHECK(rwr2.state == WFS_ST_DEAD);
        wfs_store_close(rb);
        wfs_store_close(ra);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", cstore2, (unsigned long long)rc1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (12th round, P1): an unreadable path is not a missing world ------------
    //
    // Reconciliation asked `stat(2) == 0 && S_ISDIR` and read every other answer as "the tree is
    // gone". stat(2) says no for reasons that have nothing to do with absence: EACCES on a
    // parent directory, an EIO, a volume that is not mounted this minute, an ENAMETOOLONG. The
    // row was then marked DEAD -- and a DEAD world cannot be repaired by `verify` and cannot be
    // adopted, so a transient error unregistered a live world for good. Only ENOENT/ENOTDIR is
    // an absence now; everything else, including a path that holds something which is not a
    // directory (that is a damaged world, not a missing one), is counted as unreadable, left
    // ACTIVE, and reported (docs/M1_DESIGN.md P17/P18).
    {
        char ustore[4096], usrc[4096], uhold[4096], uw[4096], udmg[4096], usnaps[4096];
        join(ustore, sizeof ustore, root, "unread-store");
        join(usrc, sizeof usrc, root, "unread-src");
        CHECK(mkdir(usrc, 0755) == 0);
        join(p, sizeof p, usrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *ua = NULL;
        CHECK_OK(wfs_store_open(ustore, &ua));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "ur";
        wfs_id us1 = 0;
        CHECK_OK(wfs_snapshot_create(ua, usrc, &sopts, &us1));
        wfs_ref uref = {WFS_K_SNAPSHOT, us1};
        join(uhold, sizeof uhold, worlds, "unread-hold");
        CHECK(mkdir(uhold, 0755) == 0);
        join(uw, sizeof uw, uhold, "w");
        memset(&opts, 0, sizeof opts);
        opts.name = "ur-w";
        opts.no_pool = 1;
        wfs_id uwid = 0;
        CHECK_OK(wfs_world_create(ua, uref, uw, &opts, &uwid));
        // And one whose tree somebody replaced with a plain file: the row's path resolves, it is
        // simply not a directory any more. Damage, not absence.
        join(udmg, sizeof udmg, worlds, "unread-dmg");
        opts.name = "ur-d";
        wfs_id udid = 0;
        CHECK_OK(wfs_world_create(ua, uref, udmg, &opts, &udid));
        rm_rf(udmg);
        write_file(udmg, "not a directory\n");

        CHECK(chmod(uhold, 0) == 0);   // stat(uw) is EACCES from here, not ENOENT
        wfs_gc_opts ugo;
        memset(&ugo, 0, sizeof ugo);
        ugo.retention_secs = 0;
        ugo.flags = WFS_GC_RECONCILE;
        wfs_gc_report urep;
        memset(&urep, 0, sizeof urep);
        CHECK_OK(wfs_gc_ex(ua, &ugo, &urep));
        CHECK(urep.worlds_unreadable == 2);     // the EACCES one and the damaged one
        CHECK(urep.worlds_dangling == 0 && urep.worlds_reconciled == 0);
        wfs_world_rec uwr;
        CHECK_OK(wfs_world_info(ua, uwid, &uwr));
        CHECK(uwr.state == WFS_ST_ACTIVE);      // still registered, still repairable
        CHECK_OK(wfs_world_info(ua, udid, &uwr));
        CHECK(uwr.state == WFS_ST_ACTIVE);
        wfs_store_stat ust;
        CHECK_OK(wfs_store_status(ua, &ust));   // and `status` says the same thing
        CHECK(ust.worlds_unreadable == 2 && ust.worlds_dangling == 0);

        // The snapshot half of the same scan, made unreadable the same way.
        join(usnaps, sizeof usnaps, ustore, "snapshots");
        CHECK(chmod(usnaps, 0) == 0);
        memset(&urep, 0, sizeof urep);
        CHECK_OK(wfs_gc_ex(ua, &ugo, &urep));
        CHECK(urep.snapshots_unreadable == 1);
        CHECK(urep.snapshots_dangling == 0 && urep.snapshots_reconciled == 0);
        CHECK(chmod(usnaps, 0755) == 0);
        wfs_snapshot_rec usr;
        CHECK_OK(wfs_snapshot_info(ua, us1, &usr));
        CHECK(usr.state == WFS_ST_ACTIVE);

        // Give the access back and the world is ordinary again -- nothing to repair, because
        // nothing was buried.
        CHECK(chmod(uhold, 0755) == 0);
        memset(&urep, 0, sizeof urep);
        CHECK_OK(wfs_gc_ex(ua, &ugo, &urep));
        CHECK(urep.worlds_unreadable == 1);     // only the one that really is damaged
        CHECK(urep.worlds_dangling == 0 && urep.worlds_reconciled == 0);
        CHECK_OK(wfs_world_info(ua, uwid, &uwr));
        CHECK(uwr.state == WFS_ST_ACTIVE && uwr.present);

        // And a world whose tree really is gone still reconciles to DEAD: this is a refinement
        // of that rule, not a retreat from it.
        CHECK(unlink(udmg) == 0);
        memset(&urep, 0, sizeof urep);
        CHECK_OK(wfs_gc_ex(ua, &ugo, &urep));
        CHECK(urep.worlds_unreadable == 0);
        CHECK(urep.worlds_dangling == 1 && urep.worlds_reconciled == 1);
        CHECK_OK(wfs_world_info(ua, udid, &uwr));
        CHECK(uwr.state == WFS_ST_DEAD);
        CHECK_OK(wfs_world_info(ua, uwid, &uwr));
        CHECK(uwr.state == WFS_ST_ACTIVE);      // the unreadable one was never in question
        wfs_store_close(ua);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", ustore, (unsigned long long)us1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (12th round, P2): `discard` does not bury a row over an lstat error ----
    //
    // "The tree is already gone" is a shortcut a discard is allowed to take: it writes the row
    // straight to DEAD, with no trash_path, because there is nothing to move and nothing to
    // unlink. It was decided by a bool over lstat(2), so an EACCES or an EIO took it too -- and
    // then nothing ever revisited <store>/snapshots/S<n>, because both halves of gc look at
    // rows (reconciliation at ACTIVE ones, the suffix sweep at `*.wfs-tmp` only). A whole
    // snapshot, silently unregistered and permanently on disk. Only ENOENT/ENOTDIR takes that
    // path now; every other errno is returned to the caller with the row untouched.
    {
        char dstore[4096], dsrc[4096], dsnaps[4096];
        join(dstore, sizeof dstore, root, "discard-probe-store");
        join(dsrc, sizeof dsrc, root, "discard-probe-src");
        CHECK(mkdir(dsrc, 0755) == 0);
        join(p, sizeof p, dsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *da = NULL;
        CHECK_OK(wfs_store_open(dstore, &da));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "dp";
        wfs_id ds1 = 0;
        CHECK_OK(wfs_snapshot_create(da, dsrc, &sopts, &ds1));
        join(dsnaps, sizeof dsnaps, dstore, "snapshots");
        CHECK(chmod(dsnaps, 0) == 0);          // lstat(<store>/snapshots/S<n>) is EACCES now
        CHECK_RC(wfs_snapshot_discard(da, ds1, 0, 0), -EACCES);
        wfs_snapshot_rec dsr;
        CHECK_OK(wfs_snapshot_info(da, ds1, &dsr));
        CHECK(dsr.state == WFS_ST_ACTIVE);     // nothing was buried
        CHECK(chmod(dsnaps, 0755) == 0);
        snprintf(p, sizeof p, "%s/S%llu", dsnaps, (unsigned long long)ds1);
        CHECK(exists(p));                      // ... and the tree is exactly where it was
        // And with the access back, the discard the caller asked for happens.
        CHECK_OK(wfs_snapshot_discard(da, ds1, 1, 0));
        CHECK_OK(wfs_snapshot_info(da, ds1, &dsr));
        CHECK(dsr.state == WFS_ST_DEAD);
        CHECK(!exists(p));
        wfs_store_close(da);
    }

    // ---- PR #1 review (12th round, P2): a file name that ends in a carriage return ----------
    //
    // The hardlink manifest's reader stripped a trailing `\r` along with the `\n`, as if the
    // file it had just written itself might have CRLF line endings. The writer escaped `\\` and
    // `\n` and nothing else, so a name ending in CR went in raw and came back out one byte
    // shorter -- and the replay then linked *that* name: `a\r` and `b\r` were read as `a` and
    // `b`, two perfectly ordinary files next to them, which the fork duly welded onto one
    // inode. Cloned content overwritten, in a fork that reported success. The writer escapes
    // CR now (`\\r`) and the reader strips only its terminator; the verify manifest, which had
    // the same writer/reader pair, goes with it.
    {
        char crsrc[4096], crw[4096], cra[4096], crb[4096];
        join(crsrc, sizeof crsrc, root, "cr-src");
        CHECK(mkdir(crsrc, 0755) == 0);
        join(cra, sizeof cra, crsrc, "a\r");
        write_file(cra, "linked\n");
        join(crb, sizeof crb, crsrc, "b\r");
        CHECK(link(cra, crb) == 0);
        // ... and the two ordinary files whose names the old reader turned those into.
        join(cra, sizeof cra, crsrc, "a");
        write_file(cra, "plain a\n");
        join(crb, sizeof crb, crsrc, "b");
        write_file(crb, "plain b\n");
        // Same size and same mtime: two generated files, a checkout, a tar extract. That is all
        // the replay's "is this still the file the scan saw" guard has to go on, so with the
        // names misread these two were interchangeable to it.
        struct timespec crts[2];
        crts[0].tv_sec = 1000000000; crts[0].tv_nsec = 0;
        crts[1] = crts[0];
        CHECK(utimensat(AT_FDCWD, cra, crts, 0) == 0);
        CHECK(utimensat(AT_FDCWD, crb, crts, 0) == 0);

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "cr";
        wfs_id scr = 0;
        CHECK_OK(wfs_snapshot_create(s, crsrc, &sopts, &scr));
        wfs_snapshot_rec crr;
        CHECK_OK(wfs_snapshot_info(s, scr, &crr));
        CHECK(crr.hl_groups == 1 && crr.hl_external == 0 && crr.hardlinks == 2);
        wfs_verify_report crv;
        CHECK_OK(wfs_snapshot_verify(s, scr, &crv));   // the verify manifest reads its own names
        CHECK(crv.missing == 0 && crv.modified == 0 && crv.extra == 0);

        join(crw, sizeof crw, worlds, "cr-world");
        wfs_ref crf = {WFS_K_SNAPSHOT, scr};
        memset(&opts, 0, sizeof opts);
        opts.name = "cr-w";
        opts.no_pool = 1;
        wfs_fork_result crfr;
        memset(&crfr, 0, sizeof crfr);
        CHECK_OK(wfs_world_create_ex(s, crf, crw, &opts, &crfr));
        CHECK(crfr.hardlinks == 1);            // the one name relinked to its canonical file
        join(cra, sizeof cra, crw, "a\r");
        join(crb, sizeof crb, crw, "b\r");
        CHECK(ino_of(cra) == ino_of(crb) && nlink_of(cra) == 2);
        join(cra, sizeof cra, crw, "a");
        join(crb, sizeof crb, crw, "b");
        CHECK(ino_of(cra) != ino_of(crb));     // and the plain pair is still two separate files
        CHECK(nlink_of(cra) == 1 && nlink_of(crb) == 1);
        CHECK_OK(read_file(cra, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain a\n"));
        CHECK_OK(read_file(crb, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain b\n"));

        // And through the pool, whose filler replays the manifest onto its own clone with no
        // verify root at all (pool.cpp): there the misread names are acted on unconditionally,
        // so the two ordinary files really were welded onto one inode and one of them lost its
        // contents -- a fork that reported success and handed back a damaged tree.
        uint64_t crmade = 0;
        CHECK_OK(wfs_pool_fill(s, scr, 1, &crmade));
        CHECK(crmade == 1);
        char crwp[4096];
        join(crwp, sizeof crwp, worlds, "cr-world-pool");
        memset(&opts, 0, sizeof opts);
        opts.name = "cr-w-pool";
        memset(&crfr, 0, sizeof crfr);
        CHECK_OK(wfs_world_create_ex(s, crf, crwp, &opts, &crfr));
        CHECK(crfr.from_pool == 1);
        join(cra, sizeof cra, crwp, "a\r");
        join(crb, sizeof crb, crwp, "b\r");
        CHECK(ino_of(cra) == ino_of(crb) && nlink_of(cra) == 2);
        join(cra, sizeof cra, crwp, "a");
        join(crb, sizeof crb, crwp, "b");
        CHECK(ino_of(cra) != ino_of(crb));
        CHECK(nlink_of(cra) == 1 && nlink_of(crb) == 1);
        CHECK_OK(read_file(crb, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain b\n"));     // and not one inode's worth of "plain a"
    }

    // ---- PR #1 review (19th round, P2): a file name that BEGINS with a space ----------------
    //
    // Both manifest readers ended their number list with a whitespace directive -- `%llu %n` --
    // and a whitespace directive in scanf(3) eats *all* the whitespace it can reach, not the one
    // separator the writer put there. Both writers emit exactly one space and then the name
    // verbatim (`fprintf(f, "hl %llu %llu ", ...)` + put_escaped in hardlinks.cpp, and the
    // `%c %o %llu %lld.%ld %llu ` head + the same escaping in Manifest::line), so a name whose
    // first byte is a space or a tab came back with that byte -- and every further one -- gone:
    // ` a` and ` b` were read as `a` and `b`, which are either somebody else's files or nobody's.
    // What that cost is the whole snapshot: `verify` right after `init` reported five findings
    // (four entry lines lstat'ing the wrong file, plus the hardlink section, whose group check
    // lstats the two names it was handed and finds two different inodes), and every fork and
    // every pool fill of it then refused with WFS_E_SNAPSHOT_DIRTY -- for ever, because the
    // manifest is on disk and the tree it describes is intact. A leading space is an ordinary
    // byte in a file name, so this is a tree a user can simply have.
    // The fix is on the readers alone: the separator is exactly one ' ', and everything after it
    // is the name. Every manifest this library has ever written is still read the same way.
    {
        char spsrc[4096], spw[4096], spa[4096], spb[4096];
        join(spsrc, sizeof spsrc, root, "sp-src");
        CHECK(mkdir(spsrc, 0755) == 0);
        join(spa, sizeof spa, spsrc, " a");
        write_file(spa, "linked\n");
        join(spb, sizeof spb, spsrc, " b");
        CHECK(link(spa, spb) == 0);
        // The tab pair, which the same directive eats just as happily.
        join(spa, sizeof spa, spsrc, "\ta");
        write_file(spa, "tabbed\n");
        join(spb, sizeof spb, spsrc, "\tb");
        CHECK(link(spa, spb) == 0);
        // ... and the two ordinary files the old reader turned those four names into.
        join(spa, sizeof spa, spsrc, "a");
        write_file(spa, "plain a\n");
        join(spb, sizeof spb, spsrc, "b");
        write_file(spb, "plain b\n");
        // Same size and same mtime as each other: that is all the replay's "is this still the
        // file the scan saw" guard has to go on, so with the names misread the decoys were
        // interchangeable to it.
        struct timespec spts[2];
        spts[0].tv_sec = 1000000000; spts[0].tv_nsec = 0;
        spts[1] = spts[0];
        CHECK(utimensat(AT_FDCWD, spa, spts, 0) == 0);
        CHECK(utimensat(AT_FDCWD, spb, spts, 0) == 0);

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "sp";
        wfs_id ssp = 0;
        CHECK_OK(wfs_snapshot_create(s, spsrc, &sopts, &ssp));
        wfs_snapshot_rec spr;
        CHECK_OK(wfs_snapshot_info(s, ssp, &spr));
        CHECK(spr.hl_groups == 2 && spr.hl_external == 0 && spr.hardlinks == 4);
        wfs_verify_report spv;
        CHECK_OK(wfs_snapshot_verify(s, ssp, &spv));   // the snapshot is clean the moment it is made
        CHECK(spv.missing == 0 && spv.modified == 0 && spv.extra == 0 && spv.unprotected == 0);

        join(spw, sizeof spw, worlds, "sp-world");
        wfs_ref spf = {WFS_K_SNAPSHOT, ssp};
        memset(&opts, 0, sizeof opts);
        opts.name = "sp-w";
        opts.no_pool = 1;
        wfs_fork_result spfr;
        memset(&spfr, 0, sizeof spfr);
        CHECK_OK(wfs_world_create_ex(s, spf, spw, &opts, &spfr));
        CHECK(spfr.hardlinks == 2);            // one name relinked per group
        join(spa, sizeof spa, spw, " a");
        join(spb, sizeof spb, spw, " b");
        CHECK(ino_of(spa) == ino_of(spb) && nlink_of(spa) == 2);
        join(spa, sizeof spa, spw, "\ta");
        join(spb, sizeof spb, spw, "\tb");
        CHECK(ino_of(spa) == ino_of(spb) && nlink_of(spa) == 2);
        join(spa, sizeof spa, spw, "a");
        join(spb, sizeof spb, spw, "b");
        CHECK(ino_of(spa) != ino_of(spb));     // and the decoys are still two separate files
        CHECK(nlink_of(spa) == 1 && nlink_of(spb) == 1);
        CHECK_OK(read_file(spa, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain a\n"));
        CHECK_OK(read_file(spb, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain b\n"));

        // And through the pool, whose filler replays the manifest onto its own clone with no
        // verify root at all (pool.cpp): there the misread names are acted on unconditionally.
        uint64_t spmade = 0;
        CHECK_OK(wfs_pool_fill(s, ssp, 1, &spmade));
        CHECK(spmade == 1);
        char spwp[4096];
        join(spwp, sizeof spwp, worlds, "sp-world-pool");
        memset(&opts, 0, sizeof opts);
        opts.name = "sp-w-pool";
        memset(&spfr, 0, sizeof spfr);
        CHECK_OK(wfs_world_create_ex(s, spf, spwp, &opts, &spfr));
        CHECK(spfr.from_pool == 1);
        join(spa, sizeof spa, spwp, " a");
        join(spb, sizeof spb, spwp, " b");
        CHECK(ino_of(spa) == ino_of(spb) && nlink_of(spa) == 2);
        join(spa, sizeof spa, spwp, "a");
        join(spb, sizeof spb, spwp, "b");
        CHECK(ino_of(spa) != ino_of(spb));
        CHECK(nlink_of(spa) == 1 && nlink_of(spb) == 1);
        CHECK_OK(read_file(spb, buf, sizeof buf));
        CHECK(!strcmp(buf, "plain b\n"));     // and not one inode's worth of "plain a"
    }

    // ---- PR #1 review (9th round, P1): a pool entry made after the scan is not an orphan ----
    //
    // pool_scan takes one snapshot of the pool rows; pool_collect then removes the trees that
    // snapshot dooms and sweeps every directory under <store>/pool that it does not name. A
    // filler that inserts its CREATING row after the scan had its `.wfs-tmp` deleted mid-clone --
    // and a partially removed entry then went on to be published READY -- while a fork claiming
    // an entry in the same window had the tree it was about to rename taken away. Every apparent
    // orphan is now re-asked of the live rows under the store mutex, and a pool row's path, that
    // path plus `.wfs-tmp`, and a CREATING world row's tmp_path all count as naming it
    // (docs/M1_DESIGN.md P18).
    {
        char pstore[4096], psrc[4096], ppool[4096], pw[4096], pw2[4096];
        join(pstore, sizeof pstore, root, "poolrace-store");
        join(psrc, sizeof psrc, root, "poolrace-src");
        CHECK(mkdir(psrc, 0755) == 0);
        join(p, sizeof p, psrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *pa = NULL;
        CHECK_OK(wfs_store_open(pstore, &pa));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "pb";
        wfs_id p1 = 0;
        CHECK_OK(wfs_snapshot_create(pa, psrc, &sopts, &p1));
        uint64_t pmade = 0, pready = 0;
        CHECK_OK(wfs_pool_fill(pa, p1, 1, &pmade));
        CHECK(pmade == 1);
        CHECK_OK(wfs_pool_ready(pa, p1, &pready));
        CHECK(pready == 1);
        snprintf(ppool, sizeof ppool, "%s/pool/S%llu", pstore, (unsigned long long)p1);

        join(pw, sizeof pw, worlds, "prworld");
        g_pool_store = pa;
        g_pool_snap = p1;
        g_pool_target = pw;
        g_pool_ran = 0;
        g_pool_rc = -1;
        // The collector runs on a handle of its own, with a gc that holds nothing back.
        wfs_store *pb = NULL;
        CHECK_OK(wfs_store_open(pstore, &pb));
        wfs_test_before_pool_sweep = fill_before_sweep;
        wfs_gc_report prep;
        memset(&prep, 0, sizeof prep);
        CHECK_OK(wfs_gc(pb, 0, &prep));
        wfs_test_before_pool_sweep = NULL;
        CHECK(g_pool_ran == 1);
        CHECK_OK(g_pool_rc);
        CHECK(g_pool_fr.from_pool == 1);          // the fork took the entry that was waiting
        // Nothing was removed and nothing was reported as stuck: neither tree was the
        // collector's to touch.
        CHECK(prep.pool_removed == 0 && prep.pool_failed == 0);
        // The fork kept the tree it claimed...
        CHECK(exists(pw));
        join(p, sizeof p, pw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        wfs_world_rec pwr;
        CHECK_OK(wfs_world_info(pa, g_pool_fr.world, &pwr));
        CHECK(pwr.state == WFS_ST_ACTIVE && pwr.present);
        // ... and the entry the filler built after the scan is still there, and still usable:
        // the row says READY and the tree behind it is whole, which is the pair that used to
        // come apart.
        CHECK_OK(wfs_pool_ready(pa, p1, &pready));
        CHECK(pready == 1);
        CHECK(n_with_prefix(ppool, "") == 3);     // `.`, `..` and the one entry
        wfs_trash_stat pst;
        CHECK_OK(wfs_gc_status(pa, 0, &pst));
        CHECK(pst.pool_stranded == 0);            // and `gc --status` says the same
        join(pw2, sizeof pw2, worlds, "prworld2");
        wfs_fork_result pfr2;
        memset(&opts, 0, sizeof opts);
        opts.name = "prworld2";
        CHECK_OK(wfs_world_create_ex(pa, (wfs_ref){WFS_K_SNAPSHOT, p1}, pw2, &opts, &pfr2));
        CHECK(pfr2.from_pool == 1);               // handed out, not re-cloned
        join(p, sizeof p, pw2, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));

        // And a directory under <store>/pool that really is row-less still goes.
        char pjunk[4096];
        join(pjunk, sizeof pjunk, ppool, "deadbeef");
        CHECK(mkdir(pjunk, 0755) == 0);
        memset(&prep, 0, sizeof prep);
        CHECK_OK(wfs_gc(pb, 0, &prep));
        CHECK(prep.pool_removed == 1 && !exists(pjunk));
        wfs_store_close(pb);
        wfs_store_close(pa);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", pstore, (unsigned long long)p1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (15th round, P2): the filler's entry is a reference, taken under the ----
    // ---- same write lock the discard counts references under ---------------------------------
    //
    // wfs_pool_fill reads the snapshot row once and then inserts one CREATING pool row per
    // entry. A `discard S<n>` landing between the two counts references under BEGIN IMMEDIATE,
    // finds no pool row (this one does not exist yet) and commits its row in TRASHING -- and the
    // filler, whose source tree has not moved yet, went on to clone it and publish a READY entry
    // for a snapshot that is on its way to the trash, which the next fork takes as a live
    // baseline. The insert re-reads the snapshot inside its own transaction now: ACTIVE, and the
    // same created_at the entry would carry, or the fill stops (docs/M1_DESIGN.md P18).
    {
        char fstore[4096], fsrc[4096], fpool[4096];
        join(fstore, sizeof fstore, root, "fillrace-store");
        join(fsrc, sizeof fsrc, root, "fillrace-src");
        CHECK(mkdir(fsrc, 0755) == 0);
        join(p, sizeof p, fsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *fa = NULL, *fb = NULL;
        CHECK_OK(wfs_store_open(fstore, &fa));
        CHECK_OK(wfs_store_open(fstore, &fb));   // the discard runs on a handle of its own
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "fb";
        wfs_id f1 = 0;
        CHECK_OK(wfs_snapshot_create(fa, fsrc, &sopts, &f1));
        snprintf(fpool, sizeof fpool, "%s/pool/S%llu", fstore, (unsigned long long)f1);
        g_pins_store = fb;
        g_pins_snap = f1;
        g_pins_ran = 0;
        g_pins_rc = 0;
        wfs_test_before_pool_insert = discard_before_pool_insert;
        uint64_t fmade = 0, fready = 0;
        CHECK_RC(wfs_pool_fill(fa, f1, 1, &fmade), -ESTALE);
        wfs_test_before_pool_insert = NULL;
        CHECK(g_pins_ran == 1);
        CHECK_RC(g_pins_rc, -EINTR);              // row committed in TRASHING, tree still in place
        CHECK(fmade == 0);
        CHECK_OK(wfs_pool_ready(fa, f1, &fready));
        CHECK(fready == 0);                       // nothing was published ...
        CHECK(n_with_prefix(fpool, "") == 2);     // ... and nothing was left on disk either
        wfs_snapshot_rec fsr;
        CHECK_OK(wfs_snapshot_info(fa, f1, &fsr));
        CHECK(fsr.state == WFS_ST_TRASHING);
        // The interrupted discard resolves the way it always did -- the tree never moved, so the
        // snapshot goes back to ACTIVE -- and a fill then does exactly what it was asked to do.
        wfs_store *fc = NULL;
        CHECK_OK(wfs_store_open(fstore, &fc));    // the open runs the TRASHING recovery
        CHECK_OK(wfs_snapshot_info(fc, f1, &fsr));
        CHECK(fsr.state == WFS_ST_ACTIVE);
        CHECK_OK(wfs_pool_fill(fc, f1, 1, &fmade));
        CHECK(fmade == 1);
        CHECK_OK(wfs_pool_ready(fc, f1, &fready));
        CHECK(fready == 1);
        CHECK(n_with_prefix(fpool, "") == 3);
        wfs_store_close(fc);
        wfs_store_close(fb);
        wfs_store_close(fa);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", fstore, (unsigned long long)f1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (21st round, P1): a fork must never claim the entry a drain is -------
    // ---- removing ---------------------------------------------------------------------------
    //
    // The 16th round made wfs_pool_drain() remove the tree first and delete the row only when
    // the tree is proven gone, so an EPERM leaves something in the store that still names the
    // clone. But the row it kept through the removal was an ordinary READY row of an ACTIVE
    // snapshot, and pool_claim() does not take the pool lock -- it cannot, a fork must never
    // wait on a filler. So a pool-backed fork could take that entry while fs_remove_tree() was
    // walking it, rename the half-emptied tree to the user's `--to` and commit an ACTIVE world
    // around whatever was left: `fork` returned 0 and the world was missing files. The drain
    // moves the row to a state no claim matches first, in a transaction of its own, and only
    // then touches the tree (docs/M1_DESIGN.md P18).
    {
        char dstore[4096], dsrc[4096], dpool[4096], dw[4096];
        join(dstore, sizeof dstore, root, "drainrace-store");
        join(dsrc, sizeof dsrc, root, "drainrace-src");
        CHECK(mkdir(dsrc, 0755) == 0);
        join(p, sizeof p, dsrc, "a.txt");
        write_file(p, "one\n");
        join(p, sizeof p, dsrc, "b.txt");
        write_file(p, "two\n");
        join(p, sizeof p, dsrc, "sub");
        CHECK(mkdir(p, 0755) == 0);
        join(q, sizeof q, p, "c.txt");
        write_file(q, "three\n");
        wfs_store *dra = NULL, *drb = NULL;
        CHECK_OK(wfs_store_open(dstore, &dra));
        CHECK_OK(wfs_store_open(dstore, &drb));   // the fork runs on a handle of its own
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "drb";
        wfs_id d1 = 0;
        CHECK_OK(wfs_snapshot_create(dra, dsrc, &sopts, &d1));
        wfs_snapshot_rec dsr;
        CHECK_OK(wfs_snapshot_info(dra, d1, &dsr));
        snprintf(dpool, sizeof dpool, "%s/pool/S%llu", dstore, (unsigned long long)d1);
        uint64_t dmade = 0, dready = 0, dremoved = 0;
        wfs_pool_stat dps[4];
        size_t dpn = 0;
        wfs_world_rec dwr;
        wfs_gc_report dgc;

        // (1) The window itself: the fork lands with the row already out of the pool and the
        // removal about to start. It must not be given the entry -- it falls back to a clone of
        // its own -- and the world it publishes must be the whole tree, not what the remover has
        // left of one. Before the fix this fork reported from_pool=1 and the world came out one
        // file short, with `fork` having returned 0.
        join(dw, sizeof dw, worlds, "drainrace-w1");
        g_drain_store = drb;
        g_drain_snap = d1;
        g_drain_target = dw;
        g_drain_pooldir = dpool;
        g_drain_phase = 1;
        g_drain_ran = 0;
        g_drain_rc = -1;
        CHECK_OK(wfs_pool_fill(dra, d1, 1, &dmade));
        CHECK(dmade == 1);
        wfs_test_in_pool_drain = fork_in_drain;
        CHECK_OK(wfs_pool_drain(dra, d1, &dremoved));
        wfs_test_in_pool_drain = NULL;
        CHECK(g_drain_ran == 1);
        CHECK_OK(g_drain_rc);
        CHECK(g_drain_fr.from_pool == 0);            // not the tree that was being removed
        CHECK_OK(wfs_world_info(dra, g_drain_fr.world, &dwr));
        CHECK(dwr.state == WFS_ST_ACTIVE && dwr.entries == dsr.entries);
        join(p, sizeof p, dw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        join(p, sizeof p, dw, "b.txt");              // the file the removal had already unlinked
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "two\n"));
        join(p, sizeof p, dw, "sub/c.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "three\n"));
        CHECK(dremoved == 1);                        // and the entry itself did go
        // Nothing is left under <store>/pool/S<n> -- the drain rmdir's the empty directory too,
        // so either it is gone or it holds nothing but `.` and `..`.
        CHECK(!exists(dpool) || n_with_prefix(dpool, "") == 2);
        CHECK_OK(wfs_pool_ready(dra, d1, &dready));
        CHECK(dready == 0);

        // (2) The mirror: the claim commits first. The drain's own update then matches no row --
        // the claim deleted it -- and the tree it named belongs to the fork, which is about to
        // rename it into place. The drain must skip it, and count nothing.
        join(dw, sizeof dw, worlds, "drainrace-w2");
        g_drain_target = dw;
        g_drain_phase = 0;
        g_drain_ran = 0;
        g_drain_rc = -1;
        CHECK_OK(wfs_pool_fill(dra, d1, 1, &dmade));
        CHECK(dmade == 1);
        dremoved = 0;
        wfs_test_in_pool_drain = fork_in_drain;
        CHECK_OK(wfs_pool_drain(dra, d1, &dremoved));
        wfs_test_in_pool_drain = NULL;
        CHECK(g_drain_ran == 1);
        CHECK_OK(g_drain_rc);
        CHECK(g_drain_fr.from_pool == 1);            // the entry was still an entry: handed out
        CHECK(dremoved == 0);                        // and the drain took nothing of its own
        CHECK_OK(wfs_world_info(dra, g_drain_fr.world, &dwr));
        CHECK(dwr.state == WFS_ST_ACTIVE && dwr.entries == dsr.entries);
        join(p, sizeof p, dw, "b.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "two\n"));
        join(p, sizeof p, dw, "sub/c.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "three\n"));
        // The entry left as a world, not as rubbish: nothing under <store>/pool/S<n>.
        CHECK(!exists(dpool) || n_with_prefix(dpool, "") == 2);

        // (3) And the state the drain leaves behind when the tree will not go. The row stays
        // DRAINING: no claim can match it, `pool ready` does not count it, `pool status` calls
        // it stale rather than ready or building, `gc --status` reports it as a stranded pool
        // entry, and `discard S<n> --force` fails with the errno of the removal instead of
        // trashing the snapshot on the strength of a 0 (the 16th round's rule, with the row now
        // in a state that makes keeping it safe).
        char dlock[4096], dentry[4096];
        char dnames[8][256];
        uint64_t dinos[8];
        CHECK_OK(wfs_pool_fill(dra, d1, 1, &dmade));
        CHECK(dmade == 1);
        CHECK(list_dir(dpool, dnames, dinos, 8) == 1);
        join(dentry, sizeof dentry, dpool, dnames[0]);
        join(dlock, sizeof dlock, dentry, "sub");
        deny_delete(dlock);
        dremoved = 0;
        int ddrc = wfs_pool_drain(dra, d1, &dremoved);
        CHECK(ddrc != 0 && dremoved == 0);
        CHECK(exists(dentry));                       // the tree is still there ...
        CHECK(db_pool_state(dstore) == 2);           // ... and its row says DRAINING
        CHECK_OK(wfs_pool_ready(dra, d1, &dready));
        CHECK(dready == 0);
        CHECK_OK(wfs_pool_status(dra, dps, 4, &dpn));
        CHECK(dpn == 1 && dps[0].ready == 0 && dps[0].building == 0 && dps[0].stale == 1);
        wfs_trash_stat dts;
        CHECK_OK(wfs_gc_status(dra, 0, &dts));
        CHECK(dts.pool_stranded >= 1);
        // A fork in this state gets a clone of its own, never the tree that is on its way out.
        join(dw, sizeof dw, worlds, "drainrace-w3");
        memset(&opts, 0, sizeof opts);
        opts.name = "drainrace-w3";
        wfs_fork_result dfr3;
        memset(&dfr3, 0, sizeof dfr3);
        CHECK_OK(wfs_world_create_ex(drb, (wfs_ref){WFS_K_SNAPSHOT, d1}, dw, &opts, &dfr3));
        CHECK(dfr3.from_pool == 0);
        CHECK(exists(dentry));                       // and the entry was not touched by it
        // `discard S<n> --force` drains first, gets the same errno, and leaves the snapshot be.
        CHECK_RC(wfs_snapshot_discard(dra, d1, 0, 1), ddrc);
        CHECK_OK(wfs_snapshot_info(dra, d1, &dsr));
        CHECK(dsr.state == WFS_ST_ACTIVE);
        // Take the ACL away and the collector finishes what the drain started: the DRAINING row
        // is doomed whatever its snapshot says, so this is the retry path for it.
        allow_delete(dlock);
        memset(&dgc, 0, sizeof dgc);
        CHECK_OK(wfs_gc(dra, 0, &dgc));
        CHECK(dgc.pool_removed >= 1);
        CHECK(!exists(dentry));
        CHECK_OK(wfs_pool_status(dra, dps, 4, &dpn));
        CHECK(dpn == 0);                             // row and tree both gone
        CHECK_OK(wfs_gc_status(dra, 0, &dts));
        CHECK(dts.pool_stranded == 0);
        wfs_store_close(drb);
        wfs_store_close(dra);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", dstore, (unsigned long long)d1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (9th round, P1): a tree a discard has just moved in is not an orphan ----
    //
    // trash_scan reads the trash paths the rows claim, drops the store mutex, and then reads the
    // directory. A `discard` that commits its TRASHING row and renames its tree into the trash
    // between the two leaves a directory the claim list has never heard of -- and a row-less
    // orphan is deleted on sight: retention skipped, restore impossible, and the discard that is
    // still running finds its tree gone. So both verdicts -- the one that queues the job and the
    // one taken immediately before the deletion starts -- are re-asked of the live rows under the
    // store mutex (docs/M1_DESIGN.md P18).
    {
        char nstore[4096], nsrc[4096], nw[4096], ntrash[4096];
        join(nstore, sizeof nstore, root, "orphan-store");
        join(nsrc, sizeof nsrc, root, "orphan-src");
        CHECK(mkdir(nsrc, 0755) == 0);
        join(p, sizeof p, nsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *na = NULL;
        CHECK_OK(wfs_store_open(nstore, &na));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "nb";
        wfs_id n1 = 0;
        CHECK_OK(wfs_snapshot_create(na, nsrc, &sopts, &n1));
        wfs_ref nf = {WFS_K_SNAPSHOT, n1};
        memset(&opts, 0, sizeof opts);
        join(nw, sizeof nw, worlds, "nworld");
        wfs_id nw1 = 0;
        CHECK_OK(wfs_world_create(na, nf, nw, &opts, &nw1));

        // The collector runs on a handle of its own, with retention 0 so anything due goes now.
        wfs_store *nb = NULL;
        CHECK_OK(wfs_store_open(nstore, &nb));
        g_orph_store = na;
        g_orph_world = nw1;
        g_orph_ran = 0;
        g_orph_rc = -1;
        wfs_test_before_trash_orphans = discard_before_orphans;
        wfs_gc_report nrep;
        memset(&nrep, 0, sizeof nrep);
        CHECK_OK(wfs_gc(nb, 0, &nrep));
        wfs_test_before_trash_orphans = NULL;
        CHECK(g_orph_ran == 1);
        CHECK_OK(g_orph_rc);                     // the discard ran to completion inside the window
        // Nothing was deleted, nothing was counted, and nothing is reported as a failure: the
        // tree is not trash the collector may touch, it is a discard that finished a moment ago.
        CHECK(nrep.trash_orphans == 0 && nrep.worlds_deleted == 0 && nrep.trash_failed == 0);
        wfs_world_rec nwr;
        CHECK_OK(wfs_world_info(na, nw1, &nwr));
        CHECK(nwr.state == WFS_ST_TRASHED && nwr.present && !exists(nw));
        // The proof that the tree really is whole: it comes back.
        CHECK_OK(wfs_world_restore(na, nw1));
        CHECK_OK(wfs_world_info(na, nw1, &nwr));
        CHECK(nwr.state == WFS_ST_ACTIVE && exists(nw));
        join(p, sizeof p, nw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));

        // And a directory in the trash that really is row-less still goes immediately, which is
        // the rule this is a refinement of, not a retreat from.
        snprintf(ntrash, sizeof ntrash, "%s/trash/W999-1", nstore);
        CHECK(mkdir(ntrash, 0755) == 0);
        join(p, sizeof p, ntrash, "junk.txt");
        write_file(p, "junk\n");
        memset(&nrep, 0, sizeof nrep);
        CHECK_OK(wfs_gc(nb, 0, &nrep));
        CHECK(nrep.trash_orphans == 1 && !exists(ntrash));
        wfs_store_close(nb);
        wfs_store_close(na);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", nstore, (unsigned long long)n1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (9th round, P1): `--now` never buries a world that came back -----------
    //
    // The helper behind `discard W<n> --now` followed the tree to whatever name it had, unlinked
    // it, and then wrote `state=DEAD, trash_path=''` with no predicate at all. A `restore W<n>`
    // landing between the lstat and the mark renames the tree home and marks the row ACTIVE; the
    // mark then answers -ENOENT, which reads as "somebody deleted it already", and that final
    // unconditional UPDATE buried a live world sitting at its home path. Both writes now carry
    // the state and the trash path this call is acting on behalf of, and zero rows changed is
    // -ESTALE rather than a silent success (docs/M1_DESIGN.md P18).
    {
        char mstore[4096], msrc[4096], mw[4096];
        join(mstore, sizeof mstore, root, "now-store");
        join(msrc, sizeof msrc, root, "now-src");
        CHECK(mkdir(msrc, 0755) == 0);
        join(p, sizeof p, msrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *ma = NULL;
        CHECK_OK(wfs_store_open(mstore, &ma));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "mb";
        wfs_id m1 = 0;
        CHECK_OK(wfs_snapshot_create(ma, msrc, &sopts, &m1));
        wfs_ref mf = {WFS_K_SNAPSHOT, m1};
        memset(&opts, 0, sizeof opts);
        join(mw, sizeof mw, worlds, "mworld");
        wfs_id mw1 = 0;
        CHECK_OK(wfs_world_create(ma, mf, mw, &opts, &mw1));
        CHECK_OK(wfs_world_discard(ma, mw1, 0, 0));
        wfs_world_rec mwr;
        CHECK_OK(wfs_world_info(ma, mw1, &mwr));
        CHECK(mwr.state == WFS_ST_TRASHED && !exists(mw));

        // The restore runs on a handle of its own, the way another process would.
        wfs_store *mb = NULL;
        CHECK_OK(wfs_store_open(mstore, &mb));
        g_now_store = mb;
        g_now_world = mw1;
        g_now_ran = 0;
        g_now_rc = -1;
        wfs_test_trash_crash = restore_before_now;
        CHECK_RC(wfs_world_discard(ma, mw1, 1, 0), -ESTALE);
        wfs_test_trash_crash = NULL;
        CHECK(g_now_ran == 1);
        CHECK_OK(g_now_rc);                       // the restore is the one that won
        CHECK_OK(wfs_world_info(ma, mw1, &mwr));
        CHECK(mwr.state == WFS_ST_ACTIVE && mwr.present && exists(mw));
        join(p, sizeof p, mw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));            // the tree came home whole
        CHECK_OK(wfs_world_diff(ma, mw1, 0, NULL, NULL));

        // PR #1 review (15th round, P1): and the restore that did not get all the way home. Its
        // row is TRASHING and its tree is still in the trash under the name `--now` followed, so
        // the rename would succeed -- and the -ESTALE below has to be an -ESTALE that did
        // nothing, not one reported after the tree was already renamed out from under a
        // restorable world.
        CHECK_OK(wfs_world_discard(ma, mw1, 0, 0));
        g_nowhalf_store = mb;
        g_nowhalf_world = mw1;
        g_nowhalf_ran = 0;
        g_nowhalf_rc = 0;
        g_nowhalf_path[0] = 0;
        wfs_test_trash_crash = restore_half_before_now;
        CHECK_RC(wfs_world_discard(ma, mw1, 1, 0), -ESTALE);
        wfs_test_trash_crash = NULL;
        CHECK(g_nowhalf_ran == 1);
        CHECK_RC(g_nowhalf_rc, -EINTR);           // the restore died between its row and its rename
        CHECK(g_nowhalf_path[0] && exists(g_nowhalf_path));
        char mdel[4200];
        snprintf(mdel, sizeof mdel, "%s.deleting", g_nowhalf_path);
        CHECK(!exists(mdel));
        join(p, sizeof p, g_nowhalf_path, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        // The interrupted restore resolves the way it always did, and the world comes back.
        wfs_store *mc = NULL;
        CHECK_OK(wfs_store_open(mstore, &mc));    // the open runs the TRASHING recovery
        CHECK_OK(wfs_world_info(mc, mw1, &mwr));
        CHECK(mwr.state == WFS_ST_TRASHED);
        CHECK_OK(wfs_world_restore(mc, mw1));
        CHECK_OK(wfs_world_info(mc, mw1, &mwr));
        CHECK(mwr.state == WFS_ST_ACTIVE && mwr.present && exists(mw));
        wfs_store_close(mc);

        // And the ordinary `--now` is untouched, from both of its two entry points: straight
        // from ACTIVE, and brought forward on a world that is already in the trash.
        CHECK_OK(wfs_world_discard(ma, mw1, 1, 0));
        CHECK_OK(wfs_world_info(ma, mw1, &mwr));
        CHECK(mwr.state == WFS_ST_DEAD && !exists(mw));
        wfs_id mw2 = 0;
        char mw2p[4096];
        join(mw2p, sizeof mw2p, worlds, "mworld2");
        CHECK_OK(wfs_world_create(ma, mf, mw2p, &opts, &mw2));
        CHECK_OK(wfs_world_discard(ma, mw2, 0, 0));
        CHECK_OK(wfs_world_discard(ma, mw2, 1, 0));
        CHECK_OK(wfs_world_info(ma, mw2, &mwr));
        CHECK(mwr.state == WFS_ST_DEAD && !exists(mw2p));
        // ... and a snapshot's `--now` still goes all the way through the same helper.
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "mb2";
        wfs_id m2 = 0;
        CHECK_OK(wfs_snapshot_create(ma, msrc, &sopts, &m2));
        CHECK_OK(wfs_snapshot_discard(ma, m2, 1, 0));
        wfs_snapshot_rec msr;
        CHECK_OK(wfs_snapshot_info(ma, m2, &msr));
        CHECK(msr.state == WFS_ST_DEAD);
        wfs_store_close(mb);
        wfs_store_close(ma);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", mstore, (unsigned long long)m1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (6th round, P2): a hardlink manifest that will not read is a failure -----
    //
    // The row's hl_groups says "this snapshot has n groups of names that share an inode"; the
    // manifest's own section says which names. A fork and a pool filler both gate on the first
    // and replay the second, and both used to treat an unreadable manifest -- or one holding
    // fewer groups than the row claims -- as "nothing to replay". clonefile(2) breaks every
    // intra-tree hardlink, so what they published was a tree with independent files where the
    // snapshot records one inode under n names: silent, and invisible afterwards, because
    // nothing downstream reads the manifest again.
    {
        char hstore[4096], hsrc[4096], hman[4096], hbak[4096], hw[4096], hlk[4096];
        join(hstore, sizeof hstore, root, "hlman-store");
        join(hsrc, sizeof hsrc, root, "hlman-src");
        CHECK(mkdir(hsrc, 0755) == 0);
        join(p, sizeof p, hsrc, "a.txt");
        write_file(p, "linked\n");
        join(hlk, sizeof hlk, hsrc, "b.txt");
        CHECK(link(p, hlk) == 0);
        wfs_store *hs = NULL;
        CHECK_OK(wfs_store_open(hstore, &hs));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlman";
        wfs_id h1 = 0;
        CHECK_OK(wfs_snapshot_create(hs, hsrc, &sopts, &h1));
        CHECK_OK(wfs_snapshot_info(hs, h1, &sr));
        CHECK(sr.hl_groups == 1);
        snprintf(hman, sizeof hman, "%s/snapshots/S%llu/manifest", hstore, (unsigned long long)h1);
        join(hbak, sizeof hbak, root, "hlman.bak");
        copy_file(hman, hbak);

        // The control, with the manifest as the snapshot wrote it: the pair is one inode again
        // on the other side of the clone.
        wfs_ref hf = {WFS_K_SNAPSHOT, h1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(hw, sizeof hw, worlds, "hlman-w");
        wfs_id hw1 = 0;
        CHECK_OK(wfs_world_create(hs, hf, hw, &opts, &hw1));
        join(p, sizeof p, hw, "a.txt");
        join(q, sizeof q, hw, "b.txt");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        CHECK_OK(wfs_world_discard(hs, hw1, 1, 0));

        // (a) the section is gone but the manifest is otherwise intact: fewer groups than the
        // row says. The old code called that "nothing to replay".
        strip_hl_lines(hman);
        wfs_verify_report hvr;
        CHECK_RC(wfs_snapshot_verify(hs, h1, &hvr), WFS_E_SNAPSHOT_DIRTY);
        CHECK(hvr.modified == 1 && strstr(hvr.first_bad, "manifest"));
        size_t before = 0;
        CHECK_OK(wfs_world_list(hs, 1, NULL, 0, &before));
        CHECK_RC(wfs_world_create(hs, hf, hw, &opts, &hw1), WFS_E_SNAPSHOT_DIRTY);
        CHECK(!exists(hw));                                     // nothing published at --to
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);        // and no clone left behind
        size_t after = 0;
        CHECK_OK(wfs_world_list(hs, 1, NULL, 0, &after));
        CHECK(after == before);                                 // the CREATING row went with it
        uint64_t made = 1;
        CHECK_RC(wfs_pool_fill(hs, h1, 1, &made), WFS_E_SNAPSHOT_DIRTY);
        CHECK(made == 0);
        uint64_t ready = 1;
        CHECK_OK(wfs_pool_ready(hs, h1, &ready));
        CHECK(ready == 0);                                      // never a READY entry

        // (b) no manifest at all. On a gated snapshot the manifest is also the gate's lock file
        // (snapshot_access.cpp), so nothing ever gets as far as the replay: the gate cannot be
        // opened and the fork stops with plain -ENOENT. A --hard snapshot has no gate, and there
        // the replay is the only thing that reads the manifest -- which is where the read error
        // used to be swallowed.
        CHECK(unlink(hman) == 0);
        CHECK_RC(wfs_world_create(hs, hf, hw, &opts, &hw1), -ENOENT);
        copy_file(hbak, hman);

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlman-hard";
        sopts.hard = 1;
        wfs_id h2 = 0;
        CHECK_OK(wfs_snapshot_create(hs, hsrc, &sopts, &h2));
        CHECK_OK(wfs_snapshot_info(hs, h2, &sr));
        CHECK(sr.hl_groups == 1);
        char hman2[4096];
        snprintf(hman2, sizeof hman2, "%s/snapshots/S%llu/manifest", hstore, (unsigned long long)h2);
        CHECK(unlink(hman2) == 0);
        wfs_ref hf2 = {WFS_K_SNAPSHOT, h2};
        char hw2[4096];
        join(hw2, sizeof hw2, worlds, "hlman-w2");
        wfs_id hw2id = 0;
        CHECK_RC(wfs_world_create(hs, hf2, hw2, &opts, &hw2id), WFS_E_SNAPSHOT_DIRTY);
        CHECK(!exists(hw2));
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);
        made = 1;
        CHECK_RC(wfs_pool_fill(hs, h2, 1, &made), WFS_E_SNAPSHOT_DIRTY);
        CHECK(made == 0);
        CHECK_OK(wfs_pool_ready(hs, h2, &ready));
        CHECK(ready == 0);

        // Put S<n>'s section back and everything works again: the refusal is about the damage,
        // not about hardlinked snapshots.
        copy_file(hbak, hman);
        CHECK_OK(wfs_snapshot_verify(hs, h1, &hvr));
        CHECK_OK(wfs_world_create(hs, hf, hw, &opts, &hw1));
        join(p, sizeof p, hw, "a.txt");
        join(q, sizeof q, hw, "b.txt");
        CHECK(ino_of(p) == ino_of(q) && nlink_of(p) == 2);
        made = 0;
        CHECK_OK(wfs_pool_fill(hs, h1, 1, &made));
        CHECK(made == 1);
        CHECK_OK(wfs_pool_ready(hs, h1, &ready));
        CHECK(ready == 1);
        wfs_store_close(hs);
    }

    // ---- PR #1 review (9th round, P2): a group that is not the size it declares is damage ----
    //
    // The 8th round's check only asked that a group hold at least two names. That catches a
    // manifest cut short, and misses the shape where a member is re-tagged into the neighbouring
    // group: the group count and the name count are both untouched, so the `#hl` header still
    // agrees with its own section, and the replay then links a name belonging to one inode onto
    // another group's canonical file -- cloned content overwritten, in a fork that reported
    // success. `nlink` is the authority and needs no new manifest field: the scan writes a group
    // only when every one of the inode's links was found inside the tree, and a group that
    // reaches outside it is counted in the header's external totals and never written as `hl`
    // lines at all. So a group's member count IS its nlink, and this fixture has one of each.
    {
        char gstore[4096], gsrc[4096], gext[4096], gman[4096], gbak[4096], gw[4096], q2[4096];
        join(gstore, sizeof gstore, root, "hlsize-store");
        join(gsrc, sizeof gsrc, root, "hlsize-src");
        join(gext, sizeof gext, root, "hlsize-ext");
        CHECK(mkdir(gsrc, 0755) == 0);
        CHECK(mkdir(gext, 0755) == 0);
        // two three-member groups, entirely inside the tree
        static const char *gnames[2][3] = {{"a1", "a2", "a3"}, {"b1", "b2", "b3"}};
        for (int gi = 0; gi < 2; ++gi) {
            join(p, sizeof p, gsrc, gnames[gi][0]);
            write_file(p, gi ? "bbb\n" : "aaa\n");
            for (int k = 1; k < 3; ++k) {
                join(q2, sizeof q2, gsrc, gnames[gi][k]);
                CHECK(link(p, q2) == 0);
            }
        }
        // ... and one group with a genuine link outside it: nlink 3, two names in the tree.
        join(p, sizeof p, gext, "shared");
        write_file(p, "ext\n");
        join(q2, sizeof q2, gsrc, "e1");
        CHECK(link(p, q2) == 0);
        join(q2, sizeof q2, gsrc, "e2");
        CHECK(link(p, q2) == 0);
        // One mtime and one length for all of them, so that the replay's "this name is still the
        // file the scan saw" check (size + mtime) cannot accidentally save a group that has been
        // handed a member belonging to another inode. That accident is what makes the damage
        // below silent rather than merely wrong.
        {
            struct timespec ts[2];
            ts[0].tv_sec = 1600000000; ts[0].tv_nsec = 0;
            ts[1] = ts[0];
            for (int gi = 0; gi < 2; ++gi)
                for (int k = 0; k < 3; ++k) {
                    join(p, sizeof p, gsrc, gnames[gi][k]);
                    CHECK(utimensat(AT_FDCWD, p, ts, 0) == 0);
                }
        }

        wfs_store *gs = NULL;
        CHECK_OK(wfs_store_open(gstore, &gs));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlsize";
        wfs_id g1 = 0;
        CHECK_OK(wfs_snapshot_create(gs, gsrc, &sopts, &g1));
        CHECK_OK(wfs_snapshot_info(gs, g1, &sr));
        CHECK(sr.hl_groups == 2);                 // the external group is counted, not claimed
        snprintf(gman, sizeof gman, "%s/snapshots/S%llu/manifest", gstore, (unsigned long long)g1);
        join(gbak, sizeof gbak, root, "hlsize.bak");
        copy_file(gman, gbak);

        // The control: a fork replays both full groups, and the external one is simply not
        // rebuilt -- which is what "a clone cannot be given links to files it does not contain"
        // has always meant, and it is not an error.
        wfs_ref gf = {WFS_K_SNAPSHOT, g1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(gw, sizeof gw, worlds, "hlsize-w");
        wfs_id gw1 = 0;
        CHECK_OK(wfs_world_create(gs, gf, gw, &opts, &gw1));
        for (int gi = 0; gi < 2; ++gi) {
            join(p, sizeof p, gw, gnames[gi][0]);
            CHECK(nlink_of(p) == 3);
            for (int k = 1; k < 3; ++k) {
                join(q2, sizeof q2, gw, gnames[gi][k]);
                CHECK(ino_of(p) == ino_of(q2));
            }
        }
        join(p, sizeof p, gw, "e1");
        join(q2, sizeof q2, gw, "e2");
        CHECK(ino_of(p) != ino_of(q2));           // never claimed, never replayed
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "ext\n"));
        wfs_verify_report gvr;
        CHECK_OK(wfs_snapshot_verify(gs, g1, &gvr));
        CHECK_OK(wfs_world_discard(gs, gw1, 1, 0));

        // The damage: the last member of the first group re-tagged into the second. 2 + 4 names,
        // the totals preserved, the header still right -- and the second group would replay four
        // names onto one inode, one of which is the first group's.
        retag_hl_line(gman, 2, 1);
        CHECK_RC(wfs_snapshot_verify(gs, g1, &gvr), WFS_E_SNAPSHOT_DIRTY);
        CHECK(gvr.modified == 1 && strstr(gvr.first_bad, "manifest"));
        size_t gbefore = 0;
        CHECK_OK(wfs_world_list(gs, 1, NULL, 0, &gbefore));
        CHECK_RC(wfs_world_create(gs, gf, gw, &opts, &gw1), WFS_E_SNAPSHOT_DIRTY);
        CHECK(!exists(gw));                                     // nothing published at --to
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);        // and no clone left behind
        size_t gafter = 0;
        CHECK_OK(wfs_world_list(gs, 1, NULL, 0, &gafter));
        CHECK(gafter == gbefore);
        uint64_t gmade = 1;
        CHECK_RC(wfs_pool_fill(gs, g1, 1, &gmade), WFS_E_SNAPSHOT_DIRTY);
        CHECK(gmade == 0);
        uint64_t gready = 1;
        CHECK_OK(wfs_pool_ready(gs, g1, &gready));
        CHECK(gready == 0);

        // Put it back and everything works again, external group and all: the refusal is about
        // the damage, not about hardlinked snapshots or about links that reach outside a tree.
        copy_file(gbak, gman);
        CHECK_OK(wfs_snapshot_verify(gs, g1, &gvr));
        CHECK_OK(wfs_world_create(gs, gf, gw, &opts, &gw1));
        join(p, sizeof p, gw, "a1");
        join(q2, sizeof q2, gw, "a3");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 3);
        join(p, sizeof p, gw, "b1");
        join(q2, sizeof q2, gw, "b3");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 3);
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "bbb\n"));            // and nobody's content came from the other group
        gmade = 0;
        CHECK_OK(wfs_pool_fill(gs, g1, 1, &gmade));
        CHECK(gmade == 1);
        wfs_store_close(gs);
    }

    // ---- PR #1 review (10th round, P1): the tmp sweep never takes a live snapshot's clone ----
    //
    // gc's CREATING pass is careful with half-built snapshots: it skips any row whose producer
    // is still alive, because `S<n>.wfs-tmp` is then a clone in progress. The suffix sweep that
    // runs straight after it asked nothing at all -- every `*.wfs-tmp` under <store>/snapshots
    // went on sight -- so it deleted the very tree that pass had just spared, and the
    // `wfs_snapshot_create()` on the other side of it failed mid-clone. The sweep's verdict is
    // "nothing names this tree", which is a verdict about live rows, so it is now asked of the
    // live rows under the store mutex: the `S<n>` is parsed back into a row id and any row in
    // any state but DEAD keeps its tree (docs/M1_DESIGN.md P18).
    {
        char tstore[4096], tsrc[4096], tsnaps[4096], tjunk[4096], tw[4096];
        join(tstore, sizeof tstore, root, "sweeprace-store");
        join(tsrc, sizeof tsrc, root, "sweeprace-src");
        CHECK(mkdir(tsrc, 0755) == 0);
        join(p, sizeof p, tsrc, "a.txt");
        write_file(p, "one\n");
        join(q, sizeof q, tsrc, "sub");
        CHECK(mkdir(q, 0755) == 0);
        join(p, sizeof p, q, "b.txt");
        write_file(p, "two\n");
        wfs_store *ta = NULL;
        CHECK_OK(wfs_store_open(tstore, &ta));
        // The collector runs on a handle of its own, the way another process would.
        wfs_store *tb = NULL;
        CHECK_OK(wfs_store_open(tstore, &tb));
        g_sweep_store = tb;
        g_sweep_ran = 0;
        g_sweep_rc = -1;
        wfs_test_before_snapshot_clone = gc_before_snapshot_clone;
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "sweeprace";
        wfs_id t1 = 0;
        CHECK_OK(wfs_snapshot_create(ta, tsrc, &sopts, &t1));   // used to fail here, mid-clone
        wfs_test_before_snapshot_clone = NULL;
        CHECK(g_sweep_ran == 1);
        CHECK_OK(g_sweep_rc);
        // Nothing removed, nothing counted as stuck, and no row buried: the tree under the
        // sweep's hand was not the collector's to touch.
        CHECK(g_sweep_rep.tmp_removed == 0 && g_sweep_rep.tmp_failed == 0);
        CHECK(g_sweep_rep.snapshots_deleted == 0);
        // And the snapshot is whole, by its own manifest and by what a fork from it gets.
        wfs_snapshot_rec tr;
        CHECK_OK(wfs_snapshot_info(ta, t1, &tr));
        CHECK(tr.state == WFS_ST_ACTIVE);
        wfs_verify_report tvr;
        CHECK_OK(wfs_snapshot_verify(ta, t1, &tvr));
        CHECK(tvr.missing == 0 && tvr.modified == 0 && tvr.extra == 0 && tvr.unprotected == 0);
        CHECK_OK(wfs_snapshot_info(tb, t1, &tr));               // the collector's handle agrees
        CHECK(tr.state == WFS_ST_ACTIVE);
        wfs_ref tf = {WFS_K_SNAPSHOT, t1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(tw, sizeof tw, worlds, "sweeprace-w");
        wfs_id tw1 = 0;
        CHECK_OK(wfs_world_create(ta, tf, tw, &opts, &tw1));
        join(p, sizeof p, tw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        join(p, sizeof p, tw, "sub/b.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "two\n"));

        wfs_gc_report trep;
        snprintf(tsnaps, sizeof tsnaps, "%s/snapshots", tstore);
        // The controls, because this is a refinement of the sweep and not a retreat from it.
        // (a) an `S<n>.wfs-tmp` whose id no row has at all still goes immediately.
        snprintf(tjunk, sizeof tjunk, "%s/S999999%s", tsnaps, WFS_TMP_SUFFIX);
        CHECK(mkdir(tjunk, 0755) == 0);
        join(p, sizeof p, tjunk, "junk.txt");
        write_file(p, "junk\n");
        memset(&trep, 0, sizeof trep);
        CHECK_OK(wfs_gc(tb, 0, &trep));
        CHECK(trep.tmp_removed == 1 && !exists(tjunk));
        // (b) so does a `*.wfs-tmp` that is not an `S<n>` name at all. Nothing writes one, and
        // an unparseable name is exactly the case no row can speak for.
        snprintf(tjunk, sizeof tjunk, "%s/stray%s", tsnaps, WFS_TMP_SUFFIX);
        CHECK(mkdir(tjunk, 0755) == 0);
        memset(&trep, 0, sizeof trep);
        CHECK_OK(wfs_gc(tb, 0, &trep));
        CHECK(trep.tmp_removed == 1 && !exists(tjunk));
        // (c) and the `.wfs-tmp` of an id a live ACTIVE row holds is left alone: `S<n>` beside
        // it is a real snapshot, so the name is not one nothing claims.
        snprintf(tjunk, sizeof tjunk, "%s/S%llu%s", tsnaps, (unsigned long long)t1,
                 WFS_TMP_SUFFIX);
        CHECK(mkdir(tjunk, 0755) == 0);
        memset(&trep, 0, sizeof trep);
        CHECK_OK(wfs_gc(tb, 0, &trep));
        CHECK(trep.tmp_removed == 0 && exists(tjunk));
        CHECK(rmdir(tjunk) == 0);
        CHECK_OK(wfs_snapshot_verify(ta, t1, &tvr));            // and the snapshot is still fine
        wfs_store_close(tb);
        wfs_store_close(ta);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", tstore, (unsigned long long)t1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (11th round, P2): no name may be in a manifest twice ------------------
    //
    // The manifest checks of the 8th and 9th rounds are all counts: the header's group and name
    // totals, and every group being exactly as big as its nlink. One damage shape keeps all of
    // them -- a member repeated, either inside its own group or pasted over a member of the next
    // one. The replay treats the repeat as already linked (it IS the canonical inode) and the
    // member it displaced is simply not in the manifest any anymore, so the fork comes back rc 0
    // with an independent inode where the snapshot has a link; across two groups the pasted-in
    // name is replayed against the other group's canonical file and a clone's content is lost.
    {
        char dstore[4096], dsrc[4096], dman[4096], dbak[4096], dw[4096], q2[4096];
        static const char *dnames[2][3] = {{"a1", "a2", "a3"}, {"b1", "b2", "b3"}};
        join(dstore, sizeof dstore, root, "hldup-store");
        join(dsrc, sizeof dsrc, root, "hldup-src");
        CHECK(mkdir(dsrc, 0755) == 0);
        for (int gi = 0; gi < 2; ++gi) {
            join(p, sizeof p, dsrc, dnames[gi][0]);
            write_file(p, gi ? "bbb\n" : "aaa\n");
            for (int k = 1; k < 3; ++k) {
                join(q2, sizeof q2, dsrc, dnames[gi][k]);
                CHECK(link(p, q2) == 0);
            }
        }
        // One size and one mtime for all six, so that the replay's "this name is still the file
        // the scan saw" guard cannot accidentally save a group handed another group's member.
        {
            struct timespec ts[2];
            ts[0].tv_sec = 1600000000; ts[0].tv_nsec = 0;
            ts[1] = ts[0];
            for (int gi = 0; gi < 2; ++gi)
                for (int k = 0; k < 3; ++k) {
                    join(p, sizeof p, dsrc, dnames[gi][k]);
                    CHECK(utimensat(AT_FDCWD, p, ts, 0) == 0);
                }
        }
        wfs_store *ds = NULL;
        CHECK_OK(wfs_store_open(dstore, &ds));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hldup";
        wfs_id d1 = 0;
        CHECK_OK(wfs_snapshot_create(ds, dsrc, &sopts, &d1));
        CHECK_OK(wfs_snapshot_info(ds, d1, &sr));
        CHECK(sr.hl_groups == 2);
        snprintf(dman, sizeof dman, "%s/snapshots/S%llu/manifest", dstore, (unsigned long long)d1);
        join(dbak, sizeof dbak, root, "hldup.bak");
        copy_file(dman, dbak);

        wfs_ref df = {WFS_K_SNAPSHOT, d1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(dw, sizeof dw, worlds, "hldup-w");
        wfs_verify_report dvr;

        // Three damaged manifests, each of which used to fork with rc 0:
        //   (a) `a3` replaced by a second `a1`: group 0 is {a1, a2, a1}, three names, nlink 3.
        //       a3 in the clone stays an inode of its own.
        //   (b) `b1` replaced by `a1`: a1 is now in both groups. Group 1 replays b2 and b3 onto
        //       *a1*, which is a different inode with the same size and mtime -- so the guard
        //       lets it through and "bbb" is gone from the fork.
        //   (c) a member replaced by a path that leaves the tree. Nothing ever checked that
        //       either, and the replay resolves these names against the clone's root -- so
        //       `../escape` is a file in the user's own directory, beside the fork's temporary,
        //       and the replay renamed the group's canonical inode onto it. `escape` below is
        //       that file, with the same size and mtime as the group so the guard cannot save
        //       it; it has to come through untouched.
        char desc[4096];
        join(desc, sizeof desc, worlds, "escape");
        write_file(desc, "xxx\n");
        {
            struct timespec ets[2];
            ets[0].tv_sec = 1600000000; ets[0].tv_nsec = 0;
            ets[1] = ets[0];
            CHECK(utimensat(AT_FDCWD, desc, ets, 0) == 0);
        }
        uint64_t desc_ino = ino_of(desc);
        for (int kind = 0; kind < 3; ++kind) {
            copy_file(dbak, dman);
            if (kind == 0) repath_hl_line(dman, 2, "a1");
            else if (kind == 1) repath_hl_line(dman, 3, "a1");
            else repath_hl_line(dman, 2, "../escape");
            CHECK_RC(wfs_snapshot_verify(ds, d1, &dvr), WFS_E_SNAPSHOT_DIRTY);
            CHECK(dvr.modified == 1 && strstr(dvr.first_bad, "manifest"));
            size_t dbefore = 0;
            CHECK_OK(wfs_world_list(ds, 1, NULL, 0, &dbefore));
            wfs_id dw1 = 0;
            CHECK_RC(wfs_world_create(ds, df, dw, &opts, &dw1), WFS_E_SNAPSHOT_DIRTY);
            CHECK(!exists(dw));                                  // nothing published at --to
            CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);     // and no clone left behind
            size_t dafter = 0;
            CHECK_OK(wfs_world_list(ds, 1, NULL, 0, &dafter));
            CHECK(dafter == dbefore);
            uint64_t dmade = 1;
            CHECK_RC(wfs_pool_fill(ds, d1, 1, &dmade), WFS_E_SNAPSHOT_DIRTY);
            CHECK(dmade == 0);
            uint64_t dready = 1;
            CHECK_OK(wfs_pool_ready(ds, d1, &dready));
            CHECK(dready == 0);
            // The file next door is the file next door, whatever a manifest says.
            CHECK(ino_of(desc) == desc_ino && nlink_of(desc) == 1);
            CHECK_OK(read_file(desc, buf, sizeof buf));
            CHECK(!strcmp(buf, "xxx\n"));
        }
        CHECK(unlink(desc) == 0);

        // Put the manifest back and the same snapshot forks with both groups rebuilt: the
        // refusal is about the repeat, not about hardlinked snapshots.
        copy_file(dbak, dman);
        CHECK_OK(wfs_snapshot_verify(ds, d1, &dvr));
        wfs_id dw1 = 0;
        CHECK_OK(wfs_world_create(ds, df, dw, &opts, &dw1));
        for (int gi = 0; gi < 2; ++gi) {
            join(p, sizeof p, dw, dnames[gi][0]);
            CHECK(nlink_of(p) == 3);
            for (int k = 1; k < 3; ++k) {
                join(q2, sizeof q2, dw, dnames[gi][k]);
                CHECK(ino_of(p) == ino_of(q2));
            }
            CHECK_OK(read_file(p, buf, sizeof buf));
            CHECK(!strcmp(buf, gi ? "bbb\n" : "aaa\n"));   // and no group took the other's file
        }
        CHECK_OK(wfs_world_discard(ds, dw1, 1, 0));
        wfs_store_close(ds);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", dstore, (unsigned long long)d1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (13th round, P2): the groups are checked against the snapshot tree ----
    //
    // Every manifest check up to here is structural -- the header's totals, each group's size
    // against its nlink, no name twice, no path that leaves the tree -- and all of them are
    // blind to one damage shape: two members exchanged between two groups. (a,b),(c,d) becomes
    // (a,c),(b,d): two groups, four names, nlink 2 each, every name once, every path inside.
    // The replay then links `c` onto `a` and `d` onto `b`, and when the four files share a size
    // and an mtime -- which they do, being clones of one snapshot walk -- the "is this still the
    // file the scan saw" guard cannot tell either: the fork came back rc 0 with `a` and `c`
    // welded and `c`'s content gone.
    //
    // Nothing in the manifest can catch this, because the manifest is the thing that is wrong.
    // The snapshot TREE is the immutable original, so every group is lstat'ed against it before
    // the replay -- one lstat per hardlinked name, the price hardlinks.h already quotes for a
    // verify -- and a group whose members do not land on one inode with that inode's nlink is
    // WFS_E_SNAPSHOT_DIRTY. The clone is never published. (Not against the clone: clonefile
    // breaks every hardlink, so the clone has nothing to say about which names shared an inode.)
    {
        char sstore[4096], ssrc[4096], sman[4096], sbak[4096], sw[4096], q2[4096];
        static const char *snames[4] = {"a", "b", "c", "d"};
        join(sstore, sizeof sstore, root, "hlswap-store");
        join(ssrc, sizeof ssrc, root, "hlswap-src");
        CHECK(mkdir(ssrc, 0755) == 0);
        join(p, sizeof p, ssrc, "a");
        write_file(p, "aaaa\n");
        join(q2, sizeof q2, ssrc, "b");
        CHECK(link(p, q2) == 0);
        join(p, sizeof p, ssrc, "c");
        write_file(p, "cccc\n");   // the same size, so only the inodes tell the pairs apart
        join(q2, sizeof q2, ssrc, "d");
        CHECK(link(p, q2) == 0);
        {
            struct timespec ts[2];
            ts[0].tv_sec = 1600000000; ts[0].tv_nsec = 0;
            ts[1] = ts[0];
            for (int k = 0; k < 4; ++k) {
                join(p, sizeof p, ssrc, snames[k]);
                CHECK(utimensat(AT_FDCWD, p, ts, 0) == 0);
            }
        }
        wfs_store *ss = NULL;
        CHECK_OK(wfs_store_open(sstore, &ss));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlswap";
        wfs_id s1 = 0;
        CHECK_OK(wfs_snapshot_create(ss, ssrc, &sopts, &s1));
        CHECK_OK(wfs_snapshot_info(ss, s1, &sr));
        CHECK(sr.hl_groups == 2 && sr.hardlinks == 4);
        CHECK(sr.hard == 0);   // a gated snapshot: the verify pass has to open the gate itself
        snprintf(sman, sizeof sman, "%s/snapshots/S%llu/manifest", sstore, (unsigned long long)s1);
        join(sbak, sizeof sbak, root, "hlswap.bak");
        copy_file(sman, sbak);

        // Lines 1 and 2 of the `hl` section are the second member of the first group and the
        // first member of the second, whichever order the scan wrote the groups in.
        swap_hl_paths(sman, 1, 2);

        wfs_ref sf = {WFS_K_SNAPSHOT, s1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(sw, sizeof sw, worlds, "hlswap-w");
        wfs_verify_report svr;
        CHECK_RC(wfs_snapshot_verify(ss, s1, &svr), WFS_E_SNAPSHOT_DIRTY);
        CHECK(svr.modified == 1 && strstr(svr.first_bad, "manifest"));
        size_t sbefore = 0;
        CHECK_OK(wfs_world_list(ss, 1, NULL, 0, &sbefore));
        wfs_id sw1 = 0;
        CHECK_RC(wfs_world_create(ss, sf, sw, &opts, &sw1), WFS_E_SNAPSHOT_DIRTY);
        CHECK(!exists(sw));                                  // nothing published at --to
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);     // and no clone left behind
        size_t safter = 0;
        CHECK_OK(wfs_world_list(ss, 1, NULL, 0, &safter));
        CHECK(safter == sbefore);
        uint64_t smade = 1;
        CHECK_RC(wfs_pool_fill(ss, s1, 1, &smade), WFS_E_SNAPSHOT_DIRTY);
        CHECK(smade == 0);
        uint64_t sready = 1;
        CHECK_OK(wfs_pool_ready(ss, s1, &sready));
        CHECK(sready == 0);

        // And the manifest as the snapshot wrote it forks, through the gate, with both pairs
        // rebuilt and neither file holding the other's bytes: the refusal is about the swap.
        copy_file(sbak, sman);
        CHECK_OK(wfs_snapshot_verify(ss, s1, &svr));
        CHECK_OK(wfs_world_create(ss, sf, sw, &opts, &sw1));
        for (int k = 0; k < 4; k += 2) {
            join(p, sizeof p, sw, snames[k]);
            join(q2, sizeof q2, sw, snames[k + 1]);
            CHECK(ino_of(p) == ino_of(q2));
            CHECK(nlink_of(p) == 2);
            CHECK_OK(read_file(p, buf, sizeof buf));
            CHECK(!strcmp(buf, k ? "cccc\n" : "aaaa\n"));
        }
        // The same through the pool, whose filler runs the same pass on its own clone.
        CHECK_OK(wfs_world_discard(ss, sw1, 1, 0));
        smade = 0;
        CHECK_OK(wfs_pool_fill(ss, s1, 1, &smade));
        CHECK(smade == 1);
        wfs_fork_result sfr;
        memset(&opts, 0, sizeof opts);
        memset(&sfr, 0, sizeof sfr);
        CHECK_OK(wfs_world_create_ex(ss, sf, sw, &opts, &sfr));
        sw1 = sfr.world;
        CHECK(sfr.from_pool == 1);
        for (int k = 0; k < 4; k += 2) {
            join(p, sizeof p, sw, snames[k]);
            join(q2, sizeof q2, sw, snames[k + 1]);
            CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        }
        CHECK_OK(wfs_world_discard(ss, sw1, 1, 0));
        wfs_store_close(ss);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", sstore, (unsigned long long)s1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (14th round, P2): a member path is a chain of real names ---------------
    //
    // manifest_path_sane() refused a leading '/' and a `..` component and let everything else
    // through -- so a member could be spelled a second way. `./d/a` and `d//a` are the same file
    // as `d/a` to every syscall and three different strings to every check the reader makes,
    // which is exactly the 11th round's repeat in a spelling the 11th round's uniqueness pass
    // cannot see: (d/a, ./d/a) is two members, both unique, nlink 2 as the header says, and
    // hardlinks_verify_groups lstats the two of them onto one inode whose nlink really is 2 --
    // because they ARE one name. The replay then finds the second member already on the
    // canonical inode, counts it linked and stops, and the real `d/b` stays an independent file
    // in a fork that reported success: the group the snapshot advertises is not in the tree.
    //
    // Nothing this library writes can be spelled that way. Every member is `rel` from the
    // walker, built by joining readdir names, and neither readdir(3) (which skips them
    // explicitly) nor getattrlistbulk(2) (which never returns them) yields `.`, `..` or an
    // empty name -- so a component is always a name and a member never has an empty one, a `.`
    // one or a trailing slash. That is the rule now, and no manifest this library ever wrote
    // becomes unreadable by it.
    {
        char pstore[4096], psrc[4096], pman[4096], pbak[4096], pw[4096], pd[4096], q2[4096];
        static const char *kSpell[3] = {"./d/a", "d//a", "d/a/"};
        join(pstore, sizeof pstore, root, "hlspell-store");
        join(psrc, sizeof psrc, root, "hlspell-src");
        CHECK(mkdir(psrc, 0755) == 0);
        join(pd, sizeof pd, psrc, "d");
        CHECK(mkdir(pd, 0755) == 0);
        join(p, sizeof p, pd, "a");
        write_file(p, "aaaa\n");
        join(q2, sizeof q2, pd, "b");
        CHECK(link(p, q2) == 0);

        wfs_store *ps = NULL;
        CHECK_OK(wfs_store_open(pstore, &ps));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlspell";
        wfs_id p1 = 0;
        CHECK_OK(wfs_snapshot_create(ps, psrc, &sopts, &p1));
        CHECK_OK(wfs_snapshot_info(ps, p1, &sr));
        CHECK(sr.hl_groups == 1 && sr.hl_external == 0 && sr.hardlinks == 2);
        snprintf(pman, sizeof pman, "%s/snapshots/S%llu/manifest", pstore, (unsigned long long)p1);
        join(pbak, sizeof pbak, root, "hlspell.bak");
        copy_file(pman, pbak);

        wfs_ref pf = {WFS_K_SNAPSHOT, p1};
        join(pw, sizeof pw, worlds, "hlspell-w");
        wfs_verify_report pvr;
        for (int k = 0; k < 3; ++k) {
            // The second member, `d/b`, respelled as a second `d/a`. Everything else in the
            // manifest -- the header, the group id, the nlink, the line count -- is untouched.
            copy_file(pbak, pman);
            repath_hl_line(pman, 1, kSpell[k]);
            CHECK_RC(wfs_snapshot_verify(ps, p1, &pvr), WFS_E_SNAPSHOT_DIRTY);
            CHECK(pvr.modified == 1 && strstr(pvr.first_bad, "manifest"));
            size_t pbefore = 0;
            CHECK_OK(wfs_world_list(ps, 1, NULL, 0, &pbefore));
            memset(&opts, 0, sizeof opts);
            opts.no_pool = 1;
            wfs_id pw1 = 0;
            CHECK_RC(wfs_world_create(ps, pf, pw, &opts, &pw1), WFS_E_SNAPSHOT_DIRTY);
            CHECK(!exists(pw));                                  // nothing published at --to
            CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);     // and no clone left behind
            size_t pafter = 0;
            CHECK_OK(wfs_world_list(ps, 1, NULL, 0, &pafter));
            CHECK(pafter == pbefore);
            uint64_t pmade = 1;
            CHECK_RC(wfs_pool_fill(ps, p1, 1, &pmade), WFS_E_SNAPSHOT_DIRTY);
            CHECK(pmade == 0);
            uint64_t pready = 1;
            CHECK_OK(wfs_pool_ready(ps, p1, &pready));
            CHECK(pready == 0);
        }

        // And the manifest as the snapshot wrote it forks with the pair rebuilt: the refusal is
        // about the spelling, not about hardlinked snapshots.
        copy_file(pbak, pman);
        CHECK_OK(wfs_snapshot_verify(ps, p1, &pvr));
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        wfs_id pw1 = 0;
        CHECK_OK(wfs_world_create(ps, pf, pw, &opts, &pw1));
        join(p, sizeof p, pw, "d/a");
        join(q2, sizeof q2, pw, "d/b");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "aaaa\n"));
        CHECK_OK(wfs_world_discard(ps, pw1, 1, 0));
        wfs_store_close(ps);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", pstore, (unsigned long long)p1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (14th round, P2): the marker is not one of the snapshot's files -------
    //
    // A checkpoint clones the world and then unlinks the `.world` marker out of the clone: the
    // source world's identity is not the snapshot's. But the scan that collects the hardlink
    // groups ran over the world, marker and all -- so a marker that somebody's tool had
    // hardlinked (a backup copy, a content-addressed store, a `cp -al` of the tree next door;
    // ordinary in a workspace, and nothing in the world's identity minds it) went into a group
    // with its twin. The replay then found the marker gone from the clone, counted it
    // `missing` -- and left `whole` true, so the group stayed in the manifest and in hl_groups.
    // The snapshot published, and from that moment every verify and every fork lstat'ed a
    // member the tree does not have: WFS_E_SNAPSHOT_DIRTY, for ever, on a snapshot that was
    // never damaged.
    //
    // Both halves are fixed. The scan is told the one name the caller takes back out, so the
    // marker never enters a group at all (its twin is then a file with one name, and the 22nd
    // round below is what makes the scan say so rather than call it external), and a member that
    // is missing from the clone at replay time marks the group broken, so it is dropped from the
    // manifest and from hl_groups -- the round-5 rule: a snapshot describes the tree it has.
    {
        char kstore[4096], ksrc[4096], kw[4096], kcw[4096], q2[4096];
        join(kstore, sizeof kstore, root, "hlmark-store");
        join(ksrc, sizeof ksrc, root, "hlmark-src");
        CHECK(mkdir(ksrc, 0755) == 0);
        join(p, sizeof p, ksrc, "c1");
        write_file(p, "pair\n");
        join(q2, sizeof q2, ksrc, "c2");
        CHECK(link(p, q2) == 0);

        wfs_store *ks = NULL;
        CHECK_OK(wfs_store_open(kstore, &ks));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlmark";
        wfs_id k1 = 0;
        CHECK_OK(wfs_snapshot_create(ks, ksrc, &sopts, &k1));
        wfs_ref kf = {WFS_K_SNAPSHOT, k1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(kw, sizeof kw, worlds, "hlmark-w");
        wfs_id kw1 = 0;
        CHECK_OK(wfs_world_create(ks, kf, kw, &opts, &kw1));
        join(p, sizeof p, kw, "c1");
        join(q2, sizeof q2, kw, "c2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);

        // The marker, hardlinked to an ordinary name in the world.
        join(p, sizeof p, kw, ".world");
        join(q2, sizeof q2, kw, "m");
        CHECK(link(p, q2) == 0);
        CHECK(nlink_of(p) == 2 && ino_of(p) == ino_of(q2));
        wfs_identity kid;
        CHECK_OK(wfs_world_verify_identity(ks, kw, &kid));   // still this world, in this store
        CHECK(kid.world_id == kw1);

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlmark-cp";
        wfs_id kc = 0;
        CHECK_OK(wfs_snapshot_create(ks, kw, &sopts, &kc));
        CHECK_OK(wfs_snapshot_info(ks, kc, &sr));
        CHECK(sr.hl_groups == 1);     // the control pair, and no group holding the marker
        // PR #1 review (22nd round, P2): and `m` is not external either. The marker was this
        // tree's own name, so with it gone `m`'s inode has ONE name and every one of them is
        // inside the snapshot -- a plain file, which is neither a group nor a link reaching out
        // of the tree, and not one of the tree's hardlinked files. (Both numbers changed in that
        // round: hl_external was 1 and hardlinks was 3.)
        CHECK(sr.hl_external == 0);
        CHECK(sr.hardlinks == 2);     // c1, c2 -- `m` has one link once the marker is gone
        wfs_verify_report kvr;
        CHECK_OK(wfs_snapshot_verify(ks, kc, &kvr));
        CHECK(kvr.missing == 0 && kvr.modified == 0 && kvr.extra == 0 && kvr.unprotected == 0);
        // The snapshot tree itself: the marker is gone, `m` is an ordinary file of its own, and
        // the control pair is one inode under two names.
        CHECK(chmod(sr.path, 0700) == 0);
        join(p, sizeof p, sr.path, ".world");
        CHECK(!exists(p));
        join(p, sizeof p, sr.path, "m");
        CHECK(nlink_of(p) == 1);
        join(p, sizeof p, sr.path, "c1");
        join(q2, sizeof q2, sr.path, "c2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        CHECK(chmod(sr.path, 0000) == 0);

        // And it forks -- which is the whole point: before this, the checkpoint published and
        // the very next fork refused it.
        wfs_ref kcf = {WFS_K_SNAPSHOT, kc};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(kcw, sizeof kcw, worlds, "hlmark-cw");
        wfs_id kcw1 = 0;
        CHECK_OK(wfs_world_create(ks, kcf, kcw, &opts, &kcw1));
        join(p, sizeof p, kcw, "c1");
        join(q2, sizeof q2, kcw, "c2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        join(p, sizeof p, kcw, "m");
        CHECK(nlink_of(p) == 1);                 // the marker's old twin, an ordinary file now
        join(q2, sizeof q2, kcw, ".world");
        CHECK(ino_of(p) != ino_of(q2));          // and the fork's marker is the fork's own
        CHECK(nlink_of(q2) == 1);

        // The same through the pool, whose filler replays the manifest with no verify root.
        CHECK_OK(wfs_world_discard(ks, kcw1, 1, 0));
        uint64_t kmade = 0;
        CHECK_OK(wfs_pool_fill(ks, kc, 1, &kmade));
        CHECK(kmade == 1);
        wfs_fork_result kfr;
        memset(&opts, 0, sizeof opts);
        memset(&kfr, 0, sizeof kfr);
        CHECK_OK(wfs_world_create_ex(ks, kcf, kcw, &opts, &kfr));
        kcw1 = kfr.world;
        CHECK(kfr.from_pool == 1);
        join(p, sizeof p, kcw, "c1");
        join(q2, sizeof q2, kcw, "c2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        join(p, sizeof p, kcw, "m");
        CHECK(nlink_of(p) == 1);

        CHECK_OK(wfs_world_discard(ks, kcw1, 1, 0));
        CHECK_OK(wfs_world_discard(ks, kw1, 1, 0));
        wfs_store_close(ks);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", kstore, (unsigned long long)k1);
        chmod(p, 0700);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", kstore, (unsigned long long)kc);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (22nd round, P2): the marker's twins are still each other's ------------
    //
    // The 14th round keeps the `.world` marker out of the scan's groups, because a checkpoint
    // unlinks it out of the clone it is about to publish. That is right, and it left one thing
    // half done: the marker's record is the only one dropped, while every OTHER name on that
    // inode keeps an st_nlink that still counts the marker. With two such names -- `.world`,
    // `m1`, `m2` on one inode, nlink 3 -- the grouping pass found 2 names for an inode that says
    // 3, called the group external, and wrote nothing. The clone had already broken the link and
    // the marker was gone, so `m1` and `m2` were published as two independent files where the
    // world has one inode under two names: exactly the damage P9 exists to prevent, silently,
    // and inherited by every fork of that snapshot.
    //
    // A name the caller takes out of the tree is not a link of that tree. So the scan remembers
    // the excluded name's inode and subtracts it from that inode's nlink: 3 - 1 == 2 == the two
    // names found, so (m1, m2) is a whole in-tree group and is written as one. The 14th round's
    // own shape -- marker plus ONE other name -- becomes a single surviving name, which is a
    // plain file: no group, and not external either, because nothing of that inode is outside
    // the tree. Both are what the published snapshot really contains.
    {
        char zstore[4096], zsrc[4096], zw[4096], zcw[4096], zm2[4096], q2[4096];
        join(zstore, sizeof zstore, root, "hlmark2-store");
        join(zsrc, sizeof zsrc, root, "hlmark2-src");
        CHECK(mkdir(zsrc, 0755) == 0);
        join(p, sizeof p, zsrc, "c1");
        write_file(p, "pair\n");
        join(q2, sizeof q2, zsrc, "c2");
        CHECK(link(p, q2) == 0);

        wfs_store *zs = NULL;
        CHECK_OK(wfs_store_open(zstore, &zs));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlmark2";
        wfs_id z1 = 0;
        CHECK_OK(wfs_snapshot_create(zs, zsrc, &sopts, &z1));
        wfs_ref zf = {WFS_K_SNAPSHOT, z1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(zw, sizeof zw, worlds, "hlmark2-w");
        wfs_id zw1 = 0;
        CHECK_OK(wfs_world_create(zs, zf, zw, &opts, &zw1));

        // Two ordinary names on the marker's inode: nlink 3, and exactly one of the three is
        // not a file of the tree the checkpoint publishes.
        join(p, sizeof p, zw, ".world");
        join(q2, sizeof q2, zw, "m1");
        CHECK(link(p, q2) == 0);
        join(zm2, sizeof zm2, zw, "m2");
        CHECK(link(p, zm2) == 0);
        CHECK(nlink_of(p) == 3 && ino_of(p) == ino_of(q2) && ino_of(p) == ino_of(zm2));
        wfs_identity zid;
        CHECK_OK(wfs_world_verify_identity(zs, zw, &zid));   // still this world, in this store
        CHECK(zid.world_id == zw1);

        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlmark2-cp";
        wfs_id zc = 0;
        CHECK_OK(wfs_snapshot_create(zs, zw, &sopts, &zc));
        CHECK_OK(wfs_snapshot_info(zs, zc, &sr));
        CHECK(sr.hl_groups == 2);     // the control pair AND (m1, m2)
        CHECK(sr.hl_external == 0);   // nothing of that inode is outside: the third name was ours
        CHECK(sr.hardlinks == 4);     // c1, c2, m1, m2 -- the marker is not a file of this tree
        wfs_verify_report zvr;
        CHECK_OK(wfs_snapshot_verify(zs, zc, &zvr));
        CHECK(zvr.missing == 0 && zvr.modified == 0 && zvr.extra == 0 && zvr.unprotected == 0);
        // The snapshot tree itself: no marker, and the pair on one inode with nlink 2.
        CHECK(chmod(sr.path, 0700) == 0);
        join(p, sizeof p, sr.path, ".world");
        CHECK(!exists(p));
        join(p, sizeof p, sr.path, "m1");
        join(q2, sizeof q2, sr.path, "m2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        join(p, sizeof p, sr.path, "c1");
        join(q2, sizeof q2, sr.path, "c2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        CHECK(chmod(sr.path, 0000) == 0);

        // And a fork of it rebuilds the pair, with the fork's own marker beside it.
        wfs_ref zcf = {WFS_K_SNAPSHOT, zc};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(zcw, sizeof zcw, worlds, "hlmark2-cw");
        wfs_id zcw1 = 0;
        CHECK_OK(wfs_world_create(zs, zcf, zcw, &opts, &zcw1));
        join(p, sizeof p, zcw, "m1");
        join(q2, sizeof q2, zcw, "m2");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        join(q2, sizeof q2, zcw, ".world");
        CHECK(nlink_of(q2) == 1 && ino_of(p) != ino_of(q2));

        CHECK_OK(wfs_world_discard(zs, zcw1, 1, 0));
        CHECK_OK(wfs_world_discard(zs, zw1, 1, 0));
        wfs_store_close(zs);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", zstore, (unsigned long long)z1);
        chmod(p, 0700);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", zstore, (unsigned long long)zc);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (17th round, P1): a member path is a chain of names, and a name is -----
    // ---- not a symlink ------------------------------------------------------------------------
    //
    // manifest_path_sane() is lexical: no leading '/', no `..`, no empty or `.` component. Every
    // one of those passes for `s/x` when `s` is a symlink to a directory -- and the three
    // syscalls the group is made of all follow it. lstat(2) does not follow the LAST component
    // and follows every one before it, so hardlinks_verify_groups approved the inode the symlink
    // led to on the SNAPSHOT side; link(2) and rename(2) follow everything, so the replay acted
    // on whatever the same symlink leads to on the CLONE side. A relative symlink lands in a
    // different place in the two trees -- the snapshot's root and a fork's temporary sit at
    // different depths, under different parents -- so a manifest that names `s/x`, `s/y` can be
    // verified against a pair planted beside the snapshot and then replayed over two files that
    // are not in the clone at all.
    //
    // Both sides now descend the member path themselves, one component at a time, with
    // O_DIRECTORY|O_NOFOLLOW, and do their work with fstatat/linkat/renameat relative to the
    // parent's descriptor: the check and the operation are the same syscall path, and a symlink
    // component is ELOOP before anything is linked anywhere.
    {
        char ystore[4096], ysrc[4096], yd[4096], yman[4096], ybak[4096], yw[4096], yw2[4096];
        char yside[4096], yx[4096], yy[4096], q2[4096];
        join(ystore, sizeof ystore, root, "hlsym-store");
        join(ysrc, sizeof ysrc, root, "hlsym-src");
        CHECK(mkdir(ysrc, 0755) == 0);
        join(yd, sizeof yd, ysrc, "d");
        CHECK(mkdir(yd, 0755) == 0);
        join(p, sizeof p, yd, "a");
        write_file(p, "aaaa\n");
        join(q2, sizeof q2, yd, "b");
        CHECK(link(p, q2) == 0);
        // The symlink, spelled relative to the tree root so that it leaves the tree: `../x` is
        // one place under <store>/snapshots/S<n>/root and another under <worlds>/<fork>.
        join(p, sizeof p, ysrc, "s");
        CHECK(symlink("../hlsym-side/d", p) == 0);

        wfs_store *ys = NULL;
        CHECK_OK(wfs_store_open(ystore, &ys));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "hlsym";
        wfs_id y1 = 0;
        CHECK_OK(wfs_snapshot_create(ys, ysrc, &sopts, &y1));
        CHECK_OK(wfs_snapshot_info(ys, y1, &sr));
        CHECK(sr.hl_groups == 1 && sr.hardlinks == 2);
        snprintf(yman, sizeof yman, "%s/snapshots/S%llu/manifest", ystore, (unsigned long long)y1);
        join(ybak, sizeof ybak, root, "hlsym.bak");
        copy_file(yman, ybak);

        // What the symlink leads to on the snapshot side: a hardlinked pair of its own, beside
        // the snapshot's root (not inside it -- the gate never sees it). This is what makes the
        // forged manifest verifiable: both members lstat to one inode whose nlink is exactly 2.
        snprintf(p, sizeof p, "%s/snapshots/S%llu/hlsym-side", ystore, (unsigned long long)y1);
        CHECK(mkdir(p, 0755) == 0);
        snprintf(yside, sizeof yside, "%s/snapshots/S%llu/hlsym-side/d", ystore,
                 (unsigned long long)y1);
        CHECK(mkdir(yside, 0755) == 0);
        join(p, sizeof p, yside, "x");
        write_file(p, "vvvv\n");
        join(q2, sizeof q2, yside, "y");
        CHECK(link(p, q2) == 0);

        // And what it leads to on the clone side: two ordinary files of somebody else's, in the
        // directory the fork's temporary is made next to. Same size and same mtime, which is
        // what every clone of one snapshot walk looks like -- so restore_group's "is this still
        // the file the scan saw" guard cannot tell them apart either.
        join(p, sizeof p, worlds, "hlsym-side");
        CHECK(mkdir(p, 0755) == 0);
        join(p, sizeof p, worlds, "hlsym-side/d");
        CHECK(mkdir(p, 0755) == 0);
        join(yx, sizeof yx, worlds, "hlsym-side/d/x");
        write_file(yx, "outx\n");
        join(yy, sizeof yy, worlds, "hlsym-side/d/y");
        write_file(yy, "outy\n");
        {
            struct stat xs;
            CHECK(lstat(yx, &xs) == 0);
            struct timespec ts[2];
            ts[0] = xs.st_atimespec;
            ts[1] = xs.st_mtimespec;
            CHECK(utimensat(AT_FDCWD, yy, ts, AT_SYMLINK_NOFOLLOW) == 0);
        }

        // The forgery: the group's two members respelled through the symlink. The header, the
        // group id, the nlink and the line count are exactly as the snapshot wrote them.
        repath_hl_line(yman, 0, "s/x");
        repath_hl_line(yman, 1, "s/y");

        wfs_ref yf = {WFS_K_SNAPSHOT, y1};
        join(yw, sizeof yw, worlds, "hlsym-w");
        wfs_verify_report yvr;
        CHECK_RC(wfs_snapshot_verify(ys, y1, &yvr), WFS_E_SNAPSHOT_DIRTY);
        CHECK(yvr.modified == 1 && strstr(yvr.first_bad, "manifest"));
        size_t ybefore = 0;
        CHECK_OK(wfs_world_list(ys, 1, NULL, 0, &ybefore));
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        wfs_id yw1 = 0;
        CHECK_RC(wfs_world_create(ys, yf, yw, &opts, &yw1), WFS_E_SNAPSHOT_DIRTY);
        CHECK(!exists(yw));                                  // nothing published at --to
        CHECK(n_with_prefix(worlds, ".wfs-fork-") == 0);     // and no clone left behind
        size_t yafter = 0;
        CHECK_OK(wfs_world_list(ys, 1, NULL, 0, &yafter));
        CHECK(yafter == ybefore);
        uint64_t ymade = 1;
        CHECK_RC(wfs_pool_fill(ys, y1, 1, &ymade), WFS_E_SNAPSHOT_DIRTY);
        CHECK(ymade == 0);
        uint64_t yready = 1;
        CHECK_OK(wfs_pool_ready(ys, y1, &yready));
        CHECK(yready == 0);
        // The two files outside the clone are exactly as they were: this is what the replay did
        // to them before this round -- `y` linked onto `x` and its own five bytes gone.
        CHECK(ino_of(yx) != ino_of(yy));
        CHECK(nlink_of(yx) == 1 && nlink_of(yy) == 1);
        CHECK_OK(read_file(yx, buf, sizeof buf));
        CHECK(!strcmp(buf, "outx\n"));
        CHECK_OK(read_file(yy, buf, sizeof buf));
        CHECK(!strcmp(buf, "outy\n"));

        // The manifest as the snapshot wrote it still forks, with the pair rebuilt: the refusal
        // is about the spelling, not about hardlinked snapshots.
        copy_file(ybak, yman);
        CHECK_OK(wfs_snapshot_verify(ys, y1, &yvr));
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        CHECK_OK(wfs_world_create(ys, yf, yw, &opts, &yw1));
        join(p, sizeof p, yw, "d/a");
        join(q2, sizeof q2, yw, "d/b");
        CHECK(ino_of(p) == ino_of(q2) && nlink_of(p) == 2);
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "aaaa\n"));

        // ---- and the same on the clone side, where no verify runs at all ---------------------
        //
        // A fork from a live WORLD replays its origin snapshot's manifest without
        // hardlinks_verify_groups: the authority there is the live tree, group by group, inside
        // the replay (5th round). So the symlink has to be refused by the replay itself -- and
        // by the live-tree check, which followed it just as happily. The world's own `s` leads
        // to a genuine pair, so the group "is still linked" there; the fork's temporary sits
        // under a different parent, where the same `s` leads to two files of somebody else's.
        join(p, sizeof p, worlds, "hlsym-side/d/y");
        CHECK(unlink(p) == 0);
        CHECK(link(yx, p) == 0);                 // the verify root's pair: one inode, nlink 2
        join(p, sizeof p, root, "hlsym-side");
        CHECK(mkdir(p, 0755) == 0);
        join(p, sizeof p, root, "hlsym-side/d");
        CHECK(mkdir(p, 0755) == 0);
        char yx2[4096], yy2[4096];
        join(yx2, sizeof yx2, root, "hlsym-side/d/x");
        write_file(yx2, "farx\n");
        join(yy2, sizeof yy2, root, "hlsym-side/d/y");
        write_file(yy2, "fary\n");
        {
            struct stat xs;
            CHECK(lstat(yx2, &xs) == 0);
            struct timespec ts[2];
            ts[0] = xs.st_atimespec;
            ts[1] = xs.st_mtimespec;
            CHECK(utimensat(AT_FDCWD, yy2, ts, AT_SYMLINK_NOFOLLOW) == 0);
        }
        repath_hl_line(yman, 0, "s/x");
        repath_hl_line(yman, 1, "s/y");
        wfs_ref yfw = {WFS_K_WORLD, yw1};
        join(yw2, sizeof yw2, root, "hlsym-w2");
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        wfs_id yw3 = 0;
        // The group is dropped (the live tree is allowed to have moved on, and a member behind a
        // symlink is not a member), so the fork itself succeeds -- and nothing outside it moved.
        CHECK_OK(wfs_world_create(ys, yfw, yw2, &opts, &yw3));
        CHECK(ino_of(yx2) != ino_of(yy2));
        CHECK(nlink_of(yx2) == 1 && nlink_of(yy2) == 1);
        CHECK_OK(read_file(yy2, buf, sizeof buf));
        CHECK(!strcmp(buf, "fary\n"));

        CHECK_OK(wfs_world_discard(ys, yw3, 1, 0));
        CHECK_OK(wfs_world_discard(ys, yw1, 1, 0));
        wfs_store_close(ys);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", ystore, (unsigned long long)y1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (17th round, P2): the marker's store path is a verdict, not a note ----
    //
    // `-o` options do not reach an FSKit module on macOS 27, so the extension reads the store's
    // location out of the mount source's `.world` marker -- and it uses that path whenever it is
    // non-empty, falling back to its own container default only when it obtained no path at all
    // (macos/fskit/WorldVolume.mm). A marker naming a store that is not the one the command
    // opened therefore mounts a DIFFERENT store: its world ids mean other worlds, and `world=<n>`
    // in the mount options is resolved against it. The CLI used to print a note promising a
    // fallback that the extension does not perform, and mount anyway.
    //
    // So the question is asked of the core, with the two answers P1/P2 already distinguish: the
    // same store under a new path (same store id -- refreshable) or somebody else's store
    // (different store id -- `adopt`'s business, never rewritten here).
    {
        char nstore[4096], nother[4096], nsrc[4096], nw[4096], nw2[4096];
        join(nstore, sizeof nstore, root, "mk-store");
        join(nother, sizeof nother, root, "mk-other");
        join(nsrc, sizeof nsrc, root, "mk-src");
        CHECK(mkdir(nsrc, 0755) == 0);
        join(p, sizeof p, nsrc, "f.txt");
        write_file(p, "mk\n");

        wfs_store *ns = NULL, *no = NULL;
        CHECK_OK(wfs_store_open(nstore, &ns));
        CHECK_OK(wfs_store_open(nother, &no));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "mk";
        wfs_id n1 = 0, o1 = 0;
        CHECK_OK(wfs_snapshot_create(ns, nsrc, &sopts, &n1));
        CHECK_OK(wfs_snapshot_create(no, nsrc, &sopts, &o1));
        wfs_ref nf = {WFS_K_SNAPSHOT, n1}, of = {WFS_K_SNAPSHOT, o1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        join(nw, sizeof nw, worlds, "mk-w");
        join(nw2, sizeof nw2, worlds, "mk-w-other");
        wfs_id nw1 = 0, ow1 = 0;
        CHECK_OK(wfs_world_create(ns, nf, nw, &opts, &nw1));
        CHECK_OK(wfs_world_create(no, of, nw2, &opts, &ow1));

        // As forked: this store, this path.
        wfs_marker_store ms;
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 1 && ms.same_store == 1 && ms.same_path == 1);
        CHECK(!strcmp(ms.path, wfs_store_dir(ns)));

        // The store moved (or the marker is stale): same store id, another path. This is what
        // the CLI called "the extension will fall back" and mounted anyway.
        marker_set_str(nw, "store_path", nother);
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 1 && ms.same_store == 1 && ms.same_path == 0);
        CHECK(!strcmp(ms.path, nother));
        // A path that resolves nowhere at all is not this store either -- the extension's open(2)
        // is the question being asked, not a spelling comparison.
        join(p, sizeof p, root, "mk-gone");
        marker_set_str(nw, "store_path", p);
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 1 && ms.same_store == 1 && ms.same_path == 0);

        // The way out: the path, and nothing else.
        wfs_identity nid;
        CHECK_OK(wfs_world_verify_identity(ns, nw, &nid));
        wfs_world_rec nrec;
        CHECK_OK(wfs_world_info(ns, nw1, &nrec));
        CHECK_OK(wfs_world_marker_refresh(ns, nw));
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 1 && ms.same_store == 1 && ms.same_path == 1);
        wfs_identity nid2;
        CHECK_OK(wfs_world_verify_identity(ns, nw, &nid2));
        CHECK(nid2.world_id == nid.world_id && nid2.snapshot_id == nid.snapshot_id);
        CHECK(!strcmp(nid2.name, nid.name) && !strcmp(nid2.store_id, nid.store_id));
        CHECK(nid2.registered == 1);

        // A marker written before T2.3 carries no path at all, and there the fallback is real:
        // has_path 0, no refusal to make -- and the refresh is what writes one.
        marker_set_str(nw, "store_path", "");
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 0 && ms.same_store == 1 && ms.same_path == 0 && !ms.path[0]);
        CHECK_OK(wfs_world_marker_refresh(ns, nw));
        CHECK_OK(wfs_world_marker_store(ns, nw, &ms));
        CHECK(ms.has_path == 1 && ms.same_path == 1);

        // And somebody else's world: the store id says so, and the refresh refuses to take it
        // over (P1/P2 -- that is what `adopt` is for) without touching a byte of its marker.
        CHECK_OK(wfs_world_marker_store(ns, nw2, &ms));
        CHECK(ms.has_path == 1 && ms.same_store == 0 && ms.same_path == 0);
        CHECK(!strcmp(ms.path, wfs_store_dir(no)));
        CHECK_RC(wfs_world_marker_refresh(ns, nw2), WFS_E_FOREIGN_STORE);
        CHECK_OK(wfs_world_marker_store(no, nw2, &ms));
        CHECK(ms.same_store == 1 && ms.same_path == 1);

        CHECK_OK(wfs_world_discard(ns, nw1, 1, 0));
        CHECK_OK(wfs_world_discard(no, ow1, 1, 0));
        wfs_store_close(ns);
        wfs_store_close(no);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", nstore, (unsigned long long)n1);
        chmod(p, 0700);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", nother, (unsigned long long)o1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (19th round, P2): a marker key is a key, not a byte sequence -----------
    //
    // json_find() looked for its key with strstr(js, "\"<key>\"") over the whole marker, so the
    // first place those bytes appear wins -- and the values are written before the keys that
    // come after them. json_escape writes a `"` inside a value as `\"`, which leaves the byte
    // sequence `"world` in the text whenever a path (or a name) contains a quote followed by
    // that word, and the value's own closing quote completes the pattern: a store directory
    // called `q"world` puts `"world"` into the marker's store_path, strstr lands there, the
    // character after it is `,` rather than `:`, and json_find RETURNS NOT-FOUND instead of
    // looking any further. The world id then read back as 0, so every world in that store was
    // WFS_E_UNREGISTERED to `verify`, to `mount` and to every command that identifies a world
    // by its marker -- a store nobody could use, over a quote in a directory name. The same
    // shape reaches every later key through the name: a world called `w"snapshot` lost its
    // snapshot id the same way.
    //
    // The scan is a top-level key walk now: the object's `"key": value` pairs in order, strings
    // parsed with their escapes, values skipped by kind, keys compared whole. The marker format
    // is untouched -- what this changes is only which bytes count as a key.
    {
        char qstore[4096], qsrc[4096], qw[4096], qdir[4096], qstore2[4096], qw2[4096];
        join(qsrc, sizeof qsrc, root, "q-src");
        CHECK(mkdir(qsrc, 0755) == 0);
        join(p, sizeof p, qsrc, "f.txt");
        write_file(p, "q\n");

        // The store directory, whose last component ends in the bytes `"world`.
        join(qstore, sizeof qstore, root, "q\"world");
        wfs_store *qs = NULL;
        CHECK_OK(wfs_store_open(qstore, &qs));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "q";
        wfs_id q1 = 0;
        CHECK_OK(wfs_snapshot_create(qs, qsrc, &sopts, &q1));
        wfs_ref qf = {WFS_K_SNAPSHOT, q1};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        opts.name = "qw\"snapshot";        // ... and a name that swallows the NEXT key
        join(qw, sizeof qw, worlds, "q-w");
        wfs_id qw1 = 0;
        CHECK_OK(wfs_world_create(qs, qf, qw, &opts, &qw1));

        wfs_identity qid;
        CHECK_OK(wfs_world_verify_identity(qs, qw, &qid));
        CHECK(qid.has_marker == 1 && qid.registered == 1 && qid.is_copy == 0);
        CHECK(qid.world_id == qw1);          // 0 before this round: the `world` key was not found
        CHECK(qid.snapshot_id == q1);        // 0 before this round: the `snapshot` key either
        CHECK(!strcmp(qid.name, "qw\"snapshot"));
        CHECK(qid.store_id[0]);              // the `store` key, read before either of those
        // And the store path the FSKit extension reads out of the same marker.
        wfs_marker_store qms;
        CHECK_OK(wfs_world_marker_store(qs, qw, &qms));
        CHECK(qms.has_path == 1 && qms.same_store == 1 && qms.same_path == 1);
        CHECK(!strcmp(qms.path, wfs_store_dir(qs)));

        // A directory component that is simply named `world` -- no quote anywhere -- was never
        // the failing shape, and must stay that way now that the walker replaced the scan.
        join(qdir, sizeof qdir, root, "world");
        CHECK(mkdir(qdir, 0755) == 0);
        join(qstore2, sizeof qstore2, qdir, "store");
        wfs_store *qs2 = NULL;
        CHECK_OK(wfs_store_open(qstore2, &qs2));
        wfs_id q2 = 0;
        CHECK_OK(wfs_snapshot_create(qs2, qsrc, &sopts, &q2));
        wfs_ref qf2 = {WFS_K_SNAPSHOT, q2};
        memset(&opts, 0, sizeof opts);
        opts.no_pool = 1;
        opts.name = "qw2";
        join(qw2, sizeof qw2, worlds, "q-w2");
        wfs_id qw2id = 0;
        CHECK_OK(wfs_world_create(qs2, qf2, qw2, &opts, &qw2id));
        wfs_identity qid2;
        CHECK_OK(wfs_world_verify_identity(qs2, qw2, &qid2));
        CHECK(qid2.registered == 1 && qid2.world_id == qw2id && qid2.snapshot_id == q2);

        // The keys a marker of another store's world carries are read the same way: this one is
        // foreign, and it is the store id -- not a missing key -- that says so.
        CHECK_RC(wfs_world_verify_identity(qs, qw2, &qid2), WFS_E_FOREIGN_STORE);
        CHECK(qid2.has_marker == 1 && qid2.world_id == qw2id && qid2.registered == 0);

        CHECK_OK(wfs_world_discard(qs, qw1, 1, 0));
        CHECK_OK(wfs_world_discard(qs2, qw2id, 1, 0));
        wfs_store_close(qs);
        wfs_store_close(qs2);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", qstore, (unsigned long long)q1);
        chmod(p, 0700);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", qstore2, (unsigned long long)q2);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (11th round, P2): a migration that failed is not a migration that ran ----
    //
    // The additive ALTERs used to be fired one by one with their results thrown away, and
    // `user_version` stamped with the current revision afterwards no matter what happened. Only
    // one kind of failure was being thought about -- "duplicate column name", i.e. the column is
    // already there -- and everything else (EIO, a full disk, a SQLITE_BUSY that outlives the
    // busy timeout, a database SQLite could only open read-only) left the store stamped as
    // migrated with a column missing. The stamp is the only thing that decides whether the
    // migrations run at all, so no later open ever retried: every prepare naming that column
    // failed from then on, for ever.
    {
        char mstore[4096], mdb[4096];
        join(mstore, sizeof mstore, root, "migrate-store");
        CHECK(mkdir(mstore, 0755) == 0);
        join(p, sizeof p, mstore, "VERSION");
        // M1's schema number. The migrations below are additive and do not move it; the 24th
        // round did, so the first open of this store also rewrites this file to 3.
        write_file(p, "2\n");
        join(mdb, sizeof mdb, mstore, "metadata.db");

        // (1) the ordinary case: a v2 store that has never seen the added columns is migrated
        //     on open, and only then stamped.
        make_v2_db(mdb, 0);
        CHECK(db_user_version(mdb) == WFS_STORE_SCHEMA_M1 * 100);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(!db_has_column(mdb, kAdded[i][0], kAdded[i][1]));
        wfs_store *ms = NULL;
        CHECK_OK(wfs_store_open(mstore, &ms));
        wfs_store_close(ms);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(db_has_column(mdb, kAdded[i][0], kAdded[i][1]));
        int stamped = db_user_version(mdb);
        CHECK(stamped > WFS_STORE_SCHEMA * 100);   // SCHEMA*100 + the revision
        // ... and the store is usable, which is what the columns were for.
        CHECK_OK(wfs_store_open(mstore, &ms));
        wfs_store_stat mst;
        CHECK_OK(wfs_store_status(ms, &mst));
        CHECK(mst.schema == WFS_STORE_SCHEMA);
        wfs_store_close(ms);
        CHECK(db_user_version(mdb) == stamped);    // a second open changes nothing

        // (2) the same store, with one ALTER that cannot succeed and a `PRAGMA user_version`
        //     that can. The open has to fail and leave the database exactly as it found it --
        //     un-stamped, so the next open is the retry. This used to come back 0 with
        //     user_version stamped, `snapshots` still missing every column, and the migrations
        //     that happened to work applied on their own.
        make_v2_db(mdb, 1);
        CHECK(db_user_version(mdb) == WFS_STORE_SCHEMA_M1 * 100);
        ms = NULL;
        CHECK_RC(wfs_store_open(mstore, &ms), -EIO);
        CHECK(ms == NULL);
        CHECK(db_user_version(mdb) == WFS_STORE_SCHEMA_M1 * 100);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(!db_has_column(mdb, kAdded[i][0], kAdded[i][1]));

        // (3) take the obstacle away and the next open migrates it, exactly as if nothing had
        //     ever gone wrong -- which is the whole point of not stamping it in (2).
        db_exec(mdb, "DROP VIEW snapshots; ALTER TABLE snapshots_real RENAME TO snapshots;");
        CHECK_OK(wfs_store_open(mstore, &ms));
        wfs_store_close(ms);
        CHECK(db_user_version(mdb) == stamped);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(db_has_column(mdb, kAdded[i][0], kAdded[i][1]));
    }

    // ---- PR #1 review (24th round, P1): the schema is also what a collector may delete ------
    //
    // Everything M2 added to the store was an additive column or a new value of an existing
    // INTEGER column -- snapshot trash rows (T2.2), WFS_ST_TRASHING with the tree at one of two
    // names, `.deleting` entries, the pool's DRAINING, owner columns, hardlink manifests -- so
    // VERSION stayed at 2 and an M1 binary built from `main` was still allowed to open an M2
    // store. Its wfs_gc() protects `state=2` world trash paths and sweeps everything else it
    // does not recognise under trash/ and snapshots/: it would recursively delete a snapshot
    // still inside its retention window, or the tree of a world whose row says TRASHING, and
    // leave every M2 row pointing at nothing. The schema is 3 now. M1's own first act on a
    // store is
    //     return v == WFS_STORE_SCHEMA ? 0 : WFS_E_SCHEMA;   // check_version(), WFS_STORE_SCHEMA == 2
    // so the number in VERSION is exactly what refuses it, and (a) below is the assertion that
    // the number gets there.
    {
        char vstore[4096], vdb[4096], vver[4096], vbuf[64];
        join(vstore, sizeof vstore, root, "schema3-store");
        CHECK(mkdir(vstore, 0755) == 0);
        join(vver, sizeof vver, vstore, "VERSION");
        join(vdb, sizeof vdb, vstore, "metadata.db");

        // (a) an M1 store -- VERSION 2, user_version 2xx, not one of the added columns -- is
        //     taken over rather than refused: the columns arrive, the stamp becomes 3xx, and
        //     the file M1 reads first says 3.
        write_file(vver, "2\n");
        make_v2_db(vdb, 0);
        CHECK(db_user_version(vdb) / 100 == WFS_STORE_SCHEMA_M1);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(!db_has_column(vdb, kAdded[i][0], kAdded[i][1]));
        wfs_store *vs = NULL;
        CHECK_OK(wfs_store_open(vstore, &vs));
        wfs_store_close(vs);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(db_has_column(vdb, kAdded[i][0], kAdded[i][1]));
        // The hole itself: this used to come out 2xx, i.e. still a store M1 would open.
        CHECK(db_user_version(vdb) / 100 == WFS_STORE_SCHEMA);
        CHECK(read_file(vver, vbuf, sizeof vbuf) == 0);
        CHECK(atoi(vbuf) == WFS_STORE_SCHEMA);
        char vtmp[4096];
        join(vtmp, sizeof vtmp, vstore, "VERSION.tmp");
        CHECK(!exists(vtmp));
        // A second open is a no-op: the store is already schema 3 and is not upgraded twice.
        int vstamp = db_user_version(vdb);
        CHECK_OK(wfs_store_open(vstore, &vs));
        wfs_store_stat vst;
        CHECK_OK(wfs_store_status(vs, &vst));
        CHECK(vst.schema == WFS_STORE_SCHEMA);
        wfs_store_close(vs);
        CHECK(db_user_version(vdb) == vstamp);

        // (b) a store from a schema we do not know: VERSION says 3, the database says 4xx. It
        //     is refused with the P13 code and comes back untouched -- not migrated, and not
        //     stamped down to 3xx, which is the one thing that would be unrecoverable.
        make_v2_db(vdb, 0);
        db_exec(vdb, "PRAGMA user_version=400");
        vs = NULL;
        CHECK_RC(wfs_store_open(vstore, &vs), WFS_E_SCHEMA);
        CHECK(vs == NULL);
        CHECK(db_user_version(vdb) == 400);
        for (size_t i = 0; i < kAddedN; ++i) CHECK(!db_has_column(vdb, kAdded[i][0], kAdded[i][1]));

        // ... and the same store refused one step earlier, by the file, before the database is
        //     opened at all -- which is the check M1 itself is relying on.
        write_file(vver, "4\n");
        vs = NULL;
        CHECK_RC(wfs_store_open(vstore, &vs), WFS_E_SCHEMA);
        CHECK(vs == NULL);
        CHECK(db_user_version(vdb) == 400);
    }

    // ---- PR #1 review (25th round, P1): a trash entry is the row's tree, not the row's name --
    //
    // The collector's last question before renaming an entry to `.deleting` and deleting it was
    // whether the row still NAMED that path. For a world discarded across volumes the entry is
    // `<parent>/.wfs-trash/W<id>-<timestamp>` -- the user's own directory, under a name they can
    // work out -- so during the retention period the tree can be moved away and another
    // directory left in its place, and the collector deleted that one, contents and all.
    // `discard --now` did the same and exited 0, and `restore` renamed it home and wrote its
    // dev/ino into the row: somebody else's data registered as the world.
    //
    // The row has carried dir_dev/dir_ino since the world was published and every rename on the
    // way into and out of the trash is same-volume, so the inode is the entry's identity for as
    // long as it exists. This checks it against the row itself, straight out of the database.
    // (The EXDEV side trash needs a second volume to exercise; the entry below is in
    // <store>/trash and goes through the same code, because the check is made against the row,
    // not against where the entry happens to live. A snapshot's trash is always inside the
    // store, and its row has no dev/ino columns -- see the reply on the thread.)
    {
        char fstore[4096], fsrc[4096], fw[4096], fdb[4096], fentry[4096], faside[4096];
        join(fstore, sizeof fstore, root, "foreign-store");
        join(fsrc, sizeof fsrc, root, "foreign-src");
        join(faside, sizeof faside, root, "foreign-aside");
        CHECK(mkdir(fsrc, 0755) == 0);
        join(p, sizeof p, fsrc, "a.txt");
        write_file(p, "one\n");
        wfs_store *fa = NULL;
        CHECK_OK(wfs_store_open(fstore, &fa));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "fb";
        wfs_id f1 = 0;
        CHECK_OK(wfs_snapshot_create(fa, fsrc, &sopts, &f1));
        wfs_ref ff = {WFS_K_SNAPSHOT, f1};
        memset(&opts, 0, sizeof opts);
        join(fw, sizeof fw, worlds, "fworld");
        wfs_id fw1 = 0;
        CHECK_OK(wfs_world_create(fa, ff, fw, &opts, &fw1));
        wfs_world_rec fwr;
        CHECK_OK(wfs_world_info(fa, fw1, &fwr));
        CHECK(fwr.dir_ino != 0);
        uint64_t want_dev = fwr.dir_dev, want_ino = fwr.dir_ino;
        CHECK_OK(wfs_world_discard(fa, fw1, 0, 0));

        // The row, read from the database: where it says the tree is, and what it says the tree
        // is. The discard does not touch the two identity columns, and the rename into
        // <store>/trash kept the inode.
        snprintf(fdb, sizeof fdb, "%s/metadata.db", fstore);
        char sql[256], val[4096];
        snprintf(sql, sizeof sql, "SELECT trash_path FROM worlds WHERE id=%llu",
                 (unsigned long long)fw1);
        CHECK(db_query_text(fdb, sql, fentry, sizeof fentry) == 1);
        CHECK(fentry[0]);
        snprintf(sql, sizeof sql, "SELECT dir_ino FROM worlds WHERE id=%llu",
                 (unsigned long long)fw1);
        CHECK(db_query_text(fdb, sql, val, sizeof val) == 1);
        CHECK(strtoull(val, NULL, 10) == want_ino);
        struct stat fst;
        CHECK(lstat(fentry, &fst) == 0);
        CHECK((uint64_t)fst.st_ino == want_ino && (uint64_t)fst.st_dev == want_dev);

        // The substitution: the world's tree moved aside, a directory of somebody else's -- with
        // a file in it -- at exactly the path the row names. Different inode, same name.
        CHECK(rename(fentry, faside) == 0);
        CHECK(mkdir(fentry, 0755) == 0);
        join(p, sizeof p, fentry, "user.txt");
        write_file(p, "not the world\n");
        CHECK(lstat(fentry, &fst) == 0);
        CHECK((uint64_t)fst.st_ino != want_ino);

        // The collector will not start on it: nothing renamed, nothing unlinked, the row left
        // TRASHED, and the entry counted where an operator will see it.
        wfs_store *fb = NULL;
        CHECK_OK(wfs_store_open(fstore, &fb));
        wfs_gc_report frep;
        memset(&frep, 0, sizeof frep);
        CHECK_OK(wfs_gc(fb, 0, &frep));
        CHECK(frep.trash_foreign == 1);
        CHECK(frep.worlds_deleted == 0 && frep.trash_orphans == 0 && frep.trash_failed == 0);
        CHECK(exists(p));                          // the user's file, untouched
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "not the world\n"));
        char fdel[4200];
        snprintf(fdel, sizeof fdel, "%s.deleting", fentry);
        CHECK(!exists(fdel));                      // and it was never even renamed
        CHECK_OK(wfs_world_info(fa, fw1, &fwr));
        CHECK(fwr.state == WFS_ST_TRASHED);
        CHECK(fwr.dir_ino == want_ino && fwr.dir_dev == want_dev);
        wfs_trash_stat fts;
        memset(&fts, 0, sizeof fts);
        CHECK_OK(wfs_gc_status(fb, 0, &fts));
        CHECK(fts.trash_foreign == 1 && !strcmp(fts.foreign_path, fentry));
        char shown[WFS_PATH_MAX];
        shown[0] = 0;
        CHECK_OK(wfs_trash_entry_path(fb, fw1, 0, shown, sizeof shown));
        CHECK(!strcmp(shown, fentry));

        // `--now` refuses instead of deleting it, and `restore` refuses instead of adopting it.
        CHECK_RC(wfs_world_discard(fa, fw1, 1, 0), WFS_E_TRASH_FOREIGN);
        CHECK(exists(p));
        CHECK_RC(wfs_world_restore(fa, fw1), WFS_E_TRASH_FOREIGN);
        CHECK(exists(p) && !exists(fw));
        CHECK_OK(wfs_world_info(fa, fw1, &fwr));
        CHECK(fwr.state == WFS_ST_TRASHED && fwr.dir_ino == want_ino);

        // Put the world's own tree back at that path and nothing about the entry was ever
        // broken: it restores, and the row's identity is the one it always had.
        rm_rf(fentry);
        CHECK(rename(faside, fentry) == 0);
        CHECK_OK(wfs_world_restore(fa, fw1));
        CHECK_OK(wfs_world_info(fa, fw1, &fwr));
        CHECK(fwr.state == WFS_ST_ACTIVE && fwr.present && exists(fw));
        CHECK(fwr.dir_ino == want_ino && fwr.dir_dev == want_dev);
        join(p, sizeof p, fw, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));

        // ... and the collector takes it exactly as it always did.
        CHECK_OK(wfs_world_discard(fa, fw1, 0, 0));
        memset(&frep, 0, sizeof frep);
        CHECK_OK(wfs_gc(fb, 0, &frep));
        CHECK(frep.worlds_deleted == 1 && frep.trash_foreign == 0 && frep.trash_failed == 0);
        CHECK_OK(wfs_world_info(fa, fw1, &fwr));
        CHECK(fwr.state == WFS_ST_DEAD);
        wfs_store_close(fb);
        wfs_store_close(fa);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", fstore, (unsigned long long)f1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    // ---- PR #1 review (26th round, P1): the identity that was checked is the identity deleted --
    //
    // The 25th round asks the entry at `j.path` whether it is the row's tree, and then renames
    // that path and deletes that path -- three separate file system calls. The SQLite
    // transaction around them serialises this core's own writers and nothing else, and the
    // other party here is not a writer of ours: for a world discarded across volumes the entry
    // sits in the user's own `<parent>/.wfs-trash`, so between the lstat and the rename they can
    // move the genuine tree away and leave a directory of their own at the name. The identity
    // check then passed on the world's tree and everything after it acted on the stranger's.
    //
    // So the claim opens the entry, checks the descriptor's dev/ino, and keeps that descriptor:
    // the tree is emptied through it (fs_remove_tree_fd), and both names it still has to use --
    // the `.deleting` one after the rename and the final rmdir -- are re-checked against it.
    // The seam below is that exact window, and what it leaves behind may not lose one byte.
    {
        char xstore[4096], xsrc[4096], xw[4096], xdb[4096], xentry[4096], xdel[4200], xsql[256];
        join(xstore, sizeof xstore, root, "swap-store");
        join(xsrc, sizeof xsrc, root, "swap-src");
        join(g_swap_aside, sizeof g_swap_aside, root, "swap-aside");
        CHECK(mkdir(xsrc, 0755) == 0);
        join(p, sizeof p, xsrc, "a.txt");
        write_file(p, "one\n");
        join(p, sizeof p, xsrc, "sub");
        CHECK(mkdir(p, 0755) == 0);
        join(p, sizeof p, xsrc, "sub/b.txt");
        write_file(p, "two\n");
        wfs_store *xa = NULL;
        CHECK_OK(wfs_store_open(xstore, &xa));
        memset(&sopts, 0, sizeof sopts);
        sopts.name = "xb";
        wfs_id x1 = 0;
        CHECK_OK(wfs_snapshot_create(xa, xsrc, &sopts, &x1));
        wfs_ref xf = {WFS_K_SNAPSHOT, x1};
        memset(&opts, 0, sizeof opts);
        join(xw, sizeof xw, worlds, "xworld");
        wfs_id xw1 = 0;
        CHECK_OK(wfs_world_create(xa, xf, xw, &opts, &xw1));
        wfs_world_rec xwr;
        CHECK_OK(wfs_world_info(xa, xw1, &xwr));
        uint64_t xdev = xwr.dir_dev, xino = xwr.dir_ino;
        CHECK(xino != 0);
        CHECK_OK(wfs_world_discard(xa, xw1, 0, 0));
        snprintf(xdb, sizeof xdb, "%s/metadata.db", xstore);
        snprintf(xsql, sizeof xsql, "SELECT trash_path FROM worlds WHERE id=%llu",
                 (unsigned long long)xw1);
        CHECK(db_query_text(xdb, xsql, xentry, sizeof xentry) == 1);
        CHECK(xentry[0]);
        snprintf(xdel, sizeof xdel, "%s.deleting", xentry);

        wfs_store *xb = NULL;
        CHECK_OK(wfs_store_open(xstore, &xb));
        g_swap_world = xw1;

        // (1) the collector. The swap happens after the claim has accepted the entry, so what
        // the rename moves is the stranger -- and a rename moves a directory, it never removes
        // one, which is why undoing it is enough. Nothing of theirs is unlinked, the row stays
        // TRASHED, and the entry is counted where an operator will see it.
        g_swap_ran = 0;
        wfs_test_between_trash_claim = swap_between_claim;
        wfs_gc_report xrep;
        memset(&xrep, 0, sizeof xrep);
        CHECK_OK(wfs_gc(xb, 0, &xrep));
        wfs_test_between_trash_claim = NULL;
        CHECK(g_swap_ran == 1);
        CHECK(!strcmp(g_swap_entry, xentry));
        CHECK(exists(g_swap_file));                       // the stranger's file, byte for byte
        CHECK_OK(read_file(g_swap_file, buf, sizeof buf));
        CHECK(!strcmp(buf, "not the world\n"));
        CHECK(!exists(xdel));                             // and put back at its own name
        join(p, sizeof p, g_swap_aside, "sub/b.txt");     // the world's tree, where it was moved
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "two\n"));
        CHECK(xrep.trash_foreign == 1);
        CHECK(xrep.worlds_deleted == 0 && xrep.trash_orphans == 0 && xrep.trash_failed == 0);
        CHECK(xrep.entries_freed == 0);
        CHECK_OK(wfs_world_info(xa, xw1, &xwr));
        CHECK(xwr.state == WFS_ST_TRASHED && xwr.dir_dev == xdev && xwr.dir_ino == xino);
        swap_undo(xentry);

        // (2) `--now` refuses in the same window, and refuses without touching anything.
        g_swap_ran = 0;
        wfs_test_between_trash_claim = swap_between_claim;
        CHECK_RC(wfs_world_discard(xa, xw1, 1, 0), WFS_E_TRASH_FOREIGN);
        wfs_test_between_trash_claim = NULL;
        CHECK(g_swap_ran == 1);
        CHECK(exists(g_swap_file));
        CHECK_OK(read_file(g_swap_file, buf, sizeof buf));
        CHECK(!strcmp(buf, "not the world\n"));
        CHECK(!exists(xdel));
        CHECK_OK(wfs_world_info(xa, xw1, &xwr));
        CHECK(xwr.state == WFS_ST_TRASHED && xwr.dir_ino == xino);
        swap_undo(xentry);

        // (3) `restore` refuses too. Its rename home is the one that moves the stranger, so the
        // check after it has to undo that rename: no world at the home path, the stranger back
        // at the entry's name with its file, and the row still TRASHED with its own identity.
        g_swap_ran = 0;
        wfs_test_between_trash_claim = swap_between_claim;
        CHECK_RC(wfs_world_restore(xa, xw1), WFS_E_TRASH_FOREIGN);
        wfs_test_between_trash_claim = NULL;
        CHECK(g_swap_ran == 1);
        CHECK(!exists(xw));                               // nothing was registered as the world
        CHECK(exists(g_swap_file));
        CHECK_OK(read_file(g_swap_file, buf, sizeof buf));
        CHECK(!strcmp(buf, "not the world\n"));
        join(p, sizeof p, g_swap_aside, "a.txt");
        CHECK_OK(read_file(p, buf, sizeof buf));
        CHECK(!strcmp(buf, "one\n"));
        CHECK_OK(wfs_world_info(xa, xw1, &xwr));
        CHECK(xwr.state == WFS_ST_TRASHED && xwr.dir_dev == xdev && xwr.dir_ino == xino);
        swap_undo(xentry);

        // (4) and with nobody swapping anything the entry is collected exactly as before --
        // through the descriptor now, which is also what counts the entries it freed (the tree
        // is `a.txt`, `sub`, `sub/b.txt` and the `.world` marker; the entry itself is not one).
        memset(&xrep, 0, sizeof xrep);
        CHECK_OK(wfs_gc(xb, 0, &xrep));
        CHECK(xrep.worlds_deleted == 1 && xrep.trash_foreign == 0 && xrep.trash_failed == 0);
        CHECK(xrep.entries_freed == 4);
        CHECK(!exists(xentry) && !exists(xdel));
        CHECK_OK(wfs_world_info(xa, xw1, &xwr));
        CHECK(xwr.state == WFS_ST_DEAD);
        wfs_store_close(xb);
        wfs_store_close(xa);
        snprintf(p, sizeof p, "%s/snapshots/S%llu/root", xstore, (unsigned long long)x1);
        chmod(p, 0700);   // the gate, so this test's own rm_rf can clear the tree
    }

    wfs_store_close(s);
    rm_rf(root);
    printf("core_test: all OK\n");
    return 0;
}
