/*
 * worldfs.h — C ABI of the WorldFS core (BranchFS), M1 "clonefile World" model.
 *
 * The core owns two kinds of object (docs/M1_DESIGN.md §1):
 *
 *   Snapshot S<n>  an immutable whole-tree clone living inside the store, protected by a gate:
 *                  the snapshot root directory is mode 0000, so nothing can traverse, list,
 *                  read or write anywhere inside it, and the entries themselves are untouched.
 *                  `--hard` instead protects every entry with chflags(UF_IMMUTABLE).
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
/* Mode of a gate-protected snapshot root: nothing gets in. */
#define WFS_GATE_CLOSED 0000
/* Mode of the same root while a clone of it is in flight (fork / checkpoint / verify). */
#define WFS_GATE_OPEN 0500
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
    WFS_E_WORLD_MISSING = -1010   /* the recorded path no longer holds this world */
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
    int hard;                    /* 1 = per-entry UF_IMMUTABLE, 0 = gate directory (default) */
    uint32_t root_mode;          /* the source root's own mode, restored on the fork's clone */
} wfs_snapshot_rec;

typedef struct wfs_snapshot_opts {
    const char *name; /* NULL = the source directory's basename */
    /* P3, the slow variant: chflags(UF_IMMUTABLE) on every entry and write bits stripped from
     * every directory. Costs a full parallel walk here (0.68 s / 50k entries) and a second one
     * on every fork from this snapshot (0.73 s / 50k). The default gate protection costs one
     * chmod and forks need no unprotect walk at all. */
    int hard;
    /* P5: proceed even when the source world has a live `world exec` lock. */
    int force;
} wfs_snapshot_opts;

/* init and checkpoint are the same operation: clone src_dir into the store and protect it.
 * Default protection is the gate: the snapshot root becomes mode 0000 and everything below it
 * is left exactly as cloned. The root is briefly reopened to WFS_GATE_OPEN, under an exclusive
 * flock on the snapshot's manifest, whenever the core has to read the tree (fork, checkpoint
 * from the snapshot, verify). opts may be NULL. */
int wfs_snapshot_create(wfs_store *s, const char *src_dir, const wfs_snapshot_opts *opts, wfs_id *out);
int wfs_snapshot_info(wfs_store *s, wfs_id id, wfs_snapshot_rec *out);
int wfs_snapshot_list(wfs_store *s, wfs_snapshot_rec *buf, size_t cap, size_t *count);

typedef struct wfs_verify_report {
    uint64_t checked;     /* manifest lines examined */
    uint64_t missing;     /* recorded but gone */
    uint64_t modified;    /* type/size/mode/mtime differ */
    uint64_t unprotected; /* P3: UF_IMMUTABLE cleared (--hard), or the gate left open */
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
    int force;            /* P5: fork from a world that has a live `world exec` lock */
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

/* P4: rename into <store>/trash and mark TRASHED; immediate != 0 deletes right away.
 * P5: refused with WFS_E_WORLD_BUSY while a `world exec` lock is live, unless force != 0. */
int wfs_world_discard(wfs_store *s, wfs_id id, int immediate, int force);
int wfs_world_restore(wfs_store *s, wfs_id id);

/* ---- the exec lock (P5) ----------------------------------------------------------------
 *
 * `world exec W<n>` holds <store>/locks/W<n>.lock: pid, start time and command line, under an
 * exclusive flock that the kernel drops when the process dies. discard, checkpoint and fork
 * from that world refuse while it is live. A lock whose pid is gone (kill(pid, 0) fails, or
 * the flock can be taken) is stale and is removed silently.
 *
 * The lock lives in the store rather than in the World root on purpose: the World root is the
 * user's project tree, and a lock file there would show up in `git status` and be cloned into
 * every child world. */
typedef struct wfs_lock_info {
    int held;           /* a live holder was found */
    int64_t pid;
    int64_t started_at; /* unix seconds */
    char cmd[256];
} wfs_lock_info;

/* Takes the lock. *out_fd receives the descriptor that holds the flock (close-on-exec); it
 * must be handed back to wfs_world_unlock_exec(). Returns WFS_E_WORLD_BUSY if a live lock
 * already exists. */
int wfs_world_lock_exec(wfs_store *s, wfs_id id, const char *cmd, int *out_fd);
/* Releases and removes the lock file. */
void wfs_world_unlock_exec(wfs_store *s, wfs_id id, int fd);
/* Reports the live holder, if any, and removes a stale lock as a side effect. */
int wfs_world_lock_check(wfs_store *s, wfs_id id, wfs_lock_info *out);

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
