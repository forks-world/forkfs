# Local segmented content storage

New repositories keep every immutable object payload out of LevelDB, including
empty values, tiny files, inode records, CoW nodes and revision descriptors.
LevelDB holds roots, counters, object location descriptors, chunk locations and
reference-only replay records. Object bytes live in append-only `DATA-<id>.seg`
files inside the private repository directory. These files are backing storage,
not a World directory or a user-visible mount.

This implements the local data-placement and commit-order portion of the original
userspace filesystem design. It does not introduce SQLite or modify upstream
LevelDB. `ObjectStore` in `daemon/object_store.h` has no LevelDB API dependency.
The `LevelStore` facade coordinates two peer components: an unwrapped LevelDB
metadata database and ObjectStore. Logical transactions are forkfs operation
vectors; only location descriptors and references enter a LevelDB WriteBatch.

ObjectStore owns append/read/sync, object encoding, chunk reuse, integrity checks,
and a bounded verified-payload LRU. The metadata coordinator supplies a location
lookup callback and atomically publishes returned index entries. It cannot publish
roots until ObjectStore synchronization has completed. Backup and replay explicitly
materialize object bytes through ObjectStore; there is no DB adapter or custom
LevelDB iterator that secretly retrieves data.

## Objects and chunks

An immutable object's canonical ID remains `Journal::object_id(payload)`, including
its existing type/schema/length framing. Moving its bytes does not change its ID,
CoW references, revision IDs, or portable replay hashes.

- Every payload is stored in ObjectStore. Small objects are packed as variable-length
  records in shared segments, without per-object files or page-sized allocation.
- Larger payloads are split at fixed 64 KiB boundaries. The last block may be short.
- Each chunk is independently content-addressed and verified. Existing chunks are
  reused across objects, Worlds and revisions in the same repository.
- The physical object descriptor lists ordered chunk IDs. The chunk index maps
  each ID to a segment ID, record offset and byte length.
- The same rule applies to file data and immutable metadata objects. Empty objects
  also have an actual zero-length record; there is no inline-data exception.

A changed byte in a 256 KiB object adds at most one 64 KiB content block if all
other blocks already exist. Small legacy content objects still use whole-value I/O. Large files now use a
logical sparse CoW index and bounded range operations; see [Sparse large files](LARGE_FILES.md).

## Physical encoding

All integers below are unsigned little-endian. Hashes are 32-byte canonical
forkfs object hashes, not hashes of machine-local offsets.

| Location | Encoding |
| --- | --- |
| `M/format` | `FFLDB005` without retained replay, `FFLDB006` with replay |
| `O/<object ID>` location index | `FFOBJ002`, total length u64, chunk count u32, ordered 32-byte chunk IDs |
| `C/<chunk ID>` | segment ID u64, record offset u64, payload length u32 |
| Segment header (48 bytes) | `FFSEG001`, segment ID u64, hash of preceding 16 bytes |
| Chunk record | `FFCHNK01`, chunk ID, payload length u32, payload |
| `L/<sequence>` | `FFLOGR01`, sequence u64, previous chain hash, encoded operations with empty payloads for object PUTs |

An empty object has length zero and one zero-length chunk. Other objects use
`ceil(length / 65536)` chunks. Descriptor length is exactly `20 + 32 * count`.

Segments are at most 64 MiB and records never span segments. Segment IDs start at
1. No in-place overwrite of referenced records is permitted. Appends after reopen
start at EOF; an unindexed partial tail is garbage and does not become visible.
An invalid segment header or an indexed truncated record is an error, not grounds
for silently discarding committed data. Segment headers, chunk lengths and hashes,
and reconstructed object hashes are checked on read.

Reference logs reconstruct canonical transaction bytes from immutable objects.
This preserves the replay chain while keeping file payloads out of the LevelDB
WAL, SST files and compaction. Retained logs therefore also retain their object
and chunk dependencies; offline GC retains them with the replay generation.

## Commit and failure ordering

The existing repository owner and writer serialization remain in force:

1. Validate and prepare the logical mutation, resolve reusable chunks, append new
   chunk records. A new segment's header and directory entry are synchronized.
2. Synchronize newly written segment data, including any rolled-over segment.
3. Publish chunk locations, object descriptors, reference logs, roots and counters
   together in the existing synchronous LevelDB WriteBatch.
4. Only then acknowledge the operation.

Failure before step 3 leaves the old committed state and may leave unused segment
bytes. Metadata publication never precedes data synchronization. Write failure
poisons the current LevelStore instance as before. Reopen can reuse existing
indexed chunks and append beyond unused tails. No automatic space reclamation or
segment compaction is implemented yet.

Physical format guards make older binaries reject segmented repositories rather
than interpret descriptors as file contents. FFLDB001/FFLDB002 raw-value repositories
and the superseded FFLDB003/FFLDB004 adapter format remain readable/exportable.
Logical writes to those layouts are rejected with a migration-required error;
there is no fallback that puts new payloads into LevelDB. Backup may enable retained
replay metadata on an older repository, without introducing object payloads.
Migration uses backup/replay into a fresh FFLDB006 repository.

## Backup, replay and migration

The logical archive remains FFRPLY01. Export resolves every external object into
its canonical bytes; machine-local chunk indexes and offsets are not exported.
Export fails on referenced missing, truncated or corrupt chunks. Incremental
export materializes reference logs into complete portable transaction records.

Restore writes into the new segmented layout and synchronizes content before
publishing metadata. Existing namespace and DAG validation then run before atomic
restore publication. A restored repository needs neither the source directory nor
its segment files. Catalog archives are self-contained within their baseline and
incremental recovery chain.

To migrate a closed legacy repository without changing IDs:

```
forkfsd backup OLD_REPOSITORY /absolute/base.pack
forkfsd replay /absolute/base.pack NEW_REPOSITORY
forkfsd fs-check NEW_REPOSITORY
```

The destination must not already exist. Keep the original until the new repository
has been verified and users have switched to it. A baseline preserves revision
contents but does not by itself preserve historical revision-creation receipts;
retain existing incremental catalog archives for historical checkpoint resolution.

## Validation and remaining limits

A standalone test runs ObjectStore against an in-memory location map without
opening LevelDB. Integration tests inspect physical rows (including empty and tiny
objects), assert single-block write growth and reuse
across reopen, compare randomized partial writes with an independent byte model,
and exercise segment rollover, source-free full/incremental restore, legacy
migration, malformed locations, missing files, truncated data and hash corruption.
Fault builds cover crashes around append/sync/publication plus partial writes,
sync failures and metadata-publication failures.

The former namespace 256 KiB file limit is removed by [sparse large files](LARGE_FILES.md).
Offline GC and segment reclamation are implemented; see [ObjectStore GC](STORAGE_GC.md).
Background incremental GC, compression, production remote synchronization and
cloud-ack durability remain separate work. This implementation
does not claim that the complete original filesystem plan is finished.

### Uncommitted-tail recovery regression

`forkfsd_object_store_recovery` runs 27 process-crash cases: fixed and seeded
mid-record cuts (including the second chunk), complete record append, data sync,
pre-metadata publication, and rollover into a new segment. The fault build exits
inside the write path without exception unwinding. One additional persisted-image
fixture discards only the unindexed suffix; this simulates possible lost tail
bytes, not a claim that the test reproduces physical power loss.

The oracle compares every LevelDB key/value before and after the interrupted
transaction, including location indexes, roots, sequence, counters and replay
state. Three reopen/verify cycles must leave those rows unchanged. A backup from
that state must recover the old contents. After retry, every new chunk location
must lie beyond the previous EOF, all crash-tail bytes must remain unchanged, and
incremental replay must expose exactly one new transaction. A separate
post-metadata crash must recover the new contents. Truncating or corrupting an
indexed record must fail reads, verification and backup without modifying metadata
or silently rolling back to the old root.
