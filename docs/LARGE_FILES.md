# Sparse large files

Regular files can now grow beyond the old 256 KiB prototype limit. Logical size
is bounded by `INT64_MAX` bytes. This is an address-space limit, not a claim that
an 8 EiB dense file has been tested. Regression coverage includes multi-MiB dense
files, a write crossing a block boundary beyond 1 TiB, and the last legal byte.

## Logical format

Files at or below 256 KiB keep their existing opaque content object until a write
or resize needs the indexed representation. Once promoted, they remain indexed,
including after shrinking to zero; opening with truncate may reset to raw format.
File bytes never determine whether a record is a descriptor.

`FFINOD05` extends the FFINOD04 inode layout, including its xattr count and entries.
It is valid only for regular files, with flags at offset 48 equal to 2. Bit 1
selects indexed content; the directory-index bit is clear. Ownership, all existing
timestamps, xattrs, identity and hardlink semantics are retained. Older inode
versions remain readable. Older binaries reject the new inode version.

All integers below are little endian. IDs are canonical forkfs object hashes.

| Record | Layout |
| --- | --- |
| Content descriptor | `FFFILE01` (8 bytes), logical size u64, root ID (32 bytes); exactly 48 bytes |
| Index node | `FFEXT001` (8 bytes), level u8, count u16, then count entries of slot u8 + child ID (32 bytes) |
| Data block | Exactly 65,536 opaque bytes |

The root has level 5. Six levels select successive 8-bit portions of the logical
block index, most significant first. Level 0 references data blocks. Entries are
strictly ordered, count is 1–256, and child IDs cannot be zero. A zero root means
an entirely sparse file; missing slots read as zero. Nodes are never empty.
All-zero blocks are omitted. Allocated blocks cannot begin beyond EOF, and the
unused portion of the last block must be zero. Index levels, lengths and object
hashes are validated; explicit verification also checks EOF and canonical holes.

## I/O and publication

A range operation visits only the requested blocks and their index paths. Writes
copy affected blocks and paths; unchanged subtrees remain shared by revision and
World roots. Sparse extension does not allocate the gap. Shrinking drops complete
subtrees and zeroes the retained partial block, preventing old bytes from returning
on later extension. Pending intermediate paths are excluded from publication.

Index/data objects, the content descriptor and the inode update are committed in
one existing transaction. ObjectStore synchronizes appended bytes before LevelDB
publishes locations and roots. Open-unlink retains the descriptor and its immutable
dependencies until the last handle closes; crash recovery releases orphan roots.
Physical object/segment reclamation remains separate future work.

Each core range read/write and whole-file input is bounded to 8 MiB, keeping a
transaction below the existing 16 MiB object budget. Larger files use repeated
range operations. Whole-file `read`/`fs-cat` refuses materialization above that
budget. CLI file inputs have the same 8 MiB bound. Resident RPC retains its 1 MiB
frame budget; range reads return at most frame size minus status bytes, including
cursor reads, so a response-size error cannot skip unread bytes. Callers loop until EOF.
FSKit returns bounded short
reads/writes for large buffers. File-size limits and per-request limits are distinct.
The historical single-file Journal retains its older transaction limits; the
supported large-file storage backend is the current LevelDB/ObjectStore repository.

## Backup and validation

Local full/incremental archives include data and index objects. Replay graph
validation follows typed FFINOD05 content edges, and staged restore validates the
namespace before publication. The revision sync mock source and independent
receiver also traverse the new typed edges, preserving the full revision closure.
Its native restore adapter bounds individual materialized files to 8 MiB; it is
still a test protocol implementation, not production remote sync.

Tests compare randomized edits against an independent byte model; check revision,
fork, hardlink and retained-handle isolation; verify sparse allocation, overflow,
EOF and shrink/regrow; reject malformed/missing index objects; cover crashes and
I/O errors around data synchronization/metadata publication; and restore full plus
incremental backups after removing the source repository. FSKit callback tests
exercise writes beyond the former boundary. Real mounted workload acceptance is
still required.

Independent oracle design, reproduction, reduction and remaining limits are
documented in [Large-file testing](LARGE_FILE_TESTING.md).
