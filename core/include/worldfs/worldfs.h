/*
 * worldfs.h — C ABI of the WorldFS core (BranchFS).
 *
 * The core owns: branch DAG, namespace overlay, version resolution, whiteouts,
 * rename metadata, changed set, diff, GC, and all namespace mutations on the
 * backing store. A frontend (FSKit on macOS, FUSE on Linux, WinFsp on Windows)
 * owns only VFS glue and the data plane: it opens the backing path the core
 * hands out and does pread/pwrite/mmap itself, so the kernel page cache stays
 * native.
 *
 * All functions return 0 on success or a negative errno.
 */
#ifndef WORLDFS_H
#define WORLDFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WFS_FS_SHORT_NAME "worldfs"
#define WFS_EXTENSION_BUNDLE_ID "world.forks.fs.extension"

typedef struct wfs_store wfs_store; /* metadata store + object storage for many worlds */
typedef struct wfs_view wfs_view;   /* a mounted view of one world; holds live inode table */

typedef uint64_t wfs_world; /* 0 = invalid */
typedef uint64_t wfs_ino;   /* logical inode, stable within a view */

enum { WFS_INO_INVALID = 0, WFS_INO_ROOT = 1 };

typedef enum wfs_type {
    WFS_T_UNKNOWN = 0,
    WFS_T_FILE,
    WFS_T_DIR,
    WFS_T_SYMLINK,
    WFS_T_FIFO,
    WFS_T_CHR,
    WFS_T_BLK,
    WFS_T_SOCK
} wfs_type;

typedef enum wfs_world_state { WFS_W_ACTIVE = 0, WFS_W_DISCARDED = 1, WFS_W_DEAD = 2 } wfs_world_state;

typedef struct wfs_timespec {
    int64_t sec;
    int64_t nsec;
} wfs_timespec;

typedef struct wfs_attr {
    wfs_ino ino;
    wfs_ino parent;
    wfs_type type;
    uint32_t mode; /* permission bits only */
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t flags; /* BSD st_flags; 0 elsewhere */
    uint64_t size;
    uint64_t alloc_size;
    wfs_timespec atime, mtime, ctime, btime;
    int shared; /* backing object is shared with other worlds (immutable) */
} wfs_attr;

typedef struct wfs_dirent {
    const char *name;
    size_t name_len;
    wfs_ino ino;
    wfs_type type;
    uint64_t next_cookie;   /* pass back to continue after this entry */
    const wfs_attr *attr;   /* non-NULL only when want_attr was set */
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

typedef struct wfs_change {
    const char *path; /* relative to world root */
    const char *old_path; /* for renames, else NULL */
    char kind;        /* 'A' added, 'M' modified, 'D' deleted, 'R' renamed, 'T' metadata */
} wfs_change;
typedef int (*wfs_change_cb)(void *ctx, const wfs_change *c);

/* ---- library ---- */
const char *wfs_version(void);

/* ---- store ---- */
int wfs_store_open(const char *store_dir, wfs_store **out);
void wfs_store_close(wfs_store *s);

/* ---- worlds (provider API: world.fs.init / fork / discard / list) ---- */
/* Base world over an existing directory; idempotent per base_dir. */
int wfs_world_init(wfs_store *s, const char *base_dir, wfs_world *out);
/* O(1): one metadata row, no tree walk (arch.md §20). */
int wfs_world_fork(wfs_store *s, wfs_world parent, wfs_world *out);
int wfs_world_discard(wfs_store *s, wfs_world w);
/* Fills up to cap ids; *count receives total available. */
int wfs_world_list(wfs_store *s, wfs_world *buf, size_t cap, size_t *count);
int wfs_world_info(wfs_store *s, wfs_world w, wfs_world *parent, wfs_world_state *state,
                   char *base_dir, size_t cap);
int wfs_diff(wfs_store *s, wfs_world w, wfs_change_cb cb, void *ctx);

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

/* Data plane handoff. writable != 0 is the COW boundary (arch.md §6): the core
 * guarantees the returned path is a private backing object before returning. */
int wfs_backing_path(wfs_view *v, wfs_ino ino, int writable, char *buf, size_t cap);

/* ---- namespace mutations ---- */
int wfs_create(wfs_view *v, wfs_ino dir, const char *name, size_t len, wfs_type type, uint32_t mode, wfs_attr *out);
int wfs_symlink(wfs_view *v, wfs_ino dir, const char *name, size_t len, const char *target, size_t tlen, wfs_attr *out);
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

/* Volume-wide statistics of the backing store. */
typedef struct wfs_statfs_info {
    uint64_t block_size, total_blocks, free_blocks, avail_blocks, total_files, free_files;
} wfs_statfs_info;
int wfs_statfs(wfs_view *v, wfs_statfs_info *out);

#ifdef __cplusplus
}
#endif
#endif /* WORLDFS_H */
