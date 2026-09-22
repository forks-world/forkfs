// Internal types of the WorldFS core. Not part of the C ABI.
// Dependency policy: arch.md §39. Strings: smallstring. Containers: Containa. No std::string /
// unordered_map / std::mutex. Bump allocation (when needed): Arena.
#pragma once
#include "worldfs/worldfs.h"
#include "worldfs/worldfs_fskit.h"

#include <container/dense_map.hpp>
#include <container/small_vectra.hpp>
#include <smallstring.hpp>

#include <errno.h>
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
// platform_posix.cpp: POSIX. platform_darwin.cpp: clonefile / chflags / FSEvents.
// platform_linux.cpp: native directory trees backed by per-file FICLONE (XFS first).

// Starts up to `want` workers running fn(arg) and returns how many really started, writing THEIR
// handles into th[0..n). The handles must be contiguous: pthread_create can fail for one slot and
// succeed for the next, and a loop that writes th[i] while counting into `started` then joins an
// uninitialised handle and never joins the live worker -- which is free to keep reading the
// caller's stack after the caller has returned. `th` must have room for `want` handles.
// wfs_test_thread_fail_mask (worldfs.h) refuses the slots whose bit is set; it is 0 in every run
// that is not a test of this function.
int threads_start(pthread_t *th, int want, void *(*fn)(void *), void *arg);

// ---- PR #1 review (3rd round): telling a crashed producer from a slow one --------------------
//
// A CREATING row means "somebody is building this tree". gc used to read it as "somebody WAS
// building this tree and is not any more", which is only true once the producer is gone -- a
// clone of a big tree easily outlives the two-second pause before an auto-spawned worker starts
// its cheap pass, and that worker would then delete an in-flight fork's tree and its row.
//
// Who the producer was: the pid plus that process's own start time. kill(pid, 0) alone is not
// enough, because pids are reused; a pid that is alive but started at a different second than
// the row recorded is somebody else. Unknown (0, or a kernel that will not say) is read as
// "still alive", because refusing to collect is always the safe mistake.
int64_t fs_pid_start_sec(int64_t pid);
bool producer_alive(int64_t pid, int64_t start_sec);

// ---- PR #1 review (34th round, P1): who else has this file open right now? -------------------
//
// The 2 -> 3 upgrade (store.cpp) shuts the door on M1 binaries by bumping the VERSION file
// before it migrates the database -- which stops every M1 process that STARTS after the rename.
// It does nothing about one that completed wfs_store_open() a millisecond before it: M1 holds no
// store-wide lock of any kind, so exclusion cannot need M1's cooperation and has to be asked of
// the operating system instead.
//
// Fills `out` with the pid of every process other than this one that has `path` open, and
// answers 0 even when there are none. A negative errno means the question could not be asked at
// all, and -ENOSYS means this platform cannot ask it -- both of which the upgrade must read as
// "cannot tell", never as "nobody". Darwin implements it with proc_listpidspath(3) from libproc
// (part of libSystem: no new dependency), which an unprivileged process may run for its own
// processes; Linux is deferred with the -ENOSYS stub (docs/M1_DESIGN.md P13).
//
// It costs ~100 ms on a machine with ~900 processes, because it is a scan of all of them. That
// is paid once, on the one open that takes a schema-2 store over, and never again.
int fs_other_holders(const char *path, Vec<int64_t> &out);
// That process's executable path, for the message that names it. Empty when the kernel will not
// say -- which is normal for a process belonging to another user.
void fs_pid_exe(int64_t pid, String &out);
// How old a CREATING row must be before gc will touch it even with its producer gone: the
// second half of the rule, and the answer for rows written by a core that recorded no producer
// at all. 60 s, or WORLD_GC_CREATING_MIN_AGE seconds.
int64_t creating_min_age_secs(void);

// ---- PR #1 review (5th round): the middle of a discard ---------------------------------------
//
// Resolves every WFS_ST_TRASHING row in the store: the tree is either still at its home path
// (the rename never happened -- back to ACTIVE) or already at trash_path (it did -- on to
// TRASHED, unless the snapshot is still somebody's baseline, in which case it goes back). Cheap
// when there is nothing to do: two indexed SELECTs that return no rows. Run on every store open
// and at the start of every gc, before anything classifies the trash. Both counters may be null.
int trashing_recover(wfs_store *s, uint64_t *restored, uint64_t *finished);

// ---- PR #1 review: the retry cap for anything gc cannot remove -------------------------------
//
// Every gc wake is a new process, so "this has failed before" cannot live in memory: the count
// goes in the store's meta table under `key`. The cap is what stops something that will never
// budge -- an ACL, an EPERM, a transient EIO that is not transient -- from waking a worker every
// two seconds for ever. It is a retry limit and never a licence to report the thing as gone:
// whatever failed is still on disk, still counted by `gc --status`, and still reported by every
// run. Trash entries, abandoned fork trees and half-built snapshots key on the tree's name;
// since the 8th round stale pool entries do too (they used to be deleted from the database
// whether or not their tree went).
extern const int64_t kGcFailCap;
int64_t gc_fail_bump(wfs_store *s, const char *key);
void gc_fail_clear(wfs_store *s, const char *key);
// The same count, read without spending one of the retries: wfs_gc_pending() asks whether a
// directory that cannot be read is still worth waking a worker for, and a question is not a
// failure (PR #1 review, 28th round). 0 when nothing has ever failed under `key`.
int64_t gc_fail_get(wfs_store *s, const char *key);

// ---- PR #1 review (27th/28th rounds, P2): a directory the collector could not READ -----------
//
// Every directory the collector scans -- <store>/trash's row-less orphans, the `*.wfs-tmp` sweep
// and the counting pass of <store>/snapshots, <store>/pool and its S<n>s, the <store>/tmp age
// sweep -- used to skip an opendir(2) that failed, in silence, and readdir(3), which reports its
// own failure through errno alone, was never asked at all. One EACCES or EIO there therefore
// answered "there is nothing in here": what was in the directory was neither collected nor
// counted, the run said nothing, `gc --status` reported a clean store, and since wfs_gc_pending()
// is the same scan the worker chain stopped as well.
//
// Only ENOENT is evidence of absence -- the 13th round's rule for the store scan. Everything
// else is recorded here (the 27th round's PoolUnreadable, made the store's in the 28th because
// the trash and snapshots need exactly the same thing and one count reads better than three):
// the collector's pass bumps the shared retry counter keyed on the DIRECTORY's path and sets
// work_remains under the cap, so the chain comes back for it; a pass that reads a directory
// right through clears its counter again; and a pass that only counts (`gc --status`,
// wfs_gc_pending) writes nothing at all, which is what keeps its "0 waiting" honest.
struct DirUnreadable {
    uint64_t count = 0;   // directories the collector could not scan
    int err = 0;          // the first one's errno, positive
    String path;          // and which directory it was
};

// The counter's key for a directory (`gcfail:dir:<path>`). Its own prefix: a directory that
// cannot be read is not a tree that would not go, and wfs_gc_pending() asks for exactly these.
String gc_dir_fail_key(const char *dir);
// Record one. `collector` is false on the counting passes, which then only fill `*u`.
void gc_note_unreadable(wfs_store *s, DirUnreadable *u, const char *dir, int err, bool collector,
                        int *work_remains);
// And the other half: this pass read `dir` right through, so whatever it could not read before
// is behind it. Only the collector's pass clears; the counting passes write nothing.
void gc_note_readable(wfs_store *s, const char *dir, bool collector);
// Is some directory still unreadable and still under the retry cap? The counter is the record --
// every gc wake is a new process -- so this is one indexed range over meta and no readdir at
// all, which is what lets wfs_gc_pending() answer for <store>/snapshots and <store>/pool
// without walking them on the fork path.
bool gc_dirs_unreadable_pending(wfs_store *s);

// PR #1 review (12th round): "is there anything at this path?", with the errno kept.
// A bool over lstat(2) -- the `exists()` every file in this core had one of -- answers "no" for
// EACCES on a parent directory, for EIO, for a volume that is not mounted any more and for
// ENAMETOOLONG, and the collector then acted on that "no": a row marked DEAD, a row deleted, a
// tree nothing in the store knew about any more. An error is not an absence. So the question is
// asked here and the answer is the errno -- 0 (something is there, `st` filled when it is given)
// or -errno -- and fs_gone() is the only verdict that means "genuinely not there". Everything
// about to destroy something, or to write a row it cannot be talked out of, asks fs_gone(); a
// plain "does this exist so I can create it" may still ask the bool. `follow` picks stat(2).
int fs_probe(const char *path, struct stat *st = nullptr, bool follow = false);
inline bool fs_gone(int rc) { return rc == -ENOENT || rc == -ENOTDIR; }

int fs_lstat(const char *path, wfs_attr &out);
int fs_readlink(const char *path, char *buf, size_t cap, size_t *len);
int fs_mkfile(const char *path, uint32_t mode);
int fs_mkdir(const char *path, uint32_t mode);
int fs_mkfifo(const char *path, uint32_t mode);
int fs_symlink(const char *target, const char *path);
int fs_link(const char *existing, const char *path);
int fs_unlink(const char *path, bool is_dir);
int fs_rename(const char *from, const char *to);
// rename(2) that refuses to replace anything already at `to` (-EEXIST). Used for the one rename
// whose destination is a path outside the store: the fork's publish (P7/P8).
int fs_rename_excl(const char *from, const char *to);
// rename(2) that EXCHANGES the two entries instead of replacing one with the other, atomically
// (PR #1 review, 36th round). The one caller is the 2 -> 3 upgrade, which may not leave the name
// an M1 binary opens free even for an instant. -ENOSYS where the platform cannot do it.
int fs_rename_swap(const char *a, const char *b);
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
// `deadline_us`/`partial` behave exactly as they do for fs_remove_tree_parallel below, and are
// what lets the parallel deleter's fallback stay inside the gc worker's batch limit (PR #1
// review, 4th round): the unprotect walk and the depth-first unlink both read the clock per
// entry, and a tree the deadline cut short comes back with *partial = 1 and rc 0.
// `entries`, when given, is incremented by what was removed below `root` (the root itself is
// not counted), exactly as for fs_remove_tree_parallel -- which is where it matters: the
// parallel deleter's fallback is this function, and its work belongs in the same count.
int fs_remove_tree(const char *root, int64_t deadline_us = 0, int *partial = nullptr,
                   uint64_t *entries = nullptr);   // unprotects first on Darwin
// T2.1: the same 4-thread walker, used to unlink. Files are removed in the parallel phase,
// directories in the serial deepest-first tail. `entries` is incremented by what was actually
// removed (the root itself is not counted). Any error at all falls back to fs_remove_tree, so a
// non-zero return means even that could not finish the job. The fallback is handed the same
// deadline, so an error in a huge tree can no longer turn a two-second wake into a minutes-long
// one (PR #1 review, 4th round).
//
// PR #1 review: `deadline_us` is an fs_mono_us() stamp (0 = no limit) and it is checked per
// entry, not per tree -- one big world is millions of unlinks, and the gc worker's whole batch
// limit is worth nothing if it can only be enforced between trees. When the deadline passes
// mid-tree the walk stops where it is, *partial becomes 1 and the return is 0: the caller is
// deleting a `.deleting` tree, which is resumable by construction, so stopping is not an error.
int fs_remove_tree_parallel(const char *root, int threads, uint64_t *entries,
                            int64_t deadline_us = 0, int *partial = nullptr);
// PR #1 review (26th round, P1): the same deletion done through a descriptor instead of a name.
// Both removers above re-resolve the whole path on every opendir/unlink/rmdir, so what they
// delete is whatever the name leads to at the instant of each call -- and the trash collector's
// authority to delete comes from an identity check (the row's dev/ino) made one call earlier.
// This removes everything INSIDE `dirfd` and leaves the now-empty directory itself to the
// caller, whose descriptor `dirfd` is: nothing in the walk ever names the root, every step is
// openat/fstatat/unlinkat relative to a descriptor already held, and O_NOFOLLOW /
// AT_SYMLINK_NOFOLLOW are on every one of them, so nothing outside the tree that descriptor
// points at can be reached however the names underneath it are moved while the walk runs.
// `entries`, `deadline_us` and `partial` mean exactly what they mean for fs_remove_tree_parallel
// (the root is not counted; the deadline is read per entry; a walk the deadline cut short comes
// back 0 with *partial = 1). `threads` is accepted and ignored: this walk is serial, because
// making the parallel walker descend by descriptor is a rewrite of the walker -- the measured
// cost is in docs/TASKS.md (26th round).
int fs_remove_tree_fd(int dirfd, int threads, uint64_t *entries, int64_t deadline_us = 0,
                      int *partial = nullptr);

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

// Probe the native storage strategy and volume compatibility. Linux ext4 copies;
// XFS/Btrfs require reflinks. Returns 0, -EXDEV, or -errno.
int fs_clone_probe(const char *store_dir, const char *src_dir);
// Native tree duplication without following symlinks: clonefile on Darwin,
// reflinks on Linux XFS/Btrfs, sparse-aware copies on ext4. allow_fallback also
// permits copies across volumes or when cloning is unsupported.
int fs_clone_tree(const char *src, const char *dst, bool allow_fallback);
// `--hard` protection: chflags(UF_IMMUTABLE) on every entry, directories last, write bits
// stripped from directories. Fills stats and (optionally) the manifest in the same walk.
int fs_protect_tree(const char *root, TreeStats *stats, Manifest *manifest);
// The inverse: directories first, owner write restored. `deadline_us` is an fs_mono_us() stamp
// (0 = no limit) checked per entry; the walk stops with -ECANCELED when it passes, which is how
// fs_remove_tree keeps its own deadline over a walk it does not otherwise control.
int fs_unprotect_tree(const char *root, int64_t deadline_us = 0);
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
