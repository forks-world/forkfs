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
/* Suffix of a half-built tree INSIDE THE STORE -- <store>/snapshots/S<n>.wfs-tmp and the pool's
 * <uuid>.wfs-tmp -- where every name is one we made and wfs_gc may sweep by suffix (P8). It is
 * never a name in a directory of the user's: `~/w/a.wfs-tmp` is somebody's own file, not our
 * scratch space. A fork's temporary is drawn instead, and recorded (see wfs_world_create). */
#define WFS_TMP_SUFFIX ".wfs-tmp"
/* T2.1: a trash entry the collector has started to unlink. The rename to this name is the FIRST
 * thing the deleter does, so a worker that is killed half-way through leaves a tree that is
 * visibly not restorable instead of one that looks intact. wfs_world_restore() refuses it. */
#define WFS_DELETING_SUFFIX ".deleting"

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
    WFS_ST_TRASHED = 2, /* sitting in <store>/trash, restorable */
    WFS_ST_DEAD = 3,
    /* PR #1 review (5th round): the middle of a discard. The row is committed with the name the
     * tree is about to have, the rename has not necessarily happened yet, and the tree is at
     * exactly one of the two names. Every store open and every gc resolves it: back to ACTIVE
     * when the tree never moved (or something still references it), on to TRASHED when it did.
     * Nothing that collects the trash may treat a directory a TRASHING row names as row-less. */
    WFS_ST_TRASHING = 4
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
    WFS_E_SOURCE_GONE = -1011,    /* the snapshot this world was forked from is no longer there */
    WFS_E_POOL_BUSY = -1012,      /* T1.5: another `pool fill` holds the store's pool lock */
    WFS_E_TRASH_DELETING = -1013, /* T2.1: the gc worker has begun unlinking this trash entry */
    WFS_E_SNAPSHOT_IN_USE = -1014,/* T2.2: an ACTIVE world (or a pool entry) still needs it */
    WFS_E_GC_BUSY = -1015,        /* T2.1: another gc worker holds <store>/locks/gc.lock */
    WFS_E_STORE_UNREACHABLE = -1016, /* T2.3: this store cannot be opened from where we are */
    /* P17: the store directory still holds snapshot trees (or a trash, or a pool) but its
     * metadata.db is missing or unreadable. Opening such a store would create an empty
     * database beside the orphans and hand out id 1 again, which the first `init` would then
     * try to write to the `snapshots/S1` that is already there. Refused instead: the database
     * has to come back from a backup, or the directory has to be moved aside. */
    WFS_E_STORE_DAMAGED = -1017
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
    uint64_t pool_ready;    /* T1.5: pre-cloned worlds waiting to be handed out */
    uint64_t pool_entries;  /* sum of their entry counts */
    /* T2.2 reconciliation, computed by one stat(2) per row: rows whose tree is not there any
     * more. `gc --reconcile` is what turns them into DEAD rows. */
    uint64_t snapshots_dangling;
    uint64_t worlds_dangling;
    /* PR #1 review (12th round): rows whose tree could not be *asked about*. stat(2) fails for
     * reasons that are not absence -- EACCES on a parent, EIO, a volume that is not mounted,
     * ENAMETOOLONG -- and a row whose path holds something that is not a directory is damaged
     * rather than gone. Neither is dangling: `gc --reconcile` buries only what is proven
     * absent, so these are reported separately and left registered. */
    uint64_t snapshots_unreadable;
    uint64_t worlds_unreadable;
    uint64_t snapshots_trashed; /* T2.2: snapshots waiting in the trash */
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
    int64_t trashed_at;          /* T2.2: unix seconds, non-zero once discarded */
    /* T2.5, P9: the hardlink groups this snapshot recorded. hl_groups counts the groups whose
     * names are all inside the tree -- those are rebuilt inside every clone of it, so link
     * identity survives a fork. hl_external counts the names whose inode also has names
     * outside the tree: those cannot be rebuilt and stay independent copies. */
    uint64_t hl_groups;
    uint64_t hl_external;
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
    /* T1.5: the pool entries of this snapshot are clones of it that nobody has taken yet.
     * They are checked the cheap way -- the root must still be there and its mtime must not be
     * newer than the moment the entry was cloned -- because a full walk per entry would make
     * `verify` cost as much as the fill did. */
    uint64_t pool_checked;
    uint64_t pool_dirty;
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
    /* T1.5: do not take a pre-cloned world out of the pool, clone here and now instead. The
     * benchmarks use it to measure the miss path; nothing else should need it. */
    int no_pool;
} wfs_fork_opts;

/* What a fork did, beyond which world it produced. */
typedef struct wfs_fork_result {
    wfs_id world;
    int from_pool;        /* 1 = a pre-cloned world was handed out (T1.5) */
    uint64_t pool_left;   /* ready entries left in that snapshot's pool afterwards */
    int64_t elapsed_us;   /* wall time inside the core, without process start */
    uint64_t hardlinks;   /* T2.5: names relinked to their canonical file inside the clone.
                           * 0 on a pool hit -- the entry was given its hardlinks back when it
                           * was filled, so the hand-out stays O(1). */
} wfs_fork_result;

/* Fork: clone `from` (a snapshot or a live world) into target_path. Publish order (P8): clone
 * into a temporary directory in the target's parent, unprotect, write the marker, rename, then
 * commit the row. The temporary is `.wfs-fork-<pid>-<counter>-<16 hex>`, drawn until one is free
 * and recorded on the CREATING row before the clone starts -- the parent directory belongs to
 * the user, so nothing there is assumed to be ours and nothing pre-existing is ever removed.
 * The publish rename is exclusive: something already at the target is EEXIST, never replaced.
 *
 * T1.5: when `from` is a snapshot and the pool holds a ready entry for it, that entry is the
 * clone -- already made, already given the source root's mode -- and the fork is reduced to
 * "write the marker, rename it into place, commit the row", which is O(1) and milliseconds.
 * The publish order is the same one, with the pool entry playing the part of the temporary. */
int wfs_world_create(wfs_store *s, wfs_ref from, const char *target_path, const wfs_fork_opts *opts,
                     wfs_id *out);
/* The same, and says whether the pool served it. `res` may be NULL. */
int wfs_world_create_ex(wfs_store *s, wfs_ref from, const char *target_path,
                        const wfs_fork_opts *opts, wfs_fork_result *res);
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

/* ---- the pre-clone pool (T1.5) -------------------------------------------------------------
 *
 * A pool entry is a World-in-waiting: <store>/pool/S<n>/<uuid> is a finished clone of snapshot
 * S<n> -- taken through the same gate a fork would open, with the source root's own mode
 * already restored -- that carries NO `.world` marker and has NO row in `worlds`. It is not a
 * world yet and nothing outside the core can reach it: it lives inside the store, which P7
 * refuses as a path and which `world exec`'s seatbelt profile denies (P14).
 *
 * Handing one out is a rename(2) on the same volume, so a fork from a snapshot with a warm pool
 * costs the marker, the rename and two SQLite transactions instead of a whole clonefile of the
 * tree -- single-digit milliseconds including process start, which is what arch.md §1 asks for.
 *
 * Entries are keyed by snapshot id AND the snapshot's created_at. Snapshot rows are immutable,
 * so a mismatch means the id was reused by a different snapshot; such an entry is never handed
 * out and wfs_gc() removes it.
 *
 * Filling is explicit (`world fs pool fill`) or automatic: the CLI re-fills in a detached
 * background process after a hit. One store-level flock (<store>/locks/pool.lock) makes two
 * fillers serialise into one; the loser returns WFS_E_POOL_BUSY rather than cloning twice. */

typedef struct wfs_pool_stat {
    wfs_id snapshot;
    char snapshot_name[WFS_NAME_MAX];
    uint64_t ready;             /* entries that can be handed out right now */
    uint64_t building;          /* rows still being cloned (or left behind by a kill) */
    uint64_t stale;             /* rows whose snapshot is gone or is a different one now */
    uint64_t entries;           /* entry count of one such world */
    int64_t oldest_at, newest_at;
} wfs_pool_stat;

/* Bring the number of ready entries for `snapshot` up to `target` (a top-up, not an addition:
 * calling it twice with target=2 leaves 2, not 4). *made receives how many were cloned.
 * Returns WFS_E_POOL_BUSY when another filler holds the store's pool lock. */
int wfs_pool_fill(wfs_store *s, wfs_id snapshot, int target, uint64_t *made);
/* One row per snapshot that has pool entries, ordered by snapshot id. */
int wfs_pool_status(wfs_store *s, wfs_pool_stat *buf, size_t cap, size_t *count);
/* Delete every entry of `snapshot` (0 = of every snapshot). *removed may be NULL. */
int wfs_pool_drain(wfs_store *s, wfs_id snapshot, uint64_t *removed);
/* Ready entries for `snapshot` right now (0 when the snapshot has none). */
int wfs_pool_ready(wfs_store *s, wfs_id snapshot, uint64_t *out);
/* 1 when a filler holds the store's pool lock right now. A caller about to spawn a background
 * top-up asks first: starting a second filler only to have it exit on the lock costs a process
 * start on the fork's own critical path. */
int wfs_pool_filling(wfs_store *s);

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

/* T2.3: the store path recorded in <world_root>/.world, read without opening any store.
 *
 * This exists for the sandboxed FSKit appex. NSApplicationSupportDirectory resolves inside its
 * container, so the extension's idea of "the default store" is
 * ~/Library/Containers/world.forks.fs.extension/Data/Library/Application Support/World/fs while
 * the CLI's is ~/Library/Application Support/World/fs -- and `mount -o` options do not reach an
 * FSKit module on macOS 27. The marker file in the mount source root does reach it, because that
 * root is exactly the resource the extension is handed, so the marker is where the store's
 * location travels.
 *
 * Returns 0 and writes the path, WFS_E_NOT_A_WORLD when there is no marker, WFS_E_SCHEMA when it
 * was written by a different schema, or -ENOENT when the marker carries no store path (every
 * marker written before T2.3). */
int wfs_marker_store_path(const char *world_root, char *buf, size_t cap);

/* PR #1 review (17th round, P2): the same marker, asked about from the store's side.
 *
 * The extension opens the path the marker names and falls back to its container default only
 * when it obtained no path at all (macos/fskit/WorldVolume.mm). So a marker whose store path is
 * non-empty and is NOT this store's directory means a mount would open a different store from
 * the one the command is talking to -- a store that has been moved since the world was forked,
 * or a world copied out of somebody else's store. That is a refusal, not a note.
 *
 * `same_store` compares the marker's store id with this store's: the same store, wherever it
 * now lives, or somebody else's (P1/P2). `same_path` is whether the recorded path resolves to
 * this store's directory -- resolves, not is spelled the same, because that is the question the
 * extension's open(2) will ask; a path that cannot be resolved at all is not this store.
 * `has_path` is 0 for a marker written before T2.3, which carries no path and for which the
 * extension's fallback is real.
 *
 * Returns 0 with `out` filled, or what marker_read says: WFS_E_NOT_A_WORLD, WFS_E_SCHEMA. */
typedef struct wfs_marker_store {
    int has_path;    /* the marker carries a store path at all */
    int same_store;  /* its store id is this store's store id */
    int same_path;   /* its store path resolves to this store's directory */
    char path[WFS_PATH_MAX];   /* what it says, empty when has_path is 0 */
} wfs_marker_store;
int wfs_world_marker_store(wfs_store *s, const char *world_root, wfs_marker_store *out);

/* The way out of the refusal above, when the store id still matches: the marker's store path is
 * rewritten to where this store actually is, and nothing else in it changes -- same world id,
 * same name, same origin snapshot, same created_at. P1/P2 decide who may: the world must be
 * registered in THIS store (marker store id, row, and inode all agreeing), so a copy is
 * WFS_E_UNREGISTERED and somebody else's world is WFS_E_FOREIGN_STORE, both of which are
 * `adopt`'s business and not a refresh's. Takes the world lock (P12) like every other
 * world-level operation, so it cannot run under a fork or a checkpoint of the same world, and
 * the new marker is written beside the old one and renamed over it. */
int wfs_world_marker_refresh(wfs_store *s, const char *world_root);

/* P1/P2. On a registered-but-moved world the store row is updated to `path` before returning.
 * Returns WFS_E_NOT_A_WORLD, WFS_E_UNREGISTERED or WFS_E_FOREIGN_STORE as appropriate; the
 * report is filled in either way. */
int wfs_world_verify_identity(wfs_store *s, const char *path, wfs_identity *out);
/* verify W<n>: identity of the recorded path. */
int wfs_world_verify(wfs_store *s, wfs_id id, wfs_identity *out);
/* P2: take over an unregistered copy as a new world, rewriting its marker.
 *
 * The copy's marker names the snapshot the original was forked from, and the adopted world
 * inherits it as its diff/verify baseline (P4/P10). So the adoption is refused, with
 * WFS_E_SOURCE_GONE, when that snapshot is no longer ACTIVE -- discarded, being discarded, or
 * gone from the store. The check runs inside the same BEGIN IMMEDIATE as the insert, which is
 * the write lock `wfs_snapshot_discard` counts references under: either the new row is there to
 * be counted, or the discard already won (PR #1 review, 6th round). A copy in that state can
 * still be kept, as a plain directory: remove its `.world` marker and `world fs init` it. */
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
 *
 * Which path is taken by default (measured on 27.0, and the reason the events path is NOT the
 * default): the 4-thread C walk of both trees is far cheaper than the design assumed. On 50k
 * entries with 800 changes a full scan is 0.164 s without the xattr leg and 1.354 s with it,
 * against 0.087 s for the events path -- under 2x, and only because of xattr. The events path
 * also carries a fixed cost of its own (building the stream and waiting for its watermark):
 * a six-file world is 0.002 s by scan and 0.4 s by events. On top of that, fseventsd flushes
 * its journal on a timer, so an isolated change made in the last ~100-600 ms may not be in a
 * stream created after it, while a scan always sees it.
 *
 * So: the scan is the default, and FSEvents is used only when the world is big enough for
 * O(changes) to matter -- more than WFS_DIFF_EVENTS_MIN_ENTRIES recorded entries -- or when the
 * caller asks for it with WFS_DIFF_EVENTS. WFS_DIFF_FULL forces the scan either way. Every
 * fallback trigger below still applies to the events path when it is taken.
 */

/* Entry count above which the events path is chosen on its own. The measured crossover is in
 * the tens of thousands; this sits an order of magnitude above it because the scan is the exact
 * answer and the events path is the optimisation. Overridable at run time with the environment
 * variable of the same name (0 = always use events when nothing else forbids it). */
#define WFS_DIFF_EVENTS_MIN_ENTRIES 200000

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
    WFS_DIFF_NO_XATTR = 1 << 2,
    /* Try the FSEvents path even though the world is below WFS_DIFF_EVENTS_MIN_ENTRIES
     * (`diff --events`). It is still only a candidate source: every fallback below sends it
     * back to the full scan, silently and with the same answer. Ignored with WFS_DIFF_FULL. */
    WFS_DIFF_EVENTS = 1 << 3,
    /* Compare com.apple.provenance too (`diff --all-xattrs`). By default it is the one xattr
     * left out: macOS 27 stamps it on every file a local process creates and will not let it
     * be removed, so it is the kernel's note of which application made the file, not anything
     * the workspace did -- and confirming that it matches costs two listxattr(2) plus two
     * getxattr(2) on every otherwise-identical file. A file whose only xattr is provenance
     * counts as having none at all, including for the EF_NO_XATTRS shortcut. Every other
     * attribute, com.apple.quarantine included, is always compared. */
    WFS_DIFF_ALL_XATTRS = 1 << 4
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
    WFS_DF_UNSUPPORTED, /* no FSEvents on this platform */
    WFS_DF_SMALL_TREE   /* the default: fewer than WFS_DIFF_EVENTS_MIN_ENTRIES entries, so the
                         * two-tree walk is the cheaper and the exact answer. Not a fallback --
                         * nothing went wrong -- and callers do not report it. */
} wfs_diff_fallback;

typedef struct wfs_diff_stats {
    uint64_t added, modified, deleted, meta;
    uint64_t candidates;  /* paths FSEvents proposed (0 for a full scan) */
    uint64_t compared;    /* paths stat'ed on both sides */
    uint64_t content_cmp; /* files whose bytes had to be read (size equal, mtime differs) */
    uint64_t bytes_read;
    /* PR #1 review: entries whose extended attributes could not be read on one side
     * (listxattr/getxattr failed with EACCES, EIO, ...). Such an entry is reported as 'T' --
     * a failed read is never equality -- and counted here, so a caller can say the comparison
     * was incomplete. A failure on the *snapshot* side is not a diff result at all: the whole
     * call returns -EACCES/-EPERM instead. */
    uint64_t xattr_errors;
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
    uint64_t snapshots_deleted; /* half-built snapshots, plus T2.2 trashed ones past retention */
    uint64_t tmp_removed;       /* half-built trees (P8): the path a CREATING row recorded, plus
                                 * *.wfs-tmp under <store>/snapshots and stale <store>/tmp files */
    uint64_t trash_orphans;     /* trash directories with no row */
    uint64_t pool_removed;      /* T1.5: pool entries of dead snapshots, half-built or orphaned */
    uint64_t entries_freed;     /* T2.1: directory entries actually unlinked */
    /* T2.2 reconciliation. The `*_dangling` counters are always filled in (detection is free:
     * one stat per row); the `*_reconciled` ones only when WFS_GC_RECONCILE was asked for. */
    uint64_t snapshots_dangling, worlds_dangling;
    uint64_t snapshots_reconciled, worlds_reconciled;
    /* PR #1 review (12th round): rows the scan could not reach a verdict about. "The tree is
     * not there" used to be a bool over stat(2), so EACCES on a parent directory, an EIO, an
     * unmounted volume or an ENAMETOOLONG all read as "gone" -- and --reconcile then marked the
     * row DEAD, which neither `verify` nor `adopt` can undo: one transient error unregistered a
     * live world for good. Only ENOENT/ENOTDIR is an absence now. Anything else (including a
     * path that holds something which is not a directory: a damaged world, not a missing one)
     * is counted here, left ACTIVE, and printed by `gc`/`gc --reconcile`. */
    uint64_t snapshots_unreadable, worlds_unreadable;
    /* T2.1: the batch limit was reached and trash entries are still waiting. A caller that can
     * spawn a worker (the CLI does) should spawn one. */
    int work_remains;
    /* Trash entries this run could not delete at all (a tree with something undeletable in it,
     * a transient EIO). They are still in the trash, `gc --status` still counts them, and no
     * report with a non-zero count here may be read as "the trash is empty". The first few
     * wakes after a failure set work_remains as well, so the worker chain comes back for them;
     * after that the entry is left alone rather than spun on forever. */
    uint64_t trash_failed;
    /* PR #1 review (5th round): abandoned half-built trees this run could not remove. A fork's
     * temporary lives in the user's own target directory, under a name drawn at random, so the
     * CREATING row is the only record of it and it is deliberately never found by a suffix
     * sweep. The row is therefore kept, CREATING and with its tmp_path intact, and the tree is
     * counted here and by `gc --status` until some later wake can remove it. Same retry cap as
     * a trash entry: the first few failures set work_remains, after that it is reported and left
     * alone rather than spun on.
     * PR #1 review (7th round): and the half-built snapshots, for the same reason. Their trees
     * are <store>/snapshots/S<n> and S<n>.wfs-tmp, and only the second is ever found by a
     * suffix sweep -- so an S<n> that would not go used to leak for ever, its row deleted along
     * with the failure. That row is kept now too, and counted here. */
    uint64_t tmp_failed;
    /* PR #1 review (8th round): stale pre-clone entries under <store>/pool this run could not
     * remove. Same rule as the two counters above and for the same reason: the removal used to
     * be assumed to have worked and the pool row deleted with it, so a clone that would not go
     * was left with nothing in the store that knew it was rubbish, no retry counter and no
     * work_remains -- it leaked until somebody ran gc by hand. The row is kept while its tree
     * is, counted here and by `gc --status`, and retried under the same cap. */
    uint64_t pool_failed;
} wfs_gc_report;

/* retention_secs < 0 uses the default (7 days). Synchronous and complete: every due trash entry
 * is unlinked before this returns. `world fs gc --now`. */
int wfs_gc(wfs_store *s, int64_t retention_secs, wfs_gc_report *out);

/* ---- T2.1: incremental, background gc -------------------------------------------------------
 *
 * `discard` is O(1) by construction -- it renames a world into <store>/trash and nothing more --
 * but the physical deletion behind it is the most expensive thing this system does: measured,
 * 1000 worlds of 10k entries are 10.4M unlink(2) calls, ~50 us each, 525 s in total, which is
 * 4.6x what creating them cost (docs/M1_RESULTS.md §3). That work must not sit on a command's
 * critical path and must not fight the foreground for the disk (P16).
 *
 * So: the trash is drained by a detached worker (`world fs gc --worker`, spawned exactly the way
 * `fork` spawns a pool filler) that holds a non-blocking store-level flock on
 * <store>/locks/gc.lock, so at most one worker per store ever runs. It works in bounded batches
 * -- at most `max_entries` trash entries or `max_secs` seconds per wake -- writes its progress
 * into the lock file, logs to <store>/logs/gc.log, and exits. Anything that creates work
 * (`discard`, `gc`, `fork`) spawns one if there is work and nobody is on it.
 *
 * Crash safety: before a single unlink, the trash entry is renamed to <name>.deleting. A worker
 * killed mid-tree therefore leaves a tree that is visibly half-gone rather than one that looks
 * restorable, wfs_world_restore() refuses it with WFS_E_TRASH_DELETING, and the next wake
 * finishes it -- *.deleting trees are collected first, before and regardless of retention.
 */

enum {
    WFS_GC_RECONCILE = 1 << 0, /* T2.2: mark dangling snapshot/world rows DEAD, not just report */
    WFS_GC_BACKGROUND = 1 << 1,/* take <store>/locks/gc.lock; WFS_E_GC_BUSY when another has it */
    /* Do the cheap half only and leave the trash to the worker. This is what plain `world fs gc`
     * asks for: the half-built trees, the stale profiles, the dead pool entries and the
     * reconciliation report are all a handful of syscalls, while the trash is minutes. */
    WFS_GC_NO_TRASH = 1 << 2
};

typedef struct wfs_gc_opts {
    int64_t retention_secs;  /* < 0 = the default, 7 days */
    uint64_t max_entries;    /* trash entries per wake; 0 = no limit */
    int64_t max_secs;        /* wall seconds per wake; 0 = no limit */
    int flags;               /* WFS_GC_* */
    int threads;             /* unlink workers; 0 = 4, the measured APFS sweet spot */
} wfs_gc_opts;

/* The one implementation; wfs_gc() is this with no limits and no flags. opts may be NULL. */
int wfs_gc_ex(wfs_store *s, const wfs_gc_opts *opts, wfs_gc_report *out);

typedef struct wfs_trash_stat {
    uint64_t entries;          /* directories sitting in <store>/trash right now */
    uint64_t deleting;         /* of those, ones already renamed *.deleting */
    uint64_t due;              /* of those, ones past the retention period */
    uint64_t worlds, snapshots;/* trashed rows, by kind */
    uint64_t tree_entries;     /* recorded entry counts of what is in there */
    /* Physical bytes are an estimate for the same reason `wfs_store_status` estimates them:
     * du(1) counts cloned blocks that are shared with other worlds, df(1) only moves when the
     * last reference goes. This is tree_entries x the measured 308 B/entry clone metadata cost,
     * alongside the volume's real df numbers so an operator can see both. */
    uint64_t bytes_estimate;
    uint64_t volume_free_bytes, volume_total_bytes;
    /* PR #1 review (5th round): abandoned fork trees whose producer is gone and which are still
     * on disk in the user's directory, named only by their CREATING row. Not trash entries --
     * they are outside the store -- but the same thing from an operator's point of view: space
     * that is waiting for a collector. PR #1 review (7th round): half-built snapshot trees a
     * collection could not remove are counted here as well; their CREATING row is likewise the
     * only thing that names them. */
    uint64_t creating_stranded;
    /* PR #1 review (6th round): stale pre-clone pool entries still on disk -- a snapshot that is
     * gone or is a different snapshot now, or a filler that died. Also not trash entries, and
     * also waiting for the collector: removing one is a whole tree, so a wake that runs out of
     * time leaves the rest of them for its successor. */
    uint64_t pool_stranded;
    /* The worker, from <store>/locks/gc.lock. pid is 0 when nobody is running. */
    int64_t worker_pid, worker_started_at;
    uint64_t worker_done, worker_remaining; /* trash entries finished / left, as it last wrote */
} wfs_trash_stat;

/* `world fs gc --status`. Never blocks and never spawns anything. */
int wfs_gc_status(wfs_store *s, int64_t retention_secs, wfs_trash_stat *out);

/* The same question reduced to what a command needs before deciding to spawn a worker: is there
 * due work, and is somebody already on it? Two indexed counts and one readdir that stops at the
 * first *.deleting name -- wfs_gc_status() classifies every trash directory against every trashed
 * row, which is quadratic in the size of the trash and measurably not free on `discard` once a
 * thousand worlds are in there. Returns 1 when there is work, 0 when there is not.
 * `worker_running` may be NULL. */
int wfs_gc_pending(wfs_store *s, int64_t retention_secs, int *worker_running);

/* ---- T2.2: discarding a snapshot ------------------------------------------------------------
 *
 * A snapshot is the baseline `diff` and `verify` compare against, so it is never taken away from
 * a world that still needs it: an ACTIVE world whose snapshot_id is this one makes the discard a
 * WFS_E_SNAPSHOT_IN_USE refusal. `force` drains the snapshot's pool entries (they are only
 * pre-made clones, nothing is lost) but still refuses while ACTIVE worlds remain -- never orphan
 * a world's source.
 *
 * A world that is still being forked counts too. The reference check and the ACTIVE -> TRASHED
 * transition run inside one BEGIN IMMEDIATE, which is the same write lock a pool claim and a
 * world row insert take, and a fork commits its CREATING world row together with its pool claim
 * (or before it starts cloning). So a fork that is half-way through is either counted here and
 * the discard is refused, or it finds the snapshot TRASHED and fails instead: there is no order
 * in which a world gets published with its baseline already in the trash.
 *
 * TRASHED worlds are not a reason to refuse; they are already on their way out. Restoring one
 * afterwards is what fails, with WFS_E_SOURCE_GONE, because bringing a world back to life
 * without a baseline would produce a world that cannot be diffed or verified.
 *
 * The discard itself is P4, the same path a world takes: <store>/snapshots/S<n> is renamed into
 * <store>/trash, the row becomes TRASHED, and the background collector unlinks it once the
 * retention period is up -- reopening the gate directory (0700) on its way in, since a 0000 root
 * cannot even be listed.
 *
 * `immediate` is `--now`, and means the same thing it means for a world: do not wait for the
 * collector, unlink the tree before returning and leave the row DEAD. It is still the crash-safe
 * two-step (rename to *.deleting, then unlink), so an interrupted --now leaves something the
 * next collector finishes rather than something that looks restorable. On a snapshot that is
 * already in the trash it is the whole of the operation -- the deletion is simply brought
 * forward -- and only without it is an already-trashed snapshot -EALREADY. */
int wfs_snapshot_discard(wfs_store *s, wfs_id id, int immediate, int force);

/* ---- test seam --------------------------------------------------------------------------------
 *
 * core_test drives two interleavings that cannot be produced from outside the library. The first
 * is the middle of a pool-backed fork, after the claim transaction has committed the fork's
 * CREATING world row and before the marker/rename/ACTIVE tail. The second is a fork dying in the
 * window the temp path exists for: the clone is built and recorded on the CREATING row, the
 * publish rename has not happened. A non-zero return from wfs_test_before_fork_publish is
 * returned straight out of wfs_world_create() with nothing unwound -- row and tree stay, exactly
 * as a killed process leaves them. All four are NULL unless a test sets them and nothing in the
 * library ever assigns them. */
extern void (*wfs_test_after_pool_claim)(void *ctx, wfs_id world);
extern void *wfs_test_after_pool_claim_ctx;
extern int (*wfs_test_before_fork_publish)(void *ctx, wfs_id world, const char *tmp_path);
extern void *wfs_test_before_fork_publish_ctx;

/* And the third (PR #1 review, 16th round): the pool hand-out's unwind, at the instant the
 * claimed entry is out of the pool and the hand-out has already failed -- after the tree has
 * been put back under its pool name and before the transaction that returns it. What a test
 * does in there is run a whole `discard S<n>`, because that is the one instant at which the
 * fork's own CREATING world row is the snapshot's only reference. NULL unless a test sets it,
 * and nothing in the library ever assigns it. */
extern void (*wfs_test_in_pool_unwind)(void *ctx);
extern void *wfs_test_in_pool_unwind_ctx;

/* The pid a CREATING row records as the process building it. 0, its value in every real run,
 * means getpid(). A test sets it to a pid that is not running to produce the one state a single
 * process cannot otherwise reach: a half-built tree whose producer is gone. */
extern int64_t wfs_test_fork_owner_pid;

/* The window a snapshot or checkpoint has between its walk of the SOURCE and the clone of it
 * (PR #1 review, 5th round): the source is a live directory, and what a test has to be able to
 * do there is change it. Called with the resolved source path, once, immediately before
 * clonefile. NULL unless a test sets it. */
extern void (*wfs_test_before_snapshot_clone)(void *ctx, const char *src_dir);
extern void *wfs_test_before_snapshot_clone_ctx;

/* And the discard's own two halves (PR #1 review, 5th round). `phase` is 0 just after the row
 * has been committed in WFS_ST_TRASHING and before the tree is renamed, 1 just after the rename
 * and before the commit that makes the row TRASHED. A non-zero return is returned straight out
 * of wfs_world_discard()/wfs_snapshot_discard() with nothing unwound -- row and tree stay
 * exactly as a kill -9 there leaves them, which is the state the recovery has to resolve.
 * NULL unless a test sets it; nothing in the library ever assigns it.
 *
 * PR #1 review (7th round): phases 2 and 3 are wfs_world_restore()'s own two halves, the same
 * protocol run backwards -- 2 just after the row has been committed in WFS_ST_TRASHING with the
 * tree still in the trash, 3 just after the rename home and before the commit that makes the row
 * ACTIVE. `trash_path` is the name the tree has in the trash. Returning 0 from phase 2 is what
 * lets a test run a whole `wfs_snapshot_discard()` inside the restore's window.
 *
 * PR #1 review (9th round): phase 4 is `--now`'s own window -- inside the helper that deletes a
 * trash entry here and now, after the tree has been followed to whatever name it has and before
 * it is marked `.deleting`. `trash_path` is the name that was found. A whole
 * `wfs_world_restore()` run in there is the race the predicated row writes exist for. */
extern int (*wfs_test_trash_crash)(void *ctx, int phase, int is_snapshot, wfs_id id,
                                   const char *trash_path);
extern void *wfs_test_trash_crash_ctx;

/* And the trash collector's own window (PR #1 review, 8th round): called once per queued entry,
 * between the scan that queued it and the rename to `<name>.deleting` that starts deleting it.
 * `row` is 0 for a directory in the trash that no row claims. What a test does in there is win
 * the race the collector has to survive -- restore the world it is holding -- and what has to
 * happen next is that the collector notices and leaves the entry alone. NULL unless a test sets
 * it; nothing in the library ever assigns it.
 *
 * PR #1 review (15th round): it sits one step later than it used to -- after the cheap re-check
 * of the row and immediately before the transaction that claims the entry by renaming it. That
 * is the window that had to be closed: a restore which commits its row in TRASHING in there and
 * has not yet renamed the tree home leaves the tree exactly where the collector expects it. */
extern void (*wfs_test_before_trash_delete)(void *ctx, int is_snapshot, wfs_id row,
                                            const char *path);
extern void *wfs_test_before_trash_delete_ctx;

/* And the window the collector's own *scan* has (PR #1 review, 9th round): called once per
 * trash scan -- wfs_gc(), wfs_gc_status(), wfs_gc_pending() -- after the store mutex has been
 * dropped and the trash paths the rows claim have been read, and before the readdir of
 * <store>/trash that decides which directories nothing claims. What a test does in there is a
 * whole `discard` on a second handle, which is the one thing that turns a claimed tree into an
 * apparent orphan. NULL unless a test sets it; nothing in the library ever assigns it. */
extern void (*wfs_test_before_trash_orphans)(void *ctx);
extern void *wfs_test_before_trash_orphans_ctx;

/* And the pool collector's equivalent (PR #1 review, 9th round): called once per pool_collect(),
 * after the pool rows have been read and before the trees they doom are removed and the
 * directories nothing claims are swept. What a test does in there is fill the pool, or fork from
 * it, on a second handle -- both of which produce a tree the row snapshot has never heard of.
 * NULL unless a test sets it; nothing in the library ever assigns it. */
extern void (*wfs_test_before_pool_sweep)(void *ctx);
extern void *wfs_test_before_pool_sweep_ctx;

/* And the pool filler's own window (PR #1 review, 15th round): called once per entry, after the
 * snapshot row has been read and before the transaction that inserts the entry's CREATING pool
 * row. What a test does in there is a `discard S<n>` on a second handle -- which, until the
 * insert re-read the snapshot under its own write lock, counted no pool rows at all and let the
 * filler publish a READY clone of a snapshot on its way to the trash. NULL unless a test sets
 * it; nothing in the library ever assigns it. */
extern void (*wfs_test_before_pool_insert)(void *ctx);
extern void *wfs_test_before_pool_insert_ctx;

/* And reconciliation's window (PR #1 review, 9th round): called once per gc, between the scan
 * that decides which ACTIVE rows have no tree at their recorded path and the updates that bury
 * them. What a test does in there is `world fs verify <the new path>` on a world that was merely
 * moved, which relocates the row by inode. NULL unless a test sets it; nothing in the library
 * ever assigns it. */
extern void (*wfs_test_before_reconcile)(void *ctx);
extern void *wfs_test_before_reconcile_ctx;

/* And the thing no test can provoke on a healthy machine: a pthread_create that fails for one
 * worker slot and succeeds for a later one. A bit set here refuses that slot (slots 0..31); 0,
 * the value it has everywhere else, means every slot is started normally. */
extern unsigned wfs_test_thread_fail_mask;
/* A unit test of that starter: start `want` workers with `fail_mask` refused, join them, and
 * report how many started and how many joined. 0 = the two agree with the number that ran. */
int wfs_test_threads_start(int want, unsigned fail_mask, int *started, int *joined);

/* And a hardlink replay the file system refused (PR #1 review, 4th round). A link(2) that comes
 * back EIO is not something a test can arrange on a developer's disk, and what has to be pinned
 * down is what happens next: the snapshot, fork or pool entry being built must fail and take its
 * half-built tree and its row with it, rather than publish a tree whose manifest advertises
 * hardlink groups it does not have. Set to a negative errno to make every non-empty replay
 * report it; 0 in every run that is not core_test. */
extern int wfs_test_hardlink_restore_err;

/* And the one xattr rule a test cannot drive from outside (PR #1 review, 9th round). The default
 * diff leaves com.apple.provenance out of the comparison, and provenance is precisely the name a
 * test cannot make differ: the kernel stamps it on every file this process creates, with the
 * same value every time, and setxattr(2)/removexattr(2) on it silently do nothing. Set this to
 * an ordinary xattr name and the diff's ignore rule treats that name exactly as it treats
 * provenance. NULL in every run that is not diff_test. */
extern const char *wfs_test_xattr_ignore;

#ifdef __cplusplus
}
#endif
#endif /* WORLDFS_H */
