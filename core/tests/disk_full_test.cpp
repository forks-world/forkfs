// P11 regression: exercise a real ENOSPC while a snapshot is being published, then prove that
// the failed operation did not damage the old snapshot/world or the SQLite store.  This binary is
// intentionally not a CTest: scripts/tests/disk_full.py is the only supported entry point and
// supplies a bounded, private APFS disk image.
#include "worldfs/worldfs.h"

#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #x, strerror(errno)); exit(1); } } while (0)
#define CHECK_OK(x) do { int _rc = (x); if (_rc != 0) { fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc)); exit(1); } } while (0)

// This seam is deliberately private to the test binary.  It is defined by world.cpp and is
// called after P11's preflight, after the CREATING row and temporary directory are committed.
extern "C" void (*wfs_test_before_snapshot_clone)(void *ctx, const char *src_dir);
extern "C" void *wfs_test_before_snapshot_clone_ctx;

static const uint64_t kMiB = 1024ull * 1024;
static const uint64_t kMinImage = 300ull * kMiB;
static const uint64_t kMaxImage = 1024ull * kMiB;

struct FillContext {
    char volume[WFS_PATH_MAX];
    char filler[WFS_PATH_MAX];
    uint64_t written;
    int saw_enospc;
    int saw_other_error;
    char metadata[WFS_PATH_MAX];
    unsigned metadata_dirs;
    int metadata_full;
};

static void join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    CHECK(n > 0 && (size_t)n < cap);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    const char *p = (const char *)data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, p, left > 1024 * 1024 ? 1024 * 1024 : left);
        CHECK(n > 0);
        p += n;
        left -= (size_t)n;
    }
    CHECK(close(fd) == 0);
}

static void mkdir_checked(const char *path) {
    CHECK(mkdir(path, 0755) == 0);
}

static uint64_t free_bytes(const char *path) {
    struct statfs fs;
    CHECK(statfs(path, &fs) == 0);
    return (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
}

static uint64_t total_bytes(const char *path) {
    struct statfs fs;
    CHECK(statfs(path, &fs) == 0);
    return (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
}

// Write in 1 MiB chunks for speed, then 4 KiB chunks near the end.  The hard byte bound is based
// on the image capacity, not the host volume, so a malformed mount can never fill the runner.
static void fill_until_enospc(void *opaque, const char *) {
    FillContext *ctx = (FillContext *)opaque;
    CHECK(snprintf(ctx->metadata, sizeof ctx->metadata, "%s.metadata", ctx->filler) > 0);
    mkdir_checked(ctx->metadata);
    int fd = open(ctx->filler, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    uint64_t limit = total_bytes(ctx->volume) + 16 * kMiB;
    unsigned char block[1024 * 1024];
    memset(block, 0xa5, sizeof block);
    while (ctx->written < limit) {
        size_t want = sizeof block;
        if (limit - ctx->written < want) want = (size_t)(limit - ctx->written);
        if (free_bytes(ctx->volume) < 8 * 1024 * 1024) want = 4096;
        ssize_t n = write(fd, block, want);
        if (n > 0) {
            ctx->written += (uint64_t)n;
            continue;
        }
        if (n < 0 && errno == ENOSPC) {
            ctx->saw_enospc = 1;
            break;
        }
        ctx->saw_other_error = errno ? errno : EIO;
        break;
    }
    // fsync is part of the observation: buffered success is not sufficient evidence that APFS
    // really allocated the blocks that made the subsequent clone fail.
    if (fsync(fd) != 0) {
        if (errno == ENOSPC) ctx->saw_enospc = 1;
        else ctx->saw_other_error = errno;
    }
    CHECK(close(fd) == 0);
    // APFS can reserve metadata blocks after data writes report ENOSPC. Exhaust that
    // allocation path too, so a tiny clone cannot legitimately succeed using the reserve.
    for (; ctx->metadata_dirs < 65536; ++ctx->metadata_dirs) {
        char path[WFS_PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%u", ctx->metadata, ctx->metadata_dirs);
        CHECK(n > 0 && (size_t)n < sizeof path);
        if (mkdir(path, 0700) == 0) continue;
        if (errno == ENOSPC) ctx->metadata_full = 1;
        else ctx->saw_other_error = errno;
        break;
    }
    CHECK(ctx->metadata_full);
    printf("observed ENOSPC: data=%llu bytes, metadata=%u directories\n",
           (unsigned long long)ctx->written, ctx->metadata_dirs);
}

// Leave a useful margin rather than racing the filesystem's reserved blocks.  P11 needs 256 MiB
// plus one KiB per entry, so <=96 MiB is unambiguously a low-space result for this fixture.
static void fill_to_low_space(FillContext *ctx, uint64_t target_free) {
    int fd = open(ctx->filler, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    uint64_t limit = total_bytes(ctx->volume) + 16 * kMiB;
    unsigned char block[1024 * 1024];
    memset(block, 0x5a, sizeof block);
    while (free_bytes(ctx->volume) > target_free && ctx->written < limit) {
        uint64_t before = free_bytes(ctx->volume);
        size_t want = sizeof block;
        uint64_t gap = before - target_free;
        if (gap < want) want = (size_t)gap;
        if (!want) break;
        ssize_t n = write(fd, block, want);
        CHECK(n > 0);
        ctx->written += (uint64_t)n;
    }
    CHECK(ctx->written < limit);
    CHECK(fsync(fd) == 0);
    CHECK(close(fd) == 0);
    uint64_t now = free_bytes(ctx->volume);
    CHECK(now > 16 * kMiB && now <= target_free + 8 * kMiB);
}

static void remove_filler(FillContext *ctx) {
    CHECK(unlink(ctx->filler) == 0);
    for (unsigned i = 0; i < ctx->metadata_dirs; ++i) {
        char path[WFS_PATH_MAX];
        CHECK(snprintf(path, sizeof path, "%s/%u", ctx->metadata, i) > 0);
        CHECK(rmdir(path) == 0);
    }
    if (ctx->metadata[0]) CHECK(rmdir(ctx->metadata) == 0);
    ctx->written = 0;
}

static void check_file(const char *path, const unsigned char *want, size_t size) {
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    unsigned char got[4096];
    size_t off = 0;
    while (off < size) {
        size_t ask = size - off > sizeof got ? sizeof got : size - off;
        ssize_t n = read(fd, got, ask);
        CHECK(n > 0 && (size_t)n == ask);
        CHECK(memcmp(got, want + off, ask) == 0);
        off += ask;
    }
    CHECK(read(fd, got, 1) == 0);
    CHECK(close(fd) == 0);
}

static unsigned char *read_all(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size > 0);
    unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
    CHECK(buf != NULL);
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + off, (size_t)st.st_size - off);
        CHECK(n > 0);
        off += (size_t)n;
    }
    CHECK(close(fd) == 0);
    *size = off;
    return buf;
}

// What SQLite itself says when this store's database is opened on a volume with nothing left.
// Printed rather than asserted: the extended code depends on which allocation the pager needs
// first, and it is the *verdict* the core draws from it that this test pins down.
static void report_open_codes(const char *db) {
    sqlite3 *h = NULL;
    int rc = sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL);
    if (rc == SQLITE_OK && h) {
        sqlite3_stmt *q = NULL;
        rc = sqlite3_prepare_v2(h, "PRAGMA user_version", -1, &q, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_step(q);
        if (q) sqlite3_finalize(q);
    }
    int ext = h ? sqlite3_extended_errcode(h) : rc;
    int se = h ? sqlite3_system_errno(h) : 0;
    struct statfs fs;
    CHECK(statfs(db, &fs) == 0);
    printf("full volume, SQLite on %s: rc=%d extended=%d system_errno=%d (%s)\n"
           "  statfs: bavail=%llu bytes of %llu capacity (bsize=%u)\n",
           db, rc, ext, se, se ? strerror(se) : "-",
           (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_bsize,
           (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_bsize,
           (unsigned)fs.f_bsize);
    if (h) sqlite3_close(h);
}

// The CLI's half of the same rule. A full volume is an environment error: say that the volume is
// full and that space has to be freed, exit 1, and send nobody to `mv <store> <store>.damaged`.
static void check_cli_message(const char *world_bin, const char *store) {
    char cmd[WFS_PATH_MAX * 2 + 64];
    CHECK(snprintf(cmd, sizeof cmd, "WORLD_STORE='%s' '%s' fs status 2>&1", store, world_bin) > 0);
    FILE *p = popen(cmd, "r");
    CHECK(p != NULL);
    char out[8192];
    size_t got = fread(out, 1, sizeof out - 1, p);
    out[got] = 0;
    int status = pclose(p);
    printf("--- `world fs status` with the store's volume full ---\n%s"
           "-----------------------------------------------------\n", out);
    CHECK(WIFEXITED(status));
    // 3 means "refused by a safety rule"; a volume that filled up is not a refusal, it is the
    // environment, which is 1 everywhere else in this CLI.
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(strstr(out, "move the directory aside") == NULL);
    CHECK(strstr(out, "mv ") == NULL);
    CHECK(strstr(out, "restore metadata3.db") == NULL);
    CHECK(strstr(out, "free space") != NULL);
}

static void check_integrity(const char *store) {
    char db[WFS_PATH_MAX];
    join(db, sizeof db, store, "metadata3.db");
    sqlite3 *h = NULL;
    CHECK(sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    CHECK(sqlite3_prepare_v2(h, "PRAGMA integrity_check", -1, &q, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(q) == SQLITE_ROW);
    const unsigned char *answer = sqlite3_column_text(q, 0);
    CHECK(answer && strcmp((const char *)answer, "ok") == 0);
    CHECK(sqlite3_finalize(q) == SQLITE_OK);
    CHECK(sqlite3_close(h) == SQLITE_OK);
}

static void validate_volume(const char *volume) {
    struct statfs fs;
    CHECK(statfs(volume, &fs) == 0);
    CHECK(strcmp(fs.f_fstypename, "apfs") == 0);
    CHECK(total_bytes(volume) >= kMinImage && total_bytes(volume) <= kMaxImage);
    struct stat mounted, parent;
    CHECK(stat(volume, &mounted) == 0);
    char parent_path[WFS_PATH_MAX];
    snprintf(parent_path, sizeof parent_path, "%s/..", volume);
    CHECK(stat(parent_path, &parent) == 0);
    CHECK(mounted.st_dev != parent.st_dev);
    CHECK(fs.f_mntonname[0] != 0 && fs.f_mntfromname[0] != 0);
}

int main(int argc, char **argv) {
    // argv[2] is the `world` CLI from the same build: the full-volume phase at the end checks
    // what it PRINTS, which is the half of this bug a library call cannot see.
    CHECK(argc == 3);
    char volume[WFS_PATH_MAX], world_bin[WFS_PATH_MAX];
    CHECK(realpath(argv[1], volume) != NULL);
    CHECK(realpath(argv[2], world_bin) != NULL);
    CHECK(access(world_bin, X_OK) == 0);
    validate_volume(volume);

    char source[WFS_PATH_MAX], store_dir[WFS_PATH_MAX], worlds[WFS_PATH_MAX];
    char world1[WFS_PATH_MAX], world2[WFS_PATH_MAX], world3[WFS_PATH_MAX], world4[WFS_PATH_MAX];
    char low_source[WFS_PATH_MAX];
    join(source, sizeof source, volume, "source");
    join(store_dir, sizeof store_dir, volume, "store");
    join(worlds, sizeof worlds, volume, "worlds");
    join(world1, sizeof world1, worlds, "world1");
    join(world2, sizeof world2, worlds, "world2");
    join(world3, sizeof world3, worlds, "world3");
    join(world4, sizeof world4, worlds, "world4");
    join(low_source, sizeof low_source, volume, "low-source");
    mkdir_checked(source);
    mkdir_checked(worlds);
    mkdir_checked(low_source);

    unsigned char payload[96 * 1024];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (unsigned char)((i * 37u) ^ (i >> 8));
    char payload_path[WFS_PATH_MAX], low_file[WFS_PATH_MAX];
    join(payload_path, sizeof payload_path, source, "payload.bin");
    join(low_file, sizeof low_file, low_source, "one.txt");
    write_bytes(payload_path, payload, sizeof payload);
    write_bytes(low_file, "low-space\n", 10);

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store_dir, &s));
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "base";
    wfs_id sid = 0;
    CHECK_OK(wfs_snapshot_create(s, source, &sopts, &sid));
    CHECK(sid != 0);

    wfs_ref from = {WFS_K_SNAPSHOT, sid};
    wfs_fork_opts fopts;
    memset(&fopts, 0, sizeof fopts);
    fopts.no_pool = 1;
    fopts.name = "survivor";
    wfs_id wid = 0;
    CHECK_OK(wfs_world_create(s, from, world1, &fopts, &wid));
    char world_payload[WFS_PATH_MAX];
    join(world_payload, sizeof world_payload, world1, "payload.bin");
    check_file(world_payload, payload, sizeof payload);

    FillContext full;
    memset(&full, 0, sizeof full);
    snprintf(full.volume, sizeof full.volume, "%s", volume);
    snprintf(full.filler, sizeof full.filler, "%s/.forkfs-enospc-%ld", volume, (long)getpid());
    wfs_test_before_snapshot_clone = fill_until_enospc;
    wfs_test_before_snapshot_clone_ctx = &full;
    wfs_id failed = 0;
    int rc = wfs_snapshot_create(s, source, &sopts, &failed);
    wfs_test_before_snapshot_clone = NULL;
    wfs_test_before_snapshot_clone_ctx = NULL;
    CHECK(full.saw_enospc && !full.saw_other_error);
    CHECK(rc != 0 && failed == 0);
    remove_filler(&full);

    check_file(payload_path, payload, sizeof payload);
    check_file(world_payload, payload, sizeof payload);
    wfs_snapshot_rec sr;
    CHECK_OK(wfs_snapshot_info(s, sid, &sr));
    wfs_verify_report vr;
    memset(&vr, 0, sizeof vr);
    CHECK_OK(wfs_snapshot_verify(s, sid, &vr));
    CHECK(vr.missing == 0 && vr.modified == 0 && vr.extra == 0);
    wfs_snapshot_rec snapshots[8];
    size_t snapshot_count = 0;
    CHECK_OK(wfs_snapshot_list(s, snapshots, 8, &snapshot_count));
    CHECK(snapshot_count == 1);
    wfs_world_rec worlds_list[8];
    size_t world_count = 0;
    CHECK_OK(wfs_world_list(s, 0, worlds_list, 8, &world_count));
    CHECK(world_count == 1 && worlds_list[0].state == WFS_ST_ACTIVE);
    check_integrity(store_dir);

    // A close/reopen is part of the regression: a failed clone must not leave a WAL or a
    // half-published row that only the original sqlite connection happens to hide.
    wfs_store_close(s);
    s = NULL;
    CHECK_OK(wfs_store_open(store_dir, &s));
    CHECK_OK(wfs_gc(s, 0, NULL));
    CHECK_OK(wfs_snapshot_verify(s, sid, &vr));
    wfs_identity identity;
    memset(&identity, 0, sizeof identity);
    CHECK_OK(wfs_world_verify_identity(s, world1, &identity));
    CHECK(identity.registered && identity.world_id == wid);
    check_integrity(store_dir);

    // Retry from the original snapshot as well as from a checkpoint.  This catches a subtle
    // rollback that leaves S1's row present but its bytes or gate damaged.
    memset(&fopts, 0, sizeof fopts);
    fopts.no_pool = 1;
    fopts.name = "original-snapshot-retry";
    wfs_id retry_original = 0;
    CHECK_OK(wfs_world_create(s, from, world3, &fopts, &retry_original));
    char world3_payload[WFS_PATH_MAX];
    join(world3_payload, sizeof world3_payload, world3, "payload.bin");
    check_file(world3_payload, payload, sizeof payload);

    // Retry both publish paths after the full-volume failure: checkpoint creates a fresh
    // snapshot from the surviving world, and a normal fork consumes that snapshot.
    sopts.name = "checkpoint";
    wfs_id checkpoint = 0;
    CHECK_OK(wfs_snapshot_create(s, world1, &sopts, &checkpoint));
    memset(&fopts, 0, sizeof fopts);
    fopts.no_pool = 1;
    fopts.name = "retry";
    wfs_id retry_world = 0;
    CHECK_OK(wfs_world_create(s, (wfs_ref){WFS_K_SNAPSHOT, checkpoint}, world2, &fopts, &retry_world));
    char world2_payload[WFS_PATH_MAX];
    join(world2_payload, sizeof world2_payload, world2, "payload.bin");
    check_file(world2_payload, payload, sizeof payload);

    // Leave roughly 64 MiB available and ensure P11 refuses before a second snapshot row is
    // published.  This is separate from the hook: P11 runs before that hook by design.
    FillContext low;
    memset(&low, 0, sizeof low);
    snprintf(low.volume, sizeof low.volume, "%s", volume);
    snprintf(low.filler, sizeof low.filler, "%s/.forkfs-low-%ld", volume, (long)getpid());
    fill_to_low_space(&low, 80 * kMiB);
    wfs_id low_id = 0;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "must-refuse-low-space";
    int low_rc = wfs_snapshot_create(s, low_source, &sopts, &low_id);
    wfs_id low_world_id = 0;
    memset(&fopts, 0, sizeof fopts);
    fopts.no_pool = 1;
    fopts.name = "must-refuse-low-space-fork";
    int low_fork_rc = wfs_world_create(s, from, world4, &fopts, &low_world_id);
    remove_filler(&low);
    CHECK(low_rc == WFS_E_LOW_SPACE && low_id == 0);
    CHECK(low_fork_rc == WFS_E_LOW_SPACE && low_world_id == 0);

    wfs_snapshot_rec final_snapshots[8];
    size_t final_snapshot_count = 0;
    CHECK_OK(wfs_snapshot_list(s, final_snapshots, 8, &final_snapshot_count));
    CHECK(final_snapshot_count == 2);
    size_t final_world_count = 0;
    CHECK_OK(wfs_world_list(s, 0, worlds_list, 8, &final_world_count));
    CHECK(final_world_count == 3);
    CHECK_OK(wfs_snapshot_verify(s, sid, &vr));
    check_file(world_payload, payload, sizeof payload);
    check_file(world2_payload, payload, sizeof payload);
    check_file(world3_payload, payload, sizeof payload);
    check_integrity(store_dir);
    wfs_store_close(s);
    s = NULL;

    // ---- P17: a database we could not open is not a database that is damaged ----------------
    //
    // Measured on 2026-09-21 (macOS 27 arm64, APFS): with the volume that holds the store full,
    // EVERY `world fs` command -- read-only `status` and `list` included, and `gc` and `discard`
    // with them -- exited 3 with "its metadata3.db is missing or unreadable ... restore
    // metadata3.db from a backup, or move the directory aside". The 40 960-byte metadata3.db was
    // perfectly intact: the store is in WAL mode, so SQLite has to make a `-shm` even to READ
    // it, and on a full volume it cannot. Following that advice would have orphaned every
    // snapshot and every world in the store for a condition that freeing 8 MiB undid.
    //
    // So: with the volume genuinely full, the open comes back -ENOSPC and not
    // WFS_E_STORE_DAMAGED, the database is byte-identical before and after, and once there is
    // room again every row is where it was.
    char dbfile[WFS_PATH_MAX];
    join(dbfile, sizeof dbfile, store_dir, "metadata3.db");
    size_t db_size = 0;
    unsigned char *db_before = read_all(dbfile, &db_size);

    FillContext wall;
    memset(&wall, 0, sizeof wall);
    snprintf(wall.volume, sizeof wall.volume, "%s", volume);
    snprintf(wall.filler, sizeof wall.filler, "%s/.forkfs-open-%ld", volume, (long)getpid());
    fill_until_enospc(&wall, NULL);
    CHECK(wall.saw_enospc && !wall.saw_other_error);
    report_open_codes(dbfile);

    wfs_store *walled = NULL;
    int wall_rc = wfs_store_open(store_dir, &walled);
    printf("full volume, wfs_store_open(%s) -> %d (%s)\n", store_dir, wall_rc,
           wfs_strerror(wall_rc));
    CHECK(walled == NULL);
    CHECK(wall_rc != WFS_E_STORE_DAMAGED);
    CHECK(wall_rc == -ENOSPC);
    check_cli_message(world_bin, store_dir);
    remove_filler(&wall);

    size_t db_size_after = 0;
    unsigned char *db_after = read_all(dbfile, &db_size_after);
    CHECK(db_size_after == db_size && memcmp(db_before, db_after, db_size) == 0);
    free(db_before);
    free(db_after);

    CHECK_OK(wfs_store_open(store_dir, &s));
    size_t after_snapshot_count = 0, after_world_count = 0;
    CHECK_OK(wfs_snapshot_list(s, final_snapshots, 8, &after_snapshot_count));
    CHECK_OK(wfs_world_list(s, 0, worlds_list, 8, &after_world_count));
    CHECK(after_snapshot_count == final_snapshot_count && after_world_count == final_world_count);
    CHECK_OK(wfs_snapshot_verify(s, sid, &vr));
    check_integrity(store_dir);
    wfs_store_close(s);

    printf("disk-full regression passed: ENOSPC rollback, P11 low-space refusal, checkpoint/fork retry,\n"
           "  full-volume open reports -ENOSPC and leaves metadata3.db byte-identical\n");
    return 0;
}
