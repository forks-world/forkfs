// T1.3: `world fs diff` — the FSEvents path, the full-scan path, and every fallback.
//
// The shape of the main fixture is the one docs/TASKS.md asks for: a 10k-file snapshot and a
// world with 500 modified, 200 added, 100 deleted and 50 mode-only changes. Every assertion is
// an exact-set assertion: the output is sorted, so the test walks it once and requires each
// line to be the change the fixture made to that exact path, with nothing missing and nothing
// extra.
//
// Plain asserts and libc only, like core_test.cpp (arch.md §39). sqlite3 appears for one job
// only: corrupting the recorded FSEvents cursor, which is how the "must silently fall back and
// still be exact" case is provoked without having to block a thread we deliberately never block.
#include "worldfs/worldfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CHECK_OK(x) do { int _rc = (x); if (_rc != 0) { fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc)); exit(1); } } while (0)
#define CHECK_RC(x, want) do { int _rc = (x); if (_rc != (want)) { fprintf(stderr, "%s:%d: %s -> %d (%s), wanted %d\n", __FILE__, __LINE__, #x, _rc, wfs_strerror(_rc), (want)); exit(1); } } while (0)

// ---- fixture geometry ---------------------------------------------------------------------

enum {
    N_DIRS = 100,        // d000 .. d099
    N_FILES = 100,       // f000 .. f099 in each
    MOD_DIRS_SIZE = 4,   // d000..d003: rewritten with a different length -> M by size alone
    MOD_DIR_SAME = 4,    // d004:       rewritten to the same length      -> M by content
    ADD_DIR = 10,        // d010:       new000..new199                    -> A
    DEL_DIR = 20,        // d020:       f000..f099 unlinked               -> D
    META_DIR = 30,       // d030:       f000..f049 chmod 0600             -> T
    N_MOD = 500, N_ADD = 200, N_DEL = 100, N_META = 50
};

static void join(char *out, size_t cap, const char *a, const char *b) { snprintf(out, cap, "%s/%s", a, b); }

static void write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    CHECK(f);
    fputs(s, f);
    CHECK(fclose(f) == 0);
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

static unsigned mode_of(const char *p) {
    struct stat st;
    CHECK(lstat(p, &st) == 0);
    return (unsigned)(st.st_mode & 07777);
}

// fseventsd writes its journal on a timer: a lone change takes 90-600 ms to become visible to
// a stream created after it (the measurements are in core/src/platform_darwin_events.cpp). A
// burst flushes promptly, which is why the 850-change fixture never needs this; the single
// changes in small_cases() do. What is under test here is the diff, not fseventsd's timer.
static void settle(void) { usleep(900 * 1000); }

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
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

// 64 bytes of deterministic content, so "same length, different bytes" is easy to arrange.
static void body(char *out, size_t cap, const char *tag, int d, int i) {
    char raw[128];
    int n = snprintf(raw, sizeof raw, "%s %03d %03d ", tag, d, i);
    while (n < 63) raw[n++] = 'x';
    raw[63] = '\n';
    raw[64] = 0;
    snprintf(out, cap, "%s", raw);
}

static void build_tree(const char *root, int dirs, int per) {
    CHECK(mkdir(root, 0755) == 0);
    char p[4096], f[4096], b[256];
    for (int d = 0; d < dirs; ++d) {
        snprintf(p, sizeof p, "%s/d%03d", root, d);
        CHECK(mkdir(p, 0755) == 0);
        for (int i = 0; i < per; ++i) {
            snprintf(f, sizeof f, "%s/f%03d.txt", p, i);
            body(b, sizeof b, "base", d, i);
            write_file(f, b);
        }
    }
}

// ---- collecting the diff ---------------------------------------------------------------------

struct Line {
    int change;
    char path[512];
};
struct Collect {
    Line *v;
    size_t n, cap;
};

static int collect_cb(void *ctx, const wfs_diff_entry *e) {
    Collect *c = (Collect *)ctx;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->v = (Line *)realloc(c->v, c->cap * sizeof *c->v);
        CHECK(c->v);
    }
    c->v[c->n].change = e->change;
    snprintf(c->v[c->n].path, sizeof c->v[c->n].path, "%s", e->path);
    c->n++;
    return 0;
}

// What the fixture did to this path, or 0 if the fixture did not touch it.
static int expected_change(const char *rel) {
    int d = -1, i = -1;
    char leaf[64];
    if (sscanf(rel, "d%d/%63s", &d, leaf) != 2) return 0;
    if (!strncmp(leaf, "new", 3)) {
        if (d == ADD_DIR && sscanf(leaf, "new%d.txt", &i) == 1 && i >= 0 && i < N_ADD) return 'A';
        return 0;
    }
    if (sscanf(leaf, "f%d.txt", &i) != 1 || i < 0 || i >= N_FILES) return 0;
    if (d < MOD_DIRS_SIZE || d == MOD_DIR_SAME) return 'M';
    if (d == ADD_DIR) return 0;
    if (d == DEL_DIR) return 'D';
    if (d == META_DIR) return i < N_META ? 'T' : 0;
    return 0;
}

// The exact-set check: sorted, no duplicates, every line is the change the fixture made, and
// the four totals are exactly what the fixture did.
static void check_exact(const char *what, Collect *c, const wfs_diff_stats *st) {
    size_t a = 0, m = 0, d = 0, t = 0;
    for (size_t i = 0; i < c->n; ++i) {
        if (i && strcmp(c->v[i - 1].path, c->v[i].path) >= 0) {
            fprintf(stderr, "%s: not sorted / duplicated at %zu: '%s' then '%s'\n", what, i,
                    c->v[i - 1].path, c->v[i].path);
            exit(1);
        }
        int want = expected_change(c->v[i].path);
        if (want != c->v[i].change) {
            fprintf(stderr, "%s: %c %s — expected %c\n", what, c->v[i].change, c->v[i].path,
                    want ? want : '-');
            exit(1);
        }
        switch (c->v[i].change) {
        case 'A': a++; break;
        case 'M': m++; break;
        case 'D': d++; break;
        default: t++; break;
        }
    }
    if (a != N_ADD || m != N_MOD || d != N_DEL || t != N_META) {
        fprintf(stderr, "%s: A=%zu M=%zu D=%zu T=%zu, wanted A=%d M=%d D=%d T=%d\n", what, a, m, d,
                t, N_ADD, N_MOD, N_DEL, N_META);
        exit(1);
    }
    CHECK(st->added == N_ADD && st->modified == N_MOD && st->deleted == N_DEL && st->meta == N_META);
    printf("  %-28s A=%zu M=%zu D=%zu T=%zu  candidates=%llu compared=%llu content=%llu  "
           "full=%d fallback=%d  %.1f ms\n",
           what, a, m, d, t, (unsigned long long)st->candidates, (unsigned long long)st->compared,
           (unsigned long long)st->content_cmp, st->full_scan, st->fallback,
           st->elapsed_us / 1000.0);
}

static void run_diff(wfs_store *s, wfs_id w, int flags, Collect *c, wfs_diff_stats *st) {
    free(c->v);
    memset(c, 0, sizeof *c);
    CHECK_OK(wfs_world_diff_ex(s, w, flags, collect_cb, c, st));
}

// ---- poking the recorded cursor (the only reason sqlite3 is here) ------------------------------

static void set_cursor(const char *store, wfs_id w, unsigned long long id) {
    char db[4096];
    join(db, sizeof db, store, "metadata.db");
    sqlite3 *h = NULL;
    CHECK(sqlite3_open(db, &h) == SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof sql, "UPDATE worlds SET fsevents_id=%llu WHERE id=%llu", id,
             (unsigned long long)w);
    CHECK(sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(h);
}

static void set_snapshot_state(const char *store, wfs_id sid, int state) {
    char db[4096];
    join(db, sizeof db, store, "metadata.db");
    sqlite3 *h = NULL;
    CHECK(sqlite3_open(db, &h) == SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof sql, "UPDATE snapshots SET state=%d WHERE id=%llu", state,
             (unsigned long long)sid);
    CHECK(sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(h);
}

// ---- the 50k benchmark (WFS_DIFF_BENCH=1) ------------------------------------------------------

static void bench(const char *root, const char *store) {
    char src[4096], w[4096], p[4096], b[256];
    join(src, sizeof src, root, "bench-src");
    join(w, sizeof w, root, "bench-w");
    printf("\n50k benchmark (500 modified / 200 added / 100 deleted = 800 changes)\n");
    build_tree(src, 500, 100); // 50 000 files + 500 dirs

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    wfs_id sid = 0;
    int64_t t0 = now_us();
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "bench";
    CHECK_OK(wfs_snapshot_create(s, src, &sopts, &sid));
    printf("  init            %.3f s\n", (now_us() - t0) / 1e6);
    wfs_ref from = {WFS_K_SNAPSHOT, sid};
    wfs_fork_opts o;
    memset(&o, 0, sizeof o);
    wfs_id wid = 0;
    t0 = now_us();
    CHECK_OK(wfs_world_create(s, from, w, &o, &wid));
    printf("  fork            %.3f s\n", (now_us() - t0) / 1e6);

    for (int d = 0; d < 5; ++d)
        for (int i = 0; i < 100; ++i) {
            snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, d, i);
            body(b, sizeof b, "modified-and-longer", d, i);
            write_file(p, b);
        }
    for (int i = 0; i < 200; ++i) {
        snprintf(p, sizeof p, "%s/d010/new%03d.txt", w, i);
        write_file(p, "added\n");
    }
    for (int i = 0; i < 100; ++i) {
        snprintf(p, sizeof p, "%s/d020/f%03d.txt", w, i);
        CHECK(unlink(p) == 0);
    }

    Collect c;
    memset(&c, 0, sizeof c);
    wfs_diff_stats st;
    double ev = 1e9, fl = 1e9, fx = 1e9, evx = 1e9;
    size_t n_events = 0, n_full = 0;
    uint64_t cand = 0;
    for (int i = 0; i < 3; ++i) {
        run_diff(s, wid, 0, &c, &st);
        CHECK(st.full_scan == 0);
        if (st.elapsed_us / 1e6 < ev) ev = st.elapsed_us / 1e6;
        n_events = c.n;
        cand = st.candidates;
        run_diff(s, wid, WFS_DIFF_NO_XATTR, &c, &st);
        if (st.elapsed_us / 1e6 < evx) evx = st.elapsed_us / 1e6;
        run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
        if (st.elapsed_us / 1e6 < fl) fl = st.elapsed_us / 1e6;
        n_full = c.n;
        run_diff(s, wid, WFS_DIFF_FULL | WFS_DIFF_NO_XATTR, &c, &st);
        if (st.elapsed_us / 1e6 < fx) fx = st.elapsed_us / 1e6;
    }
    printf("  diff (FSEvents)            %.3f s   %llu changes, %llu candidates\n", ev,
           (unsigned long long)n_events, (unsigned long long)cand);
    printf("  diff (FSEvents, no-xattr)  %.3f s\n", evx);
    printf("  diff (--full)              %.3f s   %llu changes\n", fl, (unsigned long long)n_full);
    printf("  diff (--full --no-xattr)   %.3f s\n", fx);
    CHECK(n_full == n_events);
    free(c.v);
    wfs_store_close(s);
}

// ---- small fixtures: empty dirs, renames, symlinks, mtime-only, fork-from-world ---------------

struct Want {
    int change;
    const char *path;
};

static void check_lines(const char *what, Collect *c, const Want *want, size_t n) {
    if (c->n != n) {
        fprintf(stderr, "%s: %zu lines, wanted %zu:\n", what, c->n, n);
        for (size_t i = 0; i < c->n; ++i) fprintf(stderr, "    %c %s\n", c->v[i].change, c->v[i].path);
        exit(1);
    }
    for (size_t i = 0; i < n; ++i) {
        if (c->v[i].change != want[i].change || strcmp(c->v[i].path, want[i].path)) {
            fprintf(stderr, "%s: line %zu is '%c %s', wanted '%c %s'\n", what, i, c->v[i].change,
                    c->v[i].path, want[i].change, want[i].path);
            exit(1);
        }
    }
    printf("  %-28s %zu lines, exact\n", what, n);
}

// ---- T2.4: the EF_NO_XATTRS shortcut may never skip a comparison that was needed --------------
//
// The full scan now asks the file system, per entry, whether it has any xattr at all
// (ATTR_CMNEXT_EXT_FLAGS / EF_NO_XATTRS, via getattrlistbulk(2)) and calls listxattr only when
// the answer is not "neither side has any". The flag only ever denies xattrs, so the danger is
// one-sided: a file whose xattr the flag failed to mention would be silently compared as clean.
//
// This fixture puts every combination in one tree, with the **world** side and the **snapshot**
// side each getting a turn at being the one that carries the attribute -- the snapshot-side case
// is the one a naive "look at the world entry and skip" would get wrong, because there the world
// entry genuinely has nothing.
//
// Note for anyone re-reading the numbers: on macOS 27 every file this process creates is stamped
// with com.apple.provenance and it cannot be removed, so *no* file this test makes ever gets
// EF_NO_XATTRS. That is why this case asserts the answer and not the syscall count: the shortcut
// is exercised for real on trees that came from somewhere else (see docs/TASKS.md T2.4).
#ifdef __APPLE__
static void xattr_shortcut(const char *root, const char *store) {
    char src[4096], w[4096], p[4096];
    join(src, sizeof src, root, "xa-src");
    join(w, sizeof w, root, "xa-w");
    CHECK(mkdir(src, 0755) == 0);
    // four files, all identical in every other respect
    static const char *names[] = {"plain.txt", "both.txt", "world-only.txt", "snap-only.txt"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        join(p, sizeof p, src, names[i]);
        write_file(p, "same bytes everywhere\n");
    }
    // the snapshot side carries an xattr on two of them
    join(p, sizeof p, src, "both.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "old", 3, 0, XATTR_NOFOLLOW) == 0);
    join(p, sizeof p, src, "snap-only.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "gone", 4, 0, XATTR_NOFOLLOW) == 0);

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    wfs_id sid = 0;
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "xa";
    CHECK_OK(wfs_snapshot_create(s, src, &sopts, &sid));
    wfs_ref from = {WFS_K_SNAPSHOT, sid};
    wfs_fork_opts o;
    memset(&o, 0, sizeof o);
    wfs_id wid = 0;
    CHECK_OK(wfs_world_create(s, from, w, &o, &wid));

    // plain.txt      : nothing on either side          -> clean (the shortcut's happy case)
    // both.txt       : the value changes in the world  -> T
    // world-only.txt : only the world has one          -> T
    // snap-only.txt  : only the snapshot has one       -> T  (the world entry has none at all,
    //                                                         which is exactly what a one-sided
    //                                                         shortcut would skip)
    join(p, sizeof p, w, "both.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "new", 3, 0, XATTR_NOFOLLOW) == 0);
    join(p, sizeof p, w, "world-only.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "added", 5, 0, XATTR_NOFOLLOW) == 0);
    join(p, sizeof p, w, "snap-only.txt");
    CHECK(removexattr(p, "com.forks.world.t24", XATTR_NOFOLLOW) == 0);

    Collect c;
    memset(&c, 0, sizeof c);
    wfs_diff_stats st;
    static const Want want[] = {
        {'T', "both.txt"},
        {'T', "snap-only.txt"},
        {'T', "world-only.txt"},
    };
    settle();
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    CHECK(st.full_scan == 1);
    check_lines("xattr shortcut / --full", &c, want, sizeof want / sizeof want[0]);
    run_diff(s, wid, 0, &c, &st);
    check_lines("xattr shortcut / FSEvents", &c, want, sizeof want / sizeof want[0]);
    // ... and with the comparison switched off, none of the three is visible.
    run_diff(s, wid, WFS_DIFF_FULL | WFS_DIFF_NO_XATTR, &c, &st);
    check_lines("xattr shortcut / --no-xattr", &c, NULL, 0);

    // A same-valued xattr on both sides is not a change, whichever way the flag went.
    join(p, sizeof p, w, "both.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "old", 3, 0, XATTR_NOFOLLOW) == 0);
    join(p, sizeof p, w, "world-only.txt");
    CHECK(removexattr(p, "com.forks.world.t24", XATTR_NOFOLLOW) == 0);
    join(p, sizeof p, w, "snap-only.txt");
    CHECK(setxattr(p, "com.forks.world.t24", "gone", 4, 0, XATTR_NOFOLLOW) == 0);
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    check_lines("xattr shortcut / restored", &c, NULL, 0);

    free(c.v);
    wfs_store_close(s);
}
#endif

static void small_cases(const char *root, const char *store) {
    char src[4096], w[4096], w2[4096], p[4096], q[4096];
    join(src, sizeof src, root, "small-src");
    join(w, sizeof w, root, "small-w");
    join(w2, sizeof w2, root, "small-w2");
    CHECK(mkdir(src, 0755) == 0);
    join(p, sizeof p, src, "keep.txt");
    write_file(p, "keep\n");
    join(p, sizeof p, src, "same-bytes.txt");
    write_file(p, "same\n");
    join(p, sizeof p, src, "empty-dir-that-goes");
    CHECK(mkdir(p, 0755) == 0);
    join(p, sizeof p, src, "movable");
    CHECK(mkdir(p, 0755) == 0);
    join(q, sizeof q, p, "inside.txt");
    write_file(q, "inside\n");
    join(p, sizeof p, src, "link");
    CHECK(symlink("keep.txt", p) == 0);
    join(p, sizeof p, src, "turns-into-a-file");
    CHECK(mkdir(p, 0755) == 0);
    join(q, sizeof q, p, "buried.txt");
    write_file(q, "buried\n");

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    wfs_id sid = 0;
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "small";
    CHECK_OK(wfs_snapshot_create(s, src, &sopts, &sid));
    wfs_ref from = {WFS_K_SNAPSHOT, sid};
    wfs_fork_opts o;
    memset(&o, 0, sizeof o);
    wfs_id wid = 0;
    CHECK_OK(wfs_world_create(s, from, w, &o, &wid));

    // an empty directory that only exists in the world, and one that only exists in the snapshot
    join(p, sizeof p, w, "empty-dir-that-comes");
    CHECK(mkdir(p, 0755) == 0);
    join(p, sizeof p, w, "empty-dir-that-goes");
    CHECK(rmdir(p) == 0);
    // a directory rename: FSEvents reports the two directory paths and nothing about the file
    // inside, so the verification has to expand both sides itself (M1 answer: D + A).
    join(p, sizeof p, w, "movable");
    join(q, sizeof q, w, "moved");
    CHECK(rename(p, q) == 0);
    // same bytes, new mtime -> metadata only
    join(p, sizeof p, w, "same-bytes.txt");
    write_file(p, "same\n");
    // the symlink now points elsewhere
    join(p, sizeof p, w, "link");
    CHECK(unlink(p) == 0);
    CHECK(symlink("elsewhere.txt", p) == 0);
    // a directory replaced by a plain file: M for the path, and what was buried under it is D
    join(p, sizeof p, w, "turns-into-a-file");
    join(q, sizeof q, p, "buried.txt");
    CHECK(unlink(q) == 0);
    CHECK(rmdir(p) == 0);
    write_file(p, "now a file\n");

    Collect c;
    memset(&c, 0, sizeof c);
    wfs_diff_stats st;
    settle();
    static const Want want[] = {
        {'A', "empty-dir-that-comes"},
        {'D', "empty-dir-that-goes"},
        {'M', "link"},
        {'D', "movable/inside.txt"}, // a rename is D + A in M1 ...
        {'A', "moved/inside.txt"},   // ... and the output is sorted by path, not paired
        {'T', "same-bytes.txt"},
        {'M', "turns-into-a-file"},
        {'D', "turns-into-a-file/buried.txt"},
    };
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 0);
    check_lines("small / FSEvents", &c, want, sizeof want / sizeof want[0]);
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_REQUESTED);
    check_lines("small / --full", &c, want, sizeof want / sizeof want[0]);

    // A world forked from a live world cannot use its own cursor: the changes its parent had
    // already made happened before that cursor was taken. It must fall back, silently.
    wfs_ref fromw = {WFS_K_WORLD, wid};
    wfs_id wid2 = 0;
    CHECK_OK(wfs_world_create(s, fromw, w2, &o, &wid2));
    run_diff(s, wid2, 0, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_FROM_WORLD);
    check_lines("child world / auto-full", &c, want, sizeof want / sizeof want[0]);

    // ---- the snapshot is gone ----
    // (a) the row is no longer ACTIVE, as a `snapshot discard` would leave it
    set_snapshot_state(store, sid, WFS_ST_TRASHED);
    CHECK_RC(wfs_world_diff(s, wid, 0, NULL, NULL), WFS_E_SOURCE_GONE);
    set_snapshot_state(store, sid, WFS_ST_ACTIVE);
    // (b) the tree itself is not there any more
    wfs_snapshot_rec sr;
    CHECK_OK(wfs_snapshot_info(s, sid, &sr));
    // The snapshot root itself is UF_IMMUTABLE and cannot be renamed (P3), so move the
    // <store>/snapshots/S<n> directory that holds it, which is what a trash+gc would do.
    char away[4096], snapdir[4096];
    snprintf(snapdir, sizeof snapdir, "%s", sr.path);
    char *slash = strrchr(snapdir, '/');
    CHECK(slash);
    *slash = 0;
    join(away, sizeof away, root, "snapshot-taken-away");
    CHECK(rename(snapdir, away) == 0);
    CHECK_RC(wfs_world_diff(s, wid, 0, NULL, NULL), WFS_E_SOURCE_GONE);
    CHECK_RC(wfs_world_diff(s, wid, WFS_DIFF_FULL, NULL, NULL), WFS_E_SOURCE_GONE);
    CHECK(rename(away, snapdir) == 0);

    // A world that is not where the store says it is, is not diffable either.
    char moved_world[4096];
    join(moved_world, sizeof moved_world, root, "small-w-moved");
    CHECK(rename(w, moved_world) == 0);
    // P1 first: the row follows the tree, and the diff still works from the new path.
    wfs_identity id;
    CHECK_OK(wfs_world_verify_identity(s, moved_world, &id));
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    check_lines("moved world / --full", &c, want, sizeof want / sizeof want[0]);
    CHECK(rename(moved_world, w) == 0);
    CHECK_OK(wfs_world_verify_identity(s, w, &id));

#ifdef __APPLE__
    // An xattr is the one metadata change stat(2) cannot see. FSEvents does report it
    // (ItemXattrMod), so the candidate path finds it for the price of two listxattr calls on a
    // handful of files; a full scan has to pay that price on every file in the tree, which is
    // what WFS_DIFF_NO_XATTR opts out of -- and then this change is invisible, on purpose.
    join(p, sizeof p, w, "keep.txt");
    CHECK(setxattr(p, "com.forks.world.difftest", "v", 1, 0, XATTR_NOFOLLOW) == 0);
    settle();
    static const Want want_x[] = {
        {'A', "empty-dir-that-comes"},
        {'D', "empty-dir-that-goes"},
        {'T', "keep.txt"},
        {'M', "link"},
        {'D', "movable/inside.txt"},
        {'A', "moved/inside.txt"},
        {'T', "same-bytes.txt"},
        {'M', "turns-into-a-file"},
        {'D', "turns-into-a-file/buried.txt"},
    };
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 0);
    check_lines("xattr-only / FSEvents", &c, want_x, sizeof want_x / sizeof want_x[0]);
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    check_lines("xattr-only / --full", &c, want_x, sizeof want_x / sizeof want_x[0]);
    run_diff(s, wid, WFS_DIFF_FULL | WFS_DIFF_NO_XATTR, &c, &st);
    check_lines("xattr-only / --no-xattr", &c, want, sizeof want / sizeof want[0]);
    CHECK(removexattr(p, "com.forks.world.difftest", XATTR_NOFOLLOW) == 0);
#endif

    // ---- the gate stays shut (T1.1b x T1.3) ----
    // The snapshot this world came from is gate-protected: its root is WFS_GATE_CLOSED and the
    // only thing that opens it is SnapGate, under a flock on the manifest. A diff has to be
    // able to read through it, and has to leave it exactly as it found it -- on every way out.
    CHECK_OK(wfs_snapshot_info(s, sid, &sr));
    CHECK(sr.hard == 0);
    CHECK(mode_of(sr.path) == WFS_GATE_CLOSED);
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    check_lines("gated snapshot / --full", &c, want, sizeof want / sizeof want[0]);
    CHECK(mode_of(sr.path) == WFS_GATE_CLOSED);
    run_diff(s, wid, 0, &c, &st);
    CHECK(mode_of(sr.path) == WFS_GATE_CLOSED);

    // An error raised while the gate is open: a directory in the WORLD that cannot be opened
    // makes the walk fail after SnapGate has already widened the root. The guard has to close
    // it anyway, and the flock has to come back, or every later fork from this snapshot hangs.
    join(p, sizeof p, w, "moved");
    CHECK(chmod(p, 0000) == 0);
    int blocked = wfs_world_diff(s, wid, WFS_DIFF_FULL, NULL, NULL);
    CHECK(blocked == -EACCES);
    CHECK(mode_of(sr.path) == WFS_GATE_CLOSED);
    CHECK(chmod(p, 0755) == 0);
    // ... and the gate is usable again right afterwards, from another clone of the snapshot.
    char wgate[4096];
    join(wgate, sizeof wgate, root, "small-w-gate");
    wfs_id wid3 = 0;
    CHECK_OK(wfs_world_create(s, from, wgate, &o, &wid3));
    CHECK(mode_of(sr.path) == WFS_GATE_CLOSED);
    rm_rf(wgate);

    // ---- which path is the default ----
    // The world is far below WFS_DIFF_EVENTS_MIN_ENTRIES, so with the threshold at its real
    // value the scan is chosen without anything having gone wrong; --events asks for the other
    // one, and --full still overrides both. (The rest of this test runs with the threshold
    // pinned to 0 so that flags == 0 means the events path.)
    char thr[32];
    snprintf(thr, sizeof thr, "%d", WFS_DIFF_EVENTS_MIN_ENTRIES);
    CHECK(setenv("WFS_DIFF_EVENTS_MIN_ENTRIES", thr, 1) == 0);
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_SMALL_TREE && st.candidates == 0);
    check_lines("default / scan", &c, want, sizeof want / sizeof want[0]);
    run_diff(s, wid, WFS_DIFF_EVENTS, &c, &st);
    CHECK(st.full_scan == 0 && st.fallback == WFS_DF_NONE);
    check_lines("--events", &c, want, sizeof want / sizeof want[0]);
    run_diff(s, wid, WFS_DIFF_EVENTS | WFS_DIFF_FULL, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_REQUESTED);
    // A world above the threshold picks the events path on its own.
    CHECK(setenv("WFS_DIFF_EVENTS_MIN_ENTRIES", "2", 1) == 0);
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 0 && st.fallback == WFS_DF_NONE);
    CHECK(setenv("WFS_DIFF_EVENTS_MIN_ENTRIES", "0", 1) == 0);

    free(c.v);
    wfs_store_close(s);
}

int main() {
    // Everything below is a 10k-entry fixture, three orders of magnitude under
    // WFS_DIFF_EVENTS_MIN_ENTRIES, so the default would be the full scan everywhere and the
    // event path would never be exercised. Pin the threshold to 0 for the run: flags == 0 then
    // means "events, unless something forces the scan", which is what these cases assert.
    // small_cases() puts the real threshold back for the two cases that test the default.
    CHECK(setenv("WFS_DIFF_EVENTS_MIN_ENTRIES", "0", 1) == 0);

    const char *tmp = getenv("TMPDIR");
    char tpl[4096];
    snprintf(tpl, sizeof tpl, "%swfs-diff-test.XXXXXX", (tmp && *tmp) ? tmp : "/tmp/");
    CHECK(mkdtemp(tpl));
    char root[4096];
    CHECK(realpath(tpl, root));

    char store[4096], src[4096], w[4096], p[4096], b[256];
    join(store, sizeof store, root, "store");
    join(src, sizeof src, root, "project");
    join(w, sizeof w, root, "world");

    printf("diff_test: building the 10k fixture\n");
    build_tree(src, N_DIRS, N_FILES);

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    wfs_id sid = 0;
    wfs_snapshot_opts sopts;
    memset(&sopts, 0, sizeof sopts);
    sopts.name = "proj";
    CHECK_OK(wfs_snapshot_create(s, src, &sopts, &sid));
    wfs_snapshot_rec sr;
    CHECK_OK(wfs_snapshot_info(s, sid, &sr));
    CHECK(sr.entries == N_DIRS + N_DIRS * N_FILES);

    wfs_ref from = {WFS_K_SNAPSHOT, sid};
    wfs_fork_opts o;
    memset(&o, 0, sizeof o);
    wfs_id wid = 0;
    CHECK_OK(wfs_world_create(s, from, w, &o, &wid));

    // A world nobody has touched has nothing to report, through either path.
    Collect c;
    memset(&c, 0, sizeof c);
    wfs_diff_stats st;
    run_diff(s, wid, 0, &c, &st);
    CHECK(c.n == 0 && st.full_scan == 0 && st.fallback == WFS_DF_NONE);
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    CHECK(c.n == 0 && st.full_scan == 1);
    printf("  untouched world             0 lines, both paths\n");

    // ---- 500 modified, 200 added, 100 deleted, 50 mode-only ----
    for (int d = 0; d < MOD_DIRS_SIZE; ++d)
        for (int i = 0; i < N_FILES; ++i) {
            snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, d, i);
            write_file(p, "modified, and a different length entirely\n");
        }
    for (int i = 0; i < N_FILES; ++i) { // same length, different bytes: the content compare
        snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, MOD_DIR_SAME, i);
        body(b, sizeof b, "SAME", MOD_DIR_SAME, i);
        write_file(p, b);
    }
    for (int i = 0; i < N_ADD; ++i) {
        snprintf(p, sizeof p, "%s/d%03d/new%03d.txt", w, ADD_DIR, i);
        write_file(p, "added\n");
    }
    for (int i = 0; i < N_DEL; ++i) {
        snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, DEL_DIR, i);
        CHECK(unlink(p) == 0);
    }
    for (int i = 0; i < N_META; ++i) {
        snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, META_DIR, i);
        CHECK(chmod(p, 0600) == 0);
    }

    // (a) the FSEvents path.
    // settle() first: a burst this size usually flushes at once, but not always -- measured
    // 2 incomplete replays in 27 runs without it, and the journal flush is not globally
    // ordered, so the diff's own watermark can come back before the last file of the burst
    // (docs/TASKS.md T1.3). That is fseventsd's timer, not the diff, and it is exactly why the
    // full scan is the default path now; here it would just make the exact-set assertion flaky.
    settle();
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 0 && st.fallback == WFS_DF_NONE);
    CHECK(st.candidates >= N_MOD + N_ADD + N_DEL + N_META);
    CHECK(st.content_cmp >= N_FILES); // d004 had to be read on both sides
    check_exact("10k / FSEvents", &c, &st);

    // (b) the full scan
    run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_REQUESTED && st.candidates == 0);
    check_exact("10k / --full", &c, &st);

    // (c) a cursor the event journal cannot serve. This is the same silence a dropped event
    // produces — FSEvents delivers nothing and never sends HistoryDone — and it must turn into
    // a full scan with an identical answer, without the caller being told to do anything.
    uint64_t good_cursor = 0;
    {
        wfs_world_rec wr;
        CHECK_OK(wfs_world_info(s, wid, &wr));
        good_cursor = wr.fsevents_id;
        CHECK(good_cursor != 0);
    }
    set_cursor(store, wid, 1);
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_STALE);
    check_exact("10k / stale cursor", &c, &st);

    // a cursor from the future: the id space was reset behind us
    set_cursor(store, wid, 0xfffffffffffffull);
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_WRAPPED);
    check_exact("10k / wrapped cursor", &c, &st);

    // no cursor at all (a store row written before T1.3, or a fork that could not take one)
    set_cursor(store, wid, 0);
    run_diff(s, wid, 0, &c, &st);
    CHECK(st.full_scan == 1 && st.fallback == WFS_DF_NO_CURSOR);
    check_exact("10k / no cursor", &c, &st);
    set_cursor(store, wid, good_cursor);

    // (d) churn: 5000 quick edits on top, with nobody draining anything. The consumer is a
    // dispatch queue that only copies bytes, so this is exactly the load that made the probe
    // in CLONE_MODEL_MACOS27 §6.2 drop events when the consumer was the writer's own thread.
    for (int round = 0; round < 50; ++round)
        for (int i = 0; i < N_FILES; ++i) {
            snprintf(p, sizeof p, "%s/d%03d/f%03d.txt", w, MOD_DIRS_SIZE - 1, i);
            body(b, sizeof b, "churn", round, i);
            write_file(p, b);
        }
    // d003's files end up the same length as the snapshot's but with different bytes, so they
    // are still M; everything else is untouched, so the exact set is unchanged.
    run_diff(s, wid, 0, &c, &st);
    printf("  after 5000 quick edits:     full=%d fallback=%d candidates=%llu\n", st.full_scan,
           st.fallback, (unsigned long long)st.candidates);
    check_exact("10k / after churn", &c, &st);

    // ---- WFS_DIFF_NO_CONTENT: no bytes are read, so the same-length rewrite is still M ----
    run_diff(s, wid, WFS_DIFF_NO_CONTENT, &c, &st);
    CHECK(st.content_cmp == 0 && st.bytes_read == 0);
    check_exact("10k / no-content", &c, &st);

    // ---- a callback that stops early stops the walk ----
    struct Stop {
        static int cb(void *ctx, const wfs_diff_entry *) { return *(int *)ctx = 42; }
    };
    int rc42 = 0;
    CHECK_RC(wfs_world_diff(s, wid, 0, Stop::cb, &rc42), 42);

    // ---- timing, best of three, on the 10k fixture ----
    double ev = 1e9, fl = 1e9, fx = 1e9;
    for (int i = 0; i < 3; ++i) {
        run_diff(s, wid, 0, &c, &st);
        if (st.elapsed_us / 1e6 < ev) ev = st.elapsed_us / 1e6;
        run_diff(s, wid, WFS_DIFF_FULL, &c, &st);
        if (st.elapsed_us / 1e6 < fl) fl = st.elapsed_us / 1e6;
        run_diff(s, wid, WFS_DIFF_FULL | WFS_DIFF_NO_XATTR, &c, &st);
        if (st.elapsed_us / 1e6 < fx) fx = st.elapsed_us / 1e6;
    }
    printf("  10k, 850 changes: FSEvents %.3f s   --full %.3f s   --full --no-xattr %.3f s\n", ev,
           fl, fx);

    free(c.v);
    c.v = NULL;
    wfs_store_close(s);

    printf("small cases\n");
    {
        char store2[4096];
        join(store2, sizeof store2, root, "store-small");
        small_cases(root, store2);
    }
#ifdef __APPLE__
    {
        char store4[4096];
        join(store4, sizeof store4, root, "store-xattr");
        xattr_shortcut(root, store4);
    }
#endif

    const char *bp = getenv("WFS_DIFF_BENCH");
    if (bp && *bp && strcmp(bp, "0")) {
        char store3[4096];
        join(store3, sizeof store3, root, "store-bench");
        bench(root, store3);
    }

    CHECK(exists(root));
    rm_rf(root);
    printf("diff_test: all OK\n");
    return 0;
}
