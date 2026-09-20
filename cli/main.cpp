// `world` CLI. World is the product; `fs` is the first state provider (arch.md §23).
// C-style C++: libc only (arch.md §39).
//
// Every refusal prints one line of reason and the command that would have worked, and exits 3.
// 0 = ok, 1 = error, 2 = usage, 3 = refused by a safety rule (docs/M1_DESIGN.md §3).
#include "worldfs/worldfs.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef WFS_FSKIT
#ifdef __APPLE__
extern "C" int world_fs_status_platform(void);
#endif
#endif

enum { EX_OK = 0, EX_ERR = 1, EX_USAGE = 2, EX_REFUSED = 3 };

static void usage(void) {
    fputs("usage: world fs <command>\n"
          "  init <dir> [--name N] [--hard]   snapshot <dir> as S<n> (the root is gated 0000;\n"
          "                                   --hard also sets UF_IMMUTABLE on every entry)\n"
          "  fork [--from W<n>|S<n>] [--to <path>] [--name N] [--copy] [--force] [--no-pool]\n"
          "                                   clone into a writable world (default ~/worlds/W<n>/<name>);\n"
          "                                   a snapshot with a warm pool is served in O(1), --no-pool\n"
          "                                   always clones here and now\n"
          "  checkpoint W<n> [--name N] [--hard] [--force]\n"
          "                                   snapshot a live world; the world stays writable\n"
          "  diff W<n> [--full|--events] [--stat] [--no-xattr] [--all-xattrs] [--no-content]\n"
          "                                   what changed since the fork (A/M/D/T, sorted).\n"
          "                                   Walks both trees by default -- exact, and faster than\n"
          "                                   the FSEvents path below ~200k entries; --events asks\n"
          "                                   for FSEvents anyway, --full always walks.\n"
          "                                   com.apple.provenance (the kernel's note of which app\n"
          "                                   created a file, which cannot be removed) is left out\n"
          "                                   of the xattr comparison; --all-xattrs puts it back\n"
          "  list                             snapshots and worlds\n"
          "  inspect W<n>|S<n>\n"
          "  discard W<n>|S<n> [--now] [--force]\n"
          "                                   move to the store trash (--now deletes at once).\n"
          "                                   A snapshot is refused while an active world or a pool\n"
          "                                   entry still needs it; --force drains the pool\n"
          "  restore W<n>                     bring a trashed world back to its path\n"
          "  gc [--retention <days>] [--now] [--status] [--reconcile]\n"
          "                                   collect half-built trees, stale profiles and dead pool\n"
          "                                   entries; the trash itself is emptied by a background\n"
          "                                   worker unless --now says do it here. --status reports\n"
          "                                   what is waiting and who is on it; --reconcile marks rows\n"
          "                                   whose tree is gone as dead\n"
          "  pool status                      pre-cloned worlds waiting per snapshot\n"
          "  pool fill S<n> [--count K]       top the pool up to K ready entries (default 2)\n"
          "  pool drain S<n>|--all            delete the pool entries of a snapshot\n"
          "  status                           store, counts, free space\n"
          "  verify S<n>|W<n>|<path> [--refresh-marker]\n"
          "                                   snapshot integrity, or world identity. A world whose\n"
          "                                   .world marker names a store path that is not this\n"
          "                                   store's is refused (a mount would open that one);\n"
          "                                   --refresh-marker rewrites the path when the store id\n"
          "                                   still matches -- i.e. when this store has moved\n"
          "  adopt <path> [--name N]          register an unregistered copy as a new world\n"
#ifdef WFS_FSKIT
          "  mount <W> <mountpoint> | umount <mountpoint> | fsstatus   (FSKit frontend)\n"
#endif
          "  world exec W<n> [--no-sandbox|--require-sandbox] -- <cmd...>\n"
          "                                   run <cmd> in the world: cwd = its root, WORLD_* in the\n"
          "                                   environment, an exec lock, and a seatbelt profile that\n"
          "                                   denies writes outside this world\n"
          "  world version\n"
          "options: --store <dir> (or $WORLD_STORE) selects the metadata store\n",
          stderr);
    exit(EX_USAGE);
}

static const char *g_store_override = NULL;

static void store_dir(char *out, size_t cap) {
    if (g_store_override) { snprintf(out, cap, "%s", g_store_override); return; }
    const char *e = getenv("WORLD_STORE");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    if (wfs_store_default_dir(out, cap) != 0) snprintf(out, cap, "/tmp/world-fs");
}

static int is_refusal(int rc) { return rc <= -1001 && rc >= -1099; }

static int fail(const char *what, int rc) {
    fprintf(stderr, "world: %s: %s\n", what, wfs_strerror(rc));
    return is_refusal(rc) ? EX_REFUSED : EX_ERR;
}

// A refusal with a remedy: one line of reason, one line of what to run instead.
static int refuse(const char *reason, const char *hint) {
    fprintf(stderr, "world: %s\n", reason);
    if (hint) fprintf(stderr, "  try: %s\n", hint);
    return EX_REFUSED;
}

// P5: someone is running `world exec` in this world right now.
static int busy_refusal(wfs_store *s, wfs_id w, const char *verb) {
    wfs_lock_info li;
    memset(&li, 0, sizeof li);
    wfs_world_lock_check(s, w, &li);
    char why[512], hint[128];
    if (li.held) {
        char t[32];
        time_t sec = (time_t)li.started_at;
        struct tm tmv;
        localtime_r(&sec, &tmv);
        strftime(t, sizeof t, "%H:%M:%S", &tmv);
        snprintf(why, sizeof why, "refusing to %s W%llu: pid %lld has been running `%s` in it since %s",
                 verb, (unsigned long long)w, (long long)li.pid, li.cmd, t);
    } else {
        snprintf(why, sizeof why, "refusing to %s W%llu: another command holds its lock", verb,
                 (unsigned long long)w);
    }
    snprintf(hint, sizeof hint, "wait for it to finish, or pass --force");
    return refuse(why, hint);
}

static wfs_ref parse_ref(const char *s) {
    wfs_ref r = {WFS_K_NONE, 0};
    if (!s || !*s) return r;
    char c = s[0];
    const char *p = s;
    if (c == 'W' || c == 'w') { r.kind = WFS_K_WORLD; ++p; }
    else if (c == 'S' || c == 's') { r.kind = WFS_K_SNAPSHOT; ++p; }
    else { r.kind = WFS_K_WORLD; }
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (!end || *end || v == 0) { r.kind = WFS_K_NONE; return r; }
    r.id = (wfs_id)v;
    return r;
}

static wfs_id parse_world(const char *s) {
    wfs_ref r = parse_ref(s);
    if (r.kind != WFS_K_WORLD || !r.id) { fprintf(stderr, "world: expected a world id like W1, got '%s'\n", s); exit(EX_USAGE); }
    return r.id;
}

static const char *state_name(int st) {
    switch (st) {
    case WFS_ST_CREATING: return "creating";
    case WFS_ST_ACTIVE: return "active";
    case WFS_ST_TRASHED: return "trashed";
    case WFS_ST_TRASHING: return "being discarded";
    default: return "dead";
    }
}

static void fmt_time(char *buf, size_t cap, int64_t sec) {
    if (sec <= 0) { snprintf(buf, cap, "-"); return; }
    time_t t = (time_t)sec;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tmv);
}

static void fmt_bytes(char *buf, size_t cap, uint64_t b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    size_t i = 0;
    while (v >= 1024.0 && i + 1 < sizeof u / sizeof u[0]) { v /= 1024.0; ++i; }
    snprintf(buf, cap, "%.1f %s", v, u[i]);
}

static int mkdir_p(const char *path) {
    char tmp[WFS_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -ENAMETOOLONG;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (i == n || tmp[i] == '/') {
            char c = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -errno;
            tmp[i] = c;
        }
    }
    return 0;
}

#ifdef WFS_FSKIT
static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], argv); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
#endif

// ---- P6 / P7 / P2 explanations ---------------------------------------------------------------

static int explain_path(wfs_store *s, const char *path, int rc, const char *verb) {
    char hint[WFS_PATH_MAX + 128];
    if (rc == WFS_E_PATH_REFUSED) {
        wfs_identity id;
        if (wfs_world_verify_identity(s, path, &id) == 0 && id.registered) {
            snprintf(hint, sizeof hint, "world fs checkpoint W%llu", (unsigned long long)id.world_id);
            char why[WFS_PATH_MAX + 64];
            snprintf(why, sizeof why, "%s is already world W%llu; %s would snapshot a world in place",
                     path, (unsigned long long)id.world_id, verb);
            return refuse(why, hint);
        }
        char why[WFS_PATH_MAX + 128];
        snprintf(why, sizeof why,
                 "refusing to %s %s: it is /, $HOME, inside the store, or inside another world", verb, path);
        return refuse(why, "pick a plain project directory outside ~/worlds and outside the store");
    }
    if (rc == WFS_E_CROSS_VOLUME) {
        // A snapshot has to live in the store, so for init the only fix is a store on the
        // source's volume. A fork's target is a free path, so --copy (a real per-file copy)
        // is also an answer there.
        char why[WFS_PATH_MAX + 128];
        snprintf(why, sizeof why, "%s and the %s are on different volumes: clonefile(2) returns EXDEV",
                 path, strcmp(verb, "fork") ? "store" : "fork target");
        if (!strcmp(verb, "fork"))
            snprintf(hint, sizeof hint, "world fs fork --to <path on the same volume>   (or --copy to really copy)");
        else
            snprintf(hint, sizeof hint, "world --store <dir on the same volume as %s> fs %s %s", path, verb, path);
        return refuse(why, hint);
    }
    if (rc == WFS_E_LOW_SPACE) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "not enough free space to %s %s", verb, path);
        return refuse(why, strcmp(verb, "fork") ? "free space on the store's volume"
                                                : "free space, or pass --skip-space-check");
    }
    if (rc == WFS_E_UNREGISTERED) {
        char why[WFS_PATH_MAX + 128];
        snprintf(why, sizeof why, "%s carries a .world marker but its inode is not the registered one: it is a copy", path);
        snprintf(hint, sizeof hint, "world fs adopt %s", path);
        return refuse(why, hint);
    }
    if (rc == WFS_E_NOT_A_WORLD) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "%s is not a world (no .world marker)", path);
        return refuse(why, "world fs list");
    }
    return fail(verb, rc);
}

// ---- commands ----------------------------------------------------------------------------------

static int cmd_init(wfs_store *s, int argc, char **argv) {
    const char *dir = NULL;
    wfs_snapshot_opts opts;
    memset(&opts, 0, sizeof opts);
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) opts.name = argv[++i];
        else if (!strcmp(argv[i], "--hard")) opts.hard = 1;
        else if (argv[i][0] != '-' && !dir) dir = argv[i];
        else usage();
    }
    if (!dir) usage();
    int rc = wfs_path_check(s, dir, 0);
    if (rc) return explain_path(s, dir, rc, "init");
    if ((rc = wfs_store_clone_probe(s, dir))) return explain_path(s, dir, rc, "init");
    wfs_id id = 0;
    if ((rc = wfs_snapshot_create(s, dir, &opts, &id))) return explain_path(s, dir, rc, "init");
    wfs_snapshot_rec r;
    if (wfs_snapshot_info(s, id, &r) == 0) {
        printf("S%llu  %s  %llu entries  (%s)\n", (unsigned long long)id, r.name,
               (unsigned long long)r.entries, r.hard ? "hard: UF_IMMUTABLE per entry" : "gated 0000");
        // P9 (T2.5): clonefile still breaks every hardlink, but the groups that live entirely
        // inside the tree are written down here and rebuilt inside every clone of this
        // snapshot. Only the ones reaching outside it are a warning now.
        if (r.hardlinks && r.hl_external)
            fprintf(stderr,
                    "world: warning: %llu entries in this tree have more than one link; %llu "
                    "hardlink groups are rebuilt in every fork, but %llu of those entries also "
                    "have names outside this tree and will be independent copies (P9)\n",
                    (unsigned long long)r.hardlinks, (unsigned long long)r.hl_groups,
                    (unsigned long long)r.hl_external);
        else if (r.hardlinks)
            fprintf(stderr,
                    "world: note: %llu entries in this tree have more than one link; the %llu "
                    "hardlink groups they form are rebuilt inside every fork (P9)\n",
                    (unsigned long long)r.hardlinks, (unsigned long long)r.hl_groups);
    } else {
        printf("S%llu\n", (unsigned long long)id);
    }
    return EX_OK;
}

static int latest_snapshot(wfs_store *s, wfs_id *out);

// ---- T1.5: the pre-clone pool ----------------------------------------------------------------
//
// A fork served from the pool leaves it one entry poorer, so the fork re-fills it in a detached
// background process: the next fork is fast again and this one does not pay for it. The filler
// is an ordinary `world fs pool fill`, so there is nothing to keep running and nothing to
// supervise; the store-level flock in the core makes two of them one.

#define POOL_DEFAULT_TARGET 2

static char g_exe[WFS_PATH_MAX];

static void resolve_exe(const char *argv0) {
#ifdef __APPLE__
    uint32_t n = (uint32_t)sizeof g_exe;
    if (_NSGetExecutablePath(g_exe, &n) == 0) {
        char r[WFS_PATH_MAX];
        if (realpath(g_exe, r)) snprintf(g_exe, sizeof g_exe, "%s", r);
        return;
    }
#endif
    snprintf(g_exe, sizeof g_exe, "%s", (argv0 && *argv0) ? argv0 : "world");
}

// How many entries a fork tries to keep ready. $WORLD_POOL_TOPUP overrides it; 0 turns the
// automatic top-up off (the benchmarks use that to keep a measured series honest).
static int pool_topup_target(void) {
    const char *e = getenv("WORLD_POOL_TOPUP");
    if (!e || !*e) return POOL_DEFAULT_TARGET;
    int v = atoi(e);
    return v < 0 ? 0 : v;
}

// The one way this CLI starts background work. Double fork: the intermediate child is reaped
// right here, and the grandchild is reparented to launchd, so it outlives this command without
// ever becoming a zombie. stdout and stderr go to <store>/logs/<logname>; stdin to /dev/null.
// `tail` is the argv after `world --store <dir> fs`, NULL-terminated.
static void spawn_detached(wfs_store *s, const char *logname, const char *const tail[]) {
    if (!g_exe[0]) return;
    char logp[WFS_PATH_MAX];
    snprintf(logp, sizeof logp, "%s/logs/%s", wfs_store_dir(s), logname);
    char store[WFS_PATH_MAX];
    snprintf(store, sizeof store, "%s", wfs_store_dir(s));
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} return; }
    setsid();
    if (fork() != 0) _exit(0);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) dup2(devnull, 0);
    int log = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log < 0) log = devnull;
    if (log >= 0) { dup2(log, 1); dup2(log, 2); }
    if (devnull > 2) close(devnull);
    if (log > 2) close(log);
    char *av[16];
    int k = 0;
    av[k++] = (char *)"world";
    av[k++] = (char *)"--store";
    av[k++] = store;
    av[k++] = (char *)"fs";
    for (int i = 0; tail[i] && k < 15; ++i) av[k++] = (char *)tail[i];
    av[k] = NULL;
    execv(g_exe, av);
    _exit(127);
}

static void spawn_pool_fill(wfs_store *s, wfs_id snap, int target) {
    if (target <= 0) return;
    char sid[32], cnt[32];
    snprintf(sid, sizeof sid, "S%llu", (unsigned long long)snap);
    snprintf(cnt, sizeof cnt, "%d", target);
    const char *tail[] = {"pool", "fill", sid, "--count", cnt, NULL};
    spawn_detached(s, "pool.log", tail);
}

// ---- T2.1: the background collector ----------------------------------------------------------
//
// `discard` is a rename; the unlinking behind it is minutes of work (measured: 525 s for 1000
// worlds of 10k entries). Anything that leaves due work in the trash therefore starts a worker,
// unless one is already on it -- the store-level flock in the core would make a second one exit
// immediately anyway, and that would still cost this command a process start.
static int gc_work_waiting(wfs_store *s, int64_t retention, int *worker_running) {
    return wfs_gc_pending(s, retention, worker_running);
}

// Start one, whatever the trash looks like. The caller has already decided there is work --
// a gc report that says work_remains, or a worker handing over to its successor -- and since
// the 6th round of the PR #1 review that work can be stale pool entries, which wfs_gc_pending()
// (a trash question) knows nothing about.
static void spawn_gc_worker_now(wfs_store *s, int64_t retention) {
    // The worker has to inherit the retention this command was given, or a `gc --retention 0`
    // would hand over work that the worker then decides is not due yet.
    char days[32];
    snprintf(days, sizeof days, "%.6f", (double)retention / 86400.0);
    const char *tail_r[] = {"gc", "--worker", "--retention", days, NULL};
    const char *tail_d[] = {"gc", "--worker", NULL};
    spawn_detached(s, "gc.log", retention >= 0 ? tail_r : tail_d);
}

static void spawn_gc_worker(wfs_store *s, int64_t retention) {
    int running = 0;
    if (!gc_work_waiting(s, retention, &running) || running) return;
    spawn_gc_worker_now(s, retention);
}

static int cmd_pool(wfs_store *s, int argc, char **argv) {
    if (argc < 1) usage();
    const char *verb = argv[0];
    if (!strcmp(verb, "status")) {
        size_t n = 0;
        wfs_pool_status(s, NULL, 0, &n);
        if (!n) { printf("pool: empty\n"); return EX_OK; }
        wfs_pool_stat *v = (wfs_pool_stat *)calloc(n, sizeof *v);
        if (!v) return fail("pool status", -ENOMEM);
        wfs_pool_status(s, v, n, &n);
        printf("%-6s %-20s %7s %9s %7s %10s  %s\n", "SNAP", "NAME", "READY", "BUILDING", "STALE",
               "ENTRIES", "NEWEST");
        for (size_t i = 0; i < n; ++i) {
            char id[16], t[32];
            snprintf(id, sizeof id, "S%llu", (unsigned long long)v[i].snapshot);
            fmt_time(t, sizeof t, v[i].newest_at);
            printf("%-6s %-20s %7llu %9llu %7llu %10llu  %s\n", id, v[i].snapshot_name,
                   (unsigned long long)v[i].ready, (unsigned long long)v[i].building,
                   (unsigned long long)v[i].stale, (unsigned long long)v[i].entries, t);
        }
        free(v);
        return EX_OK;
    }
    if (!strcmp(verb, "fill")) {
        wfs_ref r = {WFS_K_NONE, 0};
        int target = POOL_DEFAULT_TARGET;
        for (int i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--count") && i + 1 < argc) target = atoi(argv[++i]);
            else if (argv[i][0] != '-' && r.kind == WFS_K_NONE) r = parse_ref(argv[i]);
            else usage();
        }
        if (r.kind != WFS_K_SNAPSHOT || !r.id) {
            // The default target is the same one `fork` uses: the newest snapshot.
            wfs_id sid = 0;
            if (r.kind != WFS_K_NONE || latest_snapshot(s, &sid) != 0)
                return refuse("pool fill needs a snapshot", "world fs pool fill S<n> [--count K]");
            r.kind = WFS_K_SNAPSHOT;
            r.id = sid;
        }
        if (target < 0 || target > 4096) return refuse("--count is out of range", "--count 0..4096");
        uint64_t made = 0;
        int rc = wfs_pool_fill(s, r.id, target, &made);
        if (rc == WFS_E_POOL_BUSY)
            return refuse("another `world fs pool fill` is running for this store",
                          "wait for it to finish (see <store>/logs/pool.log)");
        if (rc == -ESTALE || rc == -ENOENT) {
            char why[96];
            snprintf(why, sizeof why, "S%llu is not an active snapshot", (unsigned long long)r.id);
            return refuse(why, "world fs list");
        }
        // PR #1 review (6th round): the entry would be a clone with the snapshot's hardlinks
        // broken, handed to the next fork as a faithful one. The filler drops it instead.
        if (rc == WFS_E_SNAPSHOT_DIRTY) {
            char why[192], hint[64];
            snprintf(why, sizeof why,
                     "S%llu's hardlink manifest is missing or damaged, so a pool entry cannot be "
                     "made to match it", (unsigned long long)r.id);
            snprintf(hint, sizeof hint, "world fs verify S%llu", (unsigned long long)r.id);
            return refuse(why, hint);
        }
        uint64_t ready = 0;
        wfs_pool_ready(s, r.id, &ready);
        if (rc) {
            fprintf(stderr, "world: pool fill S%llu: %s (made %llu, ready %llu)\n",
                    (unsigned long long)r.id, wfs_strerror(rc), (unsigned long long)made,
                    (unsigned long long)ready);
            return is_refusal(rc) ? EX_REFUSED : EX_ERR;
        }
        char t[32];
        fmt_time(t, sizeof t, (int64_t)time(NULL));
        printf("%s  pool fill S%llu: +%llu, %llu ready\n", t, (unsigned long long)r.id,
               (unsigned long long)made, (unsigned long long)ready);
        return EX_OK;
    }
    if (!strcmp(verb, "drain")) {
        wfs_ref r = {WFS_K_NONE, 0};
        int all = 0;
        for (int i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--all")) all = 1;
            else if (argv[i][0] != '-' && r.kind == WFS_K_NONE) r = parse_ref(argv[i]);
            else usage();
        }
        if (!all && r.kind != WFS_K_SNAPSHOT)
            return refuse("pool drain needs a snapshot", "world fs pool drain S<n>   (or --all)");
        uint64_t removed = 0;
        int rc = wfs_pool_drain(s, all ? 0 : r.id, &removed);
        if (rc == WFS_E_POOL_BUSY)
            return refuse("another `world fs pool fill` is running for this store",
                          "wait for it to finish (see <store>/logs/pool.log)");
        if (rc) return fail("pool drain", rc);
        printf("pool: %llu entries removed\n", (unsigned long long)removed);
        return EX_OK;
    }
    usage();
    return EX_USAGE;
}

static int latest_snapshot(wfs_store *s, wfs_id *out) {
    size_t n = 0;
    wfs_snapshot_list(s, NULL, 0, &n);
    if (!n) return -ENOENT;
    wfs_snapshot_rec *v = (wfs_snapshot_rec *)calloc(n, sizeof *v);
    if (!v) return -ENOMEM;
    wfs_snapshot_list(s, v, n, &n);
    *out = v[n - 1].id;
    free(v);
    return 0;
}

static int cmd_fork(wfs_store *s, int argc, char **argv) {
    const char *to = NULL, *name = NULL;
    wfs_ref from = {WFS_K_NONE, 0};
    wfs_fork_opts opts;
    memset(&opts, 0, sizeof opts);
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) {
            from = parse_ref(argv[++i]);
            if (from.kind == WFS_K_NONE) { fprintf(stderr, "world: bad --from '%s'\n", argv[i]); return EX_USAGE; }
        } else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--copy")) opts.allow_fallback = 1;
        else if (!strcmp(argv[i], "--no-pool")) opts.no_pool = 1;
        else if (!strcmp(argv[i], "--skip-space-check")) opts.skip_space_check = 1;
        else if (!strcmp(argv[i], "--force")) opts.force = 1;
        else usage();
    }
    if (from.kind == WFS_K_NONE) {
        wfs_id sid = 0;
        if (latest_snapshot(s, &sid) != 0)
            return refuse("no snapshot to fork from", "world fs init <dir>");
        from.kind = WFS_K_SNAPSHOT;
        from.id = sid;
    }
    // Inherit the source's name when the caller gave none, so the default path reads well.
    char inherited[WFS_NAME_MAX] = {0};
    if (!name) {
        if (from.kind == WFS_K_SNAPSHOT) {
            wfs_snapshot_rec r;
            if (wfs_snapshot_info(s, from.id, &r) == 0) snprintf(inherited, sizeof inherited, "%s", r.name);
        } else {
            wfs_world_rec r;
            if (wfs_world_info(s, from.id, &r) == 0) snprintf(inherited, sizeof inherited, "%s", r.name);
        }
        if (inherited[0]) name = inherited;
    }
    opts.name = name;

    char target[WFS_PATH_MAX];
    int rc = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (to) {
            snprintf(target, sizeof target, "%s", to);
        } else {
            wfs_id next = 0;
            if ((rc = wfs_world_next_id(s, &next))) return fail("fork", rc);
            const char *home = getenv("HOME");
            snprintf(target, sizeof target, "%s/worlds/W%llu", home ? home : "/tmp", (unsigned long long)next);
            if ((rc = mkdir_p(target))) return fail("fork", rc);
            snprintf(target + strlen(target), sizeof target - strlen(target), "/%s",
                     (name && *name) ? name : "world");
        }
        wfs_fork_result res;
        memset(&res, 0, sizeof res);
        rc = wfs_world_create_ex(s, from, target, &opts, &res);
        if (rc == -EEXIST && !to) continue; // someone else took that id; ask again
        if (rc == WFS_E_WORLD_BUSY && from.kind == WFS_K_WORLD) return busy_refusal(s, from.id, "fork from");
        // PR #1 review (6th round): the snapshot's hardlink manifest is the only record of which
        // names share an inode, and clonefile(2) breaks every one of them. Unreadable, or
        // shorter than the row says, means this fork cannot be made to match the snapshot -- the
        // core refuses rather than publishing a tree with the links silently gone.
        if (rc == WFS_E_SNAPSHOT_DIRTY) {
            wfs_id sid = from.id;
            if (from.kind == WFS_K_WORLD) {
                wfs_world_rec wr;
                sid = wfs_world_info(s, from.id, &wr) == 0 ? wr.snapshot_id : 0;
            }
            char why[224], hint[64];
            snprintf(why, sizeof why,
                     "S%llu's hardlink manifest is missing or damaged: the fork would be a copy "
                     "with the hardlinks the snapshot records silently broken",
                     (unsigned long long)sid);
            snprintf(hint, sizeof hint, "world fs verify S%llu", (unsigned long long)sid);
            return refuse(why, hint);
        }
        if (rc) return explain_path(s, target, rc, "fork");
        printf("W%llu  %s%s", (unsigned long long)res.world, target, res.from_pool ? "  (pool)" : "");
        // P9 (T2.5): only worth a word when there was something to rebuild. A pool hit says
        // nothing because the filler did it when the entry was made.
        if (res.hardlinks) printf("  (%llu hardlinks rebuilt)", (unsigned long long)res.hardlinks);
        printf("\n");
        // Put back what this fork took, in the background, so the next one is fast too --
        // unless a filler is already at work, in which case spawning a second one would only
        // cost this fork a process start to have the child exit on the lock.
        if (res.from_pool && !opts.no_pool && !wfs_pool_filling(s))
            spawn_pool_fill(s, from.id, pool_topup_target());
        // T2.1: a fork is also a good moment to notice that the trash has work waiting and
        // nobody on it -- a machine that only ever forks would otherwise never collect. The
        // check is two indexed counts and one readdir (wfs_gc_pending), and it happens after
        // the result has been printed, not on the way to it.
        spawn_gc_worker(s, -1);
        return EX_OK;
    }
    return fail("fork", rc ? rc : -EEXIST);
}

static int cmd_checkpoint(wfs_store *s, int argc, char **argv) {
    wfs_id w = 0;
    wfs_snapshot_opts opts;
    memset(&opts, 0, sizeof opts);
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) opts.name = argv[++i];
        else if (!strcmp(argv[i], "--hard")) opts.hard = 1;
        else if (!strcmp(argv[i], "--force")) opts.force = 1;
        else if (argv[i][0] != '-' && !w) w = parse_world(argv[i]);
        else usage();
    }
    if (!w) usage();
    wfs_world_rec r;
    int rc = wfs_world_info(s, w, &r);
    if (rc) return fail("checkpoint", rc);
    if (r.state != WFS_ST_ACTIVE) {
        char why[128];
        snprintf(why, sizeof why, "W%llu is %s, not active", (unsigned long long)w, state_name(r.state));
        return refuse(why, r.state == WFS_ST_TRASHED ? "world fs restore W<n>" : "world fs list");
    }
    if (!r.present) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "W%llu is not at %s any more", (unsigned long long)w, r.path);
        return refuse(why, "world fs verify <the path it was moved to>");
    }
    wfs_id id = 0;
    if ((rc = wfs_snapshot_create(s, r.path, &opts, &id))) {
        if (rc == WFS_E_WORLD_BUSY) return busy_refusal(s, w, "checkpoint");
        return explain_path(s, r.path, rc, "checkpoint");
    }
    wfs_snapshot_rec sr;
    if (wfs_snapshot_info(s, id, &sr) == 0)
        printf("S%llu  %s  %llu entries  (from W%llu)\n", (unsigned long long)id, sr.name,
               (unsigned long long)sr.entries, (unsigned long long)w);
    else
        printf("S%llu\n", (unsigned long long)id);
    return EX_OK;
}

// ---- diff (T1.3) -----------------------------------------------------------------------------

static int diff_print(void *ctx, const wfs_diff_entry *e) {
    (void)ctx;
    printf("%c %s\n", (char)e->change, e->path);
    return 0;
}

// Only the reasons worth printing: the full scan is the default path (WFS_DF_SMALL_TREE) and
// `--full` is what the user asked for, so neither is news. The rest mean "you asked for
// O(changes) and could not have it", which P10 promises to say out loud.
static const char *fallback_reason(int f) {
    switch (f) {
    case WFS_DF_NO_CURSOR: return "no FSEvents cursor was recorded when this world was forked";
    case WFS_DF_FROM_WORLD:
        return "this world was forked from another world, so its cursor cannot cover what the "
               "parent had already changed";
    case WFS_DF_MUST_SCAN: return "the kernel asked for a rescan of a subtree (MustScanSubDirs)";
    case WFS_DF_DROPPED: return "FSEvents dropped events";
    case WFS_DF_WRAPPED: return "the FSEvents id space was reset";
    case WFS_DF_STALE: return "the cursor is older than this volume's FSEvents journal";
    case WFS_DF_TIMEOUT: return "the FSEvents replay did not finish in time";
    case WFS_DF_UNSUPPORTED: return "FSEvents is not available here";
    default: return NULL;
    }
}

static int cmd_diff(wfs_store *s, int argc, char **argv) {
    wfs_id w = 0;
    int flags = 0, stat_only = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--full")) flags |= WFS_DIFF_FULL;
        else if (!strcmp(argv[i], "--events")) flags |= WFS_DIFF_EVENTS;
        else if (!strcmp(argv[i], "--stat")) stat_only = 1;
        else if (!strcmp(argv[i], "--no-content")) flags |= WFS_DIFF_NO_CONTENT;
        else if (!strcmp(argv[i], "--no-xattr")) flags |= WFS_DIFF_NO_XATTR;
        else if (!strcmp(argv[i], "--all-xattrs")) flags |= WFS_DIFF_ALL_XATTRS;
        else if (argv[i][0] != '-' && !w) w = parse_world(argv[i]);
        else usage();
    }
    if (!w) usage();

    wfs_world_rec r;
    int rc = wfs_world_info(s, w, &r);
    if (rc) return fail("diff", rc);

    wfs_diff_stats st;
    // A diff never takes the world's lock: it only reads, so a world someone is working in is
    // diffable (WFS_E_WORLD_BUSY is not a diff refusal).
    rc = wfs_world_diff_ex(s, w, flags, stat_only ? NULL : diff_print, NULL, &st);
    if (rc == WFS_E_SOURCE_GONE) {
        char why[256];
        if (r.snapshot_id)
            snprintf(why, sizeof why,
                     "W%llu was forked from S%llu, which is no longer in the store: there is "
                     "nothing left to compare against",
                     (unsigned long long)w, (unsigned long long)r.snapshot_id);
        else
            snprintf(why, sizeof why, "W%llu has no source snapshot recorded", (unsigned long long)w);
        return refuse(why, "world fs list   (and `world fs checkpoint W<n>` to give it a new baseline)");
    }
    if (rc == WFS_E_WORLD_MISSING) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "W%llu is not at %s any more", (unsigned long long)w, r.path);
        return refuse(why, "world fs verify <the path it was moved to>");
    }
    if (rc == -ESTALE) {
        char why[128];
        snprintf(why, sizeof why, "W%llu is %s, not active", (unsigned long long)w, state_name(r.state));
        return refuse(why, r.state == WFS_ST_TRASHED ? "world fs restore W<n>" : "world fs list");
    }
    if (rc) return fail("diff", rc);

    // P10 is a promise, so say out loud when the event stream could not keep it: the answer is
    // the same either way, but the cost was not.
    const char *why = st.full_scan ? fallback_reason(st.fallback) : NULL;
    if (why) fprintf(stderr, "world: note: %s; compared both trees instead\n", why);

    // PR #1 review: an entry whose xattrs could not be read is reported as a change, never as
    // clean -- but the change is a guess, so say how many of them there were.
    if (st.xattr_errors)
        fprintf(stderr, "world: note: %llu path%s reported as metadata changes because their "
                        "extended attributes could not be read\n",
                (unsigned long long)st.xattr_errors, st.xattr_errors == 1 ? " was" : "s were");

    if (stat_only) {
        printf("%llu added, %llu modified, %llu deleted, %llu metadata-only\n",
               (unsigned long long)st.added, (unsigned long long)st.modified,
               (unsigned long long)st.deleted, (unsigned long long)st.meta);
        printf("%s, %llu paths compared, %llu files read, %.3f s\n",
               st.full_scan ? "full scan of both trees" : "FSEvents since the fork",
               (unsigned long long)st.compared, (unsigned long long)st.content_cmp,
               st.elapsed_us / 1e6);
    }
    return EX_OK;
}

static int cmd_list(wfs_store *s) {
    size_t n = 0;
    wfs_snapshot_list(s, NULL, 0, &n);
    if (n) {
        wfs_snapshot_rec *v = (wfs_snapshot_rec *)calloc(n, sizeof *v);
        wfs_snapshot_list(s, v, n, &n);
        printf("%-6s %-20s %10s %8s  %s\n", "SNAP", "NAME", "ENTRIES", "CREATED", "SOURCE");
        for (size_t i = 0; i < n; ++i) {
            char t[32];
            fmt_time(t, sizeof t, v[i].created_at);
            char id[16];
            snprintf(id, sizeof id, "S%llu", (unsigned long long)v[i].id);
            printf("%-6s %-20s %10llu %8s  %s\n", id, v[i].name, (unsigned long long)v[i].entries, t + 5,
                   v[i].src_path);
        }
        free(v);
    }
    n = 0;
    wfs_world_list(s, 1, NULL, 0, &n);
    if (n) {
        wfs_world_rec *v = (wfs_world_rec *)calloc(n, sizeof *v);
        wfs_world_list(s, 1, v, n, &n);
        if (n) printf("\n%-6s %-20s %-8s %-6s %-6s %s\n", "WORLD", "NAME", "STATE", "FROM", "HERE", "PATH");
        for (size_t i = 0; i < n; ++i) {
            char id[16], from[16];
            snprintf(id, sizeof id, "W%llu", (unsigned long long)v[i].id);
            if (v[i].parent_world) snprintf(from, sizeof from, "W%llu", (unsigned long long)v[i].parent_world);
            else if (v[i].snapshot_id) snprintf(from, sizeof from, "S%llu", (unsigned long long)v[i].snapshot_id);
            else snprintf(from, sizeof from, "-");
            printf("%-6s %-20s %-8s %-6s %-6s %s\n", id, v[i].name, state_name(v[i].state), from,
                   v[i].present ? "yes" : "NO", v[i].path);
        }
        free(v);
    }
    return EX_OK;
}

static int cmd_inspect(wfs_store *s, const char *arg) {
    wfs_ref r = parse_ref(arg);
    if (r.kind == WFS_K_NONE) usage();
    char t[32];
    if (r.kind == WFS_K_SNAPSHOT) {
        wfs_snapshot_rec v;
        int rc = wfs_snapshot_info(s, r.id, &v);
        if (rc) return fail("inspect", rc);
        fmt_time(t, sizeof t, v.created_at);
        printf("snapshot:  S%llu\nname:      %s\nstate:     %s\ncreated:   %s\n"
               "entries:   %llu (%llu with >1 link)\nprotection: %s\npath:      %s\nsource:    %s\n",
               (unsigned long long)v.id, v.name, state_name(v.state), t, (unsigned long long)v.entries,
               (unsigned long long)v.hardlinks,
               v.hard ? "hard (UF_IMMUTABLE on every entry)" : "gate (the root directory is 0000)",
               v.path, v.src_path);
        if (v.hardlinks)
            printf("hardlinks: %llu groups rebuilt in every fork, %llu entries also linked from "
                   "outside the tree (P9)\n",
                   (unsigned long long)v.hl_groups, (unsigned long long)v.hl_external);
        if (v.from_world) printf("from:      W%llu\n", (unsigned long long)v.from_world);
        return EX_OK;
    }
    wfs_world_rec v;
    int rc = wfs_world_info(s, r.id, &v);
    if (rc) return fail("inspect", rc);
    fmt_time(t, sizeof t, v.created_at);
    printf("world:     W%llu\nname:      %s\nstate:     %s\ncreated:   %s\npath:      %s\n"
           "present:   %s\ninode:     %llu (dev %llu)\nentries:   %llu\nfsevents:  %llu\n",
           (unsigned long long)v.id, v.name, state_name(v.state), t, v.path, v.present ? "yes" : "no",
           (unsigned long long)v.dir_ino, (unsigned long long)v.dir_dev, (unsigned long long)v.entries,
           (unsigned long long)v.fsevents_id);
    if (v.parent_world) printf("parent:    W%llu\n", (unsigned long long)v.parent_world);
    if (v.snapshot_id) printf("snapshot:  S%llu\n", (unsigned long long)v.snapshot_id);
    if (v.trashed_at) { fmt_time(t, sizeof t, v.trashed_at); printf("trashed:   %s\n", t); }
    return EX_OK;
}

// T2.2: `discard S<n>`. The refusal has to name what is holding the snapshot, because "in use"
// with no name is the least actionable message this CLI could print.
// PR #1 review (20th round, P1): `discard --now` on an entry whose working name is taken. The
// collector renames a trash entry to `<name>.deleting` before it unlinks it, and that rename now
// refuses to touch anything that is already there -- the fold that used to remove such a
// directory recursively could not tell a leftover of ours from the user's own, and for a world
// discarded from another volume the trash is `<parent>/.wfs-trash`, the user's directory, with
// an entirely predictable name. So nothing is deleted here and nothing is renamed over; the one
// thing left to do is name the directory that is in the way, because only its owner can decide
// what happens to it.
static int trash_blocked_refusal(wfs_store *s, wfs_id id, int is_snapshot) {
    char path[WFS_PATH_MAX];
    path[0] = 0;
    wfs_trash_blocked_path(s, id, is_snapshot, path, sizeof path);
    char why[WFS_PATH_MAX + 320];
    if (path[0])
        snprintf(why, sizeof why,
                 "%c%llu: %s is in the way. The collector renames a trash entry to that name "
                 "before deleting it, and this directory is not one it made -- so nothing here "
                 "will remove it or rename over it",
                 is_snapshot ? 'S' : 'W', (unsigned long long)id, path);
    else
        snprintf(why, sizeof why,
                 "%c%llu: a directory with the collector's working name (`<entry>.deleting`) is "
                 "in the way of this entry's deletion, and it is not one this store made",
                 is_snapshot ? 'S' : 'W', (unsigned long long)id);
    return refuse(why, "move that directory aside, then run the same command again"
                       "   (`world fs gc --status` names it too)");
}

// PR #1 review (25th round, P1): the entry at that path is not this record's tree. A world row
// has carried the dev/ino of its tree since the world was published and every rename on the way
// into the trash is same-volume, so the inode is the world's identity for as long as the entry
// exists -- while the NAME is not: a world discarded from another volume keeps its trash in
// `<parent>/.wfs-trash`, the user's own directory, under a name they can work out. So the tree
// can be moved away and a directory of theirs left in its place, and until this round the
// collector deleted that one, `--now` deleted it and reported success, and `restore` carried it
// home and wrote its dev/ino into the row. Nothing is touched now; what is owed is the path,
// because whoever moved the world's tree is the only one who can put it back.
static int trash_foreign_refusal(wfs_store *s, wfs_id id, int is_snapshot, const char *verb) {
    char path[WFS_PATH_MAX];
    path[0] = 0;
    wfs_trash_entry_path(s, id, is_snapshot, path, sizeof path);
    char why[WFS_PATH_MAX + 320];
    snprintf(why, sizeof why,
             "%c%llu: the directory at %s is not %c%llu's tree -- it has a different inode from "
             "the one this store recorded, so %s it would %s somebody else's data",
             is_snapshot ? 'S' : 'W', (unsigned long long)id,
             path[0] ? path : "that trash entry's path", is_snapshot ? 'S' : 'W',
             (unsigned long long)id, verb,
             !strcmp(verb, "restoring") ? "register" : "delete");
    return refuse(why, "put the tree back at that path (or move that directory aside and let the"
                       " record go)   (`world fs gc --status` names it too)");
}

static int cmd_discard_snapshot(wfs_store *s, wfs_id sid, int now, int force, int64_t retention) {
    wfs_snapshot_rec sr;
    int rc = wfs_snapshot_info(s, sid, &sr);
    if (rc) return fail("discard", rc);
    // PR #1 review (5th round): already in the trash is a refusal only without --now. With it,
    // this is the one command that brings the deletion forward, which is what it does for a
    // world and what the API has always documented for a snapshot.
    if (sr.state == WFS_ST_TRASHED && !now) {
        char why[96];
        snprintf(why, sizeof why, "S%llu is already in the trash", (unsigned long long)sid);
        return refuse(why, "world fs discard S<n> --now   (deletes it now instead of waiting)");
    }
    rc = wfs_snapshot_discard(s, sid, now, force);
    // Before the --force diagnosis below: this one is not about the pool at all, and the
    // snapshot has not been touched either (PR #1 review, 20th round).
    if (rc == WFS_E_TRASH_BLOCKED) return trash_blocked_refusal(s, sid, 1);
    if (rc == WFS_E_TRASH_FOREIGN) return trash_foreign_refusal(s, sid, 1, "deleting");
    if (rc == WFS_E_SNAPSHOT_IN_USE) {
        // Say who. Worlds first (they are the hard refusal), then pool entries.
        char why[512];
        size_t n = 0, listed = 0;
        char names[160] = {0};
        wfs_world_list(s, 0, NULL, 0, &n);
        if (n) {
            wfs_world_rec *v = (wfs_world_rec *)calloc(n, sizeof *v);
            if (v) {
                wfs_world_list(s, 0, v, n, &n);
                for (size_t i = 0; i < n; ++i) {
                    if (v[i].snapshot_id != sid || v[i].state != WFS_ST_ACTIVE) continue;
                    if (listed < 6)
                        snprintf(names + strlen(names), sizeof names - strlen(names), "%sW%llu",
                                 listed ? " " : "", (unsigned long long)v[i].id);
                    ++listed;
                }
                free(v);
            }
        }
        if (listed) {
            snprintf(why, sizeof why,
                     "S%llu is the source of %zu active world(s) (%s%s); discarding it would leave them "
                     "with nothing to diff or verify against",
                     (unsigned long long)sid, listed, names, listed > 6 ? " ..." : "");
            return refuse(why, "discard those worlds first, or `world fs checkpoint` them to a new baseline");
        }
        uint64_t ready = 0;
        wfs_pool_ready(s, sid, &ready);
        if (!ready) {
            // Neither an active world nor a pool entry: a fork from this snapshot is in flight
            // and has its half-built world row in the book (the core counts those, so that a
            // world can never be published with its baseline already in the trash).
            snprintf(why, sizeof why,
                     "a fork from S%llu is in flight; discarding it now would leave that world "
                     "with nothing to diff or verify against",
                     (unsigned long long)sid);
            return refuse(why, "world fs list   (retry once the fork has finished)");
        }
        snprintf(why, sizeof why, "S%llu still has %llu pre-cloned pool entr%s",
                 (unsigned long long)sid, (unsigned long long)ready, ready == 1 ? "y" : "ies");
        char hint[96];
        snprintf(hint, sizeof hint, "world fs discard S%llu --force   (drains the pool first)",
                 (unsigned long long)sid);
        return refuse(why, hint);
    }
    // PR #1 review (16th round, P2): --force drains the pool before the reference count is
    // taken, and an entry whose tree will not go (an ACL, an EPERM, a transient EIO) now keeps
    // its row and fails the discard rather than leaving a row-less clone under <store>/pool
    // that nothing would ever come back for. The snapshot has not been touched, so what is left
    // to say is which directory is in the way and that the same command is the retry.
    if (force && rc && rc != WFS_E_SNAPSHOT_IN_USE && rc != -ESTALE) {
        uint64_t left = 0;
        wfs_pool_stat pst[64];
        size_t pn = 0;
        if (wfs_pool_status(s, pst, 64, &pn) == 0)
            for (size_t i = 0; i < pn && i < 64; ++i)
                if (pst[i].snapshot == sid) left = pst[i].ready + pst[i].building + pst[i].stale;
        if (left) {
            char why[640];
            snprintf(why, sizeof why,
                     "S%llu: a pre-cloned pool entry under %s/pool/S%llu could not be removed: %s",
                     (unsigned long long)sid, wfs_store_dir(s), (unsigned long long)sid,
                     wfs_strerror(rc));
            char hint[192];
            snprintf(hint, sizeof hint,
                     "fix that, then `world fs discard S%llu --force` again   (the snapshot is untouched)",
                     (unsigned long long)sid);
            return refuse(why, hint);
        }
    }
    if (rc == -ESTALE) {
        // PR #1 review (9th round): with --now this can also mean the row moved on while the
        // discard was following its tree, so the state is re-read rather than reported from the
        // snapshot taken before the call.
        wfs_snapshot_rec after;
        int st = wfs_snapshot_info(s, sid, &after) == 0 ? after.state : sr.state;
        char why[96];
        snprintf(why, sizeof why, "S%llu is %s, not active", (unsigned long long)sid, state_name(st));
        return refuse(why, "world fs list");
    }
    if (rc) return fail("discard", rc);
    if (now) {
        printf("S%llu deleted\n", (unsigned long long)sid);
        return EX_OK;
    }
    // PR #1 review (3rd round): a snapshot whose tree had already vanished is reconciled straight
    // to DEAD, because there is nothing to move and nothing for a collector to find. Say that,
    // instead of promising a retention period that no longer applies to anything.
    wfs_snapshot_rec after;
    if (wfs_snapshot_info(s, sid, &after) == 0 && after.state == WFS_ST_DEAD) {
        printf("S%llu: its tree was already gone; the row is now dead\n", (unsigned long long)sid);
        return EX_OK;
    }
    printf("S%llu moved to the trash; the collector deletes it after the retention period\n",
           (unsigned long long)sid);
    spawn_gc_worker(s, retention);
    return EX_OK;
}

static int cmd_discard(wfs_store *s, int argc, char **argv) {
    wfs_id w = 0;
    int now = 0, force = 0;
    int64_t retention = -1;
    wfs_ref target = {WFS_K_NONE, 0};
    // Flags may follow the id, so the whole line is parsed before anything is decided.
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--now")) now = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--retention") && i + 1 < argc) retention = (int64_t)(atof(argv[++i]) * 86400.0);
        else if (argv[i][0] != '-' && target.kind == WFS_K_NONE) target = parse_ref(argv[i]);
        else usage();
    }
    if (target.kind == WFS_K_SNAPSHOT && target.id) return cmd_discard_snapshot(s, target.id, now, force, retention);
    if (target.kind != WFS_K_WORLD || !target.id) usage();
    w = target.id;
    wfs_world_rec r;
    if (wfs_world_info(s, w, &r) == 0 && r.state == WFS_ST_ACTIVE && !r.present) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "W%llu is not at %s any more; refusing to discard something I cannot see",
                 (unsigned long long)w, r.path);
        return refuse(why, "world fs verify <the path it was moved to>");
    }
    int rc = wfs_world_discard(s, w, now, force);
    if (rc == WFS_E_TRASH_BLOCKED) return trash_blocked_refusal(s, w, 0);
    if (rc == WFS_E_TRASH_FOREIGN) return trash_foreign_refusal(s, w, 0, "deleting");
    if (rc == WFS_E_WORLD_BUSY) return busy_refusal(s, w, "discard");
    if (rc == WFS_E_UNREGISTERED) return explain_path(s, r.path, rc, "discard");
    if (rc == -ESTALE) {
        // PR #1 review (9th round): the row moved on while this discard was following its tree
        // -- a `restore` that won the race, or a collector that finished the entry first.
        // Nothing was deleted and nothing was buried; say which state it is in now.
        wfs_world_rec after;
        int st = wfs_world_info(s, w, &after) == 0 ? after.state : r.state;
        char why[160];
        snprintf(why, sizeof why, "W%llu is %s now; it moved on while this discard was running",
                 (unsigned long long)w, state_name(st));
        return refuse(why, "world fs list");
    }
    if (rc) return fail("discard", rc);
    if (now) printf("W%llu deleted\n", (unsigned long long)w);
    else {
        printf("W%llu moved to the trash; `world fs restore W%llu` brings it back\n",
               (unsigned long long)w, (unsigned long long)w);
        // T2.1: the rename is done, the unlinking is not. If anything in the trash is already
        // past its retention, start the collector now rather than at the next `gc`.
        spawn_gc_worker(s, retention);
    }
    return EX_OK;
}

static int cmd_restore(wfs_store *s, const char *arg) {
    wfs_id w = parse_world(arg);
    wfs_world_rec r;
    int rc = wfs_world_info(s, w, &r);
    if (rc) return fail("restore", rc);
    rc = wfs_world_restore(s, w);
    if (rc == -EEXIST) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "%s exists again; restoring W%llu would overwrite it", r.path,
                 (unsigned long long)w);
        return refuse(why, "move that directory aside, then retry");
    }
    if (rc == -ESTALE) {
        char why[64];
        snprintf(why, sizeof why, "W%llu is %s, not trashed", (unsigned long long)w, state_name(r.state));
        return refuse(why, "world fs list");
    }
    if (rc == WFS_E_TRASH_DELETING) {
        char why[160];
        snprintf(why, sizeof why,
                 "W%llu is already being deleted by the collector; what is left of it is not a world",
                 (unsigned long long)w);
        return refuse(why, "world fs fork --from <a snapshot>   (and `world fs gc --status` to watch)");
    }
    if (rc == WFS_E_TRASH_FOREIGN) return trash_foreign_refusal(s, w, 0, "restoring");
    if (rc == WFS_E_SOURCE_GONE) {
        char why[192];
        snprintf(why, sizeof why,
                 "W%llu was forked from S%llu, which has since been discarded: restoring it would "
                 "produce a world with no baseline to diff or verify against",
                 (unsigned long long)w, (unsigned long long)r.snapshot_id);
        return refuse(why, "world fs list   (the snapshot is gone; fork from a live one instead)");
    }
    if (rc) return fail("restore", rc);
    printf("W%llu restored to %s\n", (unsigned long long)w, r.path);
    return EX_OK;
}

// How much one wake of the background worker does before it exits. Both limits apply, whichever
// comes first; a wake that runs out of budget hands over to a fresh successor, so the trash
// always drains even if nobody runs another command.
static int gc_batch_entries(void) {
    const char *e = getenv("WORLD_GC_BATCH");
    int v = (e && *e) ? atoi(e) : 64;
    return v < 1 ? 1 : v;
}
static int gc_batch_secs(void) {
    const char *e = getenv("WORLD_GC_BATCH_SECS");
    int v = (e && *e) ? atoi(e) : 2;
    return v < 1 ? 1 : v;
}
// P16, and the knob that actually buys it. Measured on 27.0 (docs/TASKS.md T2.1): a collector
// unlinking flat out costs a concurrent `fork` +51..57% at 3-4 threads and +15..17% even at one
// thread -- the floor is APFS metadata contention, not CPU and not disk bandwidth, which is why
// setiopolicy_np() changes nothing. What does work is not running all the time: 2 s of work
// followed by a 2 s gap halves the contention window and brings the foreground cost to ~5%,
// at the price of a drain that takes about twice as long. It is background work; it can wait.
static long gc_pause_ms(void) {
    const char *e = getenv("WORLD_GC_PAUSE_MS");
    long v = (e && *e) ? atol(e) : 2000;
    return v < 0 ? 0 : v;
}

static void gc_log_line(const wfs_gc_report *rep, double secs) {
    char t[32];
    fmt_time(t, sizeof t, (int64_t)time(NULL));
    // CPU as well as wall: the whole point of the worker is that the machine is doing this work
    // somewhere else, and "somewhere else" still costs something. Summing this column over
    // <store>/logs/gc.log is how a drain's total cost gets measured.
    struct rusage ru;
    double cpu = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    // What this wake leaves behind. An entry it could not delete is never "trash empty":
    // something is still in there and the next command has to be able to see that (PR #1 review).
    char state[224];
    // PR #1 review (20th round): an entry blocked by a directory sitting on its `.deleting`
    // working name is one more thing still in the trash, so it belongs in the same count -- with
    // its own reason, because no number of retries will clear that one.
    // PR #1 review (25th round): and so is an entry whose path holds something that is not its
    // tree -- nothing was deleted there either, and for the same kind of reason.
    uint64_t stuck = rep->trash_failed + rep->trash_blocked + rep->trash_foreign;
    if (stuck)
        snprintf(state, sizeof state, "  (%llu entr%s could not be deleted%s%s%s)",
                 (unsigned long long)stuck, stuck == 1 ? "y" : "ies",
                 rep->trash_blocked ? ", a directory in the way of the collector's working name" : "",
                 rep->trash_foreign ? ", a directory that is not the recorded tree" : "",
                 rep->work_remains ? ", retrying" : "; left in the trash");
    else
        snprintf(state, sizeof state, "%s",
                 rep->work_remains ? "  (work remains, handing over)" : "  (trash empty)");
    printf("%s  gc worker: %llu worlds, %llu snapshots, %llu orphans, %llu entries unlinked in "
           "%.2f s wall / %.2f s cpu%s\n",
           t, (unsigned long long)rep->worlds_deleted, (unsigned long long)rep->snapshots_deleted,
           (unsigned long long)rep->trash_orphans, (unsigned long long)rep->entries_freed, secs, cpu,
           state);
    fflush(stdout);
}

// How many unlink threads. 4 is the APFS metadata sweet spot the clone walk found; deletion is
// measured separately in docs/TASKS.md (T2.1) because more threads there buy less and cost the
// foreground more.
static int gc_threads(void) {
    const char *e = getenv("WORLD_GC_THREADS");
    int v = (e && *e) ? atoi(e) : 4;
    return v < 1 ? 1 : (v > 16 ? 16 : v);
}

// The collector asks the kernel to put it behind everybody else: IOPOL_THROTTLE is what
// Spotlight and Time Machine use, plus nice +5.
//
// Measured, and worth writing down: this changes *nothing*. normal / utility / throttle all give
// the same drain rate and the same 57% penalty on a concurrent `fork` (docs/TASKS.md T2.1). The
// contention is APFS metadata transactions, not disk bandwidth and not CPU, and the disk I/O
// throttle has no opinion about those. What does work is the duty cycle in gc_pause_ms(). This
// is kept because it costs nothing and is the right declaration of intent, not because it helps.
static void gc_lower_priority(void) {
#ifdef __APPLE__
    const char *e = getenv("WORLD_GC_IOPOL");
    const char *want = (e && *e) ? e : "throttle";
    int pol = -1;
    if (!strcmp(want, "throttle")) pol = IOPOL_THROTTLE;
    else if (!strcmp(want, "utility")) pol = IOPOL_UTILITY;
    else if (!strcmp(want, "standard")) pol = IOPOL_STANDARD;
    if (pol >= 0) setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, pol);
    if (strcmp(want, "normal") != 0) setpriority(PRIO_PROCESS, 0, 5);
#endif
}

// One wake of the detached worker: take the store's gc lock, delete a bounded batch, exit. If
// the batch limit cut it short, start a successor -- that is the whole scheduler.
static int cmd_gc_worker(wfs_store *s, int64_t retention, int reconcile) {
    gc_lower_priority();
    long pause = gc_pause_ms();
    if (pause > 0) { struct timespec ts = {pause / 1000, (pause % 1000) * 1000000L}; nanosleep(&ts, NULL); }
    wfs_gc_opts o;
    memset(&o, 0, sizeof o);
    o.retention_secs = retention;
    o.threads = gc_threads();
    o.max_entries = (uint64_t)gc_batch_entries();
    o.max_secs = gc_batch_secs();
    o.flags = WFS_GC_BACKGROUND | (reconcile ? WFS_GC_RECONCILE : 0);
    wfs_gc_report rep;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = wfs_gc_ex(s, &o, &rep);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc == WFS_E_GC_BUSY) return EX_OK;   // someone else is on it; nothing to say
    if (rc) return fail("gc worker", rc);
    gc_log_line(&rep, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    if (rep.work_remains) spawn_gc_worker_now(s, retention);
    return EX_OK;
}

static int cmd_gc_status(wfs_store *s, int64_t retention) {
    wfs_trash_stat ts;
    int rc = wfs_gc_status(s, retention, &ts);
    if (rc) return fail("gc --status", rc);
    char est[32], freeb[32];
    fmt_bytes(est, sizeof est, ts.bytes_estimate);
    fmt_bytes(freeb, sizeof freeb, ts.volume_free_bytes);
    printf("trash:     %llu entries (%llu worlds, %llu snapshots), %llu due, %llu being deleted\n"
           "contents:  %llu tree entries, ~%s of clone metadata (df cannot see block sharing)\n"
           "volume:    %s free\n",
           (unsigned long long)ts.entries, (unsigned long long)ts.worlds,
           (unsigned long long)ts.snapshots, (unsigned long long)ts.due,
           (unsigned long long)ts.deleting, (unsigned long long)ts.tree_entries, est, freeb);
    if (ts.worker_pid) {
        char t[32];
        fmt_time(t, sizeof t, ts.worker_started_at);
        printf("worker:    pid %lld since %s, %llu done / %llu left in this batch\n",
               (long long)ts.worker_pid, t, (unsigned long long)ts.worker_done,
               (unsigned long long)ts.worker_remaining);
    } else {
        printf("worker:    none%s\n", (ts.due + ts.deleting) ? " (work waiting: `world fs gc` starts one)" : "");
    }
    // Not trash, but waiting for the same collector: a fork that died leaves its half-built
    // clone in the user's own directory, and only its CREATING row knows the name (PR #1 review).
    // Since the 7th round a half-built snapshot gc could not remove is counted here too -- its
    // S<n> is not swept by anything either, so the row is the only record of it. And since the
    // 11th, the other half of that family: a `*.wfs-tmp` under <store>/snapshots that no row
    // names, which is the suffix sweep's to remove and was counted by nothing while it stayed.
    if (ts.creating_stranded)
        printf("abandoned: %llu half-built tree%s still on disk (`world fs gc` retries them)\n",
               (unsigned long long)ts.creating_stranded, ts.creating_stranded == 1 ? "" : "s");
    // Nor are these: stale pre-clone entries under <store>/pool, whose snapshot is gone or is a
    // different snapshot now. Removing one is a whole tree, so a bounded wake can leave some
    // behind for its successor (PR #1 review, 6th round).
    if (ts.pool_stranded)
        printf("pool:      %llu stale pre-clone entr%s waiting for the collector\n",
               (unsigned long long)ts.pool_stranded, ts.pool_stranded == 1 ? "y" : "ies");
    // PR #1 review (27th/28th/29th rounds, P2): and what the counts above could not look at. An
    // unreadable <store>/trash, <store>/snapshots, <store>/tmp, pool root or S<n> used to be
    // passed over in silence, so this report said nothing at all while a row-less tree sat in there -- "0 stale
    // entries" read as "the store is clean" when the truth was "it could not be read". The
    // directory and the errno, because whatever is to be done about it is done by whoever owns
    // that directory.
    if (ts.dirs_unreadable)
        printf("unread:    %llu director%s under the store could not be read: %s (%s)\n"
               "           what is in there is not counted above (`world fs gc` retries it)\n",
               (unsigned long long)ts.dirs_unreadable, ts.dirs_unreadable == 1 ? "y" : "ies",
               ts.dirs_unreadable_path[0] ? ts.dirs_unreadable_path : "a directory of the store",
               strerror(ts.dirs_unreadable_errno));
    // PR #1 review (20th round, P1): and the due entries the collector will not start on,
    // because a directory it did not put there holds the `<entry>.deleting` name it renames to.
    // Naming it is the whole point: it is as likely to be in the user's own `.wfs-trash` (a
    // world discarded from another volume keeps its trash beside itself) as in <store>/trash,
    // and only its owner can say what should happen to it.
    if (ts.trash_blocked)
        printf("blocked:   %llu trash entr%s cannot be collected: %s is in the way"
               " (not a directory this store made; move it aside)\n",
               (unsigned long long)ts.trash_blocked, ts.trash_blocked == 1 ? "y" : "ies",
               ts.blocked_path[0] ? ts.blocked_path : "a `<entry>.deleting` directory");
    // PR #1 review (25th round, P1): and the entries whose path holds a directory that is not
    // the tree the record was written for. The path is the point: for a world discarded across
    // volumes the entry is in the user's own `.wfs-trash` under a name they can work out, so
    // this line is as likely to be about something they put there themselves as about a tree
    // that was moved away -- and either way nothing moves again until somebody looks.
    if (ts.trash_foreign)
        printf("foreign:   %llu trash entr%s cannot be collected: %s is not the tree that record"
               " was written for (a different inode; nothing here will touch it)\n",
               (unsigned long long)ts.trash_foreign, ts.trash_foreign == 1 ? "y" : "ies",
               ts.foreign_path[0] ? ts.foreign_path : "what is at its path");
    return EX_OK;
}

static int cmd_gc(wfs_store *s, int argc, char **argv) {
    int64_t retention = -1;
    int now = 0, status = 0, worker = 0, reconcile = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--retention") && i + 1 < argc) retention = (int64_t)(atof(argv[++i]) * 86400.0);
        else if (!strcmp(argv[i], "--now")) now = 1;
        else if (!strcmp(argv[i], "--status")) status = 1;
        else if (!strcmp(argv[i], "--worker")) worker = 1;
        else if (!strcmp(argv[i], "--reconcile")) reconcile = 1;
        else usage();
    }
    if (status) return cmd_gc_status(s, retention);
    if (worker) return cmd_gc_worker(s, retention, reconcile);

    wfs_gc_opts o;
    memset(&o, 0, sizeof o);
    o.retention_secs = retention;
    o.threads = gc_threads();
    // Without --now the trash is left to the worker: unlinking it is minutes of work and no
    // interactive command should sit on that (T2.1, P16). Everything cheap still happens here.
    o.flags = (now ? 0 : WFS_GC_NO_TRASH) | (reconcile ? WFS_GC_RECONCILE : 0);
    // PR #1 review (6th round): and "everything cheap" is not quite everything -- a stale pool
    // entry is a whole clone of a snapshot to unlink. The interactive run takes the worker's own
    // batch limit so it cannot sit there for minutes either; what it does not finish sets
    // work_remains and goes to the worker spawned below. With --now the caller has asked for the
    // whole job, so there is no limit.
    if (!now) o.max_secs = gc_batch_secs();
    wfs_gc_report rep;
    int rc = wfs_gc_ex(s, &o, &rep);
    if (rc) return fail("gc", rc);
    printf("gc: %llu worlds deleted, %llu snapshots deleted, %llu half-built trees, %llu orphan trash dirs,"
           " %llu pool entries\n",
           (unsigned long long)rep.worlds_deleted, (unsigned long long)rep.snapshots_deleted,
           (unsigned long long)rep.tmp_removed, (unsigned long long)rep.trash_orphans,
           (unsigned long long)rep.pool_removed);
    if (rep.snapshots_dangling || rep.worlds_dangling) {
        if (reconcile)
            printf("reconciled: %llu snapshots and %llu worlds whose tree is gone are now DEAD\n",
                   (unsigned long long)rep.snapshots_reconciled, (unsigned long long)rep.worlds_reconciled);
        else
            fprintf(stderr,
                    "world: note: %llu snapshot(s) and %llu world(s) are registered but their tree is\n"
                    "world:       not on disk. `world fs gc --reconcile` marks them dead. A world that was\n"
                    "world:       only moved looks the same from here: `world fs verify <its new path>`\n"
                    "world:       repairs that one instead (P1).\n",
                    (unsigned long long)rep.snapshots_dangling, (unsigned long long)rep.worlds_dangling);
    }
    if (rep.snapshots_unreadable || rep.worlds_unreadable) {
        // PR #1 review (12th round): a row the scan could not reach a verdict about. It is not
        // reconciled and it is not silently counted as present either: --reconcile buries only
        // what is proven absent, and an operator who sees this is being told that the answer is
        // missing rather than that the answer is "gone".
        fprintf(stderr,
                "world: note: %llu snapshot(s) and %llu world(s) could not be checked: the path is\n"
                "world:       unreadable (a permission, an I/O error, a volume that is not mounted), or\n"
                "world:       something that is not a directory is in the way. They are left registered --\n"
                "world:       an error is not an absence -- so fix the access and run this again.\n",
                (unsigned long long)rep.snapshots_unreadable, (unsigned long long)rep.worlds_unreadable);
    }
    if (rep.tmp_failed) {
        // PR #1 review (11th round): the `*.wfs-tmp` the suffix sweep could not remove is
        // counted here too, and that one has no row at all -- being named by nothing is what
        // makes it the sweep's -- so the note says what is true of both.
        fprintf(stderr,
                "world: note: %llu half-built tree%s could not be removed and %s still on disk.\n"
                "world:       %s still counted, and any row naming %s is kept; %s"
                "   (`world fs gc --status`)\n",
                (unsigned long long)rep.tmp_failed, rep.tmp_failed == 1 ? "" : "s",
                rep.tmp_failed == 1 ? "is" : "are", rep.tmp_failed == 1 ? "It is" : "They are",
                rep.tmp_failed == 1 ? "it" : "them",
                rep.work_remains ? "the collector will try again."
                                 : "it has failed too often to keep retrying by itself.");
    }
    if (rep.pool_failed) {
        // The same rule as the half-built trees above: a stale pre-clone entry that could not be
        // removed keeps its row, so that something in the store still knows the tree is rubbish.
        fprintf(stderr,
                "world: note: %llu stale pre-clone entr%s could not be removed and %s still under\n"
                "world:       <store>/pool. The record of %s is kept; %s   (`world fs gc --status`)\n",
                (unsigned long long)rep.pool_failed, rep.pool_failed == 1 ? "y" : "ies",
                rep.pool_failed == 1 ? "is" : "are", rep.pool_failed == 1 ? "it" : "them",
                rep.work_remains ? "the collector will try again."
                                 : "it has failed too often to keep retrying by itself.");
    }
    if (rep.dirs_unreadable) {
        // PR #1 review (27th/28th rounds, P2): and a directory the scan could not READ is
        // neither of the two above. Nothing was removed in there and nothing was counted there
        // either, so the only honest thing this run can say is which directory it could not
        // open and why -- the path and the errno come from a status read, which is the same
        // scan again. The trash, <store>/snapshots and the pool all arrive here: the collector
        // owes the same sentence for each of them.
        //
        // PR #1 review (33rd round, P2): and that second read can itself fail, so its rc is
        // looked at. It used to be dropped, and `strerror(0)` printed "Undefined error: 0" as
        // the reason a directory could not be read -- a note about a failure, ending in a
        // sentence invented by a second failure nobody was told about. The run's own count is
        // what is reported either way; only the path and the errno come from here, and when
        // they cannot be had the line says so instead of making them up.
        wfs_trash_stat us;
        memset(&us, 0, sizeof us);
        int urc = wfs_gc_status(s, retention, &us);
        fprintf(stderr,
                "world: note: %llu director%s under the store could not be read (%s: %s), so\n"
                "world:       whatever is in %s was neither collected nor counted. %s\n"
                "world:       (`world fs gc --status`)\n",
                (unsigned long long)rep.dirs_unreadable, rep.dirs_unreadable == 1 ? "y" : "ies",
                (urc == 0 && us.dirs_unreadable_path[0]) ? us.dirs_unreadable_path
                                                         : "a directory of the store",
                (urc == 0 && us.dirs_unreadable_errno) ? strerror(us.dirs_unreadable_errno)
                                                       : "the reason could not be read back",
                rep.dirs_unreadable == 1 ? "it" : "them",
                rep.work_remains ? "The collector will try again."
                                 : "It has failed too often to keep retrying by itself.");
    }
    if (rep.trash_blocked) {
        // PR #1 review (20th round, P1): not a failed deletion -- a deletion that was never
        // started, because a directory this store did not make is sitting at the `.deleting`
        // name the entry has to be renamed to. It used to be removed recursively as "a leftover
        // of ours", which since the 15th round it cannot be (the rename and the row that
        // records it commit together, so only one of the two names is ever ours). Nothing will
        // clear it but its owner, so the note says where to look.
        // 33rd round, P2: the rc is deliberately not tested here. The struct is zeroed first
        // and the only field used is a path, so a status read that fails falls through to the
        // same sentence an empty path gets -- there is nothing this note could report wrongly.
        wfs_trash_stat bs;
        memset(&bs, 0, sizeof bs);
        (void)wfs_gc_status(s, retention, &bs);
        fprintf(stderr,
                "world: note: %llu trash entr%s could not be collected: a directory is in the way of\n"
                "world:       the name the collector renames to before deleting (`<entry>.deleting`),\n"
                "world:       and it is not one this store made -- so nothing touched it%s%s\n"
                "world:       Move it aside and the next collection takes the entry.\n",
                (unsigned long long)rep.trash_blocked, rep.trash_blocked == 1 ? "y" : "ies",
                bs.blocked_path[0] ? ": " : ".", bs.blocked_path[0] ? bs.blocked_path : "");
    }
    if (rep.trash_foreign) {
        // PR #1 review (25th round, P1): not a failed deletion and not a blocked one -- a
        // deletion that was never this store's to make. What is at the entry's path has a
        // different inode from the one the record has carried since the world was published,
        // and for a world discarded across volumes that path is in the user's own `.wfs-trash`
        // under a predictable name. Nothing was renamed and nothing was removed.
        wfs_trash_stat fs_;                       // 33rd round, P2: as above -- a path or nothing
        memset(&fs_, 0, sizeof fs_);
        (void)wfs_gc_status(s, retention, &fs_);
        fprintf(stderr,
                "world: note: %llu trash entr%s could not be collected: what is at %s is not the\n"
                "world:       tree that record was written for (a different inode), so nothing\n"
                "world:       touched it. Put the tree back at that path, or move that directory\n"
                "world:       aside and let the record go.\n",
                (unsigned long long)rep.trash_foreign, rep.trash_foreign == 1 ? "y" : "ies",
                fs_.foreign_path[0] ? fs_.foreign_path : "the entry's path");
    }
    if (rep.trash_failed) {
        // Never let a failed delete read as an empty trash: say what is still in there, and
        // whether anything will come back for it by itself.
        fprintf(stderr,
                "world: note: %llu trash entr%s could not be deleted and %s still in the trash.\n"
                "world:       %s   (`world fs gc --status` lists what is left)\n",
                (unsigned long long)rep.trash_failed, rep.trash_failed == 1 ? "y" : "ies",
                rep.trash_failed == 1 ? "is" : "are",
                rep.work_remains ? "The collector will try again."
                                 : "It has failed too often to keep retrying by itself.");
    }
    if (!now && rep.work_remains) {
        // On this report, not on a second opinion: wfs_gc_pending() answers a question about the
        // trash, and what this run ran out of time for may be under <store>/pool instead.
        int running = 0;
        wfs_gc_pending(s, retention, &running);
        if (!running) spawn_gc_worker_now(s, retention);
        printf("gc: the rest is being collected in the background (`world fs gc --status`)\n");
    }
    return EX_OK;
}

static int cmd_status(wfs_store *s) {
    wfs_store_stat st;
    int rc = wfs_store_status(s, &st);
    if (rc) return fail("status", rc);
    char freeb[32], totalb[32], meta[32];
    fmt_bytes(freeb, sizeof freeb, st.volume_free_bytes);
    fmt_bytes(totalb, sizeof totalb, st.volume_total_bytes);
    fmt_bytes(meta, sizeof meta, st.metadata_estimate_bytes);
    // PR #1 review (35th round, P1): the database's name is part of the schema, so `status`
    // says it. A schema-3 store keeps it at `metadata3.db`; `metadata.db` is the empty stub
    // directory that makes an M1 binary's own open fail rather than let it at this store.
    printf("store:     %s\nschema:    %d (store %s), database metadata3.db\n"
           "snapshots: %llu (%llu entries), %llu trashed\n"
           "worlds:    %llu active, %llu trashed, %llu dead (%llu entries)\n"
           "pool:      %llu ready (%llu entries)\n"
           "volume:    %s free of %s\nclone cost: ~%s of metadata for those entries (df cannot see block sharing)\n",
           st.dir, st.schema, st.store_id, (unsigned long long)st.snapshots,
           (unsigned long long)st.snapshot_entries, (unsigned long long)st.snapshots_trashed,
           (unsigned long long)st.worlds_active,
           (unsigned long long)st.worlds_trashed, (unsigned long long)st.worlds_dead,
           (unsigned long long)st.world_entries, (unsigned long long)st.pool_ready,
           (unsigned long long)st.pool_entries, freeb, totalb, meta);
    // T2.1/T2.2: what the collector still owes, and rows whose tree is not there any more.
    //
    // PR #1 review (33rd round, P2): `status` is the one report here that is worth printing in
    // part -- everything above this line has already been read successfully, and refusing to
    // print it because the trash could not be classified would be a worse answer than printing
    // it. So the failure is not propagated; it is SAID. A silent omission used to read as "the
    // trash is empty and no worker is running", which is the shape of wrong answer this whole
    // round is about. `gc --status` is the command that then gives the errno.
    wfs_trash_stat ts;
    memset(&ts, 0, sizeof ts);
    int trc = wfs_gc_status(s, -1, &ts);
    if (trc != 0)
        printf("trash:     not counted: %s (`world fs gc --status`)\n", wfs_strerror(trc));
    else if (ts.entries || ts.worker_pid) {
        char est[32];
        fmt_bytes(est, sizeof est, ts.bytes_estimate);
        printf("trash:     %llu entries, %llu due, %llu being deleted (~%s), worker %s\n",
               (unsigned long long)ts.entries, (unsigned long long)ts.due,
               (unsigned long long)ts.deleting, est, ts.worker_pid ? "running" : "idle");
    }
    if (st.snapshots_dangling || st.worlds_dangling)
        printf("dangling:  %llu snapshot(s) and %llu world(s) registered but not on disk"
               "  (`world fs gc --reconcile`, or `world fs verify <new path>` for a moved world)\n",
               (unsigned long long)st.snapshots_dangling, (unsigned long long)st.worlds_dangling);
    // PR #1 review (12th round): and the rows whose path could not be read at all. They are
    // deliberately not in the line above: `--reconcile` will not touch them, because an EACCES,
    // an EIO or an unmounted volume is not evidence that anything is gone.
    if (st.snapshots_unreadable || st.worlds_unreadable)
        printf("unreadable: %llu snapshot(s) and %llu world(s) registered but their path cannot be"
               " checked (permissions, I/O, an unmounted volume, or something that is not a"
               " directory in the way)\n",
               (unsigned long long)st.snapshots_unreadable, (unsigned long long)st.worlds_unreadable);
    return EX_OK;
}

// PR #1 review (17th round, P2): `--refresh-marker`. The FSKit extension reads the store's
// location out of the world's `.world` marker and opens exactly that path, so a store that has
// moved since the world was forked leaves every one of its worlds pointing at a directory that
// is not this store any more. `verify` is the command that asks whether a world is what it says
// it is, so it is also the one that can put that right -- for THIS store's own worlds only
// (P1/P2: same store id, same row, same inode), and only the path changes.
static int cmd_verify(wfs_store *s, int argc, char **argv) {
    const char *arg = NULL;
    int refresh = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--refresh-marker")) refresh = 1;
        else if (argv[i][0] != '-' && !arg) arg = argv[i];
        else usage();
    }
    if (!arg) usage();
    wfs_ref r = parse_ref(arg);
    if (r.kind == WFS_K_SNAPSHOT && r.id) {
        if (refresh) {
            fprintf(stderr, "world: --refresh-marker is about a world's .world marker; %s is a snapshot\n", arg);
            return EX_USAGE;
        }
        wfs_verify_report rep;
        int rc = wfs_snapshot_verify(s, r.id, &rep);
        printf("S%llu: %llu entries checked, %llu missing, %llu modified, %llu unprotected, %llu extra,"
               " %llu pool entries (%llu touched)\n",
               (unsigned long long)r.id, (unsigned long long)rep.checked, (unsigned long long)rep.missing,
               (unsigned long long)rep.modified, (unsigned long long)rep.unprotected,
               (unsigned long long)rep.extra, (unsigned long long)rep.pool_checked,
               (unsigned long long)rep.pool_dirty);
        if (rc == WFS_E_SNAPSHOT_DIRTY) {
            char why[WFS_PATH_MAX + 64];
            snprintf(why, sizeof why, "S%llu no longer matches its manifest (first: %s)",
                     (unsigned long long)r.id, rep.first_bad);
            return refuse(why, "fork from an older snapshot, or re-init from a clean source");
        }
        if (rc) return fail("verify", rc);
        return EX_OK;
    }
    wfs_identity id;
    int rc;
    char where[WFS_PATH_MAX];
    if (r.kind == WFS_K_WORLD && r.id && arg[0] != '/' && arg[0] != '.') {
        rc = wfs_world_verify(s, r.id, &id);
        snprintf(where, sizeof where, "W%llu", (unsigned long long)r.id);
    } else {
        rc = wfs_world_verify_identity(s, arg, &id);
        snprintf(where, sizeof where, "%s", arg);
    }
    if (rc == 0) {
        printf("%s: world W%llu '%s' at %s (inode %llu)%s\n", where, (unsigned long long)id.world_id, id.name,
               id.path, (unsigned long long)id.ino, id.moved ? " [path repaired]" : "");
        // T2.3, and PR #1 review (17th round, P2): the marker's store path is the only channel a
        // mount has to tell the sandboxed extension which store this world belongs to, and the
        // extension opens it as it stands. One that does not resolve to this store's directory
        // is therefore a mount of a different store -- so it is said here, where it can still be
        // fixed, rather than discovered as "POSIX error 1009" or, worse, not discovered at all.
        wfs_marker_store ms;
        int mrc = wfs_world_marker_store(s, id.path, &ms);
        if (mrc == 0 && !(ms.has_path && ms.same_path)) {
            if (refresh && ms.same_store) {
                int frc = wfs_world_marker_refresh(s, id.path);
                if (frc) return fail("verify", frc);
                printf("%s: .world store path refreshed: %s -> %s\n", where,
                       ms.has_path ? ms.path : "(none)", wfs_store_dir(s));
                return EX_OK;
            }
            // No path at all (a world forked before T2.3): the extension's fallback to its own
            // container default is real there, so this one is a note.
            if (!ms.has_path) {
                printf("%s: .world carries no store path; a mount falls back to the extension's "
                       "container default\n", where);
                return EX_OK;
            }
            char why[2 * WFS_PATH_MAX + 256];
            snprintf(why, sizeof why,
                     "%s/.world names the store at %s, but this is the store at %s.\n"
                     "world:   a mount hands that path to the FSKit extension, which opens it as it "
                     "stands -- %s",
                     id.path, ms.path, wfs_store_dir(s),
                     ms.same_store
                         ? "this is the same store, moved since the world was forked"
                         : "and that is a DIFFERENT store id: this world was forked out of somebody "
                           "else's store (P1/P2)");
            return refuse(why, ms.same_store ? "world fs verify <world> --refresh-marker"
                                             : "open the store its marker names, or `world fs adopt <path>` "
                                               "to take the copy over as a world of this store");
        }
        if (refresh) printf("%s: .world already names this store (%s)\n", where, wfs_store_dir(s));
        return EX_OK;
    }
    if (rc == WFS_E_UNREGISTERED || rc == WFS_E_NOT_A_WORLD) return explain_path(s, id.path[0] ? id.path : arg, rc, "verify");
    if (rc == WFS_E_WORLD_MISSING) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "%s is not at %s any more", where, id.path);
        return refuse(why, "world fs verify <the path it was moved to>");
    }
    return fail("verify", rc);
}

static int cmd_adopt(wfs_store *s, int argc, char **argv) {
    const char *path = NULL, *name = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else usage();
    }
    if (!path) usage();
    wfs_id id = 0;
    int rc = wfs_world_adopt(s, path, name, &id);
    if (rc == -EEXIST) {
        wfs_identity ident;
        wfs_world_verify_identity(s, path, &ident);
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "%s is already the registered world W%llu", path,
                 (unsigned long long)ident.world_id);
        return refuse(why, "world fs list");
    }
    // PR #1 review (6th round): the copy's marker names the snapshot the original was forked
    // from, and that is the baseline the adopted world would diff and verify against. If it is
    // not in the store any more, adopting would register a world whose every `diff` answers
    // "source gone" -- so the core refuses, and this says what the copy can still be instead.
    if (rc == WFS_E_SOURCE_GONE) {
        wfs_identity ident;
        wfs_world_verify_identity(s, path, &ident);
        char why[WFS_PATH_MAX + 192];
        snprintf(why, sizeof why,
                 "%s was forked from S%llu, which is no longer an active snapshot of this store: "
                 "adopting it would register a world with nothing to diff or verify against",
                 path, (unsigned long long)ident.snapshot_id);
        return refuse(why, "world fs list   (or remove its .world marker and `world fs init` it as a plain directory)");
    }
    if (rc) return explain_path(s, path, rc, "adopt");
    printf("W%llu  %s  (adopted)\n", (unsigned long long)id, path);
    return EX_OK;
}

// ---- P5 / P14: `world exec` -------------------------------------------------------------------
//
// The command runs with the world root as its cwd, WORLD_ID / WORLD_ROOT / WORLD_STORE in the
// environment, an exec lock held for the lifetime of this process (P5), and — unless
// --no-sandbox — inside a seatbelt profile that cannot write outside the world (P14).
//
// Seatbelt rule order matters: the LAST matching rule wins, so every allow is written first
// and the denies come last, which makes them absolute.

static pid_t g_child = 0;

static void forward_signal(int sig) {
    if (g_child > 0) kill(g_child, sig);
}

static void sb_quote(FILE *f, const char *path) {
    fputc('"', f);
    for (const char *p = path; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void sb_subpath(FILE *f, const char *rule, const char *path) {
    if (!path || !*path) return;
    fprintf(f, "(%s (subpath ", rule);
    sb_quote(f, path);
    fputs("))\n", f);
}

// $HOME/<rel>, resolved; skipped silently when it does not exist.
static void sb_home(FILE *f, const char *rule, const char *rel) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char p[WFS_PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", home, rel);
    struct stat st;
    if (stat(p, &st) != 0) return;
    sb_subpath(f, rule, p);
}

// The writable whitelist. Everything here is also allowed by `(allow default)`; it is written
// out so the profile says what an agent is expected to need, and so that tightening the
// default later does not silently break agents. Documented in README.md.
static const char *kAgentHomeDirs[] = {
    ".cache", ".config", ".codex", ".claude", ".npm", ".cargo", ".rustup", ".local/state",
    "Library/Caches", "Library/Application Support/Claude", "Library/Application Support/Code",
    "Library/Application Support/Cursor",
};

static int write_profile(wfs_store *s, wfs_id w, const char *world_root, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -errno;
    fputs("; generated by `world exec` (P14). Last matching rule wins: allows first, denies last.\n"
          "(version 1)\n(allow default)\n\n; --- writable: this world, tmp, agent caches ---\n", f);
    sb_subpath(f, "allow file-write*", world_root);
    const char *tmpdir = getenv("TMPDIR");
    char real[WFS_PATH_MAX];
    if (tmpdir && *tmpdir && realpath(tmpdir, real)) sb_subpath(f, "allow file-write*", real);
    sb_subpath(f, "allow file-write*", "/private/tmp");
    sb_subpath(f, "allow file-write*", "/private/var/tmp");
    for (size_t i = 0; i < sizeof kAgentHomeDirs / sizeof kAgentHomeDirs[0]; ++i)
        sb_home(f, "allow file-write*", kAgentHomeDirs[i]);

    fputs("\n; --- denied: the store (metadata + every snapshot) and every other world ---\n", f);
    const char *store = wfs_store_dir(s);
    sb_subpath(f, "deny file-write*", store);
    char snaps[WFS_PATH_MAX];
    snprintf(snaps, sizeof snaps, "%s/snapshots", store);
    sb_subpath(f, "deny file-read*", snaps);

    size_t n = 0;
    wfs_world_list(s, 1, NULL, 0, &n);
    if (n) {
        wfs_world_rec *v = (wfs_world_rec *)calloc(n, sizeof *v);
        if (v) {
            wfs_world_list(s, 1, v, n, &n);
            for (size_t i = 0; i < n; ++i) {
                if (v[i].id == w || !v[i].path[0]) continue;
                sb_subpath(f, "deny file-write*", v[i].path);
            }
            free(v);
        }
    }
    int rc = ferror(f) ? -EIO : 0;
    if (fclose(f) != 0 && !rc) rc = -EIO;
    return rc;
}

// sandbox-exec is deprecated but present on 27.0. Rather than guess whether a failure came
// from the profile or from the command, apply the profile to /usr/bin/true first.
static int sandbox_usable(const char *profile) {
    if (access("/usr/bin/sandbox-exec", X_OK) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl("/usr/bin/sandbox-exec", "sandbox-exec", "-f", profile, "/usr/bin/true", (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int cmd_exec(wfs_store *s, int argc, char **argv) {
    wfs_id w = 0;
    int sandbox = 1, require_sandbox = 0, cmd_at = -1;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) { cmd_at = i + 1; break; }
        else if (!strcmp(argv[i], "--no-sandbox")) sandbox = 0;
        else if (!strcmp(argv[i], "--require-sandbox")) require_sandbox = 1;
        else if (argv[i][0] != '-' && !w) w = parse_world(argv[i]);
        else usage();
    }
    if (!w || cmd_at < 0 || cmd_at >= argc) usage();
    if (!sandbox && require_sandbox) {
        return refuse("--no-sandbox and --require-sandbox contradict each other", "pick one");
    }

    wfs_world_rec r;
    int rc = wfs_world_info(s, w, &r);
    if (rc) return fail("exec", rc);
    if (r.state != WFS_ST_ACTIVE) {
        char why[128];
        snprintf(why, sizeof why, "W%llu is %s, not active", (unsigned long long)w, state_name(r.state));
        return refuse(why, r.state == WFS_ST_TRASHED ? "world fs restore W<n>" : "world fs list");
    }
    // P1/P2: the path is only a hint; the marker plus the inode decide, and a moved world has
    // its row repaired here.
    wfs_identity id;
    rc = wfs_world_verify_identity(s, r.path, &id);
    if (rc) return explain_path(s, r.path, rc, "exec");

    // What the lock file records, and what a refusal shows the next person.
    char cmdline[256];
    size_t at = 0;
    for (int i = cmd_at; i < argc && at + 1 < sizeof cmdline; ++i)
        at += (size_t)snprintf(cmdline + at, sizeof cmdline - at, "%s%s", i > cmd_at ? " " : "", argv[i]);

    int lockfd = -1;
    rc = wfs_world_lock_exec(s, w, cmdline, &lockfd);
    if (rc == WFS_E_WORLD_BUSY) return busy_refusal(s, w, "exec in");
    if (rc) return fail("exec: lock", rc);

    char prof[WFS_PATH_MAX] = {0};
    if (sandbox) {
        snprintf(prof, sizeof prof, "%s/tmp/exec-W%llu-%d.sb", wfs_store_dir(s), (unsigned long long)w,
                 (int)getpid());
        rc = write_profile(s, w, id.path, prof);
        if (rc) {
            wfs_world_unlock_exec(s, w, lockfd);
            return fail("exec: sandbox profile", rc);
        }
        if (!sandbox_usable(prof)) {
            unlink(prof);
            if (require_sandbox) {
                wfs_world_unlock_exec(s, w, lockfd);
                return refuse("sandbox-exec is unavailable or rejected the generated profile",
                              "world exec W<n> --no-sandbox -- <cmd>   (the command can then write anywhere)");
            }
            fprintf(stderr,
                    "world: WARNING: sandbox-exec is unavailable or failed to initialise; running "
                    "`%s` WITHOUT a sandbox. It can write to other worlds and to the store.\n"
                    "world: pass --require-sandbox to refuse instead.\n",
                    cmdline);
            sandbox = 0;
            prof[0] = 0;
        }
    }

    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "W%llu", (unsigned long long)w);
    setenv("WORLD_ID", idbuf, 1);
    setenv("WORLD_ROOT", id.path, 1);
    setenv("WORLD_STORE", wfs_store_dir(s), 1);

    int nargs = argc - cmd_at;
    char **cav = (char **)calloc((size_t)nargs + 4, sizeof(char *));
    if (!cav) { wfs_world_unlock_exec(s, w, lockfd); return fail("exec", -ENOMEM); }
    int k = 0;
    if (sandbox) {
        cav[k++] = (char *)"/usr/bin/sandbox-exec";
        cav[k++] = (char *)"-f";
        cav[k++] = prof;
    }
    for (int i = 0; i < nargs; ++i) cav[k++] = argv[cmd_at + i];
    cav[k] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        int e = -errno;
        free(cav);
        if (prof[0]) unlink(prof);
        wfs_world_unlock_exec(s, w, lockfd);
        return fail("exec: fork", e);
    }
    if (pid == 0) {
        if (chdir(id.path) != 0) {
            fprintf(stderr, "world: exec: chdir %s: %s\n", id.path, strerror(errno));
            _exit(126);
        }
        execvp(cav[0], cav);
        fprintf(stderr, "world: exec: %s: %s\n", cav[0], strerror(errno));
        _exit(127);
    }
    g_child = pid;
    // Ctrl-C reaches the whole process group anyway; forwarding covers the case where this
    // process is signalled on its own, and either way the lock is released below.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = forward_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) { st = 0; break; }
    }
    g_child = 0;
    if (prof[0]) unlink(prof);
    wfs_world_unlock_exec(s, w, lockfd);
    free(cav);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WIFEXITED(st) ? WEXITSTATUS(st) : EX_ERR;
}

int main(int argc, char **argv) {
    resolve_exe(argc > 0 ? argv[0] : NULL);
    // --store may appear anywhere before a `--`; strip it before dispatch. Everything after
    // `--` belongs to the command `world exec` will run and is passed through untouched.
    char **av = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!av) { fprintf(stderr, "world: out of memory\n"); return EX_ERR; }
    int ac = 0;
    bool passthrough = false;
    for (int i = 0; i < argc; ++i) {
        if (!passthrough && !strcmp(argv[i], "--store") && i + 1 < argc) { g_store_override = argv[++i]; continue; }
        if (!strcmp(argv[i], "--")) passthrough = true;
        av[ac++] = argv[i];
    }
    if (ac < 2) usage();
    if (!strcmp(av[1], "version")) { printf("world %s\n", wfs_version()); return EX_OK; }
    // `world exec` is a World-level command, not a provider command: no `fs` in front of it.
    const int is_exec = !strcmp(av[1], "exec");
    if (!is_exec && (strcmp(av[1], "fs") || ac < 3)) usage();
    const char *sub = is_exec ? "exec" : av[2];
    int nargs = is_exec ? ac - 2 : ac - 3;
    char **args = av + (is_exec ? 2 : 3);

#ifdef WFS_FSKIT
    if (!strcmp(sub, "fsstatus")) {
#ifdef __APPLE__
        return world_fs_status_platform();
#else
        puts("fsstatus: not applicable on this platform");
        return EX_OK;
#endif
    }
    if (!strcmp(sub, "umount")) {
        if (nargs != 1) usage();
        char *uav[] = {(char *)"/sbin/umount", args[0], NULL};
        return run(uav);
    }
#endif

    char sd[WFS_PATH_MAX];
    store_dir(sd, sizeof sd);
    wfs_store *s = NULL;
    int rc = wfs_store_open(sd, &s);
    if (rc == WFS_E_SCHEMA) {
        char why[WFS_PATH_MAX + 96];
        snprintf(why, sizeof why, "the store at %s was written by a different schema than %d", sd,
                 WFS_STORE_SCHEMA);
        return refuse(why, "use a matching `world` build, or point --store at a new directory");
    }
    // P13, and PR #1 review (34th round, P1): this store is still schema 2 and somebody else
    // has its database open. They were admitted before the VERSION file was bumped, so the
    // file cannot lock them out any more -- and if any of them is an M1 build, its collector
    // treats M2 trash as orphans and deletes snapshots that are still inside their retention
    // window. The core put the whole upgrade back -- the database to `metadata.db`, VERSION to
    // 2 (35th round) -- and touched nothing else; all that is left to do here is say who to
    // stop.
    if (rc == WFS_E_STORE_BUSY) {
        wfs_store_holder hs[8];
        size_t n = 0;
        memset(hs, 0, sizeof hs);
        char why[WFS_PATH_MAX + 512];
        int off = snprintf(why, sizeof why,
                           "the store at %s is still schema %d and another process has its "
                           "metadata.db open.\n"
                           "  Taking it to schema %d behind a handle that is already open would "
                           "leave that process able to run an older collector over this store's "
                           "trash, so nothing was migrated and the store was left as it was.",
                           sd, WFS_STORE_SCHEMA_M1, WFS_STORE_SCHEMA);
        // snprintf reports what it WOULD have written, so a truncated first part must not be
        // appended to past the end of the buffer.
        if (off < 0 || (size_t)off >= sizeof why) off = (int)sizeof why - 1;
        if (wfs_store_holders(sd, hs, 8, &n) == 0 && n) {
            off += snprintf(why + off, sizeof why - (size_t)off, "\n  Still holding it:");
            for (size_t i = 0; i < n && i < 8 && off > 0 && (size_t)off < sizeof why; ++i)
                off += snprintf(why + off, sizeof why - (size_t)off, "\n    pid %lld  %s",
                                (long long)hs[i].pid, hs[i].exe[0] ? hs[i].exe : "(unknown)");
            if (n > 8 && off > 0 && (size_t)off < sizeof why)
                snprintf(why + off, sizeof why - (size_t)off, "\n    ... and %zu more", n - 8);
        }
        return refuse(why, "stop those processes (or wait for them to finish) and run the "
                           "command again");
    }
    // P17. The tempting thing to do here is to make a fresh database and carry on; it is also
    // the one thing that loses data. Snapshot and world ids come out of that database, so a new
    // one hands out 1 again and the next `init` writes S1 on top of the `snapshots/S1` that is
    // still sitting there. Nothing in the core can put the rows back either: `gc --reconcile`
    // reads the database to find rows whose trees are gone, and here it is the database that is
    // gone. So: stop, and say what the two real ways out are.
    if (rc == WFS_E_STORE_DAMAGED) {
        char why[WFS_PATH_MAX + 256];
        snprintf(why, sizeof why,
                 "the store at %s still holds trees (snapshots/, trash/ or pool/) but its "
                 "metadata3.db is missing or unreadable.\n"
                 "  Those trees are what the database was the index of: ids would restart at 1 "
                 "and collide with the snapshots/S<n> already on disk, so nothing will be "
                 "created here.\n"
                 "  `world fs gc --reconcile` cannot help -- it needs the database to know what "
                 "is orphaned.",
                 sd);
        return refuse(why,
                      "restore metadata3.db from a backup, or move the directory aside "
                      "(`mv <store> <store>.damaged`) and start a new store");
    }
    // P17, and PR #1 review (13th round): the guard above needs three readdirs to know whether
    // this store still holds trees, and a `snapshots/` it cannot open answers nothing. The core
    // returns that errno rather than guessing "empty", so nothing was created and nothing was
    // touched -- which is worth saying, because the same errno from any other step of the open
    // means the same thing here: stop, fix the access, try again.
    if (rc) {
        fprintf(stderr, "world: open store %s: %s\n", sd, wfs_strerror(rc));
        if (rc == -EACCES || rc == -EPERM || rc == -EIO || rc == -ENOTDIR)
            fprintf(stderr,
                    "  nothing was created: a store that cannot be read is not an empty store "
                    "(P17), so no metadata3.db and no store id were made here.\n"
                    "  try: fix the permissions on %s (or mount the volume it is on) and run the "
                    "command again\n",
                    sd);
        return EX_ERR;
    }

    int ret;
    if (is_exec) ret = cmd_exec(s, nargs, args);
    else if (!strcmp(sub, "init")) ret = cmd_init(s, nargs, args);
    else if (!strcmp(sub, "fork")) ret = cmd_fork(s, nargs, args);
    else if (!strcmp(sub, "checkpoint")) ret = cmd_checkpoint(s, nargs, args);
    else if (!strcmp(sub, "diff")) ret = cmd_diff(s, nargs, args);
    else if (!strcmp(sub, "list")) ret = (nargs == 0) ? cmd_list(s) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "inspect")) ret = (nargs == 1) ? cmd_inspect(s, args[0]) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "discard")) ret = cmd_discard(s, nargs, args);
    else if (!strcmp(sub, "restore")) ret = (nargs == 1) ? cmd_restore(s, args[0]) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "gc")) ret = cmd_gc(s, nargs, args);
    else if (!strcmp(sub, "pool")) ret = cmd_pool(s, nargs, args);
    else if (!strcmp(sub, "status")) ret = cmd_status(s);
    else if (!strcmp(sub, "verify")) ret = cmd_verify(s, nargs, args);
    else if (!strcmp(sub, "adopt")) ret = cmd_adopt(s, nargs, args);
#ifdef WFS_FSKIT
    else if (!strcmp(sub, "mount")) {
        if (nargs != 2) usage();
        wfs_id w = parse_world(args[0]);
        wfs_world_rec r;
        if ((rc = wfs_world_info(s, w, &r))) { wfs_store_close(s); return fail("mount", rc); }
        // T2.3. Two facts decide everything here:
        //   * `-o` options do not reach an FSKit module on macOS 27, so the extension cannot be
        //     told which store to use on the command line. It reads the store path out of the
        //     `.world` marker in the mount source root instead (wfs_marker_store_path), which is
        //     why a fork must have written one.
        //   * the extension is sandboxed (pkd refuses a non-sandboxed appex outright), so it can
        //     reach its own container and the security-scoped mount source and nothing else.
        //     ~/Library/Application Support -- the CLI's default store -- is denied.
        // So a mount only works when the store is inside the extension's container. Saying that
        // here is the difference between a fix and "mount: POSIX error 1009".
        {
            const char *store = wfs_store_dir(s);
            const char *home = getenv("HOME");
            char container[WFS_PATH_MAX], want[WFS_PATH_MAX];
            snprintf(container, sizeof container, "%s/Library/Containers/%s/Data",
                     home ? home : "", WFS_EXTENSION_BUNDLE_ID);
            snprintf(want, sizeof want, "%s/Library/Application Support/World/fs", container);
            size_t cn = strlen(container);
            if (strncmp(store, container, cn) != 0 || (store[cn] != 0 && store[cn] != '/')) {
                char why[WFS_PATH_MAX + 256], hint[WFS_PATH_MAX + 64];
                snprintf(why, sizeof why,
                         "the FSKit extension is sandboxed and cannot read the store at %s:\n"
                         "world:   it only has access to its own container, and mount options do not\n"
                         "world:   reach an FSKit module on macOS 27, so a mount needs the store to live\n"
                         "world:   somewhere the extension can open", store);
                snprintf(hint, sizeof hint, "WORLD_STORE='%s' world fs init <dir> && ... fs mount W<n> <mnt>", want);
                wfs_store_close(s);
                return refuse(why, hint);
            }
            // The marker is how the path gets there; a world forked by an older build has none.
            //
            // PR #1 review (17th round, P2): and a marker that names a DIFFERENT store is not a
            // note. WorldVolume.mm / WorldVolumeHandler.mm take the marker's path whenever it is
            // non-empty and fall back to the container default only when they obtained no path
            // at all -- so a store that has moved, or a world copied out of another store, makes
            // the mount open the store the marker names while every other command in this
            // invocation is talking to the store the CLI opened. Two stores, one mount point,
            // and the world ids in `-o world=<n>` mean different things in each.
            wfs_marker_store ms;
            int mrc = wfs_world_marker_store(s, r.path, &ms);
            if (mrc == 0 && ms.has_path && !ms.same_path) {
                char why[2 * WFS_PATH_MAX + 320], hint[WFS_PATH_MAX + 96];
                snprintf(why, sizeof why,
                         "%s/.world names the store at %s, but this command opened the store at %s.\n"
                         "world:   the extension cannot be told which store to use (`-o` options do not\n"
                         "world:   reach an FSKit module on macOS 27): it opens the path in the marker,\n"
                         "world:   so this mount would serve a different store from the one you asked\n"
                         "world:   about -- %s",
                         r.path, ms.path, store,
                         ms.same_store ? "the same store, moved since this world was forked"
                                       : "and its store id is not this store's either (P1/P2)");
                if (ms.same_store)
                    snprintf(hint, sizeof hint,
                             "world fs verify W%llu --refresh-marker   (then mount again)",
                             (unsigned long long)w);
                else
                    snprintf(hint, sizeof hint,
                             "mount it from the store its marker names, or `world fs adopt %s`", r.path);
                wfs_store_close(s);
                return refuse(why, hint);
            }
            if (mrc != 0 || !ms.has_path)
                fprintf(stderr,
                        "world: note: %s/.world carries no store path (it was forked before T2.3), so\n"
                        "world:       the extension will fall back to its container default.\n"
                        "world:       `world fs verify W%llu --refresh-marker` writes it.\n",
                        r.path, (unsigned long long)w);
        }
        char opt[64];
        mkdir(args[1], 0755);
        wfs_store_close(s);
        snprintf(opt, sizeof opt, "world=%llu", (unsigned long long)w);
        char *mav[] = {(char *)"/sbin/mount", (char *)"-F", (char *)"-t", (char *)WFS_FS_SHORT_NAME,
                       (char *)"-o", opt, r.path, args[1], NULL};
        return run(mav);
    }
#endif
    else { wfs_store_close(s); usage(); return EX_USAGE; }

    wfs_store_close(s);
    return ret;
}
