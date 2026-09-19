/*
 * worldfs.h — C ABI of the WorldFS core (BranchFS), M1 "clonefile World" model.
 *
 * The core owns two kinds of object (docs/M1_DESIGN.md §1):
 *
 *   Snapshot S<n>  an immutable whole-tree clone living inside the store, every entry
 *                  chflags(UF_IMMUTABLE) and directories stripped of their write bits.
 *                  Produced by `init` (from an arbitrary directory) and by `checkpoint`
 *                  (from a live World) — both go through wfs_snapshot_create().
 *   World    W<n>  a writable clonefile() clone of a Snapshot or of another World, living
 *                  at a user-visible path. All I/O inside a World is plain APFS; the core
 *                  is not on the data path at all.
 *
 * Identity is never the path (P1/P2): a World is identified by the `.world` marker file in
 * its root plus the (dev, ino) of the root directory recorded in the store. A moved World is
 * recognised and its row repaired; a copied World is refused until `adopt`ed.
 *
 * All functions return 0 on success, a negative errno, or one of the negative WFS_E_* codes
 * below for refusals that carry a specific remedy. wfs_strerror() renders both.
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

/* Store schema. A store written by a newer core is refused (P13). */
#define WFS_STORE_SCHEMA 2

#define WFS_PATH_MAX 1024
#define WFS_NAME_MAX 128

/* Name of the marker file in every World root. */
#define WFS_MARKER_NAME ".world"
/* Suffix of a half-built tree; removed by wfs_gc (P8). */
#define WFS_TMP_SUFFIX ".wfs-tmp"

typedef struct wfs_store wfs_store; /* metadata store for many snapshots and worlds */

typedef uint64_t wfs_id; /* 0 = invalid; snapshot ids and world ids are separate spaces */

typedef enum wfs_kind { WFS_K_NONE = 0, WFS_K_SNAPSHOT = 1, WFS_K_WORLD = 2 } wfs_kind;

typedef struct wfs_ref {
    wfs_kind kind;
    wfs_id id;
} wfs_ref;

/* Lifecycle of both snapshots and worlds. CREATING rows are the publish-order (P8) evidence
 * that a tree was being built when the process died; wfs_gc() collects them. */
typedef enum wfs_state {
    WFS_ST_CREATING = 0,
    WFS_ST_ACTIVE = 1,
    WFS_ST_TRASHED = 2, /* worlds only: sitting in <store>/trash, restorable */
    WFS_ST_DEAD = 3
} wfs_state;

/* Where a world came from. */
typedef enum wfs_origin {
    WFS_O_SNAPSHOT = 1, /* clone of a snapshot */
    WFS_O_WORLD = 2,    /* clone of a live world */
    WFS_O_ADOPTED = 3   /* an unregistered copy taken over by wfs_world_adopt (P2) */
} wfs_origin;

/* Refusals with a specific remedy. Chosen far outside errno so a caller can tell them apart. */
enum {
    WFS_E_CROSS_VOLUME = -1001,   /* P6: clonefile would return EXDEV; use --store or --copy */
    WFS_E_UNREGISTERED = -1002,   /* P2: marker present, inode does not match; use `adopt` */
    WFS_E_NOT_A_WORLD = -1003,    /* no .world marker here */
    WFS_E_PATH_REFUSED = -1004,   /* P7: /, $HOME, the store, inside a world or snapshot */
    WFS_E_LOW_SPACE = -1005,      /* P11 */
    WFS_E_SCHEMA = -1006,         /* P13: store written by a different schema */
    WFS_E_WORLD_BUSY = -1007,     /* P5/P12: another command holds this world's lock */
    WFS_E_SNAPSHOT_DIRTY = -1008, /* P3: snapshot content no longer matches its manifest */
    WFS_E_FOREIGN_STORE = -1009,  /* marker belongs to a different store */
    WFS_E_WORLD_MISSING = -1010,  /* the recorded path no longer holds this world */
    WFS_E_SOURCE_GONE = -1011     /* the snapshot this world was forked from is no longer there */
};

/* Human-readable text for a negative errno or a WFS_E_* code. Never NULL. */
const char *wfs_strerror(int rc);
const char *wfs_version(void);

/* ---- generic file types, shared with the platform layer ---- */

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

typedef struct wfs_timespec {
    int64_t sec;
    int64_t nsec;
} wfs_timespec;

typedef struct wfs_attr {
    uint64_t ino;
    uint64_t parent;
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

typedef struct wfs_statfs_info {
    uint64_t block_size, total_blocks, free_blocks, avail_blocks, total_files, free_files;
} wfs_statfs_info;

/* ---- store ---- */

/* Opens (creating if needed) a store directory: VERSION, metadata.db, snapshots/, trash/. */
int wfs_store_open(const char *store_dir, wfs_store **out);
void wfs_store_close(wfs_store *s);
/* ~/Library/Application Support/World/fs (macOS) or $XDG_DATA_HOME/world/fs. */
int wfs_store_default_dir(char *buf, size_t cap);
const char *wfs_store_dir(const wfs_store *s);

typedef struct wfs_store_stat {
    char dir[WFS_PATH_MAX];
    char store_id[40];
    int schema;
    uint64_t snapshots;
    uint64_t worlds_active, worlds_trashed, worlds_dead;
    uint64_t snapshot_entries; /* sum over live snapshots */
    uint64_t world_entries;    /* sum over active worlds */
    uint64_t volume_total_bytes, volume_free_bytes;
    /* df cannot see block sharing, so physical usage is estimated from the measured
     * 308 B/entry metadata cost of a clone on macOS 27.0 (CLONE_MODEL_MACOS27 §4). */
    uint64_t metadata_estimate_bytes;
} wfs_store_stat;
int wfs_store_status(wfs_store *s, wfs_store_stat *out);

/* P6: really clone a temporary file from src_dir into the store. st_dev equality does not
 * predict this. Returns 0, WFS_E_CROSS_VOLUME, or a negative errno. */
int wfs_store_clone_probe(wfs_store *s, const char *src_dir);

/* ---- snapshots ---- */

typedef struct wfs_snapshot_rec {
    wfs_id id;
    char name[WFS_NAME_MAX];
    char path[WFS_PATH_MAX];     /* <store>/snapshots/S<id>/root */
    char src_path[WFS_PATH_MAX]; /* directory it was taken from */
    wfs_id from_world;           /* non-zero when produced by `checkpoint` */
    int64_t created_at;          /* unix seconds */
    uint64_t entries, hardlinks; /* hardlinks = entries with nlink > 1 (P9) */
    int state;                   /* wfs_state */
} wfs_snapshot_rec;

/* init and checkpoint are the same operation: clone src_dir into the store and protect it. */
int wfs_snapshot_create(wfs_store *s, const char *src_dir, const char *name, wfs_id *out);
int wfs_snapshot_info(wfs_store *s, wfs_id id, wfs_snapshot_rec *out);
int wfs_snapshot_list(wfs_store *s, wfs_snapshot_rec *buf, size_t cap, size_t *count);

typedef struct wfs_verify_report {
    uint64_t checked;     /* manifest lines examined */
    uint64_t missing;     /* recorded but gone */
    uint64_t modified;    /* type/size/mode/mtime differ */
    uint64_t unprotected; /* UF_IMMUTABLE cleared (P3) */
    uint64_t extra;       /* entries present that the manifest does not list */
    char first_bad[WFS_PATH_MAX];
} wfs_verify_report;

/* P3: compare a snapshot against the manifest written when it was created. */
int wfs_snapshot_verify(wfs_store *s, wfs_id id, wfs_verify_report *out);

/* ---- worlds ---- */

typedef struct wfs_world_rec {
    wfs_id id;
    int origin; /* wfs_origin */
    wfs_id parent_world;
    wfs_id snapshot_id;
    char name[WFS_NAME_MAX];
    char path[WFS_PATH_MAX];
    uint64_t dir_dev, dir_ino;
    int state;               /* wfs_state */
    uint64_t fsevents_id;    /* FSEventsGetCurrentEventId() at fork, for T1.3 diff */
    uint64_t entries;        /* entry count inherited from the source */
    int64_t created_at, trashed_at;
    int present; /* the recorded path currently holds exactly this world */
} wfs_world_rec;

typedef struct wfs_fork_opts {
    const char *name;    /* world name, recorded in the marker; NULL = inherit the source's */
    int allow_fallback;  /* on EXDEV/ENOTSUP fall back to a 4-thread per-file clone/copy */
    int skip_space_check; /* bypass P11 */
} wfs_fork_opts;

/* Fork: clone `from` (a snapshot or a live world) into target_path. Publish order (P8):
 * clone into <target>.wfs-tmp, unprotect, write the marker, rename, then commit the row. */
int wfs_world_create(wfs_store *s, wfs_ref from, const char *target_path, const wfs_fork_opts *opts,
                     wfs_id *out);
int wfs_world_info(wfs_store *s, wfs_id id, wfs_world_rec *out);
/* The id the next world will most likely get, so a caller can build the default target path
 * ~/worlds/W<n>/<name> before the row exists. Racy by construction: two concurrent forks get
 * the same answer and the second one's mkdir/rename loses with EEXIST, which is the signal to
 * ask again. */
int wfs_world_next_id(wfs_store *s, wfs_id *out);
int wfs_world_list(wfs_store *s, int include_trashed, wfs_world_rec *buf, size_t cap, size_t *count);

/* P4: rename into <store>/trash and mark TRASHED; immediate != 0 deletes right away. */
int wfs_world_discard(wfs_store *s, wfs_id id, int immediate);
int wfs_world_restore(wfs_store *s, wfs_id id);

typedef struct wfs_identity {
    int has_marker;  /* a .world file was read */
    int registered;  /* marker + (dev, ino) match a row in this store */
    int is_copy;     /* marker matches a row but the inode does not (P2) */
    int moved;       /* registered, and the recorded path was stale: the row was repaired (P1) */
    wfs_id world_id;
    wfs_id snapshot_id;
    char store_id[40];
    char path[WFS_PATH_MAX];
    char name[WFS_NAME_MAX];
    uint64_t dev, ino;
} wfs_identity;

/* P1/P2. On a registered-but-moved world the store row is updated to `path` before returning.
 * Returns WFS_E_NOT_A_WORLD, WFS_E_UNREGISTERED or WFS_E_FOREIGN_STORE as appropriate; the
 * report is filled in either way. */
int wfs_world_verify_identity(wfs_store *s, const char *path, wfs_identity *out);
/* verify W<n>: identity of the recorded path. */
int wfs_world_verify(wfs_store *s, wfs_id id, wfs_identity *out);
/* P2: take over an unregistered copy as a new world, rewriting its marker. */
int wfs_world_adopt(wfs_store *s, const char *path, const char *name, wfs_id *out);

/* ---- diff (T1.3) --------------------------------------------------------------------------
 *
 * What changed in a world since it was forked, against the snapshot it came from. Two ways to
 * find out, one way to decide:
 *
 *   candidates   an FSEvents replay from the event id recorded at fork time gives the changed
 *                paths in O(changes) (measured: 800 changes over a 50k tree, 799 file-level
 *                paths, 21 ms). When FSEvents cannot account for everything -- MustScanSubDirs,
 *                a dropped event, a wrapped id, a cursor older than the volume's journal, or
 *                a world that was forked from another world rather than from a snapshot --
 *                the candidate set is the whole world, i.e. a full walk of both trees.
 *   verification every candidate is stat'ed on both sides and, when size and mtime disagree,
 *                its bytes are compared. Events are never trusted on their own (P10), so a
 *                spurious candidate costs one lstat and cannot produce a wrong answer.
 *
 * Only files are reported. A directory shows up only when it is empty and exists on just one
 * side; otherwise its files carry the news. Renames are reported as D + A (M1).
 */

typedef enum wfs_change {
    WFS_C_ADDED = 'A',    /* in the world, not in the snapshot */
    WFS_C_MODIFIED = 'M', /* in both, contents differ (or the type changed) */
    WFS_C_DELETED = 'D',  /* in the snapshot, not in the world */
    WFS_C_META = 'T'      /* in both, same contents, different mode/owner/flags/mtime/xattr */
} wfs_change;

typedef struct wfs_diff_entry {
    int change;        /* wfs_change */
    wfs_type type;     /* type in the world, or in the snapshot for WFS_C_DELETED */
    uint64_t size;     /* size in the world, or in the snapshot for WFS_C_DELETED */
    const char *path;  /* relative to the world root; only valid during the callback */
} wfs_diff_entry;

/* Called once per change, in ascending path order. A non-zero return stops the walk and
 * becomes the return value of wfs_world_diff(). */
typedef int (*wfs_diff_cb)(void *ctx, const wfs_diff_entry *e);

enum {
    WFS_DIFF_FULL = 1 << 0,       /* skip FSEvents: walk both trees (`diff --full`) */
    WFS_DIFF_NO_CONTENT = 1 << 1, /* never read bytes: size or mtime differing means M */
    /* Leave the xattr leg out of the T classification. It is two listxattr(2) calls per
     * otherwise-identical file and those cost ~10 us each on APFS, which is nothing for the
     * few hundred candidates of an FSEvents diff but is 8x the cost of a whole full scan
     * (measured 50k: 0.16 s -> 1.35 s). A file whose only change is an xattr then reads as
     * unchanged, so this is a speed-for-completeness trade the caller has to ask for. */
    WFS_DIFF_NO_XATTR = 1 << 2
};

/* Why the full two-tree walk was used. */
typedef enum wfs_diff_fallback {
    WFS_DF_NONE = 0,
    WFS_DF_REQUESTED,   /* WFS_DIFF_FULL */
    WFS_DF_NO_CURSOR,   /* no FSEvents id was recorded at fork */
    WFS_DF_FROM_WORLD,  /* forked from a live world (or adopted): the cursor does not cover
                         * the changes its parent had already made to the snapshot */
    WFS_DF_MUST_SCAN,   /* kFSEventStreamEventFlagMustScanSubDirs */
    WFS_DF_DROPPED,     /* UserDropped / KernelDropped: the consumer or the kernel fell behind */
    WFS_DF_WRAPPED,     /* the event id space wrapped or was reset */
    WFS_DF_STALE,       /* the cursor is older than the volume's event journal */
    WFS_DF_TIMEOUT,     /* HistoryDone never arrived */
    WFS_DF_UNSUPPORTED  /* no FSEvents on this platform */
} wfs_diff_fallback;

typedef struct wfs_diff_stats {
    uint64_t added, modified, deleted, meta;
    uint64_t candidates;  /* paths FSEvents proposed (0 for a full scan) */
    uint64_t compared;    /* paths stat'ed on both sides */
    uint64_t content_cmp; /* files whose bytes had to be read (size equal, mtime differs) */
    uint64_t bytes_read;
    uint64_t events_id;   /* the cursor that was used */
    int full_scan;        /* 1 = both trees were walked */
    int fallback;         /* wfs_diff_fallback */
    int64_t elapsed_us;
} wfs_diff_stats;

/* Diff a world against the snapshot it was forked from. The world's lock is never taken: a
 * diff is read-only, so a busy world is not a refusal. Returns 0 when the diff was computed
 * (even if nothing changed), WFS_E_SOURCE_GONE when the snapshot is no longer in the store,
 * WFS_E_WORLD_MISSING when the world is not at its recorded path, or a negative errno. */
int wfs_world_diff(wfs_store *s, wfs_id world, int flags, wfs_diff_cb cb, void *ctx);
/* The same, with the counters and the reason the full scan was used. `stats` may be NULL. */
int wfs_world_diff_ex(wfs_store *s, wfs_id world, int flags, wfs_diff_cb cb, void *ctx,
                      wfs_diff_stats *stats);

/* P7: refuse dangerous roots. for_target != 0 means "a fork is about to create this path"
 * (it must not exist yet); otherwise the path must already be a directory. */
int wfs_path_check(wfs_store *s, const char *path, int for_target);

typedef struct wfs_gc_report {
    uint64_t worlds_deleted;    /* trashed past the retention period */
    uint64_t snapshots_deleted; /* half-built snapshots */
    uint64_t tmp_removed;       /* stray *.wfs-tmp trees (P8) */
    uint64_t trash_orphans;     /* trash directories with no row */
    uint64_t entries_freed;
} wfs_gc_report;

/* retention_secs < 0 uses the default (7 days). */
int wfs_gc(wfs_store *s, int64_t retention_secs, wfs_gc_report *out);

#ifdef __cplusplus
}
#endif
#endif /* WORLDFS_H */
