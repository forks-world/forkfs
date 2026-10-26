# Online incremental ObjectStore GC

Online GC runs inside the existing `forkfsd` process and keeps the repository
open. It prunes unreachable object metadata in bounded batches, relocates chunks
from one fragmented segment at a time, and delays segment deletion until readers
of old physical locations have finished. No separate ObjectStore service is used.

## Commands

Use the manager connection to a running daemon:

```
forkfsd request /private/control/rpc.sock gc-start
forkfsd request /private/control/rpc.sock gc-status
forkfsd request /private/control/rpc.sock gc-stop
```

The explicit form is `gc-start COPY_BYTES MAX_OBJECTS MAX_STEPS [METADATA_BYTES]`. Defaults are
1 MiB of chunk payload copying, 128 object deletions per step, 64 steps, and a
64 MiB conservative metadata allocation budget per step.
Valid bounds are 65,580–8,388,608 copy bytes, 1–4,096 objects, 1–1,024 steps,
and 65,536–268,435,456 metadata bytes.
There is one GC worker per container. A second start while it is active fails.
The background worker releases the foreground namespace lock during marking;
normal filesystem requests can continue. A stop waits for the current step to
finish, then joins the worker. Container destruction also joins it before closing
storage. Jobs are explicit maintenance requests and are not automatically resumed
after restart.

`gc-status` returns `state`, `active`, `steps`, `last_step` and `error`. Terminal
states are `complete`, `blocked`, `budget_exhausted`, `stopped` or `failed`.
`blocked` means the current step cannot make more progress, including retention
by readers or replay. After those conditions change, start another job. Step
statistics count committed work, not candidates that failed a publication fence.
The step's `sequence` identifies its captured mark snapshot; concurrent writes
may have advanced the repository by the time the report is returned.

For deterministic stepping and diagnostics:

```
forkfsd request /private/control/rpc.sock gc-step 1048576 128
forkfsd gc-step /path/to/stopped/repository 1048576 128 67108864
```

Both forms of `gc-step` accept an optional final `METADATA_BYTES` after the copy
and object budgets.

The standalone command acquires the usual exclusive repository-owner lease and
cannot open a store already owned by a daemon. The RPC `gc-step` is synchronous;
use `gc-start` for foreground responsiveness. GC RPCs are manager-only and reject
World/revision/session selectors. Frontend and worker mount sessions cannot start,
stop or inspect collection through their filesystem capabilities.

## Two reader lifetimes

Logical protection covers immutable object references across multiple reads.
Namespace views, metadata-tree editors, compaction candidates and compound journal
reads hold generic pins. Directory cursors, including copies, instead hold leases
on their specific manifest: GC marks that manifest's closure as another root.
This preserves the original snapshot after writes replace the current World head
without globally blocking pruning for the lifetime of FSKit's enumeration cache.
Open-but-unlinked files retain their inode/content through the namespace's existing
orphan roots.

Direct library callers retaining IDs across separate calls must hold a generic
pin, or keep a named root/revision. `Journal::get()` already protects its compound
root lookup and object read. For example:

```cpp
auto pin = store.pin_objects();
auto id = store.root("head");
auto bytes = store.object(id);
```

Physical-location pins protect an index lookup and all reads using its result.
Ordinary object reads, transaction preparation, verification, backup and replay
export acquire them before looking up locations or taking a LevelDB snapshot.
Snapshots are released before their pins. A backup snapshot can therefore finish
reading old segment locations after GC has published replacement locations.

This implementation conservatively pauses logical pruning while any external
generic pin or physical-location reader exists. Manifest leases do not pause the
sweep; only their reachable objects are retained. Neither kind of logical lease
has to disappear before physical relocation or retirement cleanup: subsequent
object reads resolve current chunk locations under their own physical-location
pin. Cursors do retain their complete historical manifest closure until all
copies are released or evicted.

## Mark and publication fence

A GC step captures roots, object descriptors, chunk locations, counters and the
logical sequence from one LevelDB snapshot, together with registered manifest
leases and their addition epoch under the pin mutex. It validates and follows the same
typed World/revision DAG, base, inode and sparse-content references used by offline
collection, including the closures of leased manifests. Generic named roots retain opaque payloads. Internal staging aliases
do not independently keep unreachable objects alive.

Marking and candidate construction occur outside the writer lock. The step then
acquires the writer and pin gates. It deletes candidates only when the captured
sequence is still current, no external generic pins or physical-location readers exist, and retained
replay is disabled. A newly registered manifest advances the addition epoch and
also invalidates a stale mark, even when no transaction changed the sequence.
Dropping a lease may conservatively retain extra objects until the next step.
A concurrent commit invalidates the sweep; it does not cause
GC to delete according to a stale graph. The collector mutex excludes another
unlogged GC mutation throughout the complete step.

At most `MAX_OBJECTS` object rows and their staging aliases are deleted. Chunk
references are calculated from **every remaining object descriptor**, including
unreachable objects left for a later batch. Object rows, unused chunk indexes,
aliases and durable counters change in one synchronous metadata batch. Cached
bytes for deleted objects are evicted before admitting another reader, so an old
cached ID cannot be republished as a dangling root. Collection neither advances
the logical sequence nor creates a revision.

When replay is enabled, all indexed objects remain retained to preserve old
baseline/delta contents and counters. Online cleaning can still reclaim unindexed
tails. Releasing that retention requires the explicit offline checkpoint protocol
in [ObjectStore GC](STORAGE_GC.md#replay-retention).

## Relocation and retirement

The collector selects a segment containing bytes not covered by current chunk
indexes. It seals the selected source under the writer lock, creating a durable
higher-numbered successor if the source is the current highest segment. A
post-seal snapshot fixes the source's chunk references. Its index scan runs
outside the writer lock; foreground writers cannot append new chunk records to
that sealed source.

Each step copies at most `COPY_BYTES` of payload through ObjectStore, verifies
chunk hashes, synchronizes new data, then atomically publishes replacement `C/`
locations. A source larger than the budget is handled over multiple steps.
When its last current reference is moved, the same metadata batch writes a
retirement marker. Logical object IDs and descriptors do not change.

Physical formats `FFLDB007` (without replay) and `FFLDB008` (with replay) admit
these markers. The upgrade is atomic with first retirement publication. A marker
is `G/` followed by an eight-byte little-endian segment ID, with value `FFGCRET1`.
Older binaries that accept only formats 1–6 refuse the upgraded repository.
Markers are physical bookkeeping and are omitted from logical backup/replay;
offline generation rewriting also omits them.

Reaping validates that a retired segment has no current chunk references and has
a durable higher-numbered segment. With no physical-location readers, it unlinks
at most one retired segment per step, synchronizes the directory, then durably
deletes its marker. A crash between unlink and marker deletion is retried
idempotently. Keeping the higher segment prevents numeric ID reuse in that gap.
Pending retirement is attempted before allocating relocation space, so garbage
left after a crash can free a full DATA quota before another copy is admitted.
The selected copy batch's complete append cost is admitted before writing its
first record. A known capacity rejection leaves the current owner healthy and
usable for reads, barriers and metadata-only/deduplicated transactions.
Incomplete copying leaves only unindexed tail bytes, with old indexes intact.
Uncertain metadata publication or partial append failure poisons the current
owner; reopening recovers durable state before further writes.

## Validation and limits

`forkfsd_online_gc` covers bounded pruning/copying, partial sweeps, stale cached
IDs, logical pins, retained directory cursors, orphan handles, replay retention
and malformed retirement markers. `forkfsd_online_gc_concurrency` uses test-only
barriers to hold a real backup snapshot while GC and foreground writes advance,
then restores the archive without its source. A separate barrier commits a new
reference after marking and requires the stale sweep to be rejected.
It also registers an old manifest lease after marking without changing the
sequence, requiring the pin epoch to reject that sweep. Cursor tests require
unrelated garbage to be reclaimed while old cursor copies remain readable.
Additional barriers poison the owner during marking (no further metadata may
publish), and pause after the G/M-format batch commits but before its in-memory
format cache changes. Verification must use the format in its own snapshot and
continue to read valid objects in that publication window.

`forkfsd_online_gc_crash` reaches each copy/sync/publication/unlink/marker-deletion
fault across as many bounded steps as necessary, verifies recovery after every
crash, and exercises partial-write and sync errors. A background sync-error test
also verifies that the failed task remains inspectable while filesystem reads,
writes and `fs-sync` reject the poisoned owner until reopen.
`forkfsd_online_gc_rpc` checks
background start/status/stop/restart, budget validation and live namespace I/O.
`forkfsd_online_gc_model_*` interleaves randomized root writes, deletion, aliases,
deduplicated payloads and small GC budgets against an independent byte model with
three deterministic seeds, keeping the store open throughout. Final draining must
converge to exactly the unique objects retained by the model.
All are registered under the existing `metadata-regression` CI label.
Quota regressions additionally cover an already retired segment plus a dirty
successor at the exact DATA limit, and a pre-copy rejection that must preserve a
healthy live owner.

### Local validation

On 2026-10-26, the final macOS 27.0.1 arm64 Release build passed all 103 registered
tests, including 92 under `metadata-regression`. The complete run took about
350 seconds with three parallel tests. Sol architecture review and a separate
Luna implementation review found no remaining actionable issues after fixes.
Reproduction:

```
cmake -S . -B build/gc -DCMAKE_BUILD_TYPE=Release -DWFS_BUILD_LEGACY=OFF -DWFS_COW_FSKIT=OFF
cmake --build build/gc --parallel 4
ctest --test-dir build/gc --output-on-failure --parallel 3
```

The full regression also exposed socket-file readiness races in two existing
fixtures and a real RPC response-deadline issue. Fixtures now wait for a valid
RPC handshake. Receiving a request and transmitting its completed response have
separate bounded windows, with a deterministic fault-only regression rather than
a sleep-based timing assertion. Linux, remote CI and signed mounted results for
this change have not yet been obtained.

Payload copying, object deletion and unlink counts are bounded per step.
The R/O/C indexes are now scanned through LevelDB snapshot iterators; graph
references use snapshot point lookups instead of duplicating those indexes in
maps. Chunk pruning stores only hashes belonging to the bounded deletion batch,
and removes a candidate hash from that set if any retained object uses it.
Physical relocation retains only the copy-budget-sized prefix of source chunk
references, while counting all source references before deciding retirement.
Scans disable LevelDB block-cache filling to reduce cache pollution.

GC-owned metadata has a shared conservative allocation budget. It covers mark and
manifest sets, tree caches and entries, legacy run decoding, revision DAG frames
and colors, extent verification sets, candidate batches, segment inventories and
relocation metadata. `metadata_budget_bytes` and `metadata_bytes_charged` in each
successful step expose the configured ceiling and the cumulative charge. Charges
are conservative and are not returned when temporary collections are freed, so
a step can be refused even when peak live metadata would fit. The budget does not
represent process RSS: LevelDB's existing caches and snapshot/iterator internals,
ObjectStore caches, allocator overhead and bounded codec/payload working buffers
are outside this accounting. It is not a hard OS memory limit.

Exhaustion throws `GC metadata memory budget exceeded`; background jobs report
`failed` with that error. Incomplete marking never authorizes a logical sweep.
Budget refusal does not poison the owner: reads, writes and sync remain usable,
and a later job can retry with a larger budget. A step contains several separately
durable phases, so retirement cleanup or a completed prune/relocation may already
have committed before a later phase exhausts its budget; refusal does not roll
back that progress. The same crash recovery rules apply to it.

The metadata-budget regression uses opaque roots, a real namespace tree, an
independently encoded legacy manifest, a 500-node revision DAG, and a 700-block
indexed file. It checks safe early refusal, healthy read/write/sync afterward,
large-budget retry, and refusal after a committed prune. A separate 8,000-object
fixture holds a generic pin and checks that streaming the O/C indexes succeeds
with a 64 KiB charge ceiling. RPC tests check bounds and propagation of an
explicit budget to the background worker. These are accounting/correctness tests,
not measured RSS or latency bounds. The local metadata-only Release build passed
all 104 registered CTest tests (93 labeled metadata-regression) in 360.65 seconds
after this change; this does not establish signed FSKit-mounted or remote CI
acceptance.

Full metadata scans and graph marking are still not incremental across steps.
A graph that exceeds the configured maximum cannot yet be collected online;
offline GC or a subsequent disk-backed marker is required. Sustained concurrent
commits or long-lived generic pins can repeatedly defer object pruning; this
version does not guarantee logical reclamation progress under continuous
mutation. Segment movement and retirement have independent reader gates.

The next incremental design needs disposable disk-backed mark/queue state,
per-codec resumable walkers, bounded scan cursors and a generation fence. Restart
on logical sequence or manifest-addition-epoch changes preserves the present
safety model but can starve. Progress during continuous mutation additionally
requires a publication barrier covering newly exposed object closures; reusing
old marks without that barrier is unsafe. No persistent partial mark is trusted
by this implementation.

No latency target, whole-machine power-loss guarantee, remote GC or true
signed FSKit-mounted acceptance is established by these local tests.
