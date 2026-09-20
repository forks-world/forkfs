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
// ---- PR #1 review (34th round, P1): the connection whose BEGIN IMMEDIATE has just failed ----
//
// See Txn below. While this names a connection, Stmt prepares nothing against it: a caller that
// forgot to ask whether its transaction started writes nothing at all instead of writing in
// autocommit. It is per-thread (the gc worker threads have connections of their own in flight)
// and it is saved and restored by the Txn that sets it, so an inner scope cannot outlive it.
inline thread_local sqlite3 *g_txn_broken = nullptr;

struct Stmt {
    sqlite3_stmt *s = nullptr;
    // The result of the last step(2) through this object. SQLITE_OK means "not stepped yet",
    // which is deliberately neither SQLITE_ROW nor SQLITE_DONE.
    int last = SQLITE_OK;
    Stmt(sqlite3 *db, const char *sql) {
        // 34th round, P1: no transaction, no statements. `s` stays null, so `ok()` is false and
        // every caller in this core takes the -EIO exit it already has for a prepare that failed.
        if (db && db == g_txn_broken) return;
        sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    }
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

// ---- RAII BEGIN IMMEDIATE (P12). Rolls back unless commit() was called. ----------------------
//
// PR #1 review (11th round): `begin_rc` recorded what the BEGIN returned, for the one caller
// that had to know -- the schema migration in store.cpp, which must not stamp a store as
// migrated on the strength of statements it never checked.
//
// ---- PR #1 review (34th round, P1): and every other caller had to know too ------------------
//
// A recorded failure that nobody reads is a failure that did not happen. BEGIN IMMEDIATE takes
// the database write lock, and it fails: SQLITE_BUSY when the 10 s busy timeout runs out under
// another `world` process, SQLITE_IOERR off the file, SQLITE_NOMEM. The old ctor set `open_`
// regardless and said nothing, so the caller's statements ran in autocommit -- and everything
// this core is built on ("the reference check and the state change are one write transaction",
// rounds 1, 5, 15, 16, 21) quietly stopped being true. A `discard S<n>` would count the
// snapshot's references in one statement and mark it TRASHING in another, with a fork free to
// commit its CREATING row in between; the destructor's ROLLBACK then rolled back nothing,
// because there was nothing to roll back, and the discard returned 0 over a baseline that a
// live world had just claimed.
//
// Three things close it, and the third is the one that cannot be forgotten:
//   * `ok()` / `err()`, and one line at EVERY construction site -- `if (!t.ok()) return t.err();`
//     or whatever that site's "do nothing" is. The idiom is greppable on purpose: every `Txn t(`
//     in world.cpp, pool.cpp and store.cpp has a `t.ok()` within a few lines of it.
//   * `commit()` returns the rc (mapped, as every other rc in this core is) and is [[nodiscard]],
//     so a caller that must not report success over a failed COMMIT cannot drop it by accident;
//     the handful that genuinely have nothing to do about it say `(void)t.commit();` and why.
//   * a failed BEGIN names its connection in `g_txn_broken` above, and Stmt refuses to prepare
//     anything against it until this Txn goes out of scope. A site that forgets the first line
//     writes nothing at all rather than writing outside a transaction.
// `open_` follows the BEGIN now as well: a transaction that never started has nothing to roll
// back, and the old unconditional ROLLBACK in the destructor was a stray statement on a
// connection that might have had somebody else's transaction open on it.
struct Txn {
    sqlite3 *db;
    bool open_ = false;
    int begin_rc = SQLITE_OK;
    sqlite3 *prev_broken_ = nullptr;
    explicit Txn(sqlite3 *d) : db(d) {
        if (wfs_test_txn_fail_once) {
            // The test seam (worldfs.h). 0 in every run that is not a test, so this is one
            // predictable branch on an int that is never written.
            begin_rc = wfs_test_txn_fail_once;
            wfs_test_txn_fail_once = 0;
        } else {
            begin_rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        }
        open_ = begin_rc == SQLITE_OK;
        prev_broken_ = g_txn_broken;
        if (!open_) g_txn_broken = db;
    }
    ~Txn() {
        if (open_) exec(db, "ROLLBACK");
        g_txn_broken = prev_broken_;
    }
    Txn(const Txn &) = delete;
    Txn &operator=(const Txn &) = delete;
    // Did the write transaction start? Everything the caller is about to do assumes it did.
    bool ok() const { return begin_rc == SQLITE_OK; }
    // ... and what to report when it did not. SQLITE_BUSY past the busy timeout is -EBUSY, which
    // is what lets the CLI say "another command holds the store" rather than "I/O error".
    int err() const { return begin_rc == SQLITE_OK ? 0 : map_sqlite(begin_rc); }
    // The COMMIT, with its result. A commit that fails leaves the transaction open, so the
    // destructor still rolls it back; a Txn that never began reports what stopped it.
    [[nodiscard]] int commit() {
        if (!open_) return err();
        int rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return map_sqlite(rc);
        open_ = false;
        return 0;
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
