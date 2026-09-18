// `world` CLI. World is the product; `fs` is the first state provider (arch.md §23).
// C-style C++: libc only (arch.md §39).
#include "worldfs/worldfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
extern "C" int world_fs_status_platform(void);
#endif

static void usage(void) {
    fputs("usage:\n"
          "  world fs init <dir>                 register <dir> as a base world (prints world id)\n"
          "  world fs fork [--from <W>]          fork a world (default: the most recent base)\n"
          "  world fs list\n"
          "  world fs inspect <W>\n"
          "  world fs mount <W> <mountpoint>     mount -F -t worldfs -o world=<W> <base> <mountpoint>\n"
          "  world fs umount <mountpoint>\n"
          "  world fs discard <W>\n"
          "  world fs status                     installed FSKit modules (macOS)\n"
          "  world version\n"
          "env: WORLD_STORE overrides the metadata store directory\n",
          stderr);
    exit(2);
}

static void store_dir(char *out, size_t cap) {
    const char *e = getenv("WORLD_STORE");
    if (e) { snprintf(out, cap, "%s", e); return; }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
#ifdef __APPLE__
    // Must match the sandboxed FSKit extension's container (it cannot see ~/Library/Application Support).
    snprintf(out, cap, "%s/Library/Containers/%s/Data/Library/Application Support/World/fs", home, WFS_EXTENSION_BUNDLE_ID);
#else
    const char *x = getenv("XDG_DATA_HOME");
    if (x) snprintf(out, cap, "%s/world/fs", x);
    else snprintf(out, cap, "%s/.local/share/world/fs", home);
#endif
}

static void die(const char *what, int rc) {
    fprintf(stderr, "world: %s: %s\n", what, strerror(-rc));
    exit(1);
}

static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], argv); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static wfs_world parse_world(const char *s) {
    const char *p = (*s == 'W' || *s == 'w') ? s + 1 : s;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (!end || *end || v == 0) { fprintf(stderr, "world: bad world id '%s'\n", s); exit(2); }
    return (wfs_world)v;
}

static wfs_world *list_worlds(wfs_store *s, size_t *n) {
    *n = 0;
    wfs_world_list(s, NULL, 0, n);
    wfs_world *ids = (wfs_world *)calloc(*n ? *n : 1, sizeof(wfs_world));
    if (*n) wfs_world_list(s, ids, *n, n);
    return ids;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    if (!strcmp(argv[1], "version")) { printf("world %s\n", wfs_version()); return 0; }
    if (strcmp(argv[1], "fs") || argc < 3) usage();
    const char *sub = argv[2];
    int nargs = argc - 3;
    char **args = argv + 3;

    if (!strcmp(sub, "status")) {
#ifdef __APPLE__
        return world_fs_status_platform();
#else
        puts("status: not applicable on this platform");
        return 0;
#endif
    }
    if (!strcmp(sub, "umount")) {
        if (nargs != 1) usage();
        char *av[] = {(char *)"/sbin/umount", args[0], NULL};
        return run(av);
    }

    char sd[4096];
    store_dir(sd, sizeof sd);
    wfs_store *s = NULL;
    int rc = wfs_store_open(sd, &s);
    if (rc) die("open store", rc);

    if (!strcmp(sub, "init")) {
        if (nargs != 1) usage();
        wfs_world w = 0;
        if ((rc = wfs_world_init(s, args[0], &w))) die("init", rc);
        printf("W%llu\n", (unsigned long long)w);
    } else if (!strcmp(sub, "fork")) {
        wfs_world from = 0;
        for (int i = 0; i < nargs; ++i) {
            if (!strcmp(args[i], "--from") && i + 1 < nargs) from = parse_world(args[++i]);
            else usage();
        }
        if (from == 0) {
            size_t n; wfs_world *ids = list_worlds(s, &n);
            for (size_t i = 0; i < n; ++i) { wfs_world p = 1; wfs_world_info(s, ids[i], &p, NULL, NULL, 0); if (p == 0) from = ids[i]; }
            free(ids);
            if (from == 0) { fputs("world: no base world; run `world fs init <dir>` first\n", stderr); return 1; }
        }
        wfs_world w = 0;
        if ((rc = wfs_world_fork(s, from, &w))) die("fork", rc);
        printf("W%llu\n", (unsigned long long)w);
    } else if (!strcmp(sub, "list")) {
        size_t n; wfs_world *ids = list_worlds(s, &n);
        printf("%-8s %-8s %s\n", "WORLD", "PARENT", "BASE");
        for (size_t i = 0; i < n; ++i) {
            wfs_world p = 0; char base[4096] = {0};
            wfs_world_info(s, ids[i], &p, NULL, base, sizeof base);
            if (p) printf("W%-7llu W%-7llu %s\n", (unsigned long long)ids[i], (unsigned long long)p, base);
            else   printf("W%-7llu %-8s %s\n", (unsigned long long)ids[i], "-", base);
        }
        free(ids);
    } else if (!strcmp(sub, "inspect")) {
        if (nargs != 1) usage();
        wfs_world w = parse_world(args[0]), p = 0; wfs_world_state st = WFS_W_ACTIVE; char base[4096] = {0};
        if ((rc = wfs_world_info(s, w, &p, &st, base, sizeof base))) die("inspect", rc);
        printf("world:  W%llu\n", (unsigned long long)w);
        if (p) printf("parent: W%llu\n", (unsigned long long)p); else puts("parent: -");
        printf("state:  %s\nbase:   %s\n", st == WFS_W_ACTIVE ? "active" : st == WFS_W_DISCARDED ? "discarded" : "dead", base);
    } else if (!strcmp(sub, "discard")) {
        if (nargs != 1) usage();
        if ((rc = wfs_world_discard(s, parse_world(args[0])))) die("discard", rc);
    } else if (!strcmp(sub, "mount")) {
        if (nargs != 2) usage();
        wfs_world w = parse_world(args[0]);
        char base[4096] = {0}, opt[64];
        if ((rc = wfs_world_info(s, w, NULL, NULL, base, sizeof base))) die("mount", rc);
        mkdir(args[1], 0755);
        wfs_store_close(s);
        snprintf(opt, sizeof opt, "world=%llu", (unsigned long long)w);
        char *av[] = {(char *)"/sbin/mount", (char *)"-F", (char *)"-t", (char *)WFS_FS_SHORT_NAME, (char *)"-o", opt, base, args[1], NULL};
        return run(av);
    } else {
        usage();
    }
    wfs_store_close(s);
    return 0;
}
