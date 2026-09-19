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

struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    bool ok() const { return s != nullptr; }
    void text(int i, const char *v) { sqlite3_bind_text(s, i, v ? v : "", -1, SQLITE_TRANSIENT); }
    void i64(int i, int64_t v) { sqlite3_bind_int64(s, i, (sqlite3_int64)v); }
    int step() { return sqlite3_step(s); }
    bool row() { return sqlite3_step(s) == SQLITE_ROW; }
    int64_t col_i64(int i) { return (int64_t)sqlite3_column_int64(s, i); }
    const char *col_text(int i) {
        const unsigned char *t = sqlite3_column_text(s, i);
        return t ? (const char *)t : "";
    }
};

inline void exec(sqlite3 *db, const char *sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

// RAII BEGIN IMMEDIATE. Rolls back unless commit() was called (P12).
struct Txn {
    sqlite3 *db;
    bool open_ = false;
    explicit Txn(sqlite3 *d) : db(d) { exec(db, "BEGIN IMMEDIATE"); open_ = true; }
    ~Txn() { if (open_) exec(db, "ROLLBACK"); }
    Txn(const Txn &) = delete;
    Txn &operator=(const Txn &) = delete;
    void commit() { if (open_) { exec(db, "COMMIT"); open_ = false; } }
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
