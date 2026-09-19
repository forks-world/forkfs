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
          "  diff W<n> [--full|--events] [--stat] [--no-xattr] [--no-content]\n"
          "                                   what changed since the fork (A/M/D/T, sorted).\n"
          "                                   Walks both trees by default -- exact, and faster than\n"
          "                                   the FSEvents path below ~200k entries; --events asks\n"
          "                                   for FSEvents anyway, --full always walks\n"
          "  list                             snapshots and worlds\n"
          "  inspect W<n>|S<n>\n"
          "  discard W<n> [--now] [--force]   move to the store trash (--now deletes at once)\n"
          "  restore W<n>                     bring a trashed world back to its path\n"
          "  gc [--retention <days>]          delete expired trash and stray *.wfs-tmp trees\n"
          "  pool status                      pre-cloned worlds waiting per snapshot\n"
          "  pool fill S<n> [--count K]       top the pool up to K ready entries (default 2)\n"
          "  pool drain S<n>|--all            delete the pool entries of a snapshot\n"
          "  status                           store, counts, free space\n"
          "  verify S<n>|W<n>|<path>          snapshot integrity, or world identity\n"
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
        if (r.hardlinks)
            fprintf(stderr,
                    "world: warning: %llu entries in this tree have more than one link; clonefile "
                    "breaks hardlinks, so forks will see independent copies (P9)\n",
                    (unsigned long long)r.hardlinks);
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

static void spawn_pool_fill(wfs_store *s, wfs_id snap, int target) {
    if (target <= 0 || !g_exe[0]) return;
    char sid[32], cnt[32], logp[WFS_PATH_MAX];
    snprintf(sid, sizeof sid, "S%llu", (unsigned long long)snap);
    snprintf(cnt, sizeof cnt, "%d", target);
    snprintf(logp, sizeof logp, "%s/logs/pool.log", wfs_store_dir(s));
    // Double fork: the intermediate child is reaped right here, and the filler itself is
    // reparented to launchd, so it outlives this command without ever becoming a zombie.
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
    execl(g_exe, "world", "--store", wfs_store_dir(s), "fs", "pool", "fill", sid, "--count", cnt,
          (char *)NULL);
    _exit(127);
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
        if (rc) return explain_path(s, target, rc, "fork");
        printf("W%llu  %s%s\n", (unsigned long long)res.world, target, res.from_pool ? "  (pool)" : "");
        // Put back what this fork took, in the background, so the next one is fast too --
        // unless a filler is already at work, in which case spawning a second one would only
        // cost this fork a process start to have the child exit on the lock.
        if (res.from_pool && !opts.no_pool && !wfs_pool_filling(s))
            spawn_pool_fill(s, from.id, pool_topup_target());
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

static int cmd_discard(wfs_store *s, int argc, char **argv) {
    wfs_id w = 0;
    int now = 0, force = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--now")) now = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (argv[i][0] != '-' && !w) w = parse_world(argv[i]);
        else usage();
    }
    if (!w) usage();
    wfs_world_rec r;
    if (wfs_world_info(s, w, &r) == 0 && r.state == WFS_ST_ACTIVE && !r.present) {
        char why[WFS_PATH_MAX + 64];
        snprintf(why, sizeof why, "W%llu is not at %s any more; refusing to discard something I cannot see",
                 (unsigned long long)w, r.path);
        return refuse(why, "world fs verify <the path it was moved to>");
    }
    int rc = wfs_world_discard(s, w, now, force);
    if (rc == WFS_E_WORLD_BUSY) return busy_refusal(s, w, "discard");
    if (rc == WFS_E_UNREGISTERED) return explain_path(s, r.path, rc, "discard");
    if (rc) return fail("discard", rc);
    if (now) printf("W%llu deleted\n", (unsigned long long)w);
    else printf("W%llu moved to the trash; `world fs restore W%llu` brings it back\n",
                (unsigned long long)w, (unsigned long long)w);
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
    if (rc) return fail("restore", rc);
    printf("W%llu restored to %s\n", (unsigned long long)w, r.path);
    return EX_OK;
}

static int cmd_gc(wfs_store *s, int argc, char **argv) {
    int64_t retention = -1;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--retention") && i + 1 < argc) retention = (int64_t)(atof(argv[++i]) * 86400.0);
        else usage();
    }
    wfs_gc_report rep;
    int rc = wfs_gc(s, retention, &rep);
    if (rc) return fail("gc", rc);
    printf("gc: %llu worlds deleted, %llu half-built snapshots, %llu stray %s trees, %llu orphan trash dirs,"
           " %llu pool entries\n",
           (unsigned long long)rep.worlds_deleted, (unsigned long long)rep.snapshots_deleted,
           (unsigned long long)rep.tmp_removed, WFS_TMP_SUFFIX, (unsigned long long)rep.trash_orphans,
           (unsigned long long)rep.pool_removed);
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
    printf("store:     %s\nschema:    %d (store %s)\n"
           "snapshots: %llu (%llu entries)\nworlds:    %llu active, %llu trashed, %llu dead (%llu entries)\n"
           "pool:      %llu ready (%llu entries)\n"
           "volume:    %s free of %s\nclone cost: ~%s of metadata for those entries (df cannot see block sharing)\n",
           st.dir, st.schema, st.store_id, (unsigned long long)st.snapshots,
           (unsigned long long)st.snapshot_entries, (unsigned long long)st.worlds_active,
           (unsigned long long)st.worlds_trashed, (unsigned long long)st.worlds_dead,
           (unsigned long long)st.world_entries, (unsigned long long)st.pool_ready,
           (unsigned long long)st.pool_entries, freeb, totalb, meta);
    return EX_OK;
}

static int cmd_verify(wfs_store *s, const char *arg) {
    wfs_ref r = parse_ref(arg);
    if (r.kind == WFS_K_SNAPSHOT && r.id) {
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
    if (rc) { fprintf(stderr, "world: open store %s: %s\n", sd, wfs_strerror(rc)); return EX_ERR; }

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
    else if (!strcmp(sub, "verify")) ret = (nargs == 1) ? cmd_verify(s, args[0]) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "adopt")) ret = cmd_adopt(s, nargs, args);
#ifdef WFS_FSKIT
    else if (!strcmp(sub, "mount")) {
        if (nargs != 2) usage();
        wfs_id w = parse_world(args[0]);
        wfs_world_rec r;
        if ((rc = wfs_world_info(s, w, &r))) { wfs_store_close(s); return fail("mount", rc); }
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
