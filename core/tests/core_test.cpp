// M1 core tests: snapshots, worlds and every safety rule the core owns.
// Plain asserts, libc only, so this runs anywhere with a C++ compiler (arch.md §39).
//
// Each block is labelled with the rule from docs/M1_DESIGN.md §3 that it pins down.
#include "worldfs/worldfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

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

    wfs_store_close(s);
    rm_rf(root);
    printf("core_test: all OK\n");
    return 0;
}
