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
// directory, a special file, an over-budget file, or an entry that is not there); `mtime` is a
// detector kept only so that a content-less regular file whose bytes could not be captured can
// still be told from an unchanged one; `incoherent` means the bytes could not be captured as a
// stable read and the revision must say so.
struct State {
    bool present = false;
    int kind = 0;
    uint32_t mode = 0;
    uint64_t size = 0;
    int64_t mtime = 0;   // nanoseconds
    String hash;
    bool incoherent = false;
};

// An owned directory descriptor, so the capture loops close what they open on every path out.
struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd() = default;
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

// Opens the parent directory of `rel` under `rootfd` and returns it with `leaf` set, or a
// negative value. Every component is opened with O_NOFOLLOW, so a symlinked ancestor is refused
// (returns -EINVAL) instead of being followed out of the World -- the final lstat(2)/open(2)
// would otherwise resolve such a path outside the tree and capture host-file bytes.
int open_parent(int rootfd, const char *rel, String &leaf) {
    if (!rel || !*rel) return -EINVAL;
    int fd = ::dup(rootfd);
    if (fd < 0) return -errno;
    const char *p = rel;
    for (;;) {
        const char *slash = ::strchr(p, '/');
        if (!slash) { leaf.assign(p); return fd; }
        if (slash == p) { ::close(fd); return -EINVAL; }   // empty component
        String comp(p, (size_t)(slash - p));
        if (!::strcmp(comp.c_str(), ".") || !::strcmp(comp.c_str(), "..")) {
            ::close(fd);
            return -EINVAL;
        }
        int nfd = ::openat(fd, comp.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0) {
            int e = errno;
            ::close(fd);
            return (e == ELOOP || e == ENOTDIR) ? -EINVAL : -e;   // a symlinked ancestor
        }
        ::close(fd);
        fd = nfd;
        p = slash + 1;
    }
}

// Reads the already-open regular file `fd` (owned and closed here). `lst` is its
// AT_SYMLINK_NOFOLLOW stat from the parent, used to detect replacement under us.
int capture_regular(wfs_store *s, int fd, const struct stat &lst, State &st) {
    uint64_t limit = content_limit();
    st.size = (uint64_t)lst.st_size;
    if ((uint64_t)lst.st_size > limit) { ::close(fd); st.incoherent = true; return 0; }
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

// Reads one path relative to an open World/snapshot root (`.world`/`.git` paths are filtered
// before this). Follows no symlink -- not the final one (a symlink's "content" is its target
// string, exactly as a Git blob records it) and not an ancestor -- and checks identity and mtime
// before and after the read.
int capture_state(wfs_store *s, int rootfd, const char *rel, State &st) {
    String leaf;
    int pfd = open_parent(rootfd, rel, leaf);
    if (pfd < 0) {
        if (pfd == -EINVAL) { st.incoherent = true; return 0; }   // a symlinked ancestor
        if (wfs::fs_gone(pfd)) return 0;   // an ancestor is genuinely absent: not there
        return pfd;
    }
    struct stat lst;
    if (::fstatat(pfd, leaf.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        ::close(pfd);
        if (wfs::fs_gone(-e)) return 0;   // genuinely not there; st.present stays false
        return -e;
    }
    st.present = true;
    st.mode = (uint32_t)(lst.st_mode & 07777);
    {
        int64_t ms; long mn;
        mtime_of(lst, &ms, &mn);
        st.mtime = ms * 1000000000LL + mn;
    }
    if (S_ISLNK(lst.st_mode)) {
        st.kind = WFS_T_SYMLINK;
        char buf[WFS_PATH_MAX];
        ssize_t n = ::readlinkat(pfd, leaf.c_str(), buf, sizeof buf);
        ::close(pfd);
        if (n < 0) { st.incoherent = true; return 0; }
        st.size = (uint64_t)n;
        if ((size_t)n >= sizeof buf) st.incoherent = true;   // the target was truncated
        char hex[WFS_REVISION_HASH_MAX];
        if (int rc = content_put_mem(s, buf, (size_t)n, hex)) return rc;
        st.hash.assign(hex);
        return 0;
    }
    if (S_ISDIR(lst.st_mode)) { ::close(pfd); st.kind = WFS_T_DIR; return 0; }
    if (S_ISREG(lst.st_mode)) {
        int fd = ::openat(pfd, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        ::close(pfd);
        if (fd < 0) { st.incoherent = true; return 0; }
        st.kind = WFS_T_FILE;
        return capture_regular(s, fd, lst, st);
    }
    ::close(pfd);
    st.kind = type_from_mode(lst.st_mode);
    return 0;
}

// ---- database helpers ------------------------------------------------------------------------

// Reads both states a record needs for one path: `last`, what the previous revision left it as,
// and `base`, what the baseline snapshot has. The base is what reconciliation compares against
// when the baseline diff reports nothing.
int history_prev(wfs_store *s, wfs_id world, const char *path, State &last, State &base,
                 bool &has_row) {
    Stmt q(s->db,
           "SELECT last_hash, last_kind, last_mode, last_size, last_mtime,"
           " base_hash, base_kind, base_mode, base_size, base_mtime"
           " FROM history_files WHERE world_id=? AND path=?");
    if (!q.ok()) return -EIO;
    q.i64(1, (int64_t)world);
    q.text(2, path);
    if (!q.row()) { has_row = false; return q.done() ? 0 : -EIO; }
    has_row = true;
    last.hash.assign(q.col_text(0));
    last.kind = (int)q.col_i64(1);
    last.mode = (uint32_t)q.col_i64(2);
    last.size = (uint64_t)q.col_i64(3);
    last.mtime = q.col_i64(4);
    last.present = last.kind != 0;
    base.hash.assign(q.col_text(5));
    base.kind = (int)q.col_i64(6);
    base.mode = (uint32_t)q.col_i64(7);
    base.size = (uint64_t)q.col_i64(8);
    base.mtime = q.col_i64(9);
    base.present = base.kind != 0;
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
             "INSERT INTO history_files(world_id, path, base_hash, base_kind, base_mode, base_size,"
             " base_mtime) VALUES(?,?,?,?,?,?,?)");
    if (!ins.ok()) return -EIO;
    ins.i64(1, (int64_t)world);
    ins.text(2, path);
    ins.text(3, base.present ? base.hash.c_str() : "");
    ins.i64(4, base.present ? base.kind : 0);
    ins.i64(5, base.present ? (int64_t)base.mode : 0);
    ins.i64(6, base.present ? (int64_t)base.size : 0);
    ins.i64(7, base.present ? base.mtime : 0);
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
    " added, modified, deleted, meta, manifest_hash, parent_hash";

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
    wfs::copy_str(r.manifest_hash, sizeof r.manifest_hash, q.col_text(18));
    wfs::copy_str(r.parent_hash, sizeof r.parent_hash, q.col_text(19));
}

// A regular file whose content could not be captured -- over the size budget, or unreadable when
// it was read -- carries no hash. The absence of a hash is not "the same content": it is a state
// whose content is unknown, and a revision that contains one can never be called lossless.
bool hashless_file(const State &s) {
    return s.present && s.kind == WFS_T_FILE && s.hash.size() == 0;
}

// The incremental verdict: what changed between the state a path is in now and the state the
// previous revision left it in. Content and kind are the source identity; a mode change alone is
// metadata. uid/gid/mtime/xattr-only differences are not source history and are not recorded.
bool differs(const State &cur, const State &prev, int &change) {
    if (!cur.present && !prev.present) return false;
    if (cur.present && !prev.present) { change = WFS_C_ADDED; return true; }
    if (!cur.present && prev.present) { change = WFS_C_DELETED; return true; }
    if (cur.hash != prev.hash || cur.kind != prev.kind) { change = WFS_C_MODIFIED; return true; }
    // Two regular files with no content on either side (both over the budget): equal empty hashes
    // are not equality of content, so size and mtime decide. The mtime is what catches a rewrite
    // with different bytes of the same length; without it such a change would be silently lost.
    if (cur.kind == WFS_T_FILE && cur.hash.size() == 0 &&
        (cur.size != prev.size || cur.mtime != prev.mtime)) {
        change = WFS_C_MODIFIED;
        return true;
    }
    if (cur.mode != prev.mode) { change = WFS_C_META; return true; }
    return false;
}

// ---- the canonical revision manifest ---------------------------------------------------------
//
// A manifest is the store-independent description of one Work revision: enough for a peer to
// import the revision, verify it and continue recording from it, without any local id. It is
// emitted as a fixed line-oriented text form and addressed by the SHA-256 of those exact bytes,
// so the same revision has the same id in every store and the chain walks by hash. The manifest
// itself is stored as a content object (immutable, deduplicated, transferable).
//
// Every byte string that may contain spaces or newlines (a path, a world or tool name) is
// hex-encoded, which is why the format has no escaping rules and no ambiguity. "-" is the empty
// value. A change carries three explicit states -- after (the revision's result), before (the
// previous revision's result) and base (the baseline snapshot) -- so an import can rebuild the
// incremental state without reading any files.

struct ManifestChange {
    String path;
    int change = 0;
    State after, before, base;
};

struct ManifestInput {
    String world_name;
    int origin = 0, coverage = 0;
    String actor, turn, tool_call, git_head;
    int64_t capture_started = 0, capture_finished = 0, created = 0;
    String parent_hash;
    String base_store;
    int64_t base_snapshot = 0, base_created = 0;
    Vec<ManifestChange> changes;
};

void sha256_hex(const void *data, size_t n, char out[WFS_REVISION_HASH_MAX]) {
    uint8_t d[32];
    Sha256 h;
    sha256_init(&h);
    sha256_update(&h, data, n);
    sha256_final(&h, d);
    hex_encode(d, 32, out);
}

// A byte string that may contain spaces or newlines (a path, a name, an actor) is hex-encoded.
void append_bytes_or_dash(String &out, const String &v) {
    if (v.size() == 0) { out.append("-"); return; }
    Vec<char> buf(v.size() * 2 + 1);
    hex_encode((const uint8_t *)v.c_str(), v.size(), buf.data());
    out.append(buf.data());
}

// A value that is already a safe token -- a hash, a store id, a Git HEAD -- is written as is.
// State::hash is always a lowercase-hex content address, never raw bytes, so it goes here.
void append_str_or_dash(String &out, const String &v) {
    if (v.size() == 0) out.append("-");
    else out.append(v.c_str());
}

void append_num(String &out, int64_t v) {
    char b[32];
    ::snprintf(b, sizeof b, "%lld", (long long)v);
    out.append(b);
}

void append_state(String &out, const State &s) {
    append_num(out, s.present ? 1 : 0);
    out.append(" ");
    append_num(out, s.present ? s.kind : 0);
    out.append(" ");
    append_num(out, s.present ? (int64_t)s.mode : 0);
    out.append(" ");
    append_num(out, s.present ? (int64_t)s.size : 0);
    out.append(" ");
    append_str_or_dash(out, s.present ? s.hash : String());
}

int manifest_change_cmp(const void *a, const void *b) {
    return ::strcmp(((const ManifestChange *)a)->path.c_str(),
                    ((const ManifestChange *)b)->path.c_str());
}

void manifest_emit(const ManifestInput &m, String &out) {
    out.assign("worldfs-revision 1\n");
    out.append("world_name "); append_bytes_or_dash(out, m.world_name); out.append("\n");
    out.append("origin "); append_num(out, m.origin); out.append("\n");
    out.append("coverage "); append_num(out, m.coverage); out.append("\n");
    out.append("actor "); append_bytes_or_dash(out, m.actor); out.append("\n");
    out.append("turn "); append_bytes_or_dash(out, m.turn); out.append("\n");
    out.append("tool_call "); append_bytes_or_dash(out, m.tool_call); out.append("\n");
    out.append("git_head "); append_str_or_dash(out, m.git_head); out.append("\n");
    out.append("capture_started "); append_num(out, m.capture_started); out.append("\n");
    out.append("capture_finished "); append_num(out, m.capture_finished); out.append("\n");
    out.append("created "); append_num(out, m.created); out.append("\n");
    out.append("parent "); append_str_or_dash(out, m.parent_hash); out.append("\n");
    out.append("base_store "); append_str_or_dash(out, m.base_store); out.append("\n");
    out.append("base_snapshot "); append_num(out, m.base_snapshot); out.append("\n");
    out.append("base_created "); append_num(out, m.base_created); out.append("\n");
    // Changes are sorted by path so the manifest -- and therefore the revision's id -- does not
    // depend on capture order.
    Vec<ManifestChange> cs;
    for (size_t i = 0; i < m.changes.size(); ++i) cs.emplace_back(m.changes[i]);
    if (cs.size() > 1) qsort(cs.data(), cs.size(), sizeof(ManifestChange), manifest_change_cmp);
    for (size_t i = 0; i < cs.size(); ++i) {
        const ManifestChange &c = cs[i];
        char code[2] = { (char)c.change, 0 };
        out.append("change ");
        out.append(code);
        out.append(" ");
        append_state(out, c.after);
        out.append(" ");
        append_state(out, c.before);
        out.append(" ");
        append_state(out, c.base);
        out.append(" ");
        append_bytes_or_dash(out, c.path);
        out.append("\n");
    }
}

// Two hex digits per byte, or -1.
long hex_decode(const char *p, size_t n, uint8_t *out, size_t cap) {
    if (n % 2) return -1;
    size_t bytes = n / 2;
    if (bytes > cap) return -1;
    for (size_t i = 0; i < bytes; ++i) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; ++k) {
            char ch = p[i * 2 + k];
            int v = (ch >= '0' && ch <= '9') ? ch - '0'
                    : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                    : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
            if (v < 0) return -1;
            if (k == 0) hi = v; else lo = v;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (long)bytes;
}

// A parsed line's fields, as NUL-terminated tokens in place. Simpler than it sounds because no
// value can contain a space.
bool parse_state(const char **tok, int nt, int at, State &s) {
    if (at + 5 > nt) return false;
    long long v[4];
    for (int k = 0; k < 4; ++k) {
        char *end = nullptr;
        v[k] = ::strtoll(tok[at + k], &end, 10);
        if (!end || *end) return false;
    }
    s.present = v[0] != 0;
    s.kind = (int)v[1];
    s.mode = (uint32_t)v[2];
    s.size = (uint64_t)v[3];
    // State::hash is already a lowercase-hex content address (never raw bytes), so the token is
    // taken as is. A present entry without one is a directory or a special file (and an
    // over-budget file); a present regular file or symlink always has one.
    if (!::strcmp(tok[at + 4], "-")) s.hash.clear();
    else s.hash.assign(tok[at + 4]);
    return true;
}

int manifest_parse(const char *buf, size_t len, ManifestInput &m, String &err) {
    // Work on a NUL-terminated copy so tokens can be terminated in place.
    String text(buf, len);
    size_t pos = 0;
    int lineno = 0;
    auto next_line = [&](const char *&s, size_t &n) -> bool {
        if (pos >= text.size()) return false;
        size_t e = pos;
        while (e < text.size() && text.data()[e] != '\n') ++e;
        s = text.data() + pos;
        n = e - pos;
        pos = e + 1;
        ++lineno;
        return true;
    };
    const char *line;
    size_t n;
    if (!next_line(line, n) || n != ::strlen("worldfs-revision 1") ||
        ::strncmp(line, "worldfs-revision 1", n) != 0) {
        err.assign("not a worldfs-revision 1 manifest");
        return -EINVAL;
    }
    while (next_line(line, n)) {
        if (n == 0) continue;
        char tmp[8192];
        if (n >= sizeof tmp) { err.assign("manifest line too long"); return -EINVAL; }
        ::memcpy(tmp, line, n);
        tmp[n] = 0;
        // Tokenize.
        const char *tok[24];
        int nt = 0;
        for (char *p = tmp; *p && nt < 24;) {
            while (*p == ' ') ++p;
            if (!*p) break;
            tok[nt++] = p;
            while (*p && *p != ' ') ++p;
            if (*p) *p++ = 0;
        }
        if (nt == 0) continue;
        const char *key = tok[0];
        auto hexval = [&](int at, String &out) -> bool {
            if (at >= nt) return false;
            if (!::strcmp(tok[at], "-")) { out.clear(); return true; }
            uint8_t raw[WFS_PATH_MAX];
            long k = hex_decode(tok[at], ::strlen(tok[at]), raw, sizeof raw);
            if (k < 0) return false;
            out.assign((const char *)raw, (size_t)k);
            return true;
        };
        auto strval = [&](int at, String &out) -> bool {
            if (at >= nt) return false;
            if (!::strcmp(tok[at], "-")) out.clear();
            else out.assign(tok[at]);
            return true;
        };
        auto intval = [&](int at, int64_t &out) -> bool {
            if (at >= nt) return false;
            char *end = nullptr;
            long long v = ::strtoll(tok[at], &end, 10);
            if (!end || *end) return false;
            out = (int64_t)v;
            return true;
        };
        if (!::strcmp(key, "world_name")) { if (!hexval(1, m.world_name)) return -EINVAL; }
        else if (!::strcmp(key, "origin")) { int64_t v; if (!intval(1, v)) return -EINVAL; m.origin = (int)v; }
        else if (!::strcmp(key, "coverage")) { int64_t v; if (!intval(1, v)) return -EINVAL; m.coverage = (int)v; }
        else if (!::strcmp(key, "actor")) { if (!hexval(1, m.actor)) return -EINVAL; }
        else if (!::strcmp(key, "turn")) { if (!hexval(1, m.turn)) return -EINVAL; }
        else if (!::strcmp(key, "tool_call")) { if (!hexval(1, m.tool_call)) return -EINVAL; }
        else if (!::strcmp(key, "git_head")) { if (!strval(1, m.git_head)) return -EINVAL; }
        else if (!::strcmp(key, "capture_started")) { if (!intval(1, m.capture_started)) return -EINVAL; }
        else if (!::strcmp(key, "capture_finished")) { if (!intval(1, m.capture_finished)) return -EINVAL; }
        else if (!::strcmp(key, "created")) { if (!intval(1, m.created)) return -EINVAL; }
        else if (!::strcmp(key, "parent")) { if (!strval(1, m.parent_hash)) return -EINVAL; }
        else if (!::strcmp(key, "base_store")) { if (!strval(1, m.base_store)) return -EINVAL; }
        else if (!::strcmp(key, "base_snapshot")) { if (!intval(1, m.base_snapshot)) return -EINVAL; }
        else if (!::strcmp(key, "base_created")) { if (!intval(1, m.base_created)) return -EINVAL; }
        else if (!::strcmp(key, "change")) {
            if (nt != 2 + 15 + 1) { err.assign("malformed change line"); return -EINVAL; }
            ManifestChange c;
            c.change = (unsigned char)tok[1][0];
            if (!parse_state(tok, nt, 2, c.after)) { err.assign("bad change after state"); return -EINVAL; }
            if (!parse_state(tok, nt, 7, c.before)) { err.assign("bad change before state"); return -EINVAL; }
            if (!parse_state(tok, nt, 12, c.base)) { err.assign("bad change base state"); return -EINVAL; }
            if (!hexval(17, c.path)) { err.assign("bad change path"); return -EINVAL; }
            m.changes.emplace_back(c);
        } else {
            err.assign("unknown key");
            err.append(" ");
            err.append(key);
            return -EINVAL;
        }
    }
    // A change path is a relative path and never empty.
    for (size_t i = 0; i < m.changes.size(); ++i)
        if (m.changes[i].path.size() == 0) { err.assign("empty change path"); return -EINVAL; }
    return 0;
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
    String path;
    State before, after, base;
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

    // Capture reads each path relative to an open World root (and the snapshot root), never by
    // absolute path, so a symlinked ancestor cannot redirect a read outside the tree.
    Fd wfd;
    wfd.fd = ::open(wroot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (wfd.fd < 0) return -errno;

    {
        // The baseline is read inside the snapshot's gate, exactly as the diff reads it. The gate
        // window covers the whole capture; a fork from this snapshot waits behind it, which is
        // the same price the diff already pays.
        wfs::SnapshotReadGuard gate(sr.path);
        if (wfs::fs_gone(gate.rc)) return WFS_E_SOURCE_GONE;
        if (gate.rc) return gate.rc;
        Fd sfd;
        sfd.fd = ::open(sr.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sfd.fd < 0) return wfs::fs_gone(-errno) ? WFS_E_SOURCE_GONE : -errno;

        for (size_t i = 0; i < col.paths.size(); ++i) {
            const char *rel = col.paths[i].c_str();
            if (path_excluded(rel)) continue;

            State cur;
            if (int e = capture_state(s, wfd.fd, rel, cur)) return e;

            State last, base;
            bool has_row = false;
            if (int e = history_prev(s, world, rel, last, base, has_row)) return e;
            if (!has_row) {
                // No recorded state yet, so the previous state is the baseline snapshot's.
                if (int e = capture_state(s, sfd.fd, rel, base)) return e;
                last = base;
            }
            // A revision that touches a regular file whose content is unknown on either side is
            // incomplete, whatever the diff happened to say about it.
            if (cur.incoherent || base.incoherent || hashless_file(cur) || hashless_file(last) ||
                hashless_file(base))
                incoherent = true;

            int change = 0;
            if (!differs(cur, last, change)) continue;
            Out o;
            o.change = change;
            o.path = String(rel);
            o.before = last;
            o.after = cur;
            o.base = base;
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
                   "SELECT file_id, path, last_hash, last_kind, last_mode, last_size, last_mtime,"
                   " base_hash, base_kind, base_mode, base_size, base_mtime"
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
                last.mtime = q.col_i64(6);
                last.present = last.kind != 0;
                State base;
                base.hash.assign(q.col_text(7));
                base.kind = (int)q.col_i64(8);
                base.mode = (uint32_t)q.col_i64(9);
                base.size = (uint64_t)q.col_i64(10);
                base.mtime = q.col_i64(11);
                base.present = base.kind != 0;
                // The baseline's content being unavailable is a property that survives: a path
                // reconciled against it stays incomplete rather than being relabeled OBSERVED.
                if (hashless_file(base) || hashless_file(last)) incoherent = true;
                int change = 0;
                if (!differs(base, last, change)) continue;
                Out o;
                o.change = change;
                o.path = String(rel);
                o.file_id = (wfs_id)q.col_i64(0);
                o.before = last;
                o.after = base;   // a path not in the diff currently equals the baseline
                o.base = base;
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

    // The parent revision's identity, read under the marker lock we already hold, so it cannot
    // change between here and the commit.
    wfs_id parent = 0;
    String parent_hash;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT id, manifest_hash FROM revisions WHERE world_id=? ORDER BY id DESC LIMIT 1");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)world);
        if (q.row()) {
            parent = (wfs_id)q.col_i64(0);
            parent_hash.assign(q.col_text(1));
        } else if (!q.done()) {
            return -EIO;
        }
    }

    // The manifest: the store-independent description of this revision, addressed by its own
    // SHA-256. It is written as a content object before the transaction that names it, so a
    // published revision never points at a missing manifest.
    ManifestInput m;
    m.world_name.assign(wr.name);
    m.origin = WFS_RV_ORIGIN_FILESYSTEM;
    m.coverage = incoherent ? WFS_RV_COVERAGE_INCOMPLETE : WFS_RV_COVERAGE_OBSERVED;
    m.actor.assign(actor_id ? actor_id : "");
    m.turn.assign(turn_id ? turn_id : "");
    m.tool_call.assign(tool_call_id ? tool_call_id : "");
    m.git_head.assign(git_head);
    m.capture_started = t0;
    m.capture_finished = t1;
    m.created = t1;
    m.parent_hash = parent_hash;
    m.base_store.assign(s->store_id.c_str());
    m.base_snapshot = (int64_t)wr.snapshot_id;
    m.base_created = sr.created_at;
    for (size_t i = 0; i < recs.size(); ++i) {
        ManifestChange mc;
        mc.path = recs[i].path;
        mc.change = recs[i].change;
        mc.before = recs[i].before;
        mc.after = recs[i].after;
        mc.base = recs[i].base;
        m.changes.emplace_back(mc);
    }
    String manifest;
    manifest_emit(m, manifest);
    char manifest_hash[WFS_REVISION_HASH_MAX];
    sha256_hex(manifest.c_str(), manifest.size(), manifest_hash);
    {
        // The object's own hash is the manifest's hash; a mismatch would be a bug in the writer.
        char obj_hash[WFS_REVISION_HASH_MAX];
        if (int prc = content_put_mem(s, manifest.c_str(), manifest.size(), obj_hash)) return prc;
        if (::strcmp(obj_hash, manifest_hash) != 0) return -EIO;
    }

    Guard g(s->mu);
    wfs::Txn t(s->db);
    if (!t.ok()) return t.err();
    {
        Stmt ins(s->db,
                 "INSERT INTO revisions(world_id, parent_revision, baseline_snapshot, origin,"
                 " coverage, actor_id, turn_id, tool_call_id, git_head, capture_started_at,"
                 " capture_finished_at, created_at, changes, added, modified, deleted, meta,"
                 " manifest_hash, parent_hash)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
        ins.text(18, manifest_hash);
        ins.text(19, parent_hash.c_str());
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
        int kind = o.after.present ? o.after.kind : o.before.kind;
        uint32_t mode = o.after.present ? o.after.mode : o.before.mode;
        uint64_t size = o.after.present ? o.after.size : o.before.size;
        {
            Stmt c(s->db,
                   "INSERT INTO changes(revision_id, seq, file_id, change, kind, old_path,"
                   " new_path, before_hash, after_hash, size, mode) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            if (!c.ok()) return -EIO;
            c.i64(1, (int64_t)rev);
            c.i64(2, seq++);
            c.i64(3, (int64_t)fid);
            c.i64(4, o.change);
            c.i64(5, kind);
            c.text(6, del ? o.path.c_str() : "");
            c.text(7, del ? "" : o.path.c_str());
            c.text(8, o.before.present ? o.before.hash.c_str() : "");
            c.text(9, o.after.present ? o.after.hash.c_str() : "");
            c.i64(10, (int64_t)size);
            c.i64(11, (int64_t)mode);
            if (c.step() != SQLITE_DONE) return -EIO;
        }
        // The state the next record compares against is the after state; a deletion is recorded
        // absent (kind 0, empty hash).
        {
            Stmt u(s->db,
                   "UPDATE history_files SET last_hash=?, last_kind=?, last_mode=?, last_size=?,"
                   " last_mtime=?, last_revision=? WHERE file_id=?");
            if (!u.ok()) return -EIO;
            u.text(1, o.after.present ? o.after.hash.c_str() : "");
            u.i64(2, o.after.present ? o.after.kind : 0);
            u.i64(3, o.after.present ? (int64_t)o.after.mode : 0);
            u.i64(4, o.after.present ? (int64_t)o.after.size : 0);
            u.i64(5, o.after.present ? o.after.mtime : 0);
            u.i64(6, (int64_t)rev);
            u.i64(7, (int64_t)fid);
            if (u.step() != SQLITE_DONE) return -EIO;
        }
        if (o.before.present && o.before.hash.size() &&
            content_ref(s, o.before.hash.c_str(), o.before.size, t1))
            return -EIO;
        if (o.after.present && o.after.hash.size() &&
            content_ref(s, o.after.hash.c_str(), o.after.size, t1))
            return -EIO;
    }
    // The manifest object itself is referenced, and the baseline is pinned so discarding the
    // World cannot orphan this history.
    if (content_ref(s, manifest_hash, manifest.size(), t1)) return -EIO;
    {
        Stmt p(s->db,
               "INSERT OR IGNORE INTO history_pins(snapshot_id, revision_id, kind, created_at)"
               " VALUES(?,?,1,?)");
        if (!p.ok()) return -EIO;
        p.i64(1, (int64_t)wr.snapshot_id);
        p.i64(2, (int64_t)rev);
        p.i64(3, t1);
        if (p.step() != SQLITE_DONE) return -EIO;
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

// ---- export / import -------------------------------------------------------------------------

namespace {
thread_local String g_history_reason;
const char *set_history_reason(const char *why) {
    g_history_reason.assign(why ? why : "");
    return why;
}
} // namespace

extern "C" const char *wfs_history_reason(void) { return g_history_reason.c_str(); }

extern "C" int wfs_revision_export(wfs_store *s, wfs_id revision, void **buf, size_t *len,
                                   char out_hash[WFS_REVISION_HASH_MAX]) {
    if (!s || !revision || !buf || !len || !out_hash) return -EINVAL;
    *buf = nullptr;
    *len = 0;
    out_hash[0] = 0;
    char hash[WFS_REVISION_HASH_MAX] = {0};
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT manifest_hash FROM revisions WHERE id=?");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)revision);
        if (!q.row()) return q.done() ? -ENOENT : -EIO;
        wfs::copy_str(hash, sizeof hash, q.col_text(0));
    }
    if (!hash[0]) {
        set_history_reason("this revision was recorded before manifests existed");
        return WFS_E_HISTORY_MANIFEST;
    }
    void *mem = nullptr;
    size_t n = 0;
    int rc = wfs_history_content(s, hash, &mem, &n);
    if (rc) return rc;
    // The object is addressed by its own hash; verify rather than trust the row.
    char check[WFS_REVISION_HASH_MAX];
    sha256_hex(mem, n, check);
    if (::strcmp(check, hash) != 0) {
        ::free(mem);
        set_history_reason("the stored manifest does not match its hash");
        return WFS_E_HISTORY_MANIFEST;
    }
    wfs::copy_str(out_hash, WFS_REVISION_HASH_MAX, hash);
    *buf = mem;
    *len = n;
    return 0;
}

extern "C" int wfs_revision_import(wfs_store *s, wfs_id world, const void *manifest, size_t len,
                                   int flags, wfs_id *out_revision,
                                   char out_hash[WFS_REVISION_HASH_MAX]) {
    if (!s || !world || !manifest || !len || !out_revision || !out_hash) return -EINVAL;
    *out_revision = 0;
    out_hash[0] = 0;

    ManifestInput m;
    String err;
    if (manifest_parse((const char *)manifest, len, m, err) != 0) {
        set_history_reason(err.c_str());
        return WFS_E_HISTORY_MANIFEST;
    }
    // A manifest is canonical: re-emitting what we parsed must produce the same bytes, so the
    // hash is stable and a manifest that was reformatted or reordered is refused rather than
    // silently given a different id.
    String again;
    manifest_emit(m, again);
    if (again.size() != len || ::memcmp(again.c_str(), manifest, len) != 0) {
        set_history_reason("manifest is not in canonical form");
        return WFS_E_HISTORY_MANIFEST;
    }
    char hash[WFS_REVISION_HASH_MAX];
    sha256_hex(manifest, len, hash);
    wfs::copy_str(out_hash, WFS_REVISION_HASH_MAX, hash);

    wfs_world_rec wr;
    if (int rc = wfs_world_info(s, world, &wr)) return rc;
    if (wr.state != WFS_ST_ACTIVE) return -ESTALE;

    // Deduplication by hash: importing a manifest already here is a no-op.
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT id FROM revisions WHERE manifest_hash=?");
        if (!q.ok()) return -EIO;
        q.text(1, hash);
        if (q.row()) { *out_revision = (wfs_id)q.col_i64(0); return 0; }
        if (!q.done()) return -EIO;
    }

    // The baseline this revision was recorded against must be this World's baseline. Cross-store
    // import needs snapshot transfer and is not part of this slice.
    if (m.base_store.size() == 0 || m.base_store != s->store_id.c_str() ||
        m.base_snapshot != (int64_t)wr.snapshot_id) {
        set_history_reason("the manifest's baseline is not this World's baseline");
        return WFS_E_HISTORY_BASELINE_MISMATCH;
    }
    wfs_snapshot_rec sr;
    if (int rc = wfs_snapshot_info(s, wr.snapshot_id, &sr)) return rc;
    if (m.base_created != sr.created_at) {
        set_history_reason("the manifest's baseline creation time does not match");
        return WFS_E_HISTORY_BASELINE_MISMATCH;
    }

    // The chain is linear per World: the manifest's parent must be this World's latest revision,
    // and a parentless manifest is only accepted into a World with no revisions yet. This is what
    // keeps history_files a single per-path state rather than a branch set.
    wfs_id parent = 0;
    String latest_hash;
    {
        Guard g(s->mu);
        Stmt q(s->db, "SELECT id, manifest_hash FROM revisions WHERE world_id=? ORDER BY id DESC LIMIT 1");
        if (!q.ok()) return -EIO;
        q.i64(1, (int64_t)world);
        if (q.row()) { parent = (wfs_id)q.col_i64(0); latest_hash.assign(q.col_text(1)); }
        else if (!q.done()) return -EIO;
    }
    if (m.parent_hash.size() == 0) {
        if (parent) {
            set_history_reason("the import declares no parent but this World already has revisions");
            return WFS_E_HISTORY_PARENT_MISSING;
        }
    } else if (m.parent_hash != latest_hash.c_str()) {
        set_history_reason("the manifest's parent is not this World's latest revision");
        return WFS_E_HISTORY_PARENT_MISSING;
    }

    // Every content object the manifest references has to be here, or the revision would name
    // missing bytes the moment it is published.
    for (size_t i = 0; i < m.changes.size(); ++i) {
        const ManifestChange &c = m.changes[i];
        const String *hashes[2] = { nullptr, nullptr };
        if (c.before.present && c.before.hash.size()) hashes[0] = &c.before.hash;
        if (c.after.present && c.after.hash.size()) hashes[1] = &c.after.hash;
        for (const String *h : hashes) {
            if (!h) continue;
            Guard g(s->mu);
            Stmt q(s->db, "SELECT 1 FROM content WHERE hash=?");
            if (!q.ok()) return -EIO;
            q.text(1, h->c_str());
            if (!q.row()) {
                if (!q.done()) return -EIO;
                set_history_reason(h->c_str());
                return WFS_E_HISTORY_CONTENT_MISSING;
            }
            String p;
            content_path(s, h->c_str(), p);
            if (wfs::fs_probe(p.c_str()) != 0) {
                set_history_reason(h->c_str());
                return WFS_E_HISTORY_CONTENT_MISSING;
            }
        }
    }

    if (flags & WFS_HISTORY_IMPORT_CHECK) return 0;

    // Store the manifest object before the transaction that names it.
    {
        char obj_hash[WFS_REVISION_HASH_MAX];
        if (int prc = content_put_mem(s, manifest, len, obj_hash)) return prc;
        if (::strcmp(obj_hash, hash) != 0) return -EIO;
    }

    int64_t t1 = wfs::now_sec();
    Guard g(s->mu);
    wfs::Txn t(s->db);
    if (!t.ok()) return t.err();
    {
        Stmt ins(s->db,
                 "INSERT INTO revisions(world_id, parent_revision, baseline_snapshot, origin,"
                 " coverage, actor_id, turn_id, tool_call_id, git_head, capture_started_at,"
                 " capture_finished_at, created_at, changes, added, modified, deleted, meta,"
                 " manifest_hash, parent_hash)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!ins.ok()) return -EIO;
        ins.i64(1, (int64_t)world);
        ins.i64(2, (int64_t)parent);
        ins.i64(3, (int64_t)wr.snapshot_id);
        ins.i64(4, m.origin);
        ins.i64(5, m.coverage);
        ins.text(6, m.actor.c_str());
        ins.text(7, m.turn.c_str());
        ins.text(8, m.tool_call.c_str());
        ins.text(9, m.git_head.c_str());
        ins.i64(10, m.capture_started);
        ins.i64(11, m.capture_finished);
        ins.i64(12, m.created);
        int64_t added = 0, modified = 0, deleted = 0, meta = 0;
        for (size_t i = 0; i < m.changes.size(); ++i) {
            switch (m.changes[i].change) {
            case WFS_C_ADDED: added++; break;
            case WFS_C_MODIFIED: modified++; break;
            case WFS_C_DELETED: deleted++; break;
            default: meta++; break;
            }
        }
        ins.i64(13, (int64_t)m.changes.size());
        ins.i64(14, added);
        ins.i64(15, modified);
        ins.i64(16, deleted);
        ins.i64(17, meta);
        ins.text(18, hash);
        ins.text(19, m.parent_hash.c_str());
        if (ins.step() != SQLITE_DONE) return -EIO;
    }
    wfs_id rev = (wfs_id)::sqlite3_last_insert_rowid(s->db);

    int64_t seq = 0;
    for (size_t i = 0; i < m.changes.size(); ++i) {
        const ManifestChange &c = m.changes[i];
        wfs_id fid = 0;
        if (int e = file_id_get_or_create(s, world, c.path.c_str(), c.base, &fid)) return e;
        bool del = c.change == WFS_C_DELETED;
        {
            Stmt q(s->db,
                   "INSERT INTO changes(revision_id, seq, file_id, change, kind, old_path,"
                   " new_path, before_hash, after_hash, size, mode) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            if (!q.ok()) return -EIO;
            q.i64(1, (int64_t)rev);
            q.i64(2, seq++);
            q.i64(3, (int64_t)fid);
            q.i64(4, c.change);
            q.i64(5, c.after.present ? c.after.kind : c.before.kind);
            q.text(6, del ? c.path.c_str() : "");
            q.text(7, del ? "" : c.path.c_str());
            q.text(8, c.before.present ? c.before.hash.c_str() : "");
            q.text(9, c.after.present ? c.after.hash.c_str() : "");
            q.i64(10, (int64_t)(c.after.present ? c.after.size : c.before.size));
            q.i64(11, (int64_t)(c.after.present ? c.after.mode : c.before.mode));
            if (q.step() != SQLITE_DONE) return -EIO;
        }
        {
            Stmt u(s->db,
                   "UPDATE history_files SET last_hash=?, last_kind=?, last_mode=?, last_size=?,"
                   " last_mtime=?, last_revision=? WHERE file_id=?");
            if (!u.ok()) return -EIO;
            u.text(1, c.after.present ? c.after.hash.c_str() : "");
            u.i64(2, c.after.present ? c.after.kind : 0);
            u.i64(3, c.after.present ? (int64_t)c.after.mode : 0);
            u.i64(4, c.after.present ? (int64_t)c.after.size : 0);
            u.i64(5, c.after.present ? c.after.mtime : 0);   // not transferred: 0 after an import
            u.i64(6, (int64_t)rev);
            u.i64(7, (int64_t)fid);
            if (u.step() != SQLITE_DONE) return -EIO;
        }
        if (c.before.present && c.before.hash.size() &&
            content_ref(s, c.before.hash.c_str(), c.before.size, t1))
            return -EIO;
        if (c.after.present && c.after.hash.size() &&
            content_ref(s, c.after.hash.c_str(), c.after.size, t1))
            return -EIO;
    }
    if (content_ref(s, hash, len, t1)) return -EIO;
    {
        Stmt p(s->db,
               "INSERT OR IGNORE INTO history_pins(snapshot_id, revision_id, kind, created_at)"
               " VALUES(?,?,1,?)");
        if (!p.ok()) return -EIO;
        p.i64(1, (int64_t)wr.snapshot_id);
        p.i64(2, (int64_t)rev);
        p.i64(3, t1);
        if (p.step() != SQLITE_DONE) return -EIO;
    }
    if (int crc = t.commit()) return crc;
    *out_revision = rev;
    return 0;
}
