// `world` CLI. World is the product; `fs` is the first state provider (arch.md §23).
// C-style C++: libc only (arch.md §39).
//
// Every refusal prints one line of reason and the command that would have worked, and exits 3.
// 0 = ok, 1 = error, 2 = usage, 3 = refused by a safety rule (docs/M1_DESIGN.md §3).
#include "worldfs/worldfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef WFS_FSKIT
#ifdef __APPLE__
extern "C" int world_fs_status_platform(void);
#endif
#endif

enum { EX_OK = 0, EX_ERR = 1, EX_USAGE = 2, EX_REFUSED = 3 };

static void usage(void) {
    fputs("usage: world fs <command>\n"
          "  init <dir> [--name N]            snapshot <dir> as S<n> (immutable, protected)\n"
          "  fork [--from W<n>|S<n>] [--to <path>] [--name N] [--copy]\n"
          "                                   clone into a writable world (default ~/worlds/W<n>/<name>)\n"
          "  checkpoint W<n> [--name N]       snapshot a live world; the world stays writable\n"
          "  list                             snapshots and worlds\n"
          "  inspect W<n>|S<n>\n"
          "  discard W<n> [--now]             move to the store trash (--now deletes at once)\n"
          "  restore W<n>                     bring a trashed world back to its path\n"
          "  gc [--retention <days>]          delete expired trash and stray *.wfs-tmp trees\n"
          "  status                           store, counts, free space\n"
          "  verify S<n>|W<n>|<path>          snapshot integrity, or world identity\n"
          "  adopt <path> [--name N]          register an unregistered copy as a new world\n"
#ifdef WFS_FSKIT
          "  mount <W> <mountpoint> | umount <mountpoint> | fsstatus   (FSKit frontend)\n"
#endif
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
    const char *dir = NULL, *name = NULL;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (argv[i][0] != '-' && !dir) dir = argv[i];
        else usage();
    }
    if (!dir) usage();
    int rc = wfs_path_check(s, dir, 0);
    if (rc) return explain_path(s, dir, rc, "init");
    if ((rc = wfs_store_clone_probe(s, dir))) return explain_path(s, dir, rc, "init");
    wfs_id id = 0;
    if ((rc = wfs_snapshot_create(s, dir, name, &id))) return explain_path(s, dir, rc, "init");
    wfs_snapshot_rec r;
    if (wfs_snapshot_info(s, id, &r) == 0) {
        printf("S%llu  %s  %llu entries\n", (unsigned long long)id, r.name, (unsigned long long)r.entries);
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
        else if (!strcmp(argv[i], "--skip-space-check")) opts.skip_space_check = 1;
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
        wfs_id id = 0;
        rc = wfs_world_create(s, from, target, &opts, &id);
        if (rc == -EEXIST && !to) continue; // someone else took that id; ask again
        if (rc) return explain_path(s, target, rc, "fork");
        printf("W%llu  %s\n", (unsigned long long)id, target);
        return EX_OK;
    }
    return fail("fork", rc ? rc : -EEXIST);
}

static int cmd_checkpoint(wfs_store *s, int argc, char **argv) {
    const char *name = NULL;
    wfs_id w = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
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
    if ((rc = wfs_snapshot_create(s, r.path, name, &id))) return explain_path(s, r.path, rc, "checkpoint");
    wfs_snapshot_rec sr;
    if (wfs_snapshot_info(s, id, &sr) == 0)
        printf("S%llu  %s  %llu entries  (from W%llu)\n", (unsigned long long)id, sr.name,
               (unsigned long long)sr.entries, (unsigned long long)w);
    else
        printf("S%llu\n", (unsigned long long)id);
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
               "entries:   %llu (%llu with >1 link)\npath:      %s\nsource:    %s\n",
               (unsigned long long)v.id, v.name, state_name(v.state), t, (unsigned long long)v.entries,
               (unsigned long long)v.hardlinks, v.path, v.src_path);
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
    int now = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--now")) now = 1;
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
    int rc = wfs_world_discard(s, w, now);
    if (rc == WFS_E_WORLD_BUSY)
        return refuse("another world command holds this world's lock", "wait for it, then retry");
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
    printf("gc: %llu worlds deleted, %llu half-built snapshots, %llu stray %s trees, %llu orphan trash dirs\n",
           (unsigned long long)rep.worlds_deleted, (unsigned long long)rep.snapshots_deleted,
           (unsigned long long)rep.tmp_removed, WFS_TMP_SUFFIX, (unsigned long long)rep.trash_orphans);
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
           "volume:    %s free of %s\nclone cost: ~%s of metadata for those entries (df cannot see block sharing)\n",
           st.dir, st.schema, st.store_id, (unsigned long long)st.snapshots,
           (unsigned long long)st.snapshot_entries, (unsigned long long)st.worlds_active,
           (unsigned long long)st.worlds_trashed, (unsigned long long)st.worlds_dead,
           (unsigned long long)st.world_entries, freeb, totalb, meta);
    return EX_OK;
}

static int cmd_verify(wfs_store *s, const char *arg) {
    wfs_ref r = parse_ref(arg);
    if (r.kind == WFS_K_SNAPSHOT && r.id) {
        wfs_verify_report rep;
        int rc = wfs_snapshot_verify(s, r.id, &rep);
        printf("S%llu: %llu entries checked, %llu missing, %llu modified, %llu unprotected, %llu extra\n",
               (unsigned long long)r.id, (unsigned long long)rep.checked, (unsigned long long)rep.missing,
               (unsigned long long)rep.modified, (unsigned long long)rep.unprotected,
               (unsigned long long)rep.extra);
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

int main(int argc, char **argv) {
    // --store may appear anywhere; strip it before dispatch.
    char *av[64];
    int ac = 0;
    for (int i = 0; i < argc && ac < 64; ++i) {
        if (!strcmp(argv[i], "--store") && i + 1 < argc) { g_store_override = argv[++i]; continue; }
        av[ac++] = argv[i];
    }
    if (ac < 2) usage();
    if (!strcmp(av[1], "version")) { printf("world %s\n", wfs_version()); return EX_OK; }
    if (strcmp(av[1], "fs") || ac < 3) usage();
    const char *sub = av[2];
    int nargs = ac - 3;
    char **args = av + 3;

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
    if (!strcmp(sub, "init")) ret = cmd_init(s, nargs, args);
    else if (!strcmp(sub, "fork")) ret = cmd_fork(s, nargs, args);
    else if (!strcmp(sub, "checkpoint")) ret = cmd_checkpoint(s, nargs, args);
    else if (!strcmp(sub, "list")) ret = (nargs == 0) ? cmd_list(s) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "inspect")) ret = (nargs == 1) ? cmd_inspect(s, args[0]) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "discard")) ret = cmd_discard(s, nargs, args);
    else if (!strcmp(sub, "restore")) ret = (nargs == 1) ? cmd_restore(s, args[0]) : (usage(), EX_USAGE);
    else if (!strcmp(sub, "gc")) ret = cmd_gc(s, nargs, args);
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
