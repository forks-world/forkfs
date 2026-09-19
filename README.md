# forkfs — World FS provider (BranchFS)

Fork a workspace into hundreds of independently writable worlds on one Mac, on top of APFS.
Design: [`arch.md`](arch.md), [`docs/M1_DESIGN.md`](docs/M1_DESIGN.md). Task board: [`docs/TASKS.md`](docs/TASKS.md).

## Status

M1 (clonefile Worlds). A **Snapshot** is an immutable whole-tree `clonefile()` clone inside the
store, every entry `chflags(UF_IMMUTABLE)`; a **World** is a writable clone of a Snapshot or of
another World, living at a path you choose. Nothing is on the data path: a World is plain APFS,
so everything inside it runs at native speed (measured 99–102%, [`docs/CLONE_MODEL_MACOS27.md`](docs/CLONE_MODEL_MACOS27.md)).

The M0 FSKit passthrough frontend is frozen as a fallback and is not built by default
(`-DWFS_FSKIT=ON` brings back `core/src/view.cpp`, `macos/fskit/` and `world fs mount`).

C++23 core with a C ABI, C++ CLI, CMake + Command Line Tools only; no Xcode needed.
Dependency policy: arch.md §39. Design: [`docs/M1_DESIGN.md`](docs/M1_DESIGN.md).

## Build

```bash
git submodule update --init        # third_party: fmt, Arena, Containa, smallstring (header-only)
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
(cd build/Release && ctest --output-on-failure)
scripts/check-deps.sh build/Release   # binaries link only system libraries
scripts/tests/safety.sh build/Release # every fool-proofing rule, end to end, PASS/FAIL per rule
```

## Usage

```bash
world=build/Release/cli/world

$world fs init ~/src/myproj            # S1: immutable snapshot of the tree (the original is untouched)
$world fs fork --from S1 --to ~/w/a    # W1: a writable clone at ~/w/a
$world fs fork                         # W2 at ~/worlds/W2/<name>, from the newest snapshot
cd ~/w/a && <let an agent loose>       # plain APFS: native speed, no mount, no daemon
$world fs checkpoint W1 --name green   # S2: snapshot the world; the world stays writable
$world fs fork --from W1 --to ~/w/b    # W3: branch off the live world
$world fs list                         # snapshots and worlds
$world fs inspect W1                   # path, inode, origin, entry count, FSEvents cursor
$world fs verify S1                    # snapshot still matches the manifest written at init
$world fs verify ~/w/a                 # identity of a world (repairs the row if it was moved)
$world fs discard W3                   # into the store trash; restorable
$world fs restore W3
$world fs gc --retention 7             # delete trash older than 7 days and stray *.wfs-tmp trees
$world fs status                       # store, counts, free space
```

The store defaults to `~/Library/Application Support/World/fs`; `--store <dir>` or `$WORLD_STORE`
override it. **The store must be on the same APFS volume as the project**: `clonefile()` returns
`EXDEV` across volumes even when `st_dev` matches, so `init` probes with a real clone and refuses
with the command that would work.

Exit codes: `0` ok, `1` error, `2` usage, `3` refused by a safety rule. Every refusal prints one
line of reason and one line of the command to run instead:

```console
$ world fs init ~/w/a
world: /Users/me/w/a is already world W1; init would snapshot a world in place
  try: world fs checkpoint W1
$ world fs verify ~/w/a-copy
world: /Users/me/w/a-copy carries a .world marker but its inode is not the registered one: it is a copy
  try: world fs adopt /Users/me/w/a-copy
```

Rules enforced (docs/M1_DESIGN.md §3): a World is identified by its `.world` marker plus the
inode of its root, so moving it is fine (P1) and copying it is caught (P2); snapshots are
immutable and verifiable (P3); `discard` is reversible (P4); cross-volume clones are detected by
cloning, not by comparing `st_dev` (P6); `/`, `$HOME`, the store, and anything inside a World or
Snapshot are refused (P7); a fork that dies half way leaves only a `*.wfs-tmp` tree for `gc` (P8);
hardlinks that the clone will break are counted and reported (P9); free space is checked before
cloning (P11); every metadata mutation is a `BEGIN IMMEDIATE` transaction and world-level
operations take a flock on the marker (P12); a store from another schema is refused (P13).

## Measured cost (macOS 27.0, M1 Mac mini, best of 3)

| tree | `fs init` | `fs fork` | `fs verify` |
|---|---|---|---|
| 1 000 entries | 0.037 s | 0.040 s | 0.010 s |
| 10 200 entries | 0.280 s | 0.300 s | 0.041 s |
| 50 993 entries | 1.363 s | 1.350 s | 0.177 s |

Half of a 50k fork is the parallel `chflags` unprotect walk (0.73 s) and 0.44 s is the
`clonefile()` itself. Full breakdown and the thread-count scan: [`docs/TASKS.md`](docs/TASKS.md).

## FSKit passthrough frontend (frozen, optional)

```bash
cmake -S . -B build/Fskit -DCMAKE_BUILD_TYPE=Release -DWFS_FSKIT=ON && cmake --build build/Fskit --parallel
scripts/bundle.sh Release          # .app + appex → ad-hoc sign → ~/Applications → lsregister
# One-time, manual: System Settings > General > Login Items & Extensions > File System Extensions
build/Fskit/cli/world fs fsstatus  # must list world.forks.fs.extension enabled=true
build/Fskit/cli/world fs mount W1 <mountpoint>
scripts/smoke.sh <dir> <mountpoint>
```

Why it is frozen: metadata-write workloads run at 17–40% of native through FSKit because the
kernel sends 5–7 XPC round trips per mutation, and 68 µs per round trip is the FSKit floor
(Apple's own msdos module measures 72.6 µs). See [`docs/PERF_STUDY_RT_PARALLEL.md`](docs/PERF_STUDY_RT_PARALLEL.md).

## Debugging

Per-operation trace (what the kernel actually sends the extension):

```bash
log stream --level debug --style compact --predicate 'subsystem == "world.forks.fs"'
```

## Layout

```
core/include/worldfs/worldfs.h        C ABI: store / snapshots / worlds / identity / gc
core/include/worldfs/worldfs_fskit.h  C ABI of the frozen FSKit view + namespace layer (WFS_FSKIT=ON)
core/src/store.cpp                    store directory, VERSION, schema v2, status, EXDEV probe
core/src/world.cpp                    snapshot & world lifecycle, .world marker, identity, trash, gc
core/src/platform_posix.cpp           parallel tree walk, manifest, free space, recursive delete
core/src/platform_darwin.cpp          clonefile, per-file fallback, chflags protect/unprotect, FSEvents
core/src/view.cpp                     M0 inode table + namespace (WFS_FSKIT=ON only)
core/tests/                           core_test.cpp (M1), fskit_test.cpp (WFS_FSKIT=ON)
macos/fskit/                          Objective-C++ FSKit appex (WFS_FSKIT=ON only)
cli/main.cpp                          `world` CLI (C-style C++); `world fs ...` is the FS provider surface
third_party/                          header-only submodules: fmt, Arena, Containa, smallstring
scripts/                              bundle.sh, check-deps.sh, mount.sh, smoke.sh, tests/safety.sh, bench/
```

Extension lifecycle and fskitd:

```bash
log stream --predicate 'subsystem == "world.forks.fs" OR process == "fskitd"' --style compact
```

If `mount` says "No extension with fsShortName found", check pkd: it silently rejects appexes that are not
sandboxed. If it says "disabled", enable WorldFS in System Settings (needed again after the bundle record changes).
