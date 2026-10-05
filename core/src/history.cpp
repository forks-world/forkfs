// Continuous work history (docs/CONTINUOUS_WORK_HISTORY.md), first delivery slice.
//
// A Work revision is the work an agent did between useful Git commits: the source files that
// changed, linked to a tool call and a conversation turn where the caller knows them. It is not
// a snapshot and not a Git commit. This file implements the local observed form:
//
//   record   read a live World, compare it against its baseline snapshot through the ordinary
//            diff, and store the bytes that changed as immutable content-addressed objects under
//            <store>/content. What is recorded is the INCREMENTAL delta -- current state versus
//            the state the previous revision left each path in -- so repeated saves over
//            identical content publish nothing and an unchanged file costs nothing.
//   read     list revisions, describe one, list its changes, and fetch a content object.
//
// The durability and honesty rules the design asks for are followed where they apply to this
// slice, and named where they do not:
//
//   * Sampled, not lossless. The diff sees the current state of changed paths; a file rewritten
//     A->B->C between two records is observed only as A->C, and a file created and deleted
//     between two records is never seen. That is what coverage = OBSERVED means, and it is never
//     reported as anything stronger.
//   * Content first, then the row. Every content object is written, fsynced and renamed into
//     place before the transaction that publishes the revision commits, so a published revision
//     never references a missing object. A crash between the two leaves an unreferenced object,
//     never a dangling reference.
//   * Coherent reads only where they can be had. A regular file is opened without following a
//     symlink, and its device, inode, size and mtime are compared before and after the read. A
//     file still changing underneath us is stored as read AND marks the revision INCOMPLETE;
//     no read is ever claimed to be a coherent snapshot against an uncooperative writer.
//   * Size budget. A file over the content budget is not read: it is recorded with its metadata
//     and no content, and marks the revision INCOMPLETE, so the degradation is visible.
//   * Immutable objects in the store. Content lives under <store>/content, outside any World,
//     and no collector walks it. Nothing here deletes an object: revision retention and garbage
//     collection are delivery phase 3. An object written for a change that is then suppressed as
//     a no-op (a metadata-only T on the first record of a path) can be orphaned, which the design
//     anticipates and phase 3 collects.
//
// What is deliberately not here yet: managed agent tools and their writer-control contract,
// exact read references, restoration into a new World, retention/pins/quotas, and collaboration.
#include "worldfs/worldfs.h"

#include "db.h"
#include "internal.h"
#include "snapshot_access.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using wfs::Guard;
using wfs::Stmt;
using wfs::String;
using wfs::Txn;
using wfs::Vec;

namespace {

// ---- SHA-256 ---------------------------------------------------------------------------------
//
// Content is addressed by the SHA-256 of its bytes. The core may not take a crypto library
// (arch.md §39: libc + the C++ runtime + SQLite), so this is the standard algorithm, streaming so
// that a file is hashed as it is read and never held whole in memory. It is not used for
// security, only to name immutable objects, but a content address that could collide would hand
// back another file's bytes, so it is a real digest and not a cheap hash.

struct Sha256 {
    uint32_t h[8];
    uint64_t bits = 0;
    uint8_t buf[64];
    size_t len = 0;
};

const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_init(Sha256 *s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu; s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->len = 0;
}

void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->bits += (uint64_t)n * 8;
    if (s->len) {
        size_t take = 64 - s->len;
        if (take > n) take = n;
        memcpy(s->buf + s->len, p, take);
        s->len += take;
        p += take;
        n -= take;
        if (s->len == 64) { sha256_block(s, s->buf); s->len = 0; }
    }
    while (n >= 64) { sha256_block(s, p); p += 64; n -= 64; }
    if (n) { memcpy(s->buf, p, n); s->len = n; }
}

void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->bits;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->len != 56) sha256_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[7 - i] = (uint8_t)(bits >> (i * 8));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

void hex_encode(const uint8_t *p, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[i * 2] = h[p[i] >> 4]; out[i * 2 + 1] = h[p[i] & 15]; }
    out[n * 2] = 0;
}

// ---- paths -----------------------------------------------------------------------------------

String joinp(const char *a, const char *b) {
    String p(a);
    size_t n = p.size();
    if (n && p.c_str()[n - 1] != '/') p.append("/");
    p.append(b);
    return p;
}

void dirname_of(const char *p, String &out) {
    const char *slash = ::strrchr(p, '/');
    if (!slash) { out.assign("."); return; }
    if (slash == p) { out.assign("/"); return; }
    out.assign(p);
    out.resize((size_t)(slash - p));
}

// <store>/content/<first two hex digits>/<hash>. The two-digit fan-out is the Git object-store
// convention and keeps one directory from holding millions of names.
void content_path(wfs_store *s, const char *hex, String &out) {
    char shard[3] = {hex[0], hex[1], 0};
    out.assign(s->dir);
    out.append("/content/");
    out.append(shard);
    out.append("/");
    out.append(hex);
}

wfs_type type_from_mode(mode_t m) {
    if (S_ISREG(m)) return WFS_T_FILE;
    if (S_ISDIR(m)) return WFS_T_DIR;
    if (S_ISLNK(m)) return WFS_T_SYMLINK;
    if (S_ISFIFO(m)) return WFS_T_FIFO;
    if (S_ISCHR(m)) return WFS_T_CHR;
    if (S_ISBLK(m)) return WFS_T_BLK;
    if (S_ISSOCK(m)) return WFS_T_SOCK;
    return WFS_T_UNKNOWN;
}

void mtime_of(const struct stat &st, int64_t *sec, long *nsec) {
#ifdef __APPLE__
    *sec = (int64_t)st.st_mtimespec.tv_sec;
    *nsec = (long)st.st_mtimespec.tv_nsec;
#else
    *sec = (int64_t)st.st_mtim.tv_sec;
    *nsec = (long)st.st_mtim.tv_nsec;
#endif
}

void ctime_of(const struct stat &st, int64_t *sec, long *nsec) {
#ifdef __APPLE__
    *sec = (int64_t)st.st_ctimespec.tv_sec;
    *nsec = (long)st.st_ctimespec.tv_nsec;
#else
    *sec = (int64_t)st.st_ctim.tv_sec;
    *nsec = (long)st.st_ctim.tv_nsec;
#endif
}

// The content budget: a file larger than this is recorded by metadata alone and marks the
// revision incomplete. WFS_HISTORY_MAX_CONTENT overrides it; garbage is ignored. The design
// leaves the number to measurement (phase 3); 64 MiB keeps a single capture from ballooning the
// store while covering ordinary source and most binaries.
uint64_t content_limit() {
    const char *e = ::getenv("WFS_HISTORY_MAX_CONTENT");
    if (e && *e) {
        char *end = nullptr;
        unsigned long long v = ::strtoull(e, &end, 10);
        if (end != e && (!end || !*end)) return (uint64_t)v;
    }
    return 64ull * 1024 * 1024;
}

// .git, an owned .world-git and the .world marker are not source content. The design excludes
// them from ordinary source-history replay; Git context is recorded separately on the revision.
// Any path component is checked, so a submodule's own `.git` file is excluded too and not just
// the repository root's.
bool path_excluded(const char *rel) {
    if (!::strcmp(rel, WFS_MARKER_NAME)) return true;
    const char *p = rel;
    while (*p) {
        const char *end = ::strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : ::strlen(p);
        if ((n == 4 && !::strncmp(p, ".git", 4)) ||
            (n == 10 && !::strncmp(p, ".world-git", 10)))
            return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

// ---- content objects -------------------------------------------------------------------------

// Streams bytes into a temporary file under <store>/tmp while hashing them, then renames the
// temporary to <store>/content/<aa>/<hash>. The rename is same-volume and atomic, so the object
// either does not exist or is complete; an object already there is deduplication, not failure.
struct ContentWriter {
    int fd = -1;
    String tmp;
    Sha256 h;
    int err = 0;

    int begin(wfs_store *s) {
        static unsigned seq = 0;
        String p(s->dir);
        p.append("/tmp/content-");
        char nm[64];
        ::snprintf(nm, sizeof nm, "%ld-%u", (long)::getpid(), seq++);
        p.append(nm);
        tmp = p;
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) { err = -errno; tmp.clear(); return err; }
        sha256_init(&h);
        return 0;
    }
    void update(const void *data, size_t n) {
        if (err) return;
        sha256_update(&h, data, n);
        const uint8_t *b = (const uint8_t *)data;
        size_t left = n;
        while (left) {
            ssize_t w = ::write(fd, b, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                err = -errno;
                return;
            }
            b += (size_t)w;
            left -= (size_t)w;
        }
    }
    void abort() {
        if (fd >= 0) { ::close(fd); fd = -1; }
        if (tmp.size()) { ::unlink(tmp.c_str()); tmp.clear(); }
    }
    int finish(wfs_store *s, char out[WFS_REVISION_HASH_MAX]) {
        if (err) { abort(); return err; }
        if (::fsync(fd) != 0) { err = -errno; abort(); return err; }
        if (::close(fd) != 0) { fd = -1; err = -EIO; ::unlink(tmp.c_str()); tmp.clear(); return err; }
        fd = -1;
        uint8_t digest[32];
        sha256_final(&h, digest);
        hex_encode(digest, 32, out);
        String final, shard;
        content_path(s, out, final);
        dirname_of(final.c_str(), shard);
        if (int rc = wfs::fs_mkdir_p(shard.c_str())) { ::unlink(tmp.c_str()); tmp.clear(); return rc; }
        if (::rename(tmp.c_str(), final.c_str()) != 0) {
            int e = errno;
            // A concurrent recorder may have created the identical object first. The bytes are
            // the same by construction, so that is deduplication; anything else is a failure.
            if (wfs::fs_probe(final.c_str()) == 0) { ::unlink(tmp.c_str()); tmp.clear(); return 0; }
            ::unlink(tmp.c_str());
            tmp.clear();
            return -e;
        }
        tmp.clear();
        return 0;
    }
};

int content_put_mem(wfs_store *s, const void *data, size_t n, char out[WFS_REVISION_HASH_MAX]) {
    ContentWriter w;
    if (int rc = w.begin(s)) return rc;
    w.update(data, n);
    return w.finish(s, out);
}

// The state one path is in, as the recorder sees it. `hash` is the content address ("" for a
// directory, a special file, an over-budget file, or an entry that is not there); `incoherent`
// means the bytes could not be captured as a stable read and the revision must say so.
struct State {
    bool present = false;
    int kind = 0;
    uint32_t mode = 0;
    uint64_t size = 0;
    String hash;
    bool incoherent = false;
};

int capture_regular(wfs_store *s, const char *path, const struct stat &lst, State &st) {
    uint64_t limit = content_limit();
    st.size = (uint64_t)lst.st_size;
    if ((uint64_t)lst.st_size > limit) { st.incoherent = true; return 0; }
    int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { st.incoherent = true; return 0; }
    struct stat bst;
    if (::fstat(fd, &bst) != 0 || !S_ISREG(bst.st_mode) || bst.st_dev != lst.st_dev ||
        bst.st_ino != lst.st_ino) {
        ::close(fd);
        st.incoherent = true;
        return 0;
    }
    ContentWriter w;
    if (int rc = w.begin(s)) { ::close(fd); return rc; }
    char buf[64 * 1024];
    uint64_t total = 0;
    bool failed = false;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed = true;
            break;
        }
        if (n == 0) break;
        if (total + (uint64_t)n > limit) { failed = true; st.incoherent = true; break; }
        w.update(buf, (size_t)n);
        total += (uint64_t)n;
    }
    struct stat ast;
    int frc = ::fstat(fd, &ast);
    ::close(fd);
    if (failed || w.err) { w.abort(); st.incoherent = true; return 0; }
    if (frc != 0) { w.abort(); st.incoherent = true; return 0; }
    int64_t bs, as; long bn, an;
    mtime_of(bst, &bs, &bn); mtime_of(ast, &as, &an);
    int64_t bc, ac; long bcn, acn;
    ctime_of(bst, &bc, &bcn); ctime_of(ast, &ac, &acn);
    if (ast.st_size != bst.st_size || ast.st_ino != bst.st_ino || bs != as || bn != an ||
        bc != ac || bcn != acn)
        st.incoherent = true;
    char hex[WFS_REVISION_HASH_MAX];
    if (int rc = w.finish(s, hex)) { st.incoherent = true; return rc; }
    st.hash.assign(hex);
    st.size = total;
    return 0;
}

// Reads one path into `st`, materializing its content into the store when it has any. Follows no
// symlink: a symlink's "content" is the target string, exactly as a Git blob records it.
int capture_state(wfs_store *s, const char *path, State &st) {
    struct stat lst;
    if (::lstat(path, &lst) != 0) {
        int e = errno;
        if (wfs::fs_gone(-e)) return 0;   // genuinely not there; st.present stays false
        return -e;
    }
    st.present = true;
    st.mode = (uint32_t)(lst.st_mode & 07777);
    if (S_ISLNK(lst.st_mode)) {
        st.kind = WFS_T_SYMLINK;
        char buf[WFS_PATH_MAX];
        ssize_t n = ::readlink(path, buf, sizeof buf);
        if (n < 0) { st.incoherent = true; return 0; }
        st.size = (uint64_t)n;
        if ((size_t)n >= sizeof buf) st.incoherent = true;   // the target was truncated
        char hex[WFS_REVISION_HASH_MAX];
        if (int rc = content_put_mem(s, buf, (size_t)n, hex)) return rc;
        st.hash.assign(hex);
        return 0;
    }
    if (S_ISDIR(lst.st_mode)) { st.kind = WFS_T_DIR; return 0; }
    if (S_ISREG(lst.st_mode)) {
        st.kind = WFS_T_FILE;
        return capture_regular(s, path, lst, st);
    }
    st.kind = type_from_mode(lst.st_mode);
    return 0;
}

// ---- database helpers ------------------------------------------------------------------------

int history_prev(wfs_store *s, wfs_id world, const char *path, State &st, bool &has_row) {
    Stmt q(s->db, "SELECT last_hash, last_kind, last_mode FROM history_files WHERE world_id=? AND path=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)world);
    q.text(2, path);
    if (!q.row()) { has_row = false; return q.done() ? 0 : -EIO; }
    has_row = true;
    st.hash.assign(q.col_text(0));
    st.kind = (int)q.col_i64(1);
    st.mode = (uint32_t)q.col_i64(2);
    st.present = st.kind != 0;
    return 0;
}

// The logical identity of a path, created on first record with the baseline state it is
// reconciled against. An existing row is returned untouched -- its base was captured then.
int file_id_get_or_create(wfs_store *s, wfs_id world, const char *path, const State &base,
                          wfs_id *out) {
    Stmt q(s->db, "SELECT file_id FROM history_files WHERE world_id=? AND path=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)world);
    q.text(2, path);
    if (q.row()) { *out = (wfs_id)q.col_i64(0); return 0; }
    if (!q.done()) return -EIO;
    Stmt ins(s->db,
             "INSERT INTO history_files(world_id, path, base_hash, base_kind, base_mode, base_size)"
             " VALUES(?,?,?,?,?,?)");
    if (!ins.ok()) return -EIO;
    ins.i64(1, (int64_t)world);
    ins.text(2, path);
    ins.text(3, base.present ? base.hash.c_str() : "");
    ins.i64(4, base.present ? base.kind : 0);
    ins.i64(5, base.present ? (int64_t)base.mode : 0);
    ins.i64(6, base.present ? (int64_t)base.size : 0);
    if (ins.step() != SQLITE_DONE) return -EIO;
    *out = (wfs_id)::sqlite3_last_insert_rowid(s->db);
    return 0;
}

int content_ref(wfs_store *s, const char *hash, uint64_t size, int64_t now) {
    Stmt ins(s->db, "INSERT OR IGNORE INTO content(hash, size, refs, created_at) VALUES(?,?,0,?)");
    if (!ins.ok()) return -EIO;
    ins.text(1, hash);
    ins.i64(2, (int64_t)size);
    ins.i64(3, now);
    if (ins.step() != SQLITE_DONE) return -EIO;
    Stmt up(s->db, "UPDATE content SET refs=refs+1 WHERE hash=?");
    if (!up.ok()) return -EIO;
    up.text(1, hash);
    if (up.step() != SQLITE_DONE) return -EIO;
    return 0;
}

const char *kRevCols =
    "id, world_id, parent_revision, baseline_snapshot, origin, coverage, actor_id, turn_id,"
    " tool_call_id, git_head, capture_started_at, capture_finished_at, created_at, changes,"
    " added, modified, deleted, meta";

void fill_revision(Stmt &q, wfs_revision_rec &r) {
    ::memset(&r, 0, sizeof r);
    r.id = (wfs_id)q.col_i64(0);
    r.world_id = (wfs_id)q.col_i64(1);
    r.parent_revision = (wfs_id)q.col_i64(2);
    r.baseline_snapshot = (wfs_id)q.col_i64(3);
    r.origin = (int)q.col_i64(4);
    r.coverage = (int)q.col_i64(5);
    wfs::copy_str(r.actor_id, sizeof r.actor_id, q.col_text(6));
    wfs::copy_str(r.turn_id, sizeof r.turn_id, q.col_text(7));
    wfs::copy_str(r.tool_call_id, sizeof r.tool_call_id, q.col_text(8));
    wfs::copy_str(r.git_head, sizeof r.git_head, q.col_text(9));
    r.capture_started_at = q.col_i64(10);
    r.capture_finished_at = q.col_i64(11);
    r.created_at = q.col_i64(12);
    r.changes = (uint64_t)q.col_i64(13);
    r.added = (uint64_t)q.col_i64(14);
    r.modified = (uint64_t)q.col_i64(15);
    r.deleted = (uint64_t)q.col_i64(16);
    r.meta = (uint64_t)q.col_i64(17);
}

// The incremental verdict: what changed between the state a path is in now and the state the
// previous revision left it in. Content and kind are the source identity; a mode change alone is
// metadata. uid/gid/mtime/xattr-only differences are not source history and are not recorded.
bool differs(const State &cur, const State &prev, int &change) {
    if (!cur.present && !prev.present) return false;
    if (cur.present && !prev.present) { change = WFS_C_ADDED; return true; }
    if (!cur.present && prev.present) { change = WFS_C_DELETED; return true; }
    if (cur.hash != prev.hash || cur.kind != prev.kind) { change = WFS_C_MODIFIED; return true; }
    if (cur.mode != prev.mode) { change = WFS_C_META; return true; }
    return false;
}

struct Collect {
    Vec<String> paths;
};

int collect_diff(void *ctx, const wfs_diff_entry *e) {
    Collect *c = (Collect *)ctx;
    c->paths.emplace_back(String(e->path));
    return 0;
}

// The diff reports its paths sorted and deduplicated (diff.cpp sorts before handing them over),
// so membership is a binary search. Reconciliation uses it to skip the rows the loop above
// already compared.
bool in_diff(const Vec<String> &paths, const char *p) {
    size_t lo = 0, hi = paths.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = ::strcmp(p, paths[mid].c_str());
        if (cmp == 0) return true;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

// One recorded change, built either from a diff path (compared against the state the previous
// revision left it in) or from reconciliation (a recorded path the baseline diff no longer
// reports, whose current state is therefore the baseline state).
struct Out {
    int change = 0;
    int kind = 0;
    uint32_t mode = 0;
    uint64_t size = 0, before_size = 0, after_size = 0;
    String path, before, after;
    // The baseline state, used only when this change creates the path's history_files row.
    State base;
    wfs_id file_id = 0;   // non-zero: update this existing row (reconciliation)
};

// The per-World marker lock, held across capture and publication so two recorders -- and a
// concurrent fork/checkpoint/discard of the same World -- cannot read and publish against each
// other. See internal.h / world.cpp; the flock and the busy verdict are the existing world-level
// lock.
struct MarkerLock {
    int fd = -1;
    ~MarkerLock() { if (fd >= 0) wfs::world_marker_lock_release(fd); }
    MarkerLock() = default;
    MarkerLock(const MarkerLock &) = delete;
    MarkerLock &operator=(const MarkerLock &) = delete;
};

} // namespace

// ---- recording -------------------------------------------------------------------------------

extern "C" int wfs_history_record(wfs_store *s, wfs_id world, const char *actor_id,
                                  const char *turn_id, const char *tool_call_id,
                                  wfs_id *out_revision) {
    if (!s || !world || !out_revision) return -EINVAL;
    *out_revision = 0;
    int64_t t0 = wfs::now_sec();

    wfs_world_rec wr;
    if (int rc = wfs_world_info(s, world, &wr)) return rc;
    if (wr.state != WFS_ST_ACTIVE) return -ESTALE;

    // P1: follow a world that merely moved; refuse one that cannot be found.
    wfs_identity ident;
    if (int rc = wfs_world_verify(s, world, &ident)) return rc;
    const char *wroot = ident.path;

    if (wr.snapshot_id == 0) return WFS_E_SOURCE_GONE;
    wfs_snapshot_rec sr;
    int rc = wfs_snapshot_info(s, wr.snapshot_id, &sr);
    if (rc == -ENOENT) return WFS_E_SOURCE_GONE;
    if (rc) return rc;
    if (sr.state != WFS_ST_ACTIVE || !sr.path[0]) return WFS_E_SOURCE_GONE;

    // Serialize capture and publication against every other world-level operation on this World
    // (another `history record`, a fork, a checkpoint, a discard). Without this the second of two
    // overlapping records reads the old history_files state, commits with the first as its parent,
    // and can publish before_hashes from a state that no longer exists. (PR #37 review, P1.)
    MarkerLock wl;
    if (int lrc = wfs::world_marker_lock_take(wroot, &wl.fd)) return lrc;

    // The candidate set: every path that differs from the baseline snapshot. The diff is exact
    // about the current state (P10); what makes the revision incremental is the comparison below
    // against the previous revision's recorded state, not this diff.
    Collect col;
    if (int drc = wfs_world_diff_ex(s, world, 0, collect_diff, &col, nullptr)) return drc;

    Vec<Out> recs;
    bool incoherent = false;

    {
        // The baseline is read inside the snapshot's gate, exactly as the diff reads it. The gate
        // window covers the whole capture; a fork from this snapshot waits behind it, which is
        // the same price the diff already pays.
        wfs::SnapshotReadGuard gate(sr.path);
        if (wfs::fs_gone(gate.rc)) return WFS_E_SOURCE_GONE;
        if (gate.rc) return gate.rc;

        for (size_t i = 0; i < col.paths.size(); ++i) {
            const char *rel = col.paths[i].c_str();
            if (path_excluded(rel)) continue;

            State cur;
            String wp = joinp(wroot, rel);
            if (int e = capture_state(s, wp.c_str(), cur)) return e;

            State prev;
            bool has_row = false;
            if (int e = history_prev(s, world, rel, prev, has_row)) return e;
            if (!has_row) {
                String bp = joinp(sr.path, rel);
                if (int e = capture_state(s, bp.c_str(), prev)) return e;
            }
            if (cur.incoherent || prev.incoherent) incoherent = true;

            int change = 0;
            if (!differs(cur, prev, change)) continue;
            Out o;
            o.change = change;
            o.path = String(rel);
            o.kind = cur.present ? cur.kind : prev.kind;
            o.mode = cur.present ? cur.mode : prev.mode;
            o.size = cur.present ? cur.size : prev.size;
            if (prev.present) { o.before = prev.hash; o.before_size = prev.size; }
            if (cur.present) { o.after = cur.hash; o.after_size = cur.size; }
            if (!has_row) o.base = prev;   // the baseline state to reconcile against later
            recs.emplace_back(o);
        }

        // Reconciliation (PR #37 review, P1): a path the diff no longer reports is one whose live
        // state matches the baseline snapshot again -- so a file recorded as modified and then
        // reverted, or an addition that was later deleted, produces no candidate above. Every
        // recorded path that is not a candidate is therefore compared against its baseline state
        // (recorded in base_* when its row was created), and a difference is recorded against the
        // baseline. This needs no filesystem read: "not in the diff" already means "equals the
        // snapshot" for content, kind and mode.
        {
            Stmt q(s->db,
                   "SELECT file_id, path, last_hash, last_kind, last_mode, last_size,"
                   " base_hash, base_kind, base_mode, base_size"
                   " FROM history_files WHERE world_id=? ORDER BY path");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)world);
            while (q.row()) {
                const char *rel = q.col_text(1);
                if (path_excluded(rel) || in_diff(col.paths, rel)) continue;
                State last;
                last.hash.assign(q.col_text(2));
                last.kind = (int)q.col_i64(3);
                last.mode = (uint32_t)q.col_i64(4);
                last.size = (uint64_t)q.col_i64(5);
                last.present = last.kind != 0;
                State base;
                base.hash.assign(q.col_text(6));
                base.kind = (int)q.col_i64(7);
                base.mode = (uint32_t)q.col_i64(8);
                base.size = (uint64_t)q.col_i64(9);
                base.present = base.kind != 0;
                int change = 0;
                if (!differs(base, last, change)) continue;
                Out o;
                o.change = change;
                o.path = String(rel);
                o.file_id = (wfs_id)q.col_i64(0);
                o.kind = base.present ? base.kind : last.kind;
                o.mode = base.present ? base.mode : last.mode;
                o.size = base.present ? base.size : last.size;
                if (last.present) { o.before = last.hash; o.before_size = last.size; }
                if (base.present) { o.after = base.hash; o.after_size = base.size; }
                recs.emplace_back(o);
            }
            if (!q.done()) return -EIO;
        }
    }

    if (recs.empty()) return 0;   // no relevant state change: publish nothing, as the design asks

    char git_head[WFS_REVISION_HASH_MAX] = {0};
    {
        wfs_git_info gi;
        ::memset(&gi, 0, sizeof gi);
        if (wfs_git_inspect(wroot, &gi) == 0 && gi.present) wfs::copy_str(git_head, sizeof git_head, gi.head);
    }

    int64_t t1 = wfs::now_sec();
    uint64_t added = 0, modified = 0, deleted = 0, meta = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        switch (recs[i].change) {
        case WFS_C_ADDED: added++; break;
        case WFS_C_MODIFIED: modified++; break;
        case WFS_C_DELETED: deleted++; break;
        default: meta++; break;
        }
    }

    Guard g(s->mu);
    wfs::Txn t(s->db);
    if (!t.ok()) return t.err();

    wfs_id parent = 0;
    {
        Stmt q(s->db, "SELECT COALESCE(MAX(id),0) FROM revisions WHERE world_id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)world);
        if (q.row()) parent = (wfs_id)q.col_i64(0);
        else if (!q.done()) return -EIO;
    }
    {
        Stmt ins(s->db,
                 "INSERT INTO revisions(world_id, parent_revision, baseline_snapshot, origin,"
                 " coverage, actor_id, turn_id, tool_call_id, git_head, capture_started_at,"
                 " capture_finished_at, created_at, changes, added, modified, deleted, meta)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.i64(1, (int64_t)world);
        ins.i64(2, (int64_t)parent);
        ins.i64(3, (int64_t)wr.snapshot_id);
        ins.i64(4, WFS_RV_ORIGIN_FILESYSTEM);
        ins.i64(5, incoherent ? WFS_RV_COVERAGE_INCOMPLETE : WFS_RV_COVERAGE_OBSERVED);
        ins.text(6, actor_id ? actor_id : "");
        ins.text(7, turn_id ? turn_id : "");
        ins.text(8, tool_call_id ? tool_call_id : "");
        ins.text(9, git_head);
        ins.i64(10, t0);
        ins.i64(11, t1);
        ins.i64(12, t1);
        ins.i64(13, (int64_t)recs.size());
        ins.i64(14, (int64_t)added);
        ins.i64(15, (int64_t)modified);
        ins.i64(16, (int64_t)deleted);
        ins.i64(17, (int64_t)meta);
        if (ins.step() != SQLITE_DONE) return -EIO;
    }
    wfs_id rev = (wfs_id)::sqlite3_last_insert_rowid(s->db);

    int64_t seq = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        Out &o = recs[i];
        wfs_id fid = o.file_id;
        if (!fid) {
            if (int e = file_id_get_or_create(s, world, o.path.c_str(), o.base, &fid)) return e;
        }

        bool del = o.change == WFS_C_DELETED;
        {
            Stmt c(s->db,
                   "INSERT INTO changes(revision_id, seq, file_id, change, kind, old_path,"
                   " new_path, before_hash, after_hash, size, mode) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            if (!c.ok()) return -EIO;
            c.i64(1, (int64_t)rev);
            c.i64(2, seq++);
            c.i64(3, (int64_t)fid);
            c.i64(4, o.change);
            c.i64(5, o.kind);
            c.text(6, del ? o.path.c_str() : "");
            c.text(7, del ? "" : o.path.c_str());
            c.text(8, o.before.c_str());
            c.text(9, o.after.c_str());
            c.i64(10, (int64_t)o.size);
            c.i64(11, (int64_t)o.mode);
            if (c.step() != SQLITE_DONE) return -EIO;
        }
        // The state the next record compares against: a deletion is recorded absent (kind 0,
        // empty hash); everything else is the current state.
        int last_kind = del ? 0 : o.kind;
        String last_hash;
        if (!del) last_hash = o.after;
        {
            Stmt u(s->db,
                   "UPDATE history_files SET last_hash=?, last_kind=?, last_mode=?, last_size=?,"
                   " last_revision=? WHERE file_id=?");
            if (!u.ok()) return -EIO;
            u.text(1, last_hash.c_str());
            u.i64(2, last_kind);
            u.i64(3, (int64_t)o.mode);
            u.i64(4, (int64_t)o.size);
            u.i64(5, (int64_t)rev);
            u.i64(6, (int64_t)fid);
            if (u.step() != SQLITE_DONE) return -EIO;
        }
        if (o.before.size() && content_ref(s, o.before.c_str(), o.before_size, t1)) return -EIO;
        if (o.after.size() && content_ref(s, o.after.c_str(), o.after_size, t1)) return -EIO;
    }

    if (int crc = t.commit()) return crc;
    *out_revision = rev;
    return 0;
}

// ---- reading ---------------------------------------------------------------------------------

extern "C" int wfs_revision_info(wfs_store *s, wfs_id id, wfs_revision_rec *out) {
    if (!s || !out || !id) return -EINVAL;
    Guard g(s->mu);
    String sql("SELECT ");
    sql.append(kRevCols);
    sql.append(" FROM revisions WHERE id=?");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    if (!q.row()) return q.done() ? -ENOENT : -EIO;
    fill_revision(q, *out);
    return 0;
}

extern "C" int wfs_revision_list(wfs_store *s, wfs_id world, wfs_revision_rec *buf, size_t cap,
                                 size_t *count) {
    if (!s || !count) return -EINVAL;
    *count = 0;
    Guard g(s->mu);
    String sql("SELECT ");
    sql.append(kRevCols);
    sql.append(" FROM revisions");
    if (world) sql.append(" WHERE world_id=?");
    sql.append(" ORDER BY id DESC");
    Stmt q(s->db, sql.c_str());
    if (!q.ok()) return -EIO;
    if (world) q.i64(1, (int64_t)world);
    size_t n = 0;
    while (q.row()) {
        if (buf && n < cap) fill_revision(q, buf[n]);
        n++;
    }
    if (!q.done()) return -EIO;
    *count = n;
    return 0;
}

extern "C" int wfs_revision_changes(wfs_store *s, wfs_id id, wfs_revision_change *buf, size_t cap,
                                    size_t *count) {
    if (!s || !count || !id) return -EINVAL;
    *count = 0;
    Guard g(s->mu);
    Stmt q(s->db,
           "SELECT file_id, change, kind, old_path, new_path, before_hash, after_hash, size, mode"
           " FROM changes WHERE revision_id=? ORDER BY seq");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)id);
    size_t n = 0;
    while (q.row()) {
        if (buf && n < cap) {
            wfs_revision_change &c = buf[n];
            ::memset(&c, 0, sizeof c);
            c.file_id = (wfs_id)q.col_i64(0);
            c.change = (int)q.col_i64(1);
            c.type = (wfs_type)q.col_i64(2);
            const char *oldp = q.col_text(3);
            const char *newp = q.col_text(4);
            wfs::copy_str(c.path, sizeof c.path, newp[0] ? newp : oldp);
            wfs::copy_str(c.before_hash, sizeof c.before_hash, q.col_text(5));
            wfs::copy_str(c.after_hash, sizeof c.after_hash, q.col_text(6));
            c.size = (uint64_t)q.col_i64(7);
            c.mode = (uint32_t)q.col_i64(8);
        }
        n++;
    }
    if (!q.done()) return -EIO;
    *count = n;
    return 0;
}

extern "C" int wfs_history_content(wfs_store *s, const char *hash, void **buf, size_t *len) {
    if (!s || !hash || !buf || !len) return -EINVAL;
    *buf = nullptr;
    *len = 0;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT size FROM content WHERE hash=?");
        if (!q.ok()) return -EIO;
        q.text(1, hash);
        if (!q.row()) return q.done() ? -ENOENT : -EIO;
    }
    String p;
    content_path(s, hash, p);
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    struct stat st;
    if (::fstat(fd, &st) != 0) { int e = errno; ::close(fd); return -e; }
    size_t want = (size_t)st.st_size;
    void *mem = ::malloc(want ? want : 1);
    if (!mem) { ::close(fd); return -ENOMEM; }
    size_t got = 0;
    while (got < want) {
        ssize_t n = ::read(fd, (char *)mem + got, want - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::free(mem);
            ::close(fd);
            return -e;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    ::close(fd);
    *buf = mem;
    *len = got;
    return 0;
}

extern "C" void wfs_history_free(void *buf) { ::free(buf); }
