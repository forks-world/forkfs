# forkfs

forkfs is a revision-backed userspace filesystem prototype. The supported runtime
is `forkfsd`, using LevelDB and immutable content-addressed metadata trees.
World heads refer to those trees; snapshots retain immutable revision roots.
Content identity comes from object IDs, never from file size or mtime equality.

## Retirement of the native-directory backend

The previous `world` CLI, `worldfs_core`, and FSKit passthrough are retired.
They are no longer built or tested as supported products. Their source remains
in `core/`, `cli/`, and `macos/fskit/` as historical reference only. Enabling
`WFS_BUILD_LEGACY` is an error; there is no fallback to that backend.
`WFS_FSKIT=ON` builds the new revision/RPC frontend in `macos/forkfs/`,
which is independent of the retired passthrough.

This retires the old directory-scanning/FSEvents diff, stat/manifest snapshot
verification, native clone/pool lifecycle, observed file history, Git integration
and native-directory execution/sandbox interfaces. Existing installed `world`
binaries are not changed by rebuilding this repository; stop using those binaries
and their stores as the current forkfs implementation. There is no automatic
conversion of an old native World store into a revision repository. Use a fresh
build directory: configuration refuses a directory containing retired World
artifacts and preserves those existing files.

A replacement diff must compare two immutable revision roots and their referenced
metadata/content objects. A replacement history must record committed revisions.
`forkfsd fs-diff` now compares two explicit immutable revisions and pages path,
metadata and content changes. `forkfsd fs-history` pages the immutable parent
chain of a named revision. `fs-restore` atomically preserves the previous World
as a safety revision and switches to a selected revision. Neither scanning an
external directory nor comparing timestamps is an accepted substitute.

`forkfsd check` verifies physical object integrity; `fs-check` verifies the selected
World/revision namespace. These remain supported. The daemon's legacy single-file
container compatibility also uses immutable revision manifests; it is distinct
from the retired native-directory `world` backend.

## Build and test

```sh
git submodule update --init third_party/leveldb
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel 4
ctest --test-dir build/Release --output-on-failure --no-tests=error
scripts/check-deps.sh build/Release
```

macOS and Linux are supported. Linux builds require OpenSSL development headers.
LevelDB is built from the pinned submodule. The old SQLite CLI/core and their
header-only dependencies are not needed by the supported build.

## Use the revision namespace

```sh
forkfsd=build/Release/daemon/forkfsd
$forkfsd init project.forkfs
$forkfsd fs-init project.forkfs
$forkfsd fs-mkdir project.forkfs /src
$forkfsd fs-write project.forkfs /src/main.txt input.txt
$forkfsd fs-snapshot project.forkfs base
$forkfsd fs-fork project.forkfs base work
$forkfsd fs-cat project.forkfs /src/main.txt --revision base
$forkfsd fs-snapshot project.forkfs edited --world work
$forkfsd fs-diff project.forkfs base edited
$forkfsd fs-history project.forkfs edited
$forkfsd fs-restore project.forkfs base safety-before-restore --world work
$forkfsd fs-check project.forkfs --world work
$forkfsd check project.forkfs
$forkfsd gc project.forkfs  # offline physical GC for LevelDB CoW trees
```

World mutations atomically publish immutable objects and a new metadata root.
Retained revisions remain immutable. The prototype has an optional macOS 26+ FSKit frontend; see
[FSKit setup and limits](docs/FSKIT.md). It has no native Git
checkout integration or replacement for `world exec`. Regular files support up to
64 MiB through bounded 256 KiB I/O, with revision-backed xattrs up to 64 KiB
per value. See [daemon commands, guarantees and limits](daemon/README.md).

Older architecture and platform guides describe retired or proposed systems;
they do not enable additional supported runtimes. See
[revision-only transition](docs/REVISION_ONLY.md) for the replacement policy.
