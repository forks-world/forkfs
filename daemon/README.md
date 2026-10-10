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

Process-crash suites terminate the separate fault executable immediately before
or after synchronous LevelDB publication. Reopen must expose the complete old
or complete new state, including inode/content, branch roots and orphan handles.
These tests validate process-crash recovery, not machine power-loss durability.
