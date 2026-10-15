# Local segmented content storage

New repositories separate large immutable payloads from LevelDB. LevelDB holds
roots, counters, small immutable objects, content descriptors, chunk locations and
reference-only replay records. Data blocks live in append-only `DATA-<id>.seg`
files inside the private repository directory. These files are backing storage,
not a World directory or a user-visible mount.

This implements the local data-placement and commit-order portion of the original
userspace filesystem design. It does not introduce SQLite or modify upstream
LevelDB. The adapter in `daemon/segment_db.h` sits below the logical object API.

## Objects and chunks

An immutable object's canonical ID remains `Journal::object_id(payload)`, including
its existing type/schema/length framing. Moving its bytes does not change its ID,
CoW references, revision IDs, or portable replay hashes.

- Payloads of at most 256 bytes are inline in LevelDB.
- Larger payloads are split at fixed 64 KiB boundaries. The last block may be short.
- Each chunk is independently content-addressed and verified. Existing chunks are
  reused across objects, Worlds and revisions in the same repository.
- The physical object descriptor lists ordered chunk IDs. The chunk index maps
  each ID to a segment ID, record offset and byte length.
- This generic placement policy also applies to unusually large metadata objects;
  ordinary inode and CoW node objects remain inline when within the threshold.

A changed byte in a 256 KiB object adds at most one 64 KiB content block if all
other blocks already exist. The logical namespace API still prepares a complete
file value in memory; this change removes repeated unchanged bytes from physical
writes, not from all reads, hashing or copying.

## Physical encoding

All integers below are unsigned little-endian. Hashes are 32-byte canonical
forkfs object hashes, not hashes of machine-local offsets.

| Location | Encoding |
| --- | --- |
| `M/format` | `FFLDB003` without retained replay, `FFLDB004` with replay |
| `O/<object ID>` inline | one byte `I`, followed by 0–256 payload bytes |
| `O/<object ID>` external | `FFOBJ001`, total length u64, chunk count u32, ordered 32-byte chunk IDs |
| `C/<chunk ID>` | segment ID u64, record offset u64, payload length u32 |
| Segment header (48 bytes) | `FFSEG001`, segment ID u64, hash of preceding 16 bytes |
| Chunk record | `FFCHNK01`, chunk ID, payload length u32, payload |
| `L/<sequence>` | `FFLOGR01`, sequence u64, previous chain hash, encoded operations with empty payloads for object PUTs |

Segments are at most 64 MiB and records never span segments. Segment IDs start at
1. No in-place overwrite of referenced records is permitted. Appends after reopen
start at EOF; an unindexed partial tail is garbage and does not become visible.
An invalid segment header or an indexed truncated record is an error, not grounds
for silently discarding committed data. Segment headers, chunk lengths and hashes,
and reconstructed object hashes are checked on read.

Reference logs reconstruct canonical transaction bytes from immutable objects.
This preserves the replay chain while keeping file payloads out of the LevelDB
WAL, SST files and compaction. Retained logs therefore also retain their object
and chunk dependencies; a future GC must include them as roots.

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
than interpret descriptors as file contents. Existing FFLDB001/FFLDB002 repositories
continue using their legacy physical layout; they are not silently rewritten.

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

Tests inspect physical LevelDB rows, assert single-block write growth and reuse
across reopen, compare randomized partial writes with an independent byte model,
and exercise segment rollover, source-free full/incremental restore, legacy
migration, malformed locations, missing files, truncated data and hash corruption.
Fault builds cover crashes around append/sync/publication plus partial writes,
sync failures and metadata-publication failures.

The namespace's existing 256 KiB file limit remains. Large-file logical extent
indexes, range-only I/O, sparse extents, GC, segment reclamation, compression,
remote synchronization and cloud-ack durability are separate work. This implementation
does not claim that the complete original filesystem plan is finished.
