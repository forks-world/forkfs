// The pre-clone pool (T1.5), internal side. The C ABI half is in worldfs.h.
//
// A pool entry is a finished clone of a snapshot sitting at <store>/pool/S<n>/<uuid> with no
// `.world` marker and no row in `worlds`: a World-in-waiting. wfs_world_create() turns one into
// a world by writing the marker and renaming it into place, which is why a warm fork is O(1).
//
// Everything here assumes the store's pool lock where it matters: filling and draining take it
// (one filler at a time), while claiming does not -- a claim is a single SQLite transaction and
// two concurrent forks simply take different rows.
#pragma once
#include "internal.h"

namespace wfs {

// The `state` column of a pool row (the schema is in store.cpp). An ordinary integer column
// with no constraint on it, so a value the older cores never wrote needs no migration and does
// not move the schema version: a core that predates DRAINING reads such a row as "not READY",
// which is exactly what it is -- never handed out by pool_claim, and collected by its gc as a
// row whose filler is gone.
//
// PR #1 review (21st round, P1): DRAINING is the state that makes wfs_pool_drain() safe. The
// drain removes the tree first and deletes the row only when the tree is proven gone (16th
// round), and pool_claim() does not take the pool lock -- so a fork could claim a READY row
// while fs_remove_tree was walking the tree it names, rename the half-removed entry into place
// and commit an ACTIVE world around it. The drain moves the row out of the hand-out set first,
// in a transaction of its own, and only then touches the tree.
enum pool_state : int {
    POOL_CREATING = 0,   // a filler is cloning into <uuid>.wfs-tmp; it may still be alive
    POOL_READY = 1,      // a finished clone, and the only state pool_claim() ever matches
    POOL_DRAINING = 2    // the drain owns it: never handed out, and doomed whatever else is true
};

// One entry taken out of the pool. Keeping the whole row makes pool_return() exact.
struct PoolClaim {
    wfs_id row = 0;
    String path;            // <store>/pool/S<n>/<uuid>
    String uuid;
    wfs_id snapshot = 0;
    int64_t snap_created_at = 0;
    uint64_t entries = 0;
    uint32_t root_mode = 0;
    int64_t root_mtime = 0;
    int64_t created_at = 0;
};

// Whatever the claimer has to record *together with* the claim. It runs inside the claim's
// BEGIN IMMEDIATE transaction, after the pool row is gone and before the commit, and a non-zero
// return rolls the whole claim back (the entry stays in the pool).
//
// This is what closes the window the PR #1 review found: a fork that has taken the last pool
// entry but has not yet inserted its world row would be invisible to `discard S<n>`, which
// could then trash the snapshot out from under a world that is about to be published. The fork
// inserts its CREATING world row here, so claim and world become visible in the same commit.
// The hook already runs under the store mutex and inside a transaction: it must take neither.
using PoolClaimHook = int (*)(void *ctx, const PoolClaim &c);

// Takes one POOL_READY entry of `snapshot` out of the pool and deletes its row, in one
// BEGIN IMMEDIATE transaction, so two concurrent forks can never get the same entry. An entry
// whose recorded snap_created_at differs from `snap_created_at` is never handed out (the id
// belongs to a different snapshot now). Returns 0, -ENOENT when the pool is empty, or -EIO.
int pool_claim(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, PoolClaim &out,
               PoolClaimHook on_claimed = nullptr, void *hook_ctx = nullptr);

// Whatever the returner has to un-write *together with* the return. It runs inside
// pool_return's BEGIN IMMEDIATE, after the entry's row is back and before the commit, and a
// non-zero return rolls the whole return back.
//
// PR #1 review (16th round): the mirror of PoolClaimHook, closing the other half of the same
// window. The fork's CREATING world row is what makes a claimed entry visible to `discard
// S<n>`; deleting it before the entry was back in the pool left an instant in which the
// reference count saw neither of the two, and the return then published a READY entry for a
// snapshot that had been trashed in between -- a full stale clone, handed to the next fork as a
// live baseline. The hook already runs under the store mutex and inside a transaction: it must
// take neither.
using PoolReturnHook = int (*)(void *ctx, const PoolClaim &c);

// Puts a claimed entry back because the hand-out failed, together with whatever `on_returned`
// has to write in the same transaction. Returns 0 when the entry is in the pool again, and
// non-zero when it is not -- the tree is gone from under the claimer, the row will not insert,
// the snapshot is not this snapshot any more, or the hook refused. In that case the tree is
// removed instead (best effort), so nothing is left that gc would have to guess about, and the
// caller still owns whatever it was holding.
int pool_return(wfs_store *s, const PoolClaim &c, PoolReturnHook on_returned = nullptr,
                void *hook_ctx = nullptr);

// Ready entries for `snapshot` whose identity still matches. Cheap enough for `fork` to report.
int pool_ready_for(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, uint64_t *out);

// gc: entries of snapshots that are gone or no longer the same snapshot, rows left in the
// CREATING state, the *.wfs-tmp trees those rows name, and directories under <store>/pool that no
// row claims. Every one of those paths is inside the store.
//
// PR #1 review (6th round): a pool entry is a full clone of a snapshot, so this is not the cheap
// work the rest of gc's first half is -- unlinking a stale 120k-entry entry is seconds, and it
// used to run flat out in front of the deadline-controlled trash loop, which turned a two-second
// worker wake into minutes. `deadline_us` is the gc worker's fs_mono_us() stamp (0 = no limit),
// checked before each tree and inside the walk; when it passes, what is left keeps the state its
// successor rediscovers it in -- the row is still there, the orphan directory is still listed --
// and *work_remains is set to 1 so the worker chain carries on. None of the pointers has to be
// given.
//
// PR #1 review (8th round): a removal that FAILS is not the same thing as one the deadline cut
// short, and it used to be treated as one -- worse, the row was deleted anyway, so a tree that
// would not go (an ACL, an EPERM, an EIO) was left with nothing in the store that knew it was
// rubbish. The row is kept now, `*failed` counts it, and the shared gc retry cap
// (wfs::gc_fail_bump) decides whether the chain comes back for it or leaves it to be reported.
int pool_collect(wfs_store *s, uint64_t *removed, int64_t deadline_us = 0,
                 int *work_remains = nullptr, uint64_t *failed = nullptr);

// `gc --status`: how many stale pool entries are still on disk, counted the way pool_collect
// classifies them and without removing any of them. Rows whose snapshot is gone or is a
// different snapshot now, rows whose filler died, rows a drain left POOL_DRAINING because the
// tree would not go (PR #1 review, 21st round), and the row-less directories under
// <store>/pool. Same query and one readdir, as in pool_collect.
int pool_stranded(wfs_store *s, uint64_t *out);

// verify S<n>: every pool entry must still be there and must not have been written to since it
// was cloned (the root's mtime is the whole check -- see worldfs.h).
int pool_verify(wfs_store *s, wfs_id snapshot, uint64_t *checked, uint64_t *dirty, char *first_bad,
                size_t cap);

} // namespace wfs
