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
$world fs pool fill S1 --count 4       # keep 4 pre-cloned worlds ready: a fork then costs ~7 ms
$world fs pool status                  # what is waiting, per snapshot
$world fs pool drain S1                # give the space back
$world fs status                       # store, counts, free space
$world exec W1 -- make test            # run a command inside W1, sandboxed (see below)
```

### `world fs pool` — forking in single-digit milliseconds

A `clonefile()` of a big tree is fast but not free: 0.6 s for 50 000 entries. The pool moves that
cost off the fork. `world fs pool fill S<n>` pre-clones whole worlds into `<store>/pool/S<n>/<uuid>`
— finished trees with no `.world` marker and no row in `worlds`, so they are not worlds yet and
nothing outside the store can reach them (P7 refuses the path, and `world exec`'s sandbox denies
the store). A `fork` from that snapshot then writes the marker, renames the entry into place and
commits the row: O(1), whatever the tree size.

```console
$ world fs pool fill S1 --count 2
2026-09-19 15:02  pool fill S1: +2, 2 ready
$ world fs pool status
SNAP   NAME                   READY  BUILDING   STALE    ENTRIES  NEWEST
S1     myproj                     2         0       0      52001  2026-09-19 15:02
$ world fs fork --from S1 --to ~/w/c
W7  /Users/me/w/c  (pool)
```

The fork that took an entry puts one back: it spawns a detached `world fs pool fill S<n>` whose
output goes to `<store>/logs/pool.log`, so the next fork is fast again and this one does not wait
for it (and if a filler is already running, it does not even spawn one). `$WORLD_POOL_TOPUP` sets
how many to keep ready (default 2; `0` turns the automatic top-up off). A store-level `flock` makes
two fillers one. `--no-pool` on `fork` always clones here and now.

Measured (T1.7, [`docs/M1_RESULTS.md`](docs/M1_RESULTS.md)): a pool hit is **8.6 / 9.0 / 9.7 ms**
for a 1k / 10k / 50k-entry tree, whole command, process start included — against 0.026 / 0.110 /
0.505 s when the pool is empty. What the pool buys is latency, not throughput: 1000 forks back to
back are only 10% faster with it, because the machine still does 1000 `clonefile`s, just not while
the fork is waiting.

The pool is opt-in: nothing is pre-cloned until you ask for it, because every entry costs a real
tree's worth of APFS metadata (~308 B/entry) until it is used or drained. `world fs verify S<n>`
checks the waiting entries too (still there, not written to since they were cloned), and
`world fs gc` removes the entries of snapshots that are gone, half-built trees and anything under
`<store>/pool` that no row claims.

### `world fs diff`

```console
$ world fs diff W1
A src/new.c
D docs/note.md
M src/main.c
T README.md          # same bytes, different mode / owner / flags / mtime / xattr
$ world fs diff W1 --stat
1 added, 1 modified, 1 deleted, 1 metadata-only
full scan of both trees, 9900 paths compared, 1 files read, 0.158 s
$ world fs diff W1 --events --stat
1 added, 1 modified, 1 deleted, 1 metadata-only
FSEvents since the fork, 2 paths compared, 0 files read, 0.041 s
```

The world is compared against the snapshot it was forked from. **By default `diff` walks both
trees**: every path is stat'ed on both sides and, when size and mtime disagree, its bytes are
read. Only files are reported; an empty directory that exists on one side only gets its own
line, and a rename is a `D` plus an `A` in M1.

The other path replays FSEvents from the event id recorded at fork time, which makes the cost
O(changes) rather than O(tree). It is used when the world has more than 200 000 recorded entries,
or when you ask for it with `--events`; `--full` forces the walk. Events are a hint, never the
answer (P10): every candidate is verified against the snapshot exactly as the scan verifies it,
and the core silently falls back to the walk — printing one line of reason on stderr — whenever
the event stream cannot account for everything: a dropped event, a `MustScanSubDirs`, a cursor
older than the volume's FSEvents journal (which holds roughly a day), or a world that was forked
from another world rather than from a snapshot.

Why the walk is the default, measured on 27.0 with 50 000 files and 800 changes: FSEvents
**0.049 s**, `--full` **0.96 s**, `--full --no-xattr` **0.129 s**. The 4-thread walk is five
times faster than the design assumed, so the event path wins by under 2× — and only because of
the xattr leg of the metadata comparison, which `--no-xattr` drops at the price of not seeing a
change that is only an xattr. Against that, the event path has a fixed cost of its own (building
the stream and waiting for its watermark): a six-file world diffs in 0.002 s by walking and 0.4 s
through FSEvents.

The walk reads a directory with `getattrlistbulk(2)`, one call per batch rather than one
`fstatat(2)` per entry, and asks for `ATTR_CMNEXT_EXT_FLAGS` along the way: `EF_NO_XATTRS` tells
it, for free, which entries have no extended attributes at all, and those never reach
`listxattr(2)`. On a tree of ordinary files that closes the gap almost completely — a real
37 000-entry tree with 800 changes diffs in **0.17 s** by default against 0.14 s with
`--no-xattr`. The 50 000-file figure above is worse than that for one reason: macOS 27 stamps
`com.apple.provenance` on every file a local process creates and will not let it be removed, so
in a tree this machine built itself the shortcut never fires and every file pays two
`listxattr(2)` plus two `getxattr(2)` (14 µs each — `getxattr`, not `listxattr`, is the
expensive one). Trees that came from anywhere else are the common case and the fast one.

And it is not only slower on small trees, it is less certain: `fseventsd` writes its journal on
a timer, so an isolated change takes 90–600 ms to become visible to a stream created after it (a
burst flushes at once). A `--events` diff run in the same breath as a single edit can miss that
edit; the walk always sees it. Exactness first, O(changes) when the tree is big enough for it to
pay — the threshold is `WFS_DIFF_EVENTS_MIN_ENTRIES` (200 000), overridable through the
environment variable of the same name.

Both paths read the source snapshot through its gate: the root is briefly reopened to `0500`
under an exclusive `flock` on the snapshot's manifest, and only for the comparison phase, so a
diff delays a concurrent `fork` from that same snapshot by the length of the comparison and not
by the length of the command.

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

Against the arch.md §1 success criteria: [`docs/M1_RESULTS.md`](docs/M1_RESULTS.md)
(`scripts/bench/m1_criteria.sh`, six sections, re-runnable one at a time with `--only N`).

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
core/src/snapshot_access.{h,cpp}      the one door into a snapshot's contents (the 0500 gate window)
core/src/pool.{h,cpp}                 the pre-clone pool: fill, claim, status, drain, gc, verify (T1.5)
core/src/events.h                     candidate collection interface + the path bag
core/src/platform_darwin_events.cpp   the FSEvents replay: flags, journal-age check, dedicated queue
core/src/platform_posix.cpp           parallel tree walk, manifest, free space, recursive delete
core/src/platform_darwin.cpp          clonefile, per-file fallback, chflags protect/unprotect, FSEvents cursor,
                                      getattrlistbulk enumeration + the EF_NO_XATTRS verdict
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
<store>/pool/S<n>/<uuid>/             pre-cloned worlds waiting to be handed out (T1.5)
<store>/logs/pool.log                 what the detached background fillers printed
```

Extension lifecycle and fskitd:

```bash
log stream --predicate 'subsystem == "world.forks.fs" OR process == "fskitd"' --style compact
```

If `mount` says "No extension with fsShortName found", check pkd: it silently rejects appexes that are not
sandboxed. If it says "disabled", enable WorldFS in System Settings (needed again after the bundle record changes).
