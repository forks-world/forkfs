# ObjectStore physical garbage collection

The segmented ObjectStore supports **offline collection**. Stop the owning daemon
before running it as the repository owner:

```
forkfsd gc-plan STORE
forkfsd gc STORE
```

The owner lease excludes other engines, including readers, open file handles,
background namespace compaction, and backup/export snapshots. A live owner makes
collection fail; the command does not stop or unmount an active service.

## Retention and publication

Collection validates the source before modifying its generation. It marks typed
World manifests, named revision DAGs and their ancestors, World bases, legacy flat
namespace roots and orphan inode/content roots. Sparse content index nodes and
data blocks are traversed. Generic named object roots retain their exact bytes;
their contents are not guessed to be filesystem references. Internal staging
aliases do not independently retain otherwise unreachable objects.

Live payloads are copied through ObjectStore to fresh, deduplicated segments.
LevelDB receives only object descriptors, chunk locations and metadata. The new
generation is synchronized, reopened, and verified before an atomic directory
exchange. A stable ownership-lock inode spans the close/exchange interval. The
old generation is removed after publication. Process-crash hooks cover copy,
verification and exchange boundaries; retry cleans only leftovers bound to the
repository identity, canonical path and ownership inode.

The collector preserves repository identity, committed sequence, retained World
and revision identities, POSIX directory mode and group. It does not automatically
prune revisions, recycle inode identifiers or promise ACL preservation. Refer to
[the operational GC contract](p0-mount-storage.md#offline-gc) for these boundaries.

## Replay retention

Ordinary collection preserves every indexed object when replay is enabled. Old
baseline/delta chains depend on object availability **and** historical metadata
counters. Merely keeping objects referenced by a recent root or log scan is
insufficient. Unindexed crash tails can still be reclaimed.

To release that retention, explicitly create a new baseline:

```
forkfsd gc-plan STORE --checkpoint /outside/store/new-baseline.pack
forkfsd gc STORE --checkpoint /outside/store/new-baseline.pack
```

Checkpoint collection first creates a durable baseline in the staged generation,
then exchanges generations and publishes the external archive without overwriting
an existing destination. Pending/completed receipts make interruption recovery
explicit and retries idempotent. Retry with the original archive path. Existing
backup chains remain independently recoverable; new deltas need the new baseline.
Distribute it to backup destinations before relying on that chain.

## Space accounting

Both commands report these JSON fields:

| Field | Meaning |
| --- | --- |
| `data_bytes_before` | Current DATA segment file lengths, including unindexed tails. |
| `data_bytes_estimated_after` | Exact packed DATA lengths predicted from the retained unique chunks and their copy order, including record and rollover headers. |
| `reclaimable_data_bytes` | `max(0, before - estimated_after)`. |
| `temporary_data_bytes_required` | DATA bytes for the additional generation while the source remains present. |
| `data_bytes_after` | Actual DATA lengths after exchange, or null for a plan. |
| `replay_pins_all_objects` | Whether the existing replay generation retains all objects. |
| `new_replay_generation` | Whether an explicit checkpoint resets replay retention. |

Estimates are scoped to the exclusively opened repository state. Do not reuse a
plan after writes. These are file lengths, not APFS physical block usage or total
disk-space requirements: LevelDB, control files and checkpoint archive copies
consume additional space. Capacity admission remains authoritative. A plan opens
and validates LevelDB, so its normal recovery/log housekeeping may occur; it does
not create GC staging directories, delete old generations or publish a checkpoint.
Completed checkpoint retries return the original receipt/report, not a new plan.

## Regression coverage

`forkfsd_storage_gc` covers deterministic faults before/after publication,
checkpoint recovery, archive conflicts, quota/min-free admission, corruption,
repository ownership, and rejection while the daemon owns the repository.

`forkfsd_storage_gc_model_*` uses an independent namespace/content model and
multiple deterministic seeds. It compares retained Worlds and revisions around
collection, checks planned versus actual packed lengths, repeated-GC stability,
unindexed tail reclamation and source-free checkpoint plus delta recovery. The
tests run under the `metadata-regression` label in existing CI.

`forkfsd_storage_gc_plan` creates more than 64 MiB of retained packed data,
including a common chunk shared across hundreds of objects. It deletes roots,
collects through a real segment rollover, compares the estimate with measured
DATA file lengths, and verifies retained payloads and repeated-GC stability.
Empty repositories and empty/tiny/partial-chunk objects are included.

Local validation on 2026-10-26 used macOS 27.0.1 arm64, Apple Clang 21 and a
metadata-only Release build (`WFS_BUILD_LEGACY=OFF`, `WFS_COW_FSKIT=OFF`). All 14
selected tests passed: the five GC tests (fault suite, rollover test and three
model seeds), ObjectStore and crash-tail recovery, content segments, replay/core/
corruption/filename coverage, and online backup/core. The three model seeds ran
in approximately 69–105 seconds with three concurrent tests. Linux and remote CI
results for this change have not yet been obtained.

## Remaining work

Online incremental GC needs a common publication fence and durable pin lifecycle
for all readers, open handles, compaction candidates and backup/export snapshots.
The offline ownership lease cannot be relaxed in isolation. Whole-machine power
loss and every filesystem/device's persistence behavior remain separate validation
from process-crash injection. Remote physical GC is a separate protocol.
