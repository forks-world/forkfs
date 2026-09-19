// FSKit view/namespace tests (the M0 layer, built only with -DWFS_FSKIT=ON).
// Plain asserts, libc only, so they run anywhere with a C++ compiler.
//
// In M1 a world is a real clonefile'd directory tree, so the view is a passthrough of that
// tree: these tests snapshot a project directory, fork a world out of it, and then exercise
// the namespace layer against the world.
#include "worldfs/worldfs.h"
#include "worldfs/worldfs_fskit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CHECK_OK(x) do { int _rc = (x); if (_rc != 0) { fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, strerror(-_rc)); exit(1); } } while (0)

static void write_file(const char *p, const char *s) { FILE *f = fopen(p, "w"); CHECK(f); fputs(s, f); fclose(f); }
static void join(char *out, size_t cap, const char *a, const char *b) { snprintf(out, cap, "%s%s", a, b); }

struct Ent { char name[256]; wfs_ino ino; wfs_type type; uint64_t next_cookie; int has_attr; wfs_attr attr; };
struct Ents { Ent e[64]; size_t n; };
static int collect(void *ctx, const wfs_dirent *d) {
    Ents *es = (Ents *)ctx;
    if (es->n < 64) {
        Ent &e = es->e[es->n++];
        snprintf(e.name, sizeof e.name, "%.*s", (int)d->name_len, d->name);
        e.ino = d->ino; e.type = d->type; e.next_cookie = d->next_cookie;
        e.has_attr = d->attr != NULL;
        if (d->attr) e.attr = *d->attr;
    }
    return 0;
}

// The same runtime decision readdir makes (core/src/platform_posix.cpp). Declared rather than
// included so this file stays libc-only.
namespace wfs { bool fs_readdir_emits_dots(bool with_attrs); }

// Checks the dot entries of one enumeration of `dir`, whose logical parent is `parent`.
// Returns the number of dot entries that should be there for this host and enumeration kind.
static size_t check_dots(const Ents &es, wfs_ino dir, wfs_ino parent, int want_attr) {
    const bool emit = wfs::fs_readdir_emits_dots(want_attr != 0);
    size_t dots = 0, dotdots = 0;
    for (size_t i = 0; i < es.n; ++i) {
        const Ent &e = es.e[i];
        bool is_dot = !strcmp(e.name, "."), is_dotdot = !strcmp(e.name, "..");
        if (!is_dot && !is_dotdot) continue;
        CHECK(emit);                                   // must not appear when the kernel synthesizes
        CHECK(e.type == WFS_T_DIR);
        CHECK(e.ino == (is_dot ? dir : parent));       // logical inode, not the backing st_ino
        if (want_attr) {
            CHECK(e.has_attr);
            CHECK(e.attr.type == WFS_T_DIR);
            CHECK(e.attr.ino == (is_dot ? dir : parent));
        }
        (is_dot ? dots : dotdots)++;
    }
    CHECK(dots == (emit ? 1u : 0u) && dotdots == (emit ? 1u : 0u));
    return dots + dotdots;
}

int main() {
    char root[] = "/tmp/wfs-core-test.XXXXXX";
    CHECK(mkdtemp(root));
    char base[4096], store[4096], p[4096], q[4096];
    join(base, sizeof base, root, "/base");
    join(store, sizeof store, root, "/store");
    CHECK(mkdir(base, 0755) == 0);
    join(p, sizeof p, base, "/src"); CHECK(mkdir(p, 0755) == 0);
    join(p, sizeof p, base, "/hello.txt"); write_file(p, "hello\n");
    join(p, sizeof p, base, "/src/a.c"); write_file(p, "int main(){}\n");

    wfs_store *s = NULL;
    CHECK_OK(wfs_store_open(store, &s));
    CHECK(strlen(wfs_version()) > 0);

    // snapshot the project, then fork a world out of it: that world is what a mount exposes.
    wfs_id snap = 0;
    CHECK_OK(wfs_snapshot_create(s, base, "proj", &snap));
    char wpath[4096], wpath2[4096];
    join(wpath, sizeof wpath, root, "/w1");
    join(wpath2, sizeof wpath2, root, "/w2");
    wfs_ref from = {WFS_K_SNAPSHOT, snap};
    wfs_id w0 = 0, w2 = 0;
    CHECK_OK(wfs_world_create(s, from, wpath, NULL, &w0));
    CHECK_OK(wfs_world_create(s, from, wpath2, NULL, &w2));
    wfs_world_rec rec;
    CHECK_OK(wfs_world_info(s, w0, &rec));
    CHECK(rec.state == WFS_ST_ACTIVE && rec.snapshot_id == snap);
    char bd[4096];
    snprintf(bd, sizeof bd, "%s", rec.path);
    CHECK(strstr(bd, "/w1") != NULL);
    size_t n = 0;
    CHECK_OK(wfs_world_list(s, 0, NULL, 0, &n));
    CHECK(n == 2);

    // view over the world: lookup / getattr / readdir / readlink / backing path
    wfs_view *v = NULL;
    CHECK_OK(wfs_view_open(s, w0, &v));
    wfs_attr a;
    CHECK_OK(wfs_getattr(v, WFS_INO_ROOT, &a));
    CHECK(a.type == WFS_T_DIR && a.ino == WFS_INO_ROOT);
    CHECK_OK(wfs_lookup(v, WFS_INO_ROOT, "src", 3, &a));
    CHECK(a.type == WFS_T_DIR && a.parent == WFS_INO_ROOT);
    wfs_ino src = a.ino;
    CHECK_OK(wfs_lookup(v, src, "a.c", 3, &a));
    CHECK(a.type == WFS_T_FILE && a.size == 13);
    wfs_ino ac = a.ino;
    CHECK(wfs_lookup(v, src, "nope", 4, &a) == -ENOENT);
    CHECK(wfs_lookup(v, src, "..", 2, &a) == -EINVAL);
    char pb[4096];
    CHECK_OK(wfs_backing_path(v, ac, 0, pb, sizeof pb));
    join(q, sizeof q, bd, "/src/a.c"); CHECK(strcmp(pb, q) == 0);
    // Root, attribute-less enumeration: "." is the root itself and ".." is the root too (the mount
    // has no parent to expose). Whether the dot entries appear at all is the host's call.
    Ents ents = {};
    CHECK_OK(wfs_readdir(v, WFS_INO_ROOT, 0, 0, collect, &ents));
    size_t ndots = check_dots(ents, WFS_INO_ROOT, WFS_INO_ROOT, 0);
    bool saw_src = false;
    for (size_t i = 0; i < ents.n; ++i)
        if (!strcmp(ents.e[i].name, "src")) { saw_src = true; CHECK(ents.e[i].ino == src); }
    CHECK(saw_src && ents.n == ndots + 3);   // hello.txt + src + the .world marker
    // cookies stay monotonic and resumable whichever way the dot entries go
    for (size_t i = 1; i < ents.n; ++i) CHECK(ents.e[i].next_cookie > ents.e[i - 1].next_cookie);
    Ents rest = {};
    CHECK_OK(wfs_readdir(v, WFS_INO_ROOT, ents.e[0].next_cookie, 0, collect, &rest));
    CHECK(rest.n == ents.n - 1);
    for (size_t i = 0; i < rest.n; ++i) CHECK(!strcmp(rest.e[i].name, ents.e[i + 1].name));
    // The attribute-bearing enumeration is the other branch of the same decision, and carries the
    // same non-dot entries either way.
    Ents wa = {};
    CHECK_OK(wfs_readdir(v, WFS_INO_ROOT, 0, 1, collect, &wa));
    size_t ndots_attr = check_dots(wa, WFS_INO_ROOT, WFS_INO_ROOT, 1);
    CHECK(wa.n == ndots_attr + 3);
    // A subdirectory: ".." must carry the parent's logical ino, not the subdirectory's own.
    Ents sub = {};
    CHECK_OK(wfs_readdir(v, src, 0, 0, collect, &sub));
    CHECK(check_dots(sub, src, WFS_INO_ROOT, 0) == ndots);
    CHECK(sub.n == ndots + 1);   // a.c

    // mutations land in the world's own tree; rename keeps children resolvable
    CHECK_OK(wfs_create(v, src, "b.c", 3, WFS_T_FILE, 0644, &a));
    join(q, sizeof q, wpath, "/src/b.c"); CHECK(access(q, F_OK) == 0);
    CHECK_OK(wfs_rename(v, WFS_INO_ROOT, "src", 3, WFS_INO_ROOT, "lib", 3, src));
    CHECK_OK(wfs_backing_path(v, ac, 0, pb, sizeof pb));
    join(q, sizeof q, bd, "/lib/a.c"); CHECK(strcmp(pb, q) == 0);
    CHECK_OK(wfs_symlink(v, WFS_INO_ROOT, "l", 1, "hello.txt", 9, &a));
    char lb[256]; size_t ll = 0;
    CHECK_OK(wfs_readlink(v, a.ino, lb, sizeof lb, &ll));
    CHECK(ll == 9 && !memcmp(lb, "hello.txt", 9));
    wfs_setattr_req sa; memset(&sa, 0, sizeof sa); sa.valid = WFS_SET_MODE | WFS_SET_SIZE; sa.mode = 0600; sa.size = 2;
    CHECK_OK(wfs_setattr(v, ac, &sa, &a));
    CHECK(a.mode == 0600 && a.size == 2);
    CHECK_OK(wfs_setxattr(v, ac, "user.k", "v", 1, 0));
    char xb[16]; size_t xl = 0;
    CHECK_OK(wfs_getxattr(v, ac, "user.k", xb, sizeof xb, &xl));
    CHECK(xl == 1 && xb[0] == 'v');
    CHECK_OK(wfs_removexattr(v, ac, "user.k"));
    CHECK_OK(wfs_unlink(v, WFS_INO_ROOT, "l", 1, a.ino));
    CHECK_OK(wfs_forget(v, ac));
    CHECK(wfs_backing_path(v, ac, 0, pb, sizeof pb) == -ESTALE);
    wfs_statfs_info sf;
    CHECK_OK(wfs_statfs(v, &sf));
    CHECK(sf.total_blocks > 0);
    // many inodes: exercise table growth and deletion
    for (uint64_t i = 0; i < 5000; ++i) { char nm[32]; snprintf(nm, sizeof nm, "f%llu", (unsigned long long)i); join(q, sizeof q, wpath, "/lib/"); strcat(q, nm); write_file(q, "x"); CHECK_OK(wfs_lookup(v, src, nm, strlen(nm), &a)); if (i % 3 == 0) CHECK_OK(wfs_forget(v, a.ino)); }
    CHECK_OK(wfs_lookup(v, src, "f4999", 5, &a));
    wfs_view_close(v);

    // a sibling world is a tree of its own: writable, and untouched by what w1 did
    CHECK_OK(wfs_view_open(s, w2, &v));
    CHECK_OK(wfs_lookup(v, WFS_INO_ROOT, "hello.txt", 9, &a));
    CHECK(wfs_lookup(v, WFS_INO_ROOT, "lib", 3, &a) == -ENOENT);   // w1 renamed src -> lib
    CHECK_OK(wfs_create(v, WFS_INO_ROOT, "x", 1, WFS_T_FILE, 0644, &a));
    wfs_view_close(v);

    // a discarded world cannot be mounted
    CHECK_OK(wfs_world_discard(s, w2, 0));
    CHECK(wfs_view_open(s, w2, &v) == -ESTALE);
    CHECK_OK(wfs_world_list(s, 0, NULL, 0, &n));
    CHECK(n == 1);

    wfs_store_close(s);
    printf("core_test: all OK (%s)\n", root);
    return 0;
}
