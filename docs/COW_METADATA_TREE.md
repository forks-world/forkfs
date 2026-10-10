# CoW metadata tree

The metadata index is a persistent ordered treap above LevelDB, not a new LSM
implementation. LevelDB owns WAL, SST files and compaction. Journal provides
verified SHA-256 object reads and synchronous atomic root publication. Each tree
editor maintains its own decoded-node cache and uncommitted objects.

## Format and algorithm

`FFTRNO01` nodes contain an 8-byte magic, little-endian u32 key length, three
32-byte IDs (value, left, right), then the key. Zero child IDs mean empty subtrees;
value IDs must be nonzero. Keys start with the eight bytes `\0forkfs/` and have
length 8..1024. A `FFTREE01` manifest contains its 8-byte magic and 32-byte root ID.
SHA-256 of `forkfs.metadata.priority.v1` plus key determines treap priority; keys
break ties. The binary search ordering and heap ordering are checked by entries().
Depth is limited to 256. Lookups and bounded prefix pagination visit relevant
paths, while verification via entries() traverses the whole tree.

`apply` takes a sorted map of changed values or deletions, path-copies affected
nodes and rotates/merges subtrees. It returns the manifest and reachable new node
payloads. The caller must publish those objects and the manifest atomically, along
with all new value objects, via Journal::transact. Tree editing itself does not
commit. The namespace root key must remain present. Keeping the old manifest
retains the old logical view, and starting another editor at that root implements
fork sharing. This PR demonstrates both without adding the namespace/RPC layer.

## Limits and validation

Historical objects are retained indefinitely; physical GC, persistent reference
counts and World/revision deletion are future work. This tree uses single-writer
editing; caller serialization and expected-sequence checks are required for
concurrent writes. Node decoding validates format and object integrity, whereas
whole-tree ordering/heap validation is explicit. The 256 KiB object limit and
4096 mutations / 16 MiB unique payload transaction limits belong to the store.

Tests cover current/historical trees after reopening, deterministic updates,
ordered pagination, real insert/update/delete and complete tree checks. Storage
regressions cover root cache invalidation, concurrent reads, immutable object
validation and poisoned writes. These are library tests, not mounted filesystem
or Git/build performance acceptance. No measured latency improvement is claimed.

## Unchanged paths

Setting an existing key to its current object ID or deleting an absent key keeps
the same node ID. An unchanged child ID propagates the original parent ID back
up the path, avoiding encoding, hash and pending objects for that path. These
comparisons use decoded verified nodes and do not skip integrity checks. Actual
namespace timestamp changes would still be real updates; this does not weaken
write durability or equate all repeated file writes with no-ops. Tests exercise
all-value no-op batches and absent deletion on persisted trees, including reopen.

## Publication session lifetime

A MetadataTree editor belongs to one publication session. It may produce a plan,
retry it with apply({}), or compose further changes before publication. Returned
plans own their bytes but do not acknowledge durable storage: pending nodes must
remain until publication succeeds. Automatically clearing them when returning a
plan would make retry/composition omit nodes that exist only in RAM.

After successful Journal::transact, construct a fresh MetadataTree at that
committed manifest root before the next apply. Its pending set is empty and
unchanged edits emit no objects. The current API has no publication acknowledgement;
same-editor reuse across published transactions is outside this lifecycle. Tests
publish only the latest of two composed, previously unpublished plans, then decode
with a fresh editor and reopen the store, covering preservation of both edits.

Malformed batches, namespace-root deletion and missing-root initialization are
rejected before mutation. This is not a general rollback guarantee for corruption,
depth-limit errors or allocation failures during editing.

## Storage locking

Input validation, reference verification and payload hashing in transact() run
before the writer mutex. After acquiring it, the writer rechecks health and the
expected sequence before checking current roots, validating existing immutable
payloads and publishing one synchronous batch. Stale preparation publishes no
objects or counters. The caller must keep mutation buffers unchanged throughout
the synchronous call. Writes and full-sync acknowledgement remain serialized.

verify() pins a LevelDB snapshot and reads its durable counters, index and root
references with the same ReadOptions. It does not hold the writer mutex during
its scan and does not compare against potentially newer in-memory counters. All
snapshot error paths release the pin; health is checked before and after scanning.
compact() relies on LevelDB's internal synchronization and uses snapshot verify
afterward instead of holding the writer mutex across manual compaction. LevelDB
can still stall writers or contend internally during maintenance.

Root cache locking and LevelDB's own cache/DB locks remain. This change does not
make the legacy single-file Journal or MetadataTree editor thread-safe. Callers
must join operations before destroying LevelStore; a snapshot is not an exported
handle that can outlive the store. Future logical GC needs its own retention
contract. Container/namespace and the serial RPC loop are outside this library PR
and still require follow-up work for concurrent end-to-end request handling.

The dedicated test executable alone compiles FORKFS_TEST_LOCKS barriers. It pauses
preparation, snapshot capture and compaction entry and requires another writer to
finish before release. It also checks stale publication rejection and concurrent
scan/write consistency, then reopens and verifies. Restoring the broad preparation
lock makes the barrier test fail with `writer blocked by unrelated
preparation/maintenance`; the narrowed version passes. This is correctness and
progress evidence, not a p95 or mounted-filesystem performance measurement.
