/*
 * worldfs_fskit.h — the M0 view / namespace C ABI.
 *
 * This is the surface the FSKit frontend (macos/fskit) sits on: one mounted view per world,
 * a live logical inode table, and every namespace mutation. It is compiled only when the
 * core is built with -DWFS_FSKIT=ON; the M1 clonefile World model (worldfs.h) does not use
 * it, because a World is a plain APFS directory and the kernel talks to APFS directly.
 *
 * Kept because the FSKit passthrough is the frozen fallback frontend (docs/TASKS.md, M0).
 * Its measured ceiling is in docs/PERF_STUDY_RT_PARALLEL.md.
 */
#ifndef WORLDFS_FSKIT_H
#define WORLDFS_FSKIT_H

#include "worldfs/worldfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wfs_view wfs_view; /* a mounted view of one world; holds the live inode table */

typedef wfs_id wfs_world;
typedef uint64_t wfs_ino; /* logical inode, stable within a view */

enum { WFS_INO_INVALID = 0, WFS_INO_ROOT = 1 };

typedef struct wfs_dirent {
    const char *name;
    size_t name_len;
    wfs_ino ino;
    wfs_type type;
    uint64_t next_cookie; /* pass back to continue after this entry */
    const wfs_attr *attr; /* non-NULL only when want_attr was set */
} wfs_dirent;

/* Return 0 to continue enumeration, non-zero to stop (buffer full). */
typedef int (*wfs_readdir_cb)(void *ctx, const wfs_dirent *e);

enum {
    WFS_SET_MODE = 1 << 0,
    WFS_SET_UID = 1 << 1,
    WFS_SET_GID = 1 << 2,
    WFS_SET_FLAGS = 1 << 3,
    WFS_SET_SIZE = 1 << 4,
    WFS_SET_ATIME = 1 << 5,
    WFS_SET_MTIME = 1 << 6,
    WFS_SET_BTIME = 1 << 7
};

typedef struct wfs_setattr_req {
    uint32_t valid; /* WFS_SET_* mask of fields to apply */
    uint32_t mode, uid, gid, flags;
    uint64_t size;
    wfs_timespec atime, mtime, btime;
} wfs_setattr_req;

enum { WFS_XATTR_CREATE = 1 << 0, WFS_XATTR_REPLACE = 1 << 1 };

/* ---- view (one mount) ---- */
int wfs_view_open(wfs_store *s, wfs_world w, wfs_view **out);
void wfs_view_close(wfs_view *v);
wfs_world wfs_view_world(const wfs_view *v);

int wfs_getattr(wfs_view *v, wfs_ino ino, wfs_attr *out);
int wfs_lookup(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_attr *out);
int wfs_readdir(wfs_view *v, wfs_ino dir, uint64_t cookie, int want_attr, wfs_readdir_cb cb, void *ctx);
int wfs_readlink(wfs_view *v, wfs_ino ino, char *buf, size_t cap, size_t *len);
/* Kernel dropped its reference; the core may free the inode record. */
int wfs_forget(wfs_view *v, wfs_ino ino);

/* Data plane handoff: the frontend opens this path and does pread/pwrite/mmap itself. */
int wfs_backing_path(wfs_view *v, wfs_ino ino, int writable, char *buf, size_t cap);

/* ---- namespace mutations ---- */
int wfs_create(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_type type, uint32_t mode,
               wfs_attr *out);
int wfs_symlink(wfs_view *v, wfs_ino dir, const char *name, size_t len, const char *target, size_t tlen,
                wfs_attr *out);
int wfs_link(wfs_view *v, wfs_ino ino, wfs_ino dir, const char *name, size_t len);
int wfs_unlink(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_ino ino);
int wfs_rename(wfs_view *v, wfs_ino sdir, const char *sname, size_t slen, wfs_ino ddir, const char *dname,
               size_t dlen, wfs_ino ino);
int wfs_setattr(wfs_view *v, wfs_ino ino, const wfs_setattr_req *req, wfs_attr *out);

int wfs_getxattr(wfs_view *v, wfs_ino ino, const char *name, void *buf, size_t cap, size_t *len);
int wfs_setxattr(wfs_view *v, wfs_ino ino, const char *name, const void *data, size_t len, int flags);
int wfs_removexattr(wfs_view *v, wfs_ino ino, const char *name);
/* NUL-separated list, like listxattr(2). */
int wfs_listxattr(wfs_view *v, wfs_ino ino, char *buf, size_t cap, size_t *len);

int wfs_statfs(wfs_view *v, wfs_statfs_info *out);

#ifdef __cplusplus
}
#endif
#endif /* WORLDFS_FSKIT_H */
