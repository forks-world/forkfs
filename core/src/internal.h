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

// What a walk already knows about an entry's extended attributes, without anyone having
// called listxattr(2). On Darwin it comes from ATTR_CMNEXT_EXT_FLAGS / EF_NO_XATTRS, which
// only ever *denies* xattrs: the file system says "this one has none at all" or says nothing.
// Measured on 27.0 over 137,665 entries: 117,168 EF_NO_XATTRS, 0 of them with a listxattr
// that returned anything (and 5,809 entries without the bit whose listxattr was empty — the
// bit is conservative in the one direction that is safe).
enum fs_xattr_state : uint8_t {
    FS_XATTR_UNKNOWN = 0,  // nobody asked, or the file system would not say: go and look
    FS_XATTR_NONE = 1,     // EF_NO_XATTRS: there are none, listxattr(2) would return 0
    FS_XATTR_SOME = 2      // at least one (or the answer was not conclusive): go and look
};

// One entry handed to a walk. `st` is a real lstat(2)-equivalent: on the getattrlistbulk(2)
// path every field is filled from the attributes and was cross-checked against lstat over
// 148,593 entries, field by field, before this walker replaced the old one.
struct FsEntry {
    const char *path;     // absolute (the walk root plus `rel`)
    const char *rel;      // relative to the walk root, never a leading '/'; "" is the root
    const struct stat *st;
    bool is_dir;
    uint8_t xattr;        // fs_xattr_state
};

// Per-entry visitor for fs_walk_tree. `rel` is relative to the walk root (never leading '/').
// A non-zero return aborts the walk and becomes its result.
using fs_entry_fn = int (*)(void *ctx, const char *path, const char *rel, const struct stat &st, bool is_dir);
// The same, for a caller that also wants the xattr verdict (diff.cpp).
using fs_entry_ex_fn = int (*)(void *ctx, const FsEntry &e);

enum fs_dir_order {
    FS_DIRS_PRE = 0,  // a directory is visited when it is opened
    FS_DIRS_POST = 1  // directories are visited after every file, deepest first
};

// Parallel tree walk (pthread, `threads` workers, dynamic per-directory queue). The root
// itself is visited too. 4 workers is the APFS metadata-transaction sweet spot measured in
// docs/CLONE_MODEL_MACOS27.md §9.
int fs_walk_tree(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_fn fn);
// The same walk, with FsEntry::xattr filled in. fs_walk_tree is this with the verdict dropped.
int fs_walk_tree_ex(const char *root, int threads, fs_dir_order order, void *ctx, fs_entry_ex_fn fn);

int fs_count_entries(const char *root, TreeStats &out);
// The gate-protection equivalent of fs_protect_tree: one walk that writes the manifest and
// counts, and changes nothing at all. The protection itself is one chmod on the root.
int fs_scan_tree(const char *root, TreeStats *stats, Manifest *manifest);
int fs_free_space(const char *path, uint64_t *avail, uint64_t *total);
// One monotonic clock for everything that has a deadline: the gc worker sets its batch limit in
// world.cpp and the deleter enforces it in platform_posix.cpp, so the two have to agree on what
// "now" is. Microseconds, CLOCK_MONOTONIC.
int64_t fs_mono_us(void);
int fs_remove_tree(const char *root);   // unprotects first on Darwin
// T2.1: the same 4-thread walker, used to unlink. Files are removed in the parallel phase,
// directories in the serial deepest-first tail. `entries` is incremented by what was actually
// removed (the root itself is not counted). Any error at all falls back to fs_remove_tree, so a
// non-zero return means even that could not finish the job.
//
// PR #1 review: `deadline_us` is an fs_mono_us() stamp (0 = no limit) and it is checked per
// entry, not per tree -- one big world is millions of unlinks, and the gc worker's whole batch
// limit is worth nothing if it can only be enforced between trees. When the deadline passes
// mid-tree the walk stops where it is, *partial becomes 1 and the return is 0: the caller is
// deleting a `.deleting` tree, which is resumable by construction, so stopping is not an error.
int fs_remove_tree_parallel(const char *root, int threads, uint64_t *entries,
                            int64_t deadline_us = 0, int *partial = nullptr);

// ---- Darwin-only primitives (stubs elsewhere) --------------------------------------------

// One entry of a getattrlistbulk(2) pass. `st` is complete and `xattr` is fs_xattr_state.
using fs_bulk_entry_fn = int (*)(void *ctx, const char *name, size_t nlen, const struct stat &st,
                                 uint8_t xattr);
// Enumerate an already-open directory with getattrlistbulk(2), one call per batch instead of
// one fstatat(2) per entry, and hand every entry to `fn` with the xattr verdict attached.
// Returns 0 when the directory was enumerated to the end (or `fn` asked to stop, in which case
// *cb_rc is its non-zero return), or a negative errno. **-ENOTSUP means "this file system
// cannot do it, use readdir(3)"** and is only ever returned before the first entry has been
// handed over, so the caller can start the directory again without reporting anything twice.
// `dirfd` is left open and its directory offset is unspecified afterwards.
int fs_bulk_dir(int dirfd, void *ctx, fs_bulk_entry_fn fn, int *cb_rc);
// lstat(2) plus the xattr verdict, for one path: getattrlist(2) on Darwin, lstat(2) with
// FS_XATTR_UNKNOWN everywhere else. This is what the other side of a diff is read with.
int fs_lstat_xattr(const char *path, struct stat &st, uint8_t &xattr);

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
