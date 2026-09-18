// View: live inode table + namespace operations for one mounted world.
// M0: passthrough over the base directory. Overlay/whiteout/COW arrive in M1/M2
// behind exactly this API, so frontends do not change.
#include "internal.h"

#include <errno.h>
#ifndef ENOATTR
#  ifdef __APPLE__
#    define ENOATTR 93        /* hidden by strict -std=c++NN; must match <sys/errno.h> */
#  else
#    define ENOATTR ENODATA
#  endif
#endif
#include <string.h>

using wfs::Guard;
using wfs::NodeRec;
using wfs::String;

namespace {

enum { kMaxDepth = 1024 };

// Build the backing path for `ino` (caller holds v->mu). -ESTALE if the chain is broken.
int path_locked(wfs_view *v, wfs_ino ino, String &out) {
    const NodeRec *chain[kMaxDepth];
    int depth = 0;
    for (wfs_ino cur = ino; cur != WFS_INO_ROOT;) {
        auto it = v->nodes.find(cur);
        if (it == v->nodes.end()) return -ESTALE;
        if (depth == kMaxDepth) return -ELOOP;
        chain[depth++] = &it->second;
        cur = it->second.parent;
    }
    out.assign(v->base_dir.data(), v->base_dir.size());
    for (int i = depth - 1; i >= 0; --i) {
        out.push_back('/');
        out.append(chain[i]->name.data(), chain[i]->name.size());
    }
    return 0;
}

int path_of(wfs_view *v, wfs_ino ino, String &out) {
    Guard g(v->mu);
    return path_locked(v, ino, out);
}

bool valid_name(const char *name, size_t len) {
    if (len == 0 || memchr(name, '/', len) || memchr(name, 0, len)) return false;
    if (len == 1 && name[0] == '.') return false;
    if (len == 2 && name[0] == '.' && name[1] == '.') return false;
    return true;
}

int child_path(wfs_view *v, wfs_ino dir, const char *name, size_t len, String &out) {
    if (!valid_name(name, len)) return -EINVAL;
    if (int rc = path_of(v, dir, out)) return rc;
    out.push_back('/');
    out.append(name, len);
    return 0;
}

// Register (or refresh) a node discovered under `dir`.
void intern(wfs_view *v, wfs_ino dir, const char *name, size_t len, const wfs_attr &a) {
    Guard g(v->mu);
    auto it = v->nodes.find(a.ino);
    if (it != v->nodes.end()) { it->second.type = a.type; return; } // hardlinks keep their first name (M0)
    v->nodes.try_emplace(a.ino, NodeRec{dir, a.type, String(name, len)});
}

void fill_parent(wfs_view *v, wfs_attr &a) {
    if (a.ino == WFS_INO_ROOT) { a.parent = WFS_INO_ROOT; return; }
    Guard g(v->mu);
    auto it = v->nodes.find(a.ino);
    a.parent = it != v->nodes.end() ? it->second.parent : WFS_INO_ROOT;
}

} // namespace

extern "C" int wfs_view_open(wfs_store *s, wfs_world w, wfs_view **out) {
    if (!s || !out || w == 0) return -EINVAL;
    wfs_world parent = 0;
    wfs_world_state st = WFS_W_ACTIVE;
    if (int rc = wfs_world_info(s, w, &parent, &st, nullptr, 0)) return rc;
    if (st != WFS_W_ACTIVE) return -ESTALE;
    wfs_view *v = new wfs_view();
    v->store = s;
    v->world = w;
    if (int rc = wfs::store_world_base_dir(s, w, v->base_dir)) { delete v; return rc; }
    v->writable = (parent == 0);   // M0: forks are read-only views of their base
    v->nodes.try_emplace(WFS_INO_ROOT, NodeRec{WFS_INO_ROOT, WFS_T_DIR, String()});
    *out = v;
    return 0;
}

extern "C" void wfs_view_close(wfs_view *v) { delete v; }
extern "C" wfs_world wfs_view_world(const wfs_view *v) { return v ? v->world : 0; }

extern "C" int wfs_getattr(wfs_view *v, wfs_ino ino, wfs_attr *out) {
    if (!v || !out) return -EINVAL;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    if (int rc = wfs::fs_lstat(p.c_str(), *out)) return rc;
    if (ino == WFS_INO_ROOT) out->ino = WFS_INO_ROOT;
    fill_parent(v, *out);
    return 0;
}

extern "C" int wfs_lookup(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_attr *out) {
    if (!v || !name || !out) return -EINVAL;
    String p;
    if (int rc = child_path(v, dir, name, len, p)) return rc;
    if (int rc = wfs::fs_lstat(p.c_str(), *out)) return rc;
    intern(v, dir, name, len, *out);
    out->parent = dir;
    return 0;
}

extern "C" int wfs_forget(wfs_view *v, wfs_ino ino) {
    if (!v) return -EINVAL;
    if (ino == WFS_INO_ROOT) return 0;
    Guard g(v->mu);
    v->nodes.erase(ino);
    return 0;
}

extern "C" int wfs_readlink(wfs_view *v, wfs_ino ino, char *buf, size_t cap, size_t *len) {
    if (!v || !buf || !len) return -EINVAL;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    return wfs::fs_readlink(p.c_str(), buf, cap, len);
}

extern "C" int wfs_backing_path(wfs_view *v, wfs_ino ino, int writable, char *buf, size_t cap) {
    if (!v || !buf) return -EINVAL;
    if (writable && !v->writable) return -EROFS;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    if (p.size() + 1 > cap) return -ENAMETOOLONG;
    memcpy(buf, p.c_str(), p.size() + 1);
    return 0;
}

namespace {
struct ReaddirCtx {
    wfs_view *v;
    wfs_ino dir;
    String dir_path;
    int want_attr;
    wfs_readdir_cb cb;
    void *ctx;
};

int readdir_trampoline(void *cp, const char *name, size_t len, uint64_t ino, wfs_type type, uint64_t index) {
    ReaddirCtx *c = (ReaddirCtx *)cp;
    // The kernel (FSKit/VFS) synthesizes "." and ".." for every directory it enumerates, so packing
    // the ones the backing opendir returns makes `ls -fa` show each of them twice. Skip them here and
    // keep the backing readdir index as the cookie: cookies stay monotonic and a continuation that
    // resumes at a cookie lands on the same backing entry it would have without the skip.
    if (len == 1 && name[0] == '.') return 0;
    if (len == 2 && name[0] == '.' && name[1] == '.') return 0;
    wfs_dirent e;
    memset(&e, 0, sizeof e);
    e.name = name;
    e.name_len = len;
    e.type = type;
    e.next_cookie = index + 1;
    e.ino = (wfs_ino)ino;
    wfs_attr a;
    if (c->want_attr) {
        String p(c->dir_path);
        p.push_back('/');
        p.append(name, len);
        if (wfs::fs_lstat(p.c_str(), a) == 0) {
            a.parent = c->dir;
            intern(c->v, c->dir, name, len, a);
            e.attr = &a;
            e.type = a.type;
        }
    }
    return c->cb(c->ctx, &e);
}
} // namespace

extern "C" int wfs_readdir(wfs_view *v, wfs_ino dir, uint64_t cookie, int want_attr, wfs_readdir_cb cb, void *ctx) {
    if (!v || !cb) return -EINVAL;
    ReaddirCtx c{v, dir, String(), want_attr, cb, ctx};
    {
        Guard g(v->mu);
        if (int rc = path_locked(v, dir, c.dir_path)) return rc;
    }
    return wfs::fs_readdir(c.dir_path.c_str(), cookie, readdir_trampoline, &c);
}

// ---- mutations (M0: directly on the base directory; M1/M2: overlay + COW) ----

extern "C" int wfs_create(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_type type, uint32_t mode, wfs_attr *out) {
    if (!v || !name || !out) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = child_path(v, dir, name, len, p)) return rc;
    int rc;
    switch (type) {
    case WFS_T_FILE: rc = wfs::fs_mkfile(p.c_str(), mode); break;
    case WFS_T_DIR: rc = wfs::fs_mkdir(p.c_str(), mode); break;
    case WFS_T_FIFO: rc = wfs::fs_mkfifo(p.c_str(), mode); break;
    default: return -ENOTSUP;
    }
    if (rc) return rc;
    return wfs_lookup(v, dir, name, len, out);
}

extern "C" int wfs_symlink(wfs_view *v, wfs_ino dir, const char *name, size_t len, const char *target, size_t tlen, wfs_attr *out) {
    if (!v || !name || !target || !out) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = child_path(v, dir, name, len, p)) return rc;
    String t(target, tlen);
    if (int rc = wfs::fs_symlink(t.c_str(), p.c_str())) return rc;
    return wfs_lookup(v, dir, name, len, out);
}

extern "C" int wfs_link(wfs_view *v, wfs_ino ino, wfs_ino dir, const char *name, size_t len) {
    if (!v || !name) return -EINVAL;
    if (!v->writable) return -EROFS;
    String src, dst;
    if (int rc = path_of(v, ino, src)) return rc;
    if (int rc = child_path(v, dir, name, len, dst)) return rc;
    return wfs::fs_link(src.c_str(), dst.c_str());
}

extern "C" int wfs_unlink(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_ino ino) {
    if (!v || !name) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = child_path(v, dir, name, len, p)) return rc;
    bool is_dir = false;
    {
        Guard g(v->mu);
        auto it = v->nodes.find(ino);
        if (it != v->nodes.end()) is_dir = it->second.type == WFS_T_DIR;
    }
    if (int rc = wfs::fs_unlink(p.c_str(), is_dir)) return rc;
    // The node remembers exactly one name per inode (M0). If that was this name, the record is now
    // dangling: drop it so the next lookup of a surviving hardlink re-interns the inode under its
    // own name instead of resolving to the deleted one.
    Guard g(v->mu);
    auto it = v->nodes.find(ino);
    if (it != v->nodes.end() && it->second.parent == dir && it->second.name.size() == len &&
        memcmp(it->second.name.data(), name, len) == 0)
        v->nodes.erase(it);
    return 0;
}

extern "C" int wfs_rename(wfs_view *v, wfs_ino sdir, const char *sname, size_t slen, wfs_ino ddir, const char *dname,
                          size_t dlen, wfs_ino ino) {
    if (!v || !sname || !dname) return -EINVAL;
    if (!v->writable) return -EROFS;
    if (!valid_name(sname, slen) || !valid_name(dname, dlen)) return -EINVAL;
    // The backing rename and the node-table update are one critical section: every path is built
    // from the node table at request time, so a concurrent request must see either the old name or
    // the new one. Splitting them leaves a window in which the moved inode still resolves to the
    // source name that rename(2) has already removed, and a write-back landing in that window fails
    // with ENOENT (the kernel drops the data without telling the writer).
    Guard g(v->mu);
    String from, to;
    if (int rc = path_locked(v, sdir, from)) return rc;
    from.push_back('/');
    from.append(sname, slen);
    if (int rc = path_locked(v, ddir, to)) return rc;
    to.push_back('/');
    to.append(dname, dlen);
    // rename(2) unlinks whatever the destination name held. That inode keeps one name (M0); if it
    // was this one, its record now points at the file we are about to move in, so drop it.
    wfs_attr over;
    bool has_over = wfs::fs_lstat(to.c_str(), over) == 0 && over.ino != ino;
    if (int rc = wfs::fs_rename(from.c_str(), to.c_str())) return rc;
    if (has_over) {
        auto ov = v->nodes.find(over.ino);
        if (ov != v->nodes.end() && ov->second.parent == ddir && ov->second.name.size() == dlen &&
            memcmp(ov->second.name.data(), dname, dlen) == 0)
            v->nodes.erase(ov);
    }
    auto it = v->nodes.find(ino);
    if (it != v->nodes.end()) {
        it->second.parent = ddir;
        it->second.name.assign(dname, dlen);
    }
    return 0;
}

extern "C" int wfs_setattr(wfs_view *v, wfs_ino ino, const wfs_setattr_req *req, wfs_attr *out) {
    if (!v || !req || !out) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    if (int rc = wfs::fs_setattr(p.c_str(), *req)) return rc;
    return wfs_getattr(v, ino, out);
}

extern "C" int wfs_getxattr(wfs_view *v, wfs_ino ino, const char *name, void *buf, size_t cap, size_t *len) {
    if (!v || !name || !len) return -EINVAL;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    if (wfs::fs_xattr_is_own_quarantine(p.c_str(), name, strlen(name))) return -ENOATTR;
    return wfs::fs_getxattr(p.c_str(), name, buf, cap, len);
}
extern "C" int wfs_setxattr(wfs_view *v, wfs_ino ino, const char *name, const void *data, size_t len, int flags) {
    if (!v || !name) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    return wfs::fs_setxattr(p.c_str(), name, data, len, flags);
}
extern "C" int wfs_removexattr(wfs_view *v, wfs_ino ino, const char *name) {
    if (!v || !name) return -EINVAL;
    if (!v->writable) return -EROFS;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    return wfs::fs_removexattr(p.c_str(), name);
}
extern "C" int wfs_listxattr(wfs_view *v, wfs_ino ino, char *buf, size_t cap, size_t *len) {
    if (!v || !len) return -EINVAL;
    String p;
    if (int rc = path_of(v, ino, p)) return rc;
    // Fetch the full list, then drop entries the namespace hides.
    size_t full = 0;
    if (int rc = wfs::fs_listxattr(p.c_str(), nullptr, 0, &full)) return rc;
    if (full == 0) { *len = 0; return 0; }
    String tmp(full, '\0');
    if (int rc = wfs::fs_listxattr(p.c_str(), tmp.data(), full, &full)) return rc;
    size_t out = 0;
    for (size_t i = 0, start = 0; i < full; ++i) {
        if (tmp[i] != 0) continue;
        const char *nm = tmp.data() + start;
        size_t nl = i - start;
        start = i + 1;
        if (wfs::fs_xattr_is_own_quarantine(p.c_str(), nm, nl)) continue;
        if (buf) { if (out + nl + 1 > cap) return -ERANGE; memcpy(buf + out, nm, nl + 1); }
        out += nl + 1;
    }
    *len = out;
    return 0;
}

extern "C" int wfs_statfs(wfs_view *v, wfs_statfs_info *out) {
    if (!v || !out) return -EINVAL;
    return wfs::fs_statfs(v->base_dir.c_str(), *out);
}
