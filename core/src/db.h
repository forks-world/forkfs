// SQLite plumbing shared by store.cpp and world.cpp. Not part of the C ABI.
// Every mutation runs under BEGIN IMMEDIATE (P12): the write lock is taken at BEGIN, so two
// concurrent `world` processes serialise instead of failing halfway through with SQLITE_BUSY.
#pragma once
#include "internal.h"

#include <errno.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

struct wfs_store {
    wfs::String dir;
    wfs::String store_id;
    sqlite3 *db = nullptr;
    wfs::Mutex mu;
};

namespace wfs {

inline int map_sqlite(int rc) {
    switch (rc) {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW: return 0;
    case SQLITE_BUSY: case SQLITE_LOCKED: return -EBUSY;
    case SQLITE_NOMEM: return -ENOMEM;
    case SQLITE_CONSTRAINT: return -EEXIST;
    case SQLITE_READONLY: return -EROFS;
    default: return -EIO;
    }
}

// ---- PR #1 review (32nd round, P2): "no rows" and "I could not ask" are different answers ----
//
// `row()` returned sqlite3_step() == SQLITE_ROW, and every caller read its false as "the rows
// ran out". They do not: a prepared statement fails at step(2) time too -- SQLITE_IOERR off the
// database file, SQLITE_BUSY past the busy timeout, SQLITE_NOMEM, SQLITE_CORRUPT -- and the
// false those produce is indistinguishable from SQLITE_DONE. Under a `while (q.row())` it means
// a COUNT that comes back short or a list that stops half way; under an `if (!q.row())` it means
// "no row names it". Both of those are the premise of a deletion somewhere in this core: a
// snapshot's reference count, "does any row still claim this trash path", "is this pool entry
// still doomed". One EIO on the wrong read and gc removes a baseline that live worlds are using.
//
// So the last step's result is kept. `row()` is unchanged for the caller that only wants rows;
// `done()` is the question every verdict has to ask afterwards -- did this iteration end because
// the statement was finished? -- and `err()` is the raw code for the callers that map it. The
// idiom across world.cpp, pool.cpp and store.cpp is one line, so the audit stays greppable:
//     while (q.row()) { ... }
//     if (!q.done()) return -EIO;          // a loop that decides something
//     if (!q.row()) return q.done() ? -ENOENT : -EIO;   // a single-row lookup
// and, for the helpers that answer a bool, the failure takes whichever side does nothing
// destructive ("cannot tell" is never "delete it").
struct Stmt {
    sqlite3_stmt *s = nullptr;
    // The result of the last step(2) through this object. SQLITE_OK means "not stepped yet",
    // which is deliberately neither SQLITE_ROW nor SQLITE_DONE.
    int last = SQLITE_OK;
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    bool ok() const { return s != nullptr; }
    void text(int i, const char *v) { sqlite3_bind_text(s, i, v ? v : "", -1, SQLITE_TRANSIENT); }
    void i64(int i, int64_t v) { sqlite3_bind_int64(s, i, (sqlite3_int64)v); }
    int step() { return last = (s ? sqlite3_step(s) : SQLITE_MISUSE); }
    bool row() {
        // The test seam (worldfs.h). NULL in every run that is not a test, so this is one
        // predictable branch on a pointer that is never written.
        if (wfs_test_stmt_fail_sql && s) {
            const char *sql = sqlite3_sql(s);
            if (sql && strstr(sql, wfs_test_stmt_fail_sql)) {
                wfs_test_stmt_fail_sql = nullptr;
                last = SQLITE_IOERR;
                return false;
            }
        }
        return (last = (s ? sqlite3_step(s) : SQLITE_MISUSE)) == SQLITE_ROW;
    }
    // The iteration ended, and it ended because the statement had no more rows to give.
    bool done() const { return last == SQLITE_DONE; }
    // ... and what it ended with when it did not. Callers that map it use map_sqlite().
    int err() const { return last; }
    int64_t col_i64(int i) { return (int64_t)sqlite3_column_int64(s, i); }
    const char *col_text(int i) {
        const unsigned char *t = sqlite3_column_text(s, i);
        return t ? (const char *)t : "";
    }
};

inline void exec(sqlite3 *db, const char *sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

// RAII BEGIN IMMEDIATE. Rolls back unless commit() was called (P12).
// PR #1 review (11th round): `begin_rc` and `commit_rc()` are for the one caller that has to
// know -- the schema migration in store.cpp, which must not stamp a store as migrated on the
// strength of statements it never checked. Everything else is a row write whose own
// sqlite3_changes() is the answer, and goes on using commit().
struct Txn {
    sqlite3 *db;
    bool open_ = false;
    int begin_rc = SQLITE_OK;
    explicit Txn(sqlite3 *d) : db(d) {
        begin_rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        open_ = true;
    }
    ~Txn() { if (open_) exec(db, "ROLLBACK"); }
    Txn(const Txn &) = delete;
    Txn &operator=(const Txn &) = delete;
    void commit() { if (open_) { exec(db, "COMMIT"); open_ = false; } }
    // The COMMIT whose result is looked at. A commit that fails leaves the transaction open, so
    // the destructor still rolls it back.
    int commit_rc() {
        if (!open_) return SQLITE_OK;
        int rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK) open_ = false;
        return rc;
    }
};

inline int64_t now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

inline void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

} // namespace wfs
