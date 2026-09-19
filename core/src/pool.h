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

// Takes one ready entry of `snapshot` out of the pool and deletes its row, in one
// BEGIN IMMEDIATE transaction, so two concurrent forks can never get the same entry. An entry
// whose recorded snap_created_at differs from `snap_created_at` is never handed out (the id
// belongs to a different snapshot now). Returns 0, -ENOENT when the pool is empty, or -EIO.
int pool_claim(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, PoolClaim &out,
               PoolClaimHook on_claimed = nullptr, void *hook_ctx = nullptr);

// Puts a claimed entry back because the hand-out failed. Best effort: if the row cannot be
// written the tree is removed instead, so nothing is left that gc would have to guess about.
void pool_return(wfs_store *s, const PoolClaim &c);

// Ready entries for `snapshot` whose identity still matches. Cheap enough for `fork` to report.
int pool_ready_for(wfs_store *s, wfs_id snapshot, int64_t snap_created_at, uint64_t *out);

// gc: entries of snapshots that are gone or no longer the same snapshot, rows left in the
// CREATING state, stray *.wfs-tmp trees and directories under <store>/pool that no row claims.
int pool_collect(wfs_store *s, uint64_t *removed);

// verify S<n>: every pool entry must still be there and must not have been written to since it
// was cloned (the root's mtime is the whole check -- see worldfs.h).
int pool_verify(wfs_store *s, wfs_id snapshot, uint64_t *checked, uint64_t *dirty, char *first_bad,
                size_t cap);

} // namespace wfs
