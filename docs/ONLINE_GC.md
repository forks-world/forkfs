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

The explicit form is `gc-start COPY_BYTES MAX_OBJECTS MAX_STEPS [METADATA_BYTES [MARK_WORK]]`. Defaults are
1 MiB of chunk payload copying, 128 object deletions per step, 64 steps, and a
64 MiB conservative metadata allocation budget per step and 128 logical work
units per step.
Valid bounds are 65,580–8,388,608 copy bytes, 1–4,096 objects, 1–1,024 steps,
65,536–268,435,456 metadata bytes, and 1–65,536 logical work units.
There is one GC worker per container. A second start while it is active fails.
The background worker releases the foreground namespace lock during marking;
normal filesystem requests can continue. A stop waits for the current step to
finish, joins the worker and discards its partial mark so later foreground writes
do not keep appending to a stopped queue. Budget-exhausted/blocked jobs retain
cycle state and insertion barriers when a cycle exists (replay retention does
not create one) for a later start within the same owner; use
`gc-stop` to release that state explicitly. Container destruction also joins it before closing
storage. Jobs are explicit maintenance requests and are not automatically resumed
after restart.

`gc-status` returns `state`, `active`, `steps`, `last_step` and `error`. Terminal
states are `complete`, `blocked`, `budget_exhausted`, `stopped` or `failed`.
`blocked` means the current step cannot make more progress, including retention
by readers or replay. After those conditions change, start another job. Step
statistics count committed work, not candidates that failed a publication fence.
The step's `sequence` identifies the cycle's initial snapshot; concurrent writes
may have advanced the repository by the time the report is returned.

For deterministic stepping and diagnostics:

```
forkfsd request /private/control/rpc.sock gc-step 1048576 128
forkfsd gc-step /path/to/stopped/repository 1048576 128 67108864
```

Both forms of `gc-step` accept `[METADATA_BYTES [MARK_WORK]]` after the copy
and object budgets. RPC defaults to 128 work units. The standalone, exclusive
command defaults to 65,536: its process cannot carry an in-memory cycle across
invocations. An explicitly tiny standalone work budget may only mark, then lose
that unfinished work when it exits. Use the daemon for incremental maintenance
on large stores; standalone partial marks are never trusted by a later owner.

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

A cycle holds a LevelDB metadata snapshot defining the finite baseline root and
object universe. Root scanning, existing manifest-lease seeding, graph marking,
object sweeping, retained-object chunk-reference scanning and chunk sweeping
advance across calls. `MARK_WORK` bounds the logical records/tasks processed in
each call. Reports include `mark_work_done`, `mark_processed`, `mark_pending`,
`mark_phase`, and `chunks_deleted`; zero deletions while `mark_pending` is true
does not mean collection is complete. The background worker counts marking and
scan work as progress. A round that deletes metadata or observes sequence changes
requests a fresh follow-up round (`mark_phase: cycle_complete`); completion needs
a clean round, so the last empty deletion batch cannot hide newer garbage.
Continuous commits can prevent a complete status even while reclamation advances.

A private `.gc-mark-*` directory contains a separate LevelDB with the typed FIFO
frontier, task deduplication, object and chunk liveness, and effective legacy-run
entries. It contains metadata only. Each task atomically publishes its child
work, live marks and continuation with removal of the completed queue entry.
Run entries, overlay resolution and radix children have resumable cursors;
no full namespace map, live-object set or object/chunk index is kept in memory.
World/revision/base and leased-manifest closures are traversed by type. Inline
file data is distinguished from indexed content by its paired inode, including
legacy direct roots and open-unlink orphans. Arbitrary named roots remain opaque;
canonical staging aliases are weak roots. Every shadowed legacy PUT is retained
as opaque data, while only effective inodes are interpreted.

Publication has an insertion barrier. While still holding the writer mutex,
`publish` first synchronizes object bytes and commits the main metadata batch,
then enqueues newly PUT roots and protects chunks of newly published objects in
the active scratch generation before releasing the mutex. Manifest registration
uses the same writer fence to enqueue its closure. Sweeping holds the writer and
pin gates, checks that its generation remains active and the queue is empty, and
pauses while any external generic pin or physical-location reader exists. It no
longer requires an unchanged logical sequence: concurrent writes extend the
frontier rather than restarting the baseline scan. A new root inserted between
marking and sweeping prevents that sweep until its work is processed.

Only objects in the initial snapshot are sweep candidates. Objects born during
this cycle are left for a later generation. Once object sweeping finishes, a
second metadata snapshot scans **every retained object descriptor** into the disk
chunk-live set. Publication protects later new objects' chunks under the same
writer fence. Only after this scan and all pending graph work finish can unused
C rows be deleted. Object/alias/counter pruning and chunk pruning are separate
synced metadata batches; unreachable chunks can therefore remain indexed after
an interruption, but live object descriptors do not lose their chunks. Cached
bytes for deleted objects are evicted before admitting another reader. No GC
phase advances the logical sequence or creates a revision. Each writer-lock
sweep interval visits at most 128 rows, independently of the requested work quota.

The directory and queue are disposable, never a recovery authority. Normal cycle
completion or owner destruction removes them; after acquiring the exclusive
owner lease, reopening removes abandoned scratch paths, including links without
following their targets, and starts a fresh scan. A crash in the gap between
main publication and the scratch barrier is safe because that old generation is
never reused. Scratch/read/validation failures invalidate the cycle before any
further sweep. A scratch publication-barrier failure is reported to GC, while
the already committed user write and owner remain usable. Uncertain main-store
or DATA writes retain the existing poison-and-reopen behavior.

This permits reclamation while ordinary writes continue when the marker drains
work faster than those writes introduce it. It is not a guarantee against an
unbounded adversarial rate of new reachable graphs; pending work can still
outpace collection. Long-lived generic pins and replay retention also prevent
logical sweeping.

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
reference after marking and requires the insertion barrier to defer that sweep.
It also registers an old manifest lease after marking without changing the
sequence, requiring its newly queued closure to defer that sweep. Cursor tests require
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

Payload copying, object/chunk deletion and unlink counts are bounded per step.
The typed frontier, deduplication and live marks live on disk. Scan cursors and
phase state live in memory; both persist across calls within one owner, and all
are discarded after reopening.
The metadata charge includes a fixed 32 KiB marker/task working reserve plus
bounded sweep and physical-compaction collections. The configured ceiling and
charge remain available as `metadata_budget_bytes` and `metadata_bytes_charged`.
A small ceiling may suffice for graph marking but refuse a later relocation
batch; retry with a larger budget is supported. Charges are conservative, are
not returned for temporary allocations within a step, and are not process RSS.
Existing main/scratch LevelDB caches, memtables and iterator internals,
ObjectStore caches, allocator overhead, and bounded codec/payload buffers remain
outside this accounting. Scratch uses a 64 KiB requested write buffer, no block
cache retention, no compression and a small open-file setting; it consumes disk
space proportional to the graph and queued publication work. Long-lived
metadata snapshots can also retain old main-LevelDB versions until the cycle
finishes or is cancelled.

`MARK_WORK` is a logical-work bound, not a syscall, byte-I/O or wall-clock bound.
A bounded tree lookup can still read up to 256 nodes, and manifest validation
can inspect up to 32 bounded legacy runs. Physical source selection, sealed-C
reference validation and retirement checks still stream complete chunk indexes
outside the writer lock; physical scan cursors are not persisted across calls.
Stopping a worker waits for the current step. Strict latency/RSS bounds and
adversarial producer backpressure are not established by this implementation.

Exhaustion throws `GC metadata memory budget exceeded`; a background job reports
`failed`. Incomplete marking never authorizes a sweep. Earlier durable pruning
or relocation phases may already have committed, so refusal does not roll back
progress. Reads, writes and sync remain usable unless a separate main-store I/O
failure has poisoned the owner. Reopen abandons scratch and safely rebuilds
reachability from current committed roots.

The incremental regression covers one-unit stepping, live writes during
reclamation, exact-ID revival, pins, restart cleanup and corrupt scratch. A
separate independent-encoder marker suite covers legacy overlays, raw magic-like
content, direct and orphan inode pairs, directory/symlink roots, sparse extents,
partial tails and malformed runs/extents. Existing seeded models and concurrent
publication/lease/poison tests remain in the regression suite. Fault-only process
crashes cover marking and sweep boundaries in addition to physical relocation.

The incremental implementation passed all 109 registered tests in the local
metadata-only Release build (98 labeled metadata-regression, 415.05 seconds).
This includes the independent codec fixtures, three seeded namespace models,
background stop/restart, scratch-barrier failure without owner poisoning, and
process crashes immediately before/after both main-root publication barriers.

No signed FSKit-mounted acceptance, whole-machine power-loss guarantee, remote
GC, process RSS ceiling or end-to-end latency target is established by these
local tests. Earlier local counts above describe their historical changes.

### Resumable physical scans

Physical collection now has a separate, restart-discardable state machine. Each
`collect_step(..., MARK_WORK)` admits at most `MARK_WORK` logical units **and**
`MARK_WORK` physical units. `physical_work_done`, `physical_phase` and
`physical_pending` report the latter; `mark_pending` also includes physical
continuation for existing clients. A physical unit visits one directory entry
(including non-segment entries), one chunk-index row, or a bounded phase/EOF
operation. Copy bytes and unlink counts remain independently capped. These are
work bounds, not deadlines: an individual read, directory operation, LevelDB
seek/write or fsync can still stall in the host OS.

Directory traversal and exclusive chunk-key cursors survive calls within the
same owner. Inventory captures a segment-ID high water so concurrent writers do
not continually reset an old pass. It examines one candidate at a time and
retains constant cursor/counter state, rather than maps of every segment.
Candidate accounting and source scans run outside the writer lock. A selected
active source is sealed under that lock; each relocated chunk is checked against
its current location again before publication. A deletion or replacement therefore
cannot be resurrected by a stale scan. Data sync still precedes the synchronous
index publication. The source is retired only after the post-seal scan reaches
EOF: publication cannot introduce new locations in a sealed source.

Retired-marker validation also advances incrementally. A reader location pin
postpones unlink without preventing inventory/relocation of other segments. Once
that work finishes, the background worker reports blocked rather than treating
repeated waits on the same pin as progress. No location pin survives a collection
call. `pending_segments` now reports a discovered pending marker (0 or 1), rather
than an eager global marker count; `physical_pending` is the continuation signal.
Cancellation/reopen discards cursors and rebuilds proofs from durable metadata;
there is no new on-disk format or persisted scan cursor.

ObjectStore discovers the highest segment at owner startup and maintains that
identity and EOF as it appends/rotates. Per-step rotation/admission and already
unlinked retirement recovery no longer enumerate the repository. Startup discovery
and capacity accounting remain proportional to the number of files.

This implementation prioritizes bounded per-call work. Candidate accounting still
scans the chunk index separately per candidate: aggregate work can be proportional
to segments times chunk rows. It does not establish an overall GC throughput,
process-RSS or scratch-space ceiling. Persisted per-segment accounting would need
its own format/recovery design before claiming those properties. Relocations
publish at most eight chunks at a time, limiting the writer critical section but
potentially increasing sync cost compared with a large batch. Production p95/p99 targets require larger mounted workload
measurements, beyond the regression's local timing samples.

`forkfsd_physical_gc_budget` (registered as `metadata-regression`) exercises
one-unit physical calls, finite foreground churn, deletion/new references,
publication during a sealed-source scan, DATA reduction/peak-growth assertions,
and cancellation/reopen. It prints local step and interleaved read/write timing
percentiles without a machine-dependent timing assertion. The concurrency suite
holds a backup location pin through a complete inventory, asserts zero fictitious
physical progress in `wait_pins`, then releases it and checks durable unlink.
The crash suite separately injects chunk-index publication and retired-marker
publication, including recovery with an exact DATA capacity limit.

For repositories requiring more scan work than one invocation allows, use the
long-lived daemon/RPC owner. Standalone CLI processes cannot share these in-memory
cursors: reopening rebuilds proofs, even with the CLI's larger default work
allowance. Repeated short standalone invocations are not a resumable GC protocol.

Local validation (2026-10-27, macOS 27.0.1 arm64, Apple Clang 21, Release,
`WFS_BUILD_LEGACY=OFF`, `WFS_COW_FSKIT=OFF`): all 110 registered tests passed,
including 99 `metadata-regression` tests, in 541.96 seconds with three workers.
The final production changes were built before the GC tests began; the new
concurrency and physical-budget tests were also rebuilt before their execution.

The deliberately one-unit physical regression took 66,232 continuation calls.
Its DATA bytes were 2,192,365 initially, 3,213,445 peak, and 817,260 after drain.
Collection-step p50/p95/p99 were 147/4,842/6,846 microseconds. Eighteen interleaved
foreground samples reported write p50/p95/p99 9,401/24,581/24,581 microseconds and
read 1,088/1,328/1,328 microseconds. These include the namespace API and synchronous
publication, are from a small synthetic fixture with intentionally tiny budgets,
and are neither a native-filesystem comparison nor a production latency SLO.
No signed mounted or remote-CI results were obtained in this implementation turn.
