# forkfs — World FS provider (BranchFS)

Fork a workspace into hundreds of independently writable worlds on one Mac, on top of APFS.
Design: [`arch.md`](arch.md), [`docs/M1_DESIGN.md`](docs/M1_DESIGN.md). Task board: [`docs/TASKS.md`](docs/TASKS.md).

## Status

M1 (clonefile Worlds). A **Snapshot** is an immutable whole-tree `clonefile()` clone inside the
store, protected by a *gate*: the snapshot root directory is mode `0000`, so nothing can traverse,
list, read or write anywhere inside it, while the entries themselves are left exactly as cloned —
which is why forking one costs a `clonefile()` and nothing else. A **World** is a writable clone of
a Snapshot or of another World, living at a path you choose. Nothing is on the data path: a World is plain APFS,
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
WFS_DIFF_BENCH=1 build/Release/core/diff_test   # adds the 50 000-file diff benchmark
```

## Usage

```bash
world=build/Release/cli/world

$world fs init ~/src/myproj            # S1: immutable snapshot of the tree (the original is untouched)
$world fs init ~/src/myproj --hard     # ... with UF_IMMUTABLE on every entry instead of the gate
$world fs fork --from S1 --to ~/w/a    # W1: a writable clone at ~/w/a
$world fs fork                         # W2 at ~/worlds/W2/<name>, from the newest snapshot
cd ~/w/a && <let an agent loose>       # plain APFS: native speed, no mount, no daemon
$world fs diff W1                      # what changed since the fork: A/M/D/T, sorted
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
$world exec W1 -- make test            # run a command inside W1, sandboxed (see below)
```

### `world fs diff`

```console
$ world fs diff W1
A src/new.c
D docs/note.md
M src/main.c
T README.md          # same bytes, different mode / owner / flags / mtime / xattr
$ world fs diff W1 --stat
1 added, 1 modified, 1 deleted, 1 metadata-only
FSEvents since the fork, 2 paths compared, 0 files read, 0.041 s
```

The world is compared against the snapshot it was forked from. Candidates come from an FSEvents
replay starting at the event id recorded at fork time, so the usual cost is O(changes); every
candidate is then stat'ed on both sides and, when size and mtime disagree, its bytes are read.
Events are a hint, never the answer (P10). Only files are reported; an empty directory that
exists on one side only gets its own line, and a rename is a `D` plus an `A` in M1.

`--full` skips FSEvents and walks both trees. The core does that by itself, printing one line of
reason on stderr, whenever the event stream cannot account for everything: a dropped event, a
`MustScanSubDirs`, a cursor older than the volume's FSEvents journal (which holds roughly a day),
or a world that was forked from another world rather than from a snapshot.

Measured on 27.0, 50 000 files with 800 changes: FSEvents **0.087 s**, `--full` 1.354 s, and
`--full --no-xattr` **0.164 s**. Most of the full scan is the xattr leg of the metadata
comparison — `listxattr(2)` costs ~10 µs per call on APFS and a full scan makes two per file;
`--no-xattr` drops it, at the price of not seeing a change that is only an xattr.

The event path pays a fixed cost of its own (building the stream and waiting for its watermark),
so on a small tree `--full` wins outright: a six-file world diffs in 0.002 s with `--full` and
0.4 s through FSEvents. The crossover is somewhere in the tens of thousands of files.

One caveat worth knowing: `fseventsd` writes its journal on a timer, and an isolated change takes
90–600 ms to become visible to a stream created after it (a burst flushes at once). A diff run in
the same breath as a single edit can therefore miss that edit; `--full` is always exact.

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
hardlinks that the clone will break are counted and reported (P9); `diff` treats FSEvents as
candidates and decides by comparing against the snapshot, falling back to a full scan the moment
the event stream cannot account for everything (P10); free space is checked before
cloning (P11); every metadata mutation is a `BEGIN IMMEDIATE` transaction and world-level
operations take a flock on the marker (P12); a store from another schema is refused (P13); a World
somebody is running `world exec` in cannot be discarded, checkpointed or forked from (P5); and that
command runs in a seatbelt sandbox that cannot write to any other World or to the store (P14).

## Running an agent in a World: `world exec`

```bash
world exec W1 -- claude -p "fix the failing test"
world exec W1 --no-sandbox -- make test        # opt out of the sandbox entirely
world exec W1 --require-sandbox -- ./agent.sh  # refuse to run if the sandbox is unavailable
```

`world exec` chdirs into the World root, exports `WORLD_ID`, `WORLD_ROOT` and `WORLD_STORE`, and
takes an **exec lock** (`<store>/locks/W<n>.lock`: pid, start time, command, held under `flock`).
While it is held, `discard`, `checkpoint` and `fork --from` that World are refused with exit code 3
and a `--force` hint (P5); a lock whose process is gone is cleaned up silently. The command's exit
code is `world exec`'s exit code, `SIGINT`/`SIGTERM`/`SIGHUP` are forwarded to it, and a command
killed by a signal reports `128 + signo`.

Unless you pass `--no-sandbox`, the command runs under `sandbox-exec` with a generated seatbelt
profile (P14). Seatbelt lets the **last** matching rule win, so the profile allows first and denies
last, which makes the denies absolute:

| | |
|---|---|
| **denied, write** | the whole store (metadata, trash and every snapshot), and every other World's root |
| **denied, read** | `<store>/snapshots` |
| allowed, write | this World's root |
| | `$TMPDIR`, `/private/tmp`, `/private/var/tmp` |
| | `~/.cache`, `~/.config`, `~/.codex`, `~/.claude`, `~/.npm`, `~/.cargo`, `~/.rustup`, `~/.local/state` |
| | `~/Library/Caches`, `~/Library/Application Support/{Claude,Code,Cursor}` |
| everything else | allowed (`(allow default)`): the sandbox is a fence around other Worlds, not a jail |

The allow list is redundant while the profile starts from `(allow default)` — it is written out so
the profile states what an agent is expected to need, and so that tightening the default later does
not silently break agents. To change it, edit `kAgentHomeDirs` in `cli/main.cpp`.

Two consequences worth knowing: a `world fs` command that *writes* metadata (`fork`, `init`,
`discard`, …) fails with `Operation not permitted` when run from inside a sandboxed `world exec` —
read-only ones like `list` work; and `sandbox-exec` is deprecated on macOS, so the profile is first
tried on `/usr/bin/true`. If that probe fails, `world exec` falls back to running without a sandbox
and says so loudly, unless `--require-sandbox` was given.

## Measured cost (macOS 27.0, M1 Mac mini, best of 3)

| tree (entries incl. root) | `fs init` | `fs fork` | `fs verify` |
|---|---|---|---|
| 1 041 | 0.055 s | 0.048 s | 0.035 s |
| 10 401 | 0.175 s | 0.141 s | 0.068 s |
| 52 001 | 0.785 s | 0.615 s | 0.213 s |

A bare `clonefile()` of the same 52 001-entry tree at the same moment was 0.563 s, so a fork is
`clonefile()` plus ~50 ms: the gate protection is one `chmod` and nothing is cloned into the World
that has to be undone. With `--hard` the same fork costs 1.509 s, because the clone inherits
`UF_IMMUTABLE` on all 52 001 entries and a parallel `chflags` walk has to take it off again.
Full numbers and the thread-count scan: [`docs/TASKS.md`](docs/TASKS.md).

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
core/include/worldfs/worldfs.h        C ABI: store / snapshots / worlds / identity / diff / gc
core/include/worldfs/worldfs_fskit.h  C ABI of the frozen FSKit view + namespace layer (WFS_FSKIT=ON)
core/src/store.cpp                    store directory, VERSION, schema v2, status, EXDEV probe
core/src/world.cpp                    snapshot & world lifecycle, .world marker, identity, trash, gc
core/src/diff.cpp                     candidate verification, full two-tree scan, A/M/D/T
core/src/snapshot_access.{h,cpp}      the one door into a snapshot's contents (the future 0500 window)
core/src/events.h                     candidate collection interface + the path bag
core/src/platform_darwin_events.cpp   the FSEvents replay: flags, journal-age check, dedicated queue
core/src/platform_posix.cpp           parallel tree walk, manifest, free space, recursive delete
core/src/platform_darwin.cpp          clonefile, per-file fallback, chflags protect/unprotect, FSEvents cursor
core/src/view.cpp                     M0 inode table + namespace (WFS_FSKIT=ON only)
core/tests/                           core_test.cpp (M1), diff_test.cpp (T1.3), fskit_test.cpp (WFS_FSKIT=ON)
macos/fskit/                          Objective-C++ FSKit appex (WFS_FSKIT=ON only)
cli/main.cpp                          `world` CLI (C-style C++); `world fs ...` is the FS provider surface
third_party/                          header-only submodules: fmt, Arena, Containa, smallstring
scripts/                              bundle.sh, check-deps.sh, mount.sh, smoke.sh, tests/safety.sh, bench/

<store>/snapshots/S<n>/root           the snapshot tree; the root itself is the 0000 gate
<store>/snapshots/S<n>/manifest       what `verify` checks against, and the lock for the gate window
<store>/locks/W<n>.lock               the `world exec` lock (P5)
<store>/tmp/                          seatbelt profiles generated by `world exec` (P14)
```

Extension lifecycle and fskitd:

```bash
log stream --predicate 'subsystem == "world.forks.fs" OR process == "fskitd"' --style compact
```

If `mount` says "No extension with fsShortName found", check pkd: it silently rejects appexes that are not
sandboxed. If it says "disabled", enable WorldFS in System Settings (needed again after the bundle record changes).
