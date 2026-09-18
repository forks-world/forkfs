# forkfs — World FS provider (BranchFS)

Fork a workspace into hundreds of independently writable worlds on one Mac, on top of APFS.
Design: [`arch.md`](arch.md). Task board: [`docs/TASKS.md`](docs/TASKS.md).

## Status

M0 (FSKit passthrough, no branching). C++23 core with a C ABI, Objective-C++ FSKit frontend,
C++ CLI. Builds with CMake + Command Line Tools only; no Xcode needed. Dependency policy: arch.md §39.

## Build, install, enable, mount

```bash
git submodule update --init        # third_party: fmt, Arena, Containa, smallstring (header-only)
scripts/bundle.sh Release          # cmake build + ctest → WorldFS.app + appex → ad-hoc sign → ~/Applications → lsregister
scripts/check-deps.sh              # binaries link only system libraries
# One-time, manual: System Settings > General > Login Items & Extensions > File System Extensions > enable WorldFS
build/Release/cli/world fs status  # must list world.forks.fs.extension enabled=true
world fs init <dir>                # W1: base world over <dir>
scripts/mount.sh <dir> <mountpoint>              # base world passthrough (= mount -F -t worldfs <dir> <mountpoint>)
world fs fork && world fs mount W2 <mountpoint2>  # forked world (read-only until M2)
scripts/smoke.sh <dir> <mountpoint>              # POSIX semantics check through the mount
umount <mountpoint>
```

## Benchmark (M0 go/no-go)

```bash
scripts/bench/mktree.sh /tmp/base/tree 200 50 4  # 10k-file synthetic tree
scripts/mount.sh /tmp/base /tmp/mnt
scripts/bench/compare.sh /tmp/base /tmp/mnt 3    # native vs passthrough, prints native%
```

```bash
scripts/bench/realwork.sh /tmp/base /tmp/mnt    # tar/git/find/cmake build on the fmt tree, native vs mount
```

Target: passthrough ≥ 90% of native APFS (arch.md §29). First results and analysis: `docs/TASKS.md`.

## Debugging

Per-operation trace (what the kernel actually sends the extension):

```bash
log stream --level debug --style compact --predicate 'subsystem == "world.forks.fs"'
```

## Layout

```
core/include/worldfs/worldfs.h   C ABI: store / worlds / view / namespace ops / data-plane handoff
core/src/                        C++23 core: store (SQLite), view (inode table + namespace), platform_posix
core/tests/core_test.cpp         libc-only test program (ctest)
macos/fskit/                     Objective-C++ FSKit appex: WorldFileSystem, WorldVolume, WorldItem + plists
cli/                             `world` CLI (C++); `world fs ...` is the FS provider surface
third_party/                     header-only submodules: fmt, Arena, Containa, smallstring
scripts/                         bundle.sh, check-deps.sh, mount.sh, smoke.sh, bench/
```

Extension lifecycle and fskitd:

```bash
log stream --predicate 'subsystem == "world.forks.fs" OR process == "fskitd"' --style compact
```

If `mount` says "No extension with fsShortName found", check pkd: it silently rejects appexes that are not
sandboxed. If it says "disabled", enable WorldFS in System Settings (needed again after the bundle record changes).
