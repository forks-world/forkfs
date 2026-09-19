// Internal types of the WorldFS core. Not part of the C ABI.
// Dependency policy: arch.md §39. Strings: smallstring. Containers: Containa. No std::string /
// unordered_map / std::mutex. Bump allocation (when needed): Arena.
#pragma once
#include "worldfs/worldfs.h"
#include "worldfs/worldfs_fskit.h"

#include <container/dense_map.hpp>
#include <container/small_vectra.hpp>
#include <smallstring.hpp>

#include <mutex>   // std::lock_guard only (header-only)
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string_view>

struct sqlite3;

namespace wfs {

using String = small::small_string;
template <typename T>
using Vec = stdb::container::small_vectra<T, 1>;

// Portable mutex wrapper (pthread now; SRWLOCK on Windows later). Works with std::lock_guard.
class Mutex {
public:
    Mutex() { pthread_mutex_init(&m_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t *raw() { return &m_; }
private:
    pthread_mutex_t m_;
};
using Guard = std::lock_guard<Mutex>;

// One live logical inode (FSKit view only). Path is derived from the parent chain so that a
// directory rename is O(1) and children never hold stale paths.
struct NodeRec {
    wfs_ino parent;
    wfs_type type;
    String name;
};

using InodeTable = stdb::container::dense_map<uint64_t, NodeRec>;

// ---- platform layer ----------------------------------------------------------------------
// platform_posix.cpp: everything POSIX. platform_darwin.cpp: clonefile / chflags / FSEvents.

int fs_lstat(const char *path, wfs_attr &out);
int fs_readlink(const char *path, char *buf, size_t cap, size_t *len);
int fs_mkfile(const char *path, uint32_t mode);
int fs_mkdir(const char *path, uint32_t mode);
int fs_mkfifo(const char *path, uint32_t mode);
int fs_symlink(const char *target, const char *path);
int fs_link(const char *existing, const char *path);
int fs_unlink(const char *path, bool is_dir);
int fs_rename(const char *from, const char *to);
int fs_setattr(const char *path, const wfs_setattr_req &req);
int fs_getxattr(const char *path, const char *name, void *buf, size_t cap, size_t *len);
int fs_setxattr(const char *path, const char *name, const void *data, size_t len, int flags);
int fs_removexattr(const char *path, const char *name);
int fs_listxattr(const char *path, char *buf, size_t cap, size_t *len);
int fs_statfs(const char *path, wfs_statfs_info &out);
// True if `name` is a quarantine xattr that this very process stamped on the file (macOS only).
// Such entries are hidden from the namespace; see platform_posix.cpp for why.
bool fs_xattr_is_own_quarantine(const char *path, const char *name, size_t name_len);
int fs_realpath(const char *path, String &out);
// realpath() of a path that does not exist yet: resolves the parent and re-appends the leaf.
int fs_realpath_parent(const char *path, String &out);
int fs_mkdir_p(const char *path);
bool fs_readdir_emits_dots(bool with_attrs);
int fs_readdir(const char *path, uint64_t skip,
               int (*cb)(void *ctx, const char *name, size_t len, uint64_t ino, wfs_type type, uint64_t index),
               void *ctx);

// Totals gathered by one walk of a tree.
struct TreeStats {
    uint64_t entries = 0;   // everything below the root, root itself excluded
    uint64_t dirs = 0;
    uint64_t files = 0;
    uint64_t hardlinks = 0; // entries with st_nlink > 1: clonefile breaks these (P9)
};

// Manifest of a snapshot: one line per entry, written from the protect walk so the tree is
// only traversed once. Lines are unordered (the walk is parallel); wfs_snapshot_verify reads
// them back line by line.
struct Manifest {
    FILE *f = nullptr;
    Mutex mu;
    void line(const char *rel, const struct stat &st, bool is_dir);
};

// Per-entry visitor for fs_walk_tree. `rel` is relative to the walk root (never leading '/').
// A non-zero return aborts the walk and becomes its result.
using fs_entry_fn = int (*)(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir);

enum fs_dir_order {
    FS_DIRS_PRE = 0,  // a directory is visited when it is opened
    FS_DIRS_POST = 1  // directories are visited after every file, deepest first
};

// Parallel tree walk (pthread, `threads` workers, dynamic per-directory queue). The root
// itself is visited too. 4 workers is the APFS metadata-transaction sweet spot measured in
// docs/CLONE_MODEL_MACOS27.md §9.
int fs_walk_tree(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_fn fn);

int fs_count_entries(const char *root, TreeStats &out);
// The gate-protection equivalent of fs_protect_tree: one walk that writes the manifest and
// counts, and changes nothing at all. The protection itself is one chmod on the root.
int fs_scan_tree(const char *root, TreeStats *stats, Manifest *manifest);
int fs_free_space(const char *path, uint64_t *avail, uint64_t *total);
int fs_remove_tree(const char *root);   // unprotects first on Darwin
// T2.1: the same 4-thread walker, used to unlink. Files are removed in the parallel phase,
// directories in the serial deepest-first tail. `entries` is incremented by what was actually
// removed (the root itself is not counted). Any error at all falls back to fs_remove_tree, so a
// non-zero return means even that could not finish the job.
int fs_remove_tree_parallel(const char *root, int threads, uint64_t *entries);

// ---- Darwin-only primitives (stubs elsewhere) --------------------------------------------

// P6: really clone a temp file from src_dir into store_dir. Returns 0, -EXDEV, or -errno.
int fs_clone_probe(const char *store_dir, const char *src_dir);
// clonefile(src, dst, CLONE_NOFOLLOW). With allow_fallback, EXDEV/ENOTSUP degrade to a
// 4-thread per-file clonefileat walk (per-file copy across volumes).
int fs_clone_tree(const char *src, const char *dst, bool allow_fallback);
// `--hard` protection: chflags(UF_IMMUTABLE) on every entry, directories last, write bits
// stripped from directories. Fills stats and (optionally) the manifest in the same walk.
int fs_protect_tree(const char *root, TreeStats *stats, Manifest *manifest);
// The inverse: directories first, owner write restored.
int fs_unprotect_tree(const char *root);
// FSEventsGetCurrentEventId() — recorded at fork time for the T1.3 O(changes) diff.
uint64_t fs_events_current_id(void);

} // namespace wfs

struct wfs_view {
    wfs_store *store = nullptr;
    wfs_world world = 0;
    wfs::String base_dir;   // backing root of the world this view exposes
    bool writable = false;
    wfs::Mutex mu;
    wfs::InodeTable nodes;
};
