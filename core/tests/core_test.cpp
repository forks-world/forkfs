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
    char crashtgt[4096];
    join(crashtgt, sizeof crashtgt, worlds, "wcrash");
    crash_seen.world = 0;
    crash_seen.tmp[0] = 0;
    wfs_test_before_fork_publish = crash_before_publish;
    wfs_test_before_fork_publish_ctx = &crash_seen;
    wfs_id wcrash = 0;
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    wfs_test_before_fork_publish = NULL;
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
    CHECK_RC(wfs_world_create(s, from, crashtgt, &opts, &wcrash), -EINTR);
    wfs_test_before_fork_publish = NULL;
    CHECK(exists(crash_seen.tmp));
    rm_rf(crash_seen.tmp);
    memset(&gc, 0, sizeof gc);
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(gc.tmp_removed == 0);
    CHECK_OK(wfs_world_info(s, crash_seen.world, &wr));
    CHECK(wr.state == WFS_ST_DEAD);
    CHECK(exists(mine_dir) && exists(mine_file));

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

    wfs_store_close(s);
    rm_rf(root);
    printf("core_test: all OK\n");
    return 0;
}
