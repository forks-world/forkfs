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
The LevelDB submodule is pinned to upstream `13e3c4efc66b8d7317c7648766a930b5d7e48aa7`
(1.23 plus its immediate compaction boundary fix and Apple C-header fix).
Unpatched 1.23 can drop tombstones when the second input level splits one user
key across adjacent SSTs, exposing obsolete aliases after GC. The pinned
[upstream fix](https://github.com/google/leveldb/commit/13e3c4efc66b8d7317c7648766a930b5d7e48aa7)
includes boundary files in both compaction input levels. A real-SST regression
checks point reads, iteration and reopening; it fails with the old pin. Updating
the pin prevents this compaction error; it does not repair an already damaged
store. Failed stores from this investigation are preserved unchanged.

When replay retention skips logical sweeping, the async job stops as `blocked`
once a clean physical round has no pending work or new reclamation. Cleanup may
consume `physical_work_done` even in this terminal round; that progress alone
must not restart indexing. The terminal report keeps `logical_sweep_skipped=true`
and `physical_pending=false` / `mark_pending=false`, allowing callers to distinguish
physical completion from a complete logical reclaim.

Marker advancement serializes its callers separately from the publication mutex.
It selects a queued task under that mutex, retains its Q/P record, and releases the
mutex while reading immutable objects and interpreting their references. Child
tasks and semantic scratch writes are staged locally. The commit reacquires the
mutex, resolves deduplication against concurrent seeds, and atomically publishes
children, semantic records and cursor continuation. Q removal is unconditional;
a P row is removed only if its value still matches the processed version.
Foreground seed/chunk protection never accesses advancement's staging state.
Consequently a blocked object reader does not itself block publication, and
`has_work()` cannot report an empty frontier while a task is in flight. Scratch
I/O during insertion/commit can still block; this is not a lock-free design.

Publication-origin mutable World roots use a disk-backed P row per World key.
Repeated publication overwrites that pending value, coalescing intermediate
Worlds before traversal. Ordinary Q tasks take priority; if Q is empty the marker
processes one P value, retaining it during unlocked reads. If publication replaces
that P row meanwhile, commit leaves the newer value pending. Same-value ABA is
safe because object IDs identify immutable closures. Both Q and P participate in
`has_work()` and the existing writer-locked sweep/completion rechecks.

Baseline roots, revision/base roots and explicit manifest leases still use their
independent Q tasks. Expanded MANIFEST/TREE tasks are never cancelled. Old World
root IDs may remain conservatively live without retaining an obsolete transient
closure; any independently retained revision or lease still traverses that
closure. Scratch P state is discarded on restart with the rest of the cycle.

Staged children use encoded records, capped at the codecs' maximum fanout of 65.
Their encodings are released as the commit batch grows, avoiding a second large
resident vector of decoded tasks. A reader-gate regression verifies foreground
completion before releasing the reader, followed by liveness and pending-count
checks; the same test fails against the earlier lock scope.

Storage profiles expose cumulative `data_prepare_*`, `data_sync_*`,
`metadata_publish_*` and `gc_barrier_*` calls/nanoseconds for completed phase
instrumentation points. `gc_foreground_wait_ns` / `gc_foreground_hold_ns` include all root,
manifest, object and chunk seeding, including the collector's own seeds;
`gc_advance_wait_ns` / `gc_advance_hold_ns` measure the short marker mutex sections,
excluding the separate serialization mutex and unlocked object processing.
Counters survive collection completion/cancellation in one store owner. They are
aggregate durations, not percentiles, and concurrent work may overlap.

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

The first bounded-scan implementation prioritized per-call work. Its candidate accounting
scanned the chunk index separately per candidate: aggregate work could be proportional
to segments times chunk rows. It does not establish an overall GC throughput,
process-RSS or scratch-space ceiling. The scratch reverse-index implementation below removes that repeated scan without
introducing persistent per-segment accounting. Relocations
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


### Per-round segment reverse index

Physical GC now builds a private disk-backed reverse index in
`.gc-physical-XXXXXX/db`. The owning daemon captures the initial tail ID and a
main-LevelDB snapshot together under the publication lock. It visits each `C/`
row once, in budgeted slices, storing scratch metadata only:

- `R/<sid:8 little-endian><chunk hash:32>` → original 20-byte chunk location.
- `N/<sid:8 little-endian>` → summed payload bytes plus record headers, uint64 LE.

No file payload is stored in the scratch database. Its memtable request is
64 KiB, block cache retention is disabled, compression is disabled and the
open-file setting is small. These LevelDB internals are outside the conservative
GC-owned metadata charge, as with logical marking. Scratch disk usage is O(C)
metadata; it is not included in the DATA-segment capacity limit and can fail on
host ENOSPC. Scratch failure abandons that physical pass without authorizing a
retirement or independently poisoning the main store. Successful earlier main
publications remain committed. Startup/cancellation discard the scratch index;
there is no new repository format, persistent index or migration.

Candidate accounting is a scratch counter lookup. Relocation and retirement
validation visit only that segment's reverse-reference prefix. Every relocation
still checks the current main `C/` location for exact equality; retired-reference
validation rejects any current location in the retired segment, including an
unexpected alternate offset. Stale scratch rows resulting from deletes/moves
are conservative and never resurrect a reference. A segment sealed before the
initial snapshot cannot gain new `C/` locations under the writer protocol.

The captured active segment is different: foreground writes may add locations
there before it is sealed, even when the foreground already rotated it by the
time GC selects it. Selection therefore seals it if needed, then captures a
fresh snapshot and performs one additional budgeted `C/` scan for that segment.
The refresh completes before copying or retirement. A retirement proof must
belong to the indexed generation: a marker for the captured tail is rejected
unless that tail's authoritative refresh completed. Merely seeing a newer
current tail does not establish coverage of the older one.

Per physical round, full main-index visits are bounded by the initial scan plus
at most one active-tail refresh. Other visits follow segment prefixes: row visits
scale with C rows and segments rather than their product. Actual CPU/I/O also
includes LevelDB seeks, scratch writes/compaction, payload copying and syncs;
this is not a strict wall-clock linear-time or latency guarantee. Concurrent
changes can require follow-up rounds. Main snapshots are released immediately
after their index/refresh completes; no location pin survives a call.

Reports add `physical_index_work` (main C rows visited),
`physical_reference_work` (scratch references visited), and
`physical_candidate_work` (candidate-counter lookups), each charged within
`physical_work_done`. `forkfsd_gc_segment_index` compares 4- and 12-segment sparse
fixtures using independently counted pre-GC C rows, normalized scan-growth bounds,
known payload oracles, and a write during the actual index build. A separate
corrupt-marker case rotates the captured tail during construction and requires
refusal without unlink or main-owner poisoning. Crash tests include index-row
and index-complete boundaries; restart tests discard corrupt scratch and avoid
traversing scratch symlinks.

After refreshing a captured tail, GC rechecks its immutable sealed size against
the refreshed byte counter. A now fully packed tail is skipped rather than
recopied; the regression requires exactly one copy of each old-source chunk in
that case. Normal scratch cleanup is a separate budgeted phase: closing the
scratch DB, each file unlink, and directory removal are charged operations.
LevelDB close can still wait for background compaction and free version metadata;
this bounds explicit filesystem deletion count, not close latency. Cancellation
and error teardown discard scratch synchronously and are not latency guarantees.

Validation for the reverse-index increment (2026-10-27, same macOS arm64 Release
configuration): the complete 111-test run passed 110 tests in 566.79 seconds. The
sole failure was a 30-second mock HTTP wait in the deep-directory sync case;
all other mock cases passed. With builds and competing tests finished, the entire
34-case sync mock passed unchanged in an isolated CTest rerun (53.00 seconds).
The final production tree then passed all 13 targeted GC tests in 119.16 seconds,
including memory budgets, crash injection, concurrent pins, three namespace seeds
and three online models. No assertion or timeout was relaxed.

The 4-/12-segment fixtures had independently counted pre-GC populations of 19/59
C rows and published one additional live chunk while indexing. The final targeted
run visited 59/239 main C rows in total, across all necessary physical rounds;
the earlier full run visited 59/179. Directory ordering and follow-up rounds can
change totals, so tests assert normalized growth and an explicit visit ceiling,
not exact counts. The packed-tail fixture moved exactly 18 old-source chunks
instead of recopying the fully packed tail.

The final one-unit regression also passed separately in 139.562 seconds with
71,195 calls: DATA initial/peak/final 2,283,834/3,235,906/817,260 bytes; step
p50/p95/p99 154/4,810/6,803 microseconds. Its 18 interleaved samples reported write
13,577/27,238/27,238 and read 843/1,053/1,053 microseconds. As above, these are
small local diagnostic samples, not mounted acceptance or a throughput/SLO claim.
