# CoW metadata storage foundation

This directory introduces the immutable object storage used by the forthcoming
forkfs metadata tree. It does not add a daemon executable or mounted filesystem.
LevelDB 1.23 stores SHA-256 addressed objects and mutable retention roots; one
synchronous WriteBatch publishes roots and the sequence together. Verified object
reads have a bounded cache. Root facts are cached within a committed sequence.
Failed writes poison the store until reopen. Historical objects are retained;
physical garbage collection is not implemented.

Journal is the current tree-facing API. It also contains the preceding single-file
journal reader/writer for compatibility; new stores use LevelStore::create and a
Journal opened on the LevelDB directory. This PR does not introduce the legacy
container format's CLI or its migration workflow.

Build and test independently of the existing world CLI:

```sh
 git submodule update --init --recursive
 cmake -S . -B build/cow -DWFS_BUILD_LEGACY=OFF -DCMAKE_BUILD_TYPE=Debug
 cmake --build build/cow -j4
 ctest --test-dir build/cow --output-on-failure
```

macOS and Linux are supported. Linux requires OpenSSL development headers.
Upstream LevelDB is pinned and compiled as C++17, with RTTI disabled in the Env
wrapper to match upstream. The object store tests exercise immutable validation,
cache eviction, root invalidation, concurrent reads and failure poisoning.

MetadataTree is a deterministic persistent treap: immutable node objects contain
key/value/left/right references. Updates copy only the search path and rotations;
revision/fork callers can retain or share the old root. The standalone tests cover
insert/update/delete, tree invariants, bounded pagination, deterministic encoding,
atomic root publication and reopening with both historical and current roots.
World/revision naming, namespace operations and OS mount adapters are outside
this PR. See ../docs/COW_METADATA_TREE.md for the format and remaining limits.

Storage lock regression tests are also registered. Preparation hashes outside the
writer mutex; verify scans a consistent LevelDB snapshot; manual compaction does
not hold the outer publication lock. Writers still serialize durable publication.
See the storage locking section of docs/COW_METADATA_TREE.md for concurrency limits.

## Namespace version views

`NamespaceView` resolves World heads and revision descriptors to immutable CoW
metadata roots. LevelDB plans migrate legacy flat/sorted-run views to `FFTREE01`
without copying file payloads. Reads, prefix enumeration and paginated tree
enumeration use the captured root, even if another head is subsequently published.
Each `plan()` creates an independent editor: returning or publishing a plan does
not change the source view. Construct a fresh view to read the new head.

`freeze()` captures a CoW view's complete reference map for the existing offline
sorted-run compaction interface. This compatibility path scans the tree and does
not promise logarithmic compaction or change the source World. Legacy sorted runs
remain readable and can be migrated back to a CoW tree on LevelDB.

This is the version-view library layer. The mounted filesystem, inode operations,
Container coordination and RPC frontend are not part of this integration.

## Container and file-operation library

`forkfs_namespace` adds the repository owner and namespace/file-operation API
above `forkfs_metadata`. `Container::create()` creates a LevelDB repository;
`Namespace::initialize()` creates its root inode. Namespace operations publish
content, inode and CoW head updates in the same synchronous transaction.
Snapshots retain immutable roots, and forks publish independent World heads.

The API supports directories, hardlinks, symlinks, attributes, offset I/O,
append/truncate, and open handles that retain unlinked files until final close.
Container coordination serializes logical operations and protects handle state;
the narrower storage locks do not yet establish parallel namespace requests.
File content is currently bounded to 256 KiB per object/file. This library does
not expose a mounted filesystem or install an operating-system frontend.

Standalone Release validation includes file I/O, handle lifetime and transaction
concurrency suites alongside the storage/tree/view suites. The CLI/RPC and their
process-crash tests are the next integration layer.

## forkfsd CLI and resident RPC

Build the service without the legacy world frontend:

```sh
cmake -S . -B build/forkfsd -DWFS_BUILD_LEGACY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/forkfsd --parallel 4
ctest --test-dir build/forkfsd --output-on-failure --no-tests=error
build/forkfsd/daemon/forkfsd init project.forkfs
mkdir -m 700 runtime
build/forkfsd/daemon/forkfsd serve project.forkfs --socket "$PWD/runtime/control.sock" --rpc
```

From another terminal:

```sh
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-init
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-mkdir /src
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-write /src/main.txt input.txt
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-cat /src/main.txt
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-snapshot base
build/forkfsd/daemon/forkfsd request "$PWD/runtime/control.sock" fs-fork base work
```

Input file bytes are read by the client and sent in the bounded RPC frame; the
server does not open a host input filename supplied by a write request. Handle
operations require a session token, with lease renewal and expiry cleanup. RPC
requests currently execute serially. The socket's private parent directory must
be owned by the service user and inaccessible to other users. This authenticates
an OS identity; an agent broker/sandbox boundary and mounted frontend remain
separate work. After a crash, confirm the old process has exited before removing
its stale socket path. Production builds contain no fault-injection hooks.

Metadata requests use the same resident service:

| Command | Arguments after command |
| --- | --- |
| `fs-istat` | `INODE_ID` |
| `fs-setattr` | `PATH PATCH` |
| `fs-isetattr` / `fs-hsetattr` | `INODE_ID/HANDLE PATCH` |
| `fs-ilistxattr` / `fs-hlistxattr` | `INODE_ID/HANDLE` |
| `fs-igetxattr` / `fs-hgetxattr` | `INODE_ID/HANDLE NAME` |
| `fs-isetxattr` / `fs-hsetxattr` | `INODE_ID/HANDLE NAME POLICY VALUE` |
| `fs-iremovexattr` / `fs-hremovexattr` | `INODE_ID/HANDLE NAME` |

`PATCH` is a nonempty comma-separated list of `key=value` assignments. Supported
keys are `mode` (octal permission bits), `size`, `uid`, `gid`, `modified_ns`,
`accessed_ns`, and `created_ns` (all other values unsigned decimal). Duplicate or
unknown keys, empty values and out-of-range numbers fail before mutation. All
requested attributes and resized content publish in one transaction; omitted
attributes retain backend semantics, including automatic mtime/ctime updates.
For example, `fs-isetattr INODE_ID mode=600,size=128,uid=501,gid=20` changes all
four fields atomically. Obtain inode IDs from `fs-stat` or `fs-lstat`; direct inode
operations do not follow symlinks. Deleted inodes require a retained handle.

Xattr `POLICY` is explicitly `upsert`, `create`, or `replace`. The raw wire `VALUE`
is binary, including NUL and empty values. In `forkfsd request`, pass an input
**filename** for `VALUE`; the client reads that file. Get returns the exact bytes,
and list returns sorted newline-delimited names (names cannot contain newlines).
The existing 64 KiB value and 128 KiB inode limits still apply. Handle operations
require `--session TOKEN`; all metadata commands accept `--world NAME` or
`--revision NAME`. Revisions reject mutations. Session ownership, view matching,
and open-unlink retention follow the existing handle rules. These are service
interfaces, not additional offline CLI commands or permission enforcement.

`FFRPC001` accepts 1..16 length-prefixed fields, with the existing 1 MiB frame
limit; 9 fields are needed for setxattr with both view and session selectors.
`FFRSP001` status is zero on success, 1 for untyped/protocol failures, or
`0x80000000 | errno` for POSIX `system_error` failures. Errno values use the
**server platform** numbering; this protocol is local to a Unix socket. Older
clients that reject all nonzero statuses remain compatible; the C++ client now
throws `std::system_error` for tagged errno responses. Existing legacy operations
may still return untyped failures. Invalid attribute patches and xattr policies
return EINVAL; underlying inode/xattr errors retain their errno.

The `forkfsd_rpc_metadata` black-box test runs under the `metadata-regression`
CI label. It checks complete attribute/content state after rejected requests,
exact errno values, binary/empty and size-limited xattrs, session/view isolation,
read-only revisions, open-unlink handles, CLI transport, and daemon restart.

Process-crash suites terminate the separate fault executable immediately before
or after synchronous LevelDB publication. Reopen must expose the complete old
or complete new state, including inode/content, branch roots and orphan handles.
These tests validate process-crash recovery, not machine power-loss durability.

## Inode reads for platform frontends

Namespace exposes `root_inode`, `lookup_child`, `stat_inode`, `read_inode`,
`readlink_inode` and `list_inode`. Lookup returns the child inode itself without
following a final symlink; the platform can then resolve that symlink. Regular
file reads reject directories and symlinks. Each call validates the selected view
under the Container lock, and hardlinks share one logical inode ID. Directory
renames do not invalidate child lookup by the directory inode.

Directory pages include attributes from the same logical state as the selected
names. The continuation is the last returned name; pagination of a mutable World
is a live scan and is not a snapshot across calls. Use an immutable revision or
the existing pinned DirectoryCursor API for consistent multi-call enumeration.
CoW views traverse the bounded key range; legacy views retain a full-enumeration
fallback. Inode APIs reject unlinked/deleted IDs; already-open files must use the
handle API to preserve open-unlink lifetime. These methods are platform-neutral
backend operations, not an installed FSKit or FUSE mount.

## Metadata manager regression model

`forkfsd_metadata_manager` uses a fixed random seed and an independent map model
to check 200 batches of inserts, replacements and deletes. Every batch compares
the live view and varied-size pagination with that model, while captured views
and old manifest roots must remain unchanged. Periodic stale-sequence publication
attempts must leave roots, object counts and sequence untouched, and retrying the
same view must produce the same plan bytes.

Named revisions are checked during further writes, after native compaction and
after reopen. Invalid root-deletion plans and malformed persisted manifests
(truncated tree, zero tree root, zero runs, duplicate runs) must be rejected
without affecting the valid main head. This is deterministic logical/recovery
coverage; it does not simulate power loss or qualify mounted performance.

The model suite runs four fixed seeds (800 total batches), including the maximum
32-bit seed. Mixed invalid transactions cover missing immutable references,
duplicate keys, deletion of an absent root and oversized payloads; valid earlier
mutations must not leak into durable roots or object/sequence counters.

CI has a dedicated `Metadata regressions` job on macOS and Linux. It builds with
`WFS_BUILD_LEGACY=OFF` and runs `ctest -L metadata-regression --no-tests=error`,
covering the model seeds, tree/view, transaction and lock tests, preview recovery,
and file/handle process-crash suites. The suites also remain in the existing full
Release checks. Failure logs are uploaded separately for each platform.

The proposed unified scenario/model/replay/resource harness is described in
[TEST_HARNESS_DESIGN.md](../docs/TEST_HARNESS_DESIGN.md). It separates metadata,
service and actual mounted-filesystem evidence; the unified runner is not yet implemented.

### Restore a World (CLI/RPC)

`forkfsd fs-restore CONTAINER REVISION [--world WORLD]` restores an existing World
and revokes its live handles, locks and directory cursors. Preserve current live
state with an explicit snapshot before restoring if needed. Revisions and other
Worlds remain accessible. RPC uses the same `fs-restore` command. See
[the restore contract](../docs/RESTORE_DESIGN.md) for the local durability boundary,
orphan batch limit and the still-unimplemented live mount cache barrier.

### Standalone script control

Use [scripts/forkfs.py](../scripts/forkfs.py) for repository, foreground RPC service,
file, snapshot, branch and restore operations without the legacy world executable.
See [the script guide](../docs/SCRIPT_CONTROL.md) for complete examples. Native
mount/unmount deliberately report unavailable until FSKit mount control is ready.

## Inode-addressed file handles

Native frontends can call `Namespace::open_inode(id, access[, options])` after
lookup, without reconstructing a path. It opens an existing regular inode in the
selected branch/revision and shares the path-open handle lifecycle, permissions,
append behavior, duplication, orphan retention and restore revocation. Opening
and optional truncation pin the inode under the same namespace lock and commit
boundary. Create/exclusive options are rejected; create remains a namespace
operation. Inode opening does not follow symbolic links. After the last directory
link disappears, only existing handles can access the orphan; opening its ID again
returns ENOENT. Revision handles are read-only. This API is backend groundwork;
the FSKit adapter still exposes only immutable revision reads.

Native frontends can also create entries with `create_child(parent, name, directory,
mode)`, and remove or rename entries by parent inode and name. Removal requires
an expected inode ID. Rename requires an expected source ID and either an expected
overwritten target ID or an absent target (the default). Identity comparisons and
mutation occur within one namespace update, so a stale FSKit item cannot delete
or overwrite a different file which has taken its name. Path and inode operations
share retirement, replacement and directory-link accounting. Existing handles to
an overwritten or unlinked regular file remain valid until closed.

Indexed views prevent rename cycles by walking destination ancestors and verifying
parent backlinks; the work depends on directory depth, not moved subtree size.
Old unindexed views retain the paged subtree fallback until explicitly upgraded. Names and
modes retain the prototype's printable-ASCII and 0777 restrictions.
`symlink_child(parent, name, target)` creates a symbolic link without resolving
the target; `readlink_inode()` reads its stored target. `link_child(source_inode,
parent, name)` adds a hard link to the exact inode in this view, including symbolic
links themselves. It rejects directory sources and unlinked or stale source
inodes, even if a handle still retains their contents. Both operations validate
the parent and publish atomically, share their mutation logic with path APIs,
and reject revision writes. Richer attributes and writable FSKit callbacks remain
outstanding. `set_inode_attributes(inode, attributes)` updates the selected inode's
mode, modification time and optional regular-file size atomically, without following symbolic links or relying
on its current path. Hard-link aliases observe the same attributes; snapshots and
other branches remain unchanged. Missing or unlinked inodes return ENOENT, revision
writes return EROFS, and invalid attributes return EINVAL. An empty update validates
the inode but does not publish a transaction. Handle-based updates remain available
for retained unlinked files.

Size updates share the path/inode/handle attribute transaction: growing a file
zero-fills its suffix, shrinking drops the suffix, and hard links observe the same
content and size. A resize updates mtime unless the transaction supplies an explicit
mtime. An unchanged size alone does not publish a transaction. Directories reject
size with EISDIR, symbolic links with EINVAL, and sizes beyond the 256 KiB prototype
limit with EFBIG. Handle size updates require a writable handle (EBADF otherwise),
including when that handle retains an unlinked inode. Errors leave all requested
attributes and content unchanged. The inode-size regression runs in metadata CI.

Ownership (`uid`, `gid`), access time (`accessed_ns`) and birth time (`created_ns`)
are optional attributes in the same transaction. They are exposed in inode stat
results and CLI/RPC stat JSON; reads do not automatically update atime. Historical
records report zero for these previously unstored fields. Setting any of them
upgrades only the affected inode to `FFINOD03` (80 bytes): the original first 48
bytes, directory-index flag at 48, uid/gid at 52/56, zero reserved word at 60,
and access/birth nanoseconds at 64/72. Integers are little-endian; timestamps must
fit signed 64-bit nanoseconds. New readers accept all three inode formats; older
readers reject the new record rather than dropping its extra attributes. Future
updates retain the extended format even when every new attribute is set to zero.
These fields store metadata; user identity mapping and ownership authorization
remain frontend responsibilities. ACLs remain outstanding.

Inode and handle xattr APIs support list/get/set/remove, with Upsert/Create/Replace
policies. Missing names return ENOATTR on macOS and ENODATA on Linux; Create on an
existing name returns EEXIST. Names are 1..255 printable non-space ASCII bytes,
values are binary (including empty), with at most 128 names and 64 KiB per value.
The complete inode record is limited to 128 KiB; budget errors return E2BIG before
publication. Revision mutations return EROFS. Hard links share attributes, forks
and revisions keep independent versions, and handles retain xattrs after unlink.
Reads and mutation use logical inodes; host backing-file xattrs are never queried.

The first xattr mutation uses FFINOD04: the FFINOD03 80-byte field layout followed
by a little-endian uint32 count and sorted records (uint32 name length, uint32
value length, name bytes, value bytes). Duplicate/unsorted names, unknown flags,
bad lengths and trailing bytes are rejected. Removing the last xattr retains the
new format. Xattrs reside inside the CoW inode payload, so existing orphan retention
and cleanup apply without extra roots. Copies of InodeInfo share an immutable xattr
map; mutations copy that map. The read-only revision FSKit adapter reads stored
xattrs and advertises 64 KiB values; all set/delete callbacks still return EROFS.

`open_directory_inode()` opens a stable directory cursor by inode on LevelDB.
It shares path-based cursor semantics: enumeration and optional attributes come
from the captured metadata root, survive later rename/removal, and are revoked by
restore or Container destruction. `list_inode()` instead pages the current view;
its name cookie does not freeze concurrent changes. Legacy journal views support
live paging but return ENOTSUP for stable cursors. Inode reads report EINVAL for
malformed IDs, ENOENT for missing inodes, ENOTDIR/EISDIR for wrong directory types,
and ELOOP when a regular-file read addresses a symbolic link.


## Versioned directory parent index

Newly initialized namespaces include `\0forkfs/parent-index` with `FFPIDX01` and
one `\0forkfs/parent/<inode>` record per directory. Each record contains `FFPAR001`,
the parent's 32-byte ASCII inode ID, and the directory entry name (0..255 bytes).
Only the root has an empty name and points to itself. Regular files and symlinks
have no parent records because files can have multiple links. Each directory
create, rename (including same-parent name changes), replacement and removal
updates its record in the same transaction as directory entries and link counts.
These keys live inside the same CoW namespace as other metadata, so snapshots,
forks, restore and compaction carry the correct version automatically.

`parent_inode(id)` verifies the recorded parent/entry backlink before returning a
parent. Full namespace verification requires exactly one matching record for
every directory and rejects missing, extra or inconsistent records. A malformed
index is an error, never a reason to silently fall back or reconstruct it.

Existing namespaces without the marker remain readable/writable without a
migration. `build_parent_index()` explicitly upgrades only the selected writable
view in one transaction, is idempotent, and never rewrites earlier revisions or
other branches. Read-only revision queries use a traversal fallback and never
write an index. The initial atomic upgrade considers at most 1024 directories (including root),
and preflights the actual generated mutation batch: at most 64 mutations for the
legacy object journal or 4096 for LevelDB. Parent payloads, versioned inode
payloads, metadata pages/runs and the World manifest all count, so a tree below
1024 directories can still exceed its backend budget. Each value is limited to
256 KiB; unique value payloads in the batch are limited to 1 MiB for the legacy
object journal or 16 MiB for LevelDB, matching each backend's deduplication.
Such an upgrade returns
E2BIG before upgrade publication; the original view remains usable. A view already
at the run limit first follows normal compaction/wait/retry handling, then the
fresh view's generated upgrade batch is checked. Oversized historical trees need a staged upgrade design. New indexed namespaces have no such directory-count restriction.
Indexed directories use `FFINOD02` with the same 48-byte layout; regular files and
symlinks retain `FFINOD01`. Upgrade changes every directory inode's version in the
same transaction as the marker and parent records. This makes old directory
readers/writers reject indexed views instead of silently skipping index updates;
old revisions retain their original directory versions. Verification rejects
mixed inode/index versions.
