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

    // ---- P8: a half-built tree is collected ----
    char stray[4096], strayfile[4096];
    join(stray, sizeof stray, worlds, "interrupted.wfs-tmp");
    CHECK(mkdir(stray, 0755) == 0);
    join(strayfile, sizeof strayfile, stray, "half");
    write_file(strayfile, "x");
    CHECK_OK(wfs_gc(s, 0, &gc));
    CHECK(!exists(stray));
    CHECK(gc.tmp_removed >= 1);
    CHECK(exists(w1path) && exists(w3path));                       // and nothing else went with it

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

    // ---- P13: a store from another schema is refused before anything is read ----
    char other[4096], ver[4096];
    join(other, sizeof other, root, "other-store");
    CHECK(mkdir(other, 0755) == 0);
    join(ver, sizeof ver, other, "VERSION");
    write_file(ver, "99\n");
    wfs_store *bad = NULL;
    CHECK_RC(wfs_store_open(other, &bad), WFS_E_SCHEMA);
    CHECK(bad == NULL);

    wfs_store_close(s);
    rm_rf(root);
    printf("core_test: all OK\n");
    return 0;
}
