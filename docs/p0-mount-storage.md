# P0 mount, workload acceptance and storage lifecycle

The implementation is local first. LevelDB indexes metadata; immutable objects live
in DATA segment files. A filesystem mount, rather than the backing directory, is
the application interface. No remote acknowledgement is required by this work.

## Mount and access boundary

The optional `ForkfsRevision.app` now requests a mount through FSClient on macOS
27+ when built with a macOS 27+ SDK. SDK 26 builds retain a disabled mount entry.
Select a repository containing exactly one existing FSKit World/revision
marker. A signed, provisioned and enabled extension is required. The host retains
the user-selected read/write security scope until it exits or changes selection.
The extension owns the engine; another daemon cannot own the repository at once.

Run the read-only preflight:

```
python3 scripts/fskit/mount_probe.py --app build/p0/ForkfsRevision.app
```

Use the actual app path emitted by the build. An unsigned build is not installable
proof. `--mount PATH` separately checks kernel statfs: FS short name
`forkrevision`, FSKit flag, exact mount root and writable status must all match.
Neither a directory with a marker nor a child directory of a mount passes.

For a local workload process tree:

```
python3 scripts/fskit/restricted_workload.py --mount MOUNT --backing STORE -- TOOL ARGS
```

This launcher uses a deny-by-default macOS sandbox. Mount and private temporary
storage are writable; system/toolchain directories are readable. Additional
read-only toolchains require explicit `--read-only PATH` grants. Overlapping
backing grants are refused. At launch, the harness walks the backing tree and
rejects symlinks, special files, and any non-directory inode with a link count
other than one. This catches hardlink aliases hidden anywhere in an allowed
read-only toolchain. It also requires the real `OWNERSHIP` file to be a
single-link regular file whose nonblocking exclusive `flock` reports contention
from another process; an unlocked lease is rejected. This is a liveness check,
not proof of the lock holder's identity or its association with the mounted volume.

`close_fds` closes extra descriptors, while inherited standard streams are
checked separately. Socket and unsupported stdio types are rejected. Pipes,
terminals, and `/dev/null` are allowed; regular files are allowed only when
their resolved path is outside backing and their device/inode identity is absent
from the backing inventory.
Concurrent same-UID host changes can invalidate these startup checks, and the
harness does not prevent backing replacement during the mount lifetime. This
admission check therefore depends on the trusted-host assumption below.
Network access and arbitrary Mach service access are not granted. Some toolchains
will need further scoped permissions; failures must remain failures, without an
unrestricted retry. The sandbox is tested with real denied reads/writes on macOS.

This is **not isolation from other processes owned by the host user**. An agent
launched outside this sandbox still has the host user's access. Global protection
requires an independently enforced launcher/service identity boundary. The
FSKit host's own app sandbox does not sandbox arbitrary application clients.

## Mounted acceptance

```
python3 scripts/tests/mounted_acceptance.py --mount MOUNT --native NATIVE_DIR --output RESULT_DIR
```

The harness refuses native directories before creating workload files. Its native
reference must be on a different filesystem device; results live outside both
workspaces. It verifies mmap/flush/fsync contents, old descriptors after replacement,
hardlinks, truncate/regrow zeroes and cross-process flock conflict errno. It runs
Git commits/checkout and a C build/executable, then records raw stat and 4 KiB
save/fsync/rename durations. Default 1000 samples provide p50/p95/p99; smaller runs
omit tail percentiles. OS caches are not dropped. Rename does not include a parent
directory fsync and is not a crash-durable editor-save assertion.

Native runs of the workload functions test the harness only. They are not forkfs
performance measurements. Real mount cache coherency, dirty buffer writeback,
permissions, large project item limits and mount latency remain acceptance gates.
The current adapter retains at most 4096 items and is not yet approved for large
repositories.

## Offline GC

```
forkfsd gc-plan STORE
forkfsd gc STORE
forkfsd gc STORE --checkpoint NEW_BASELINE_ARCHIVE
```

Run GC as the repository owner; another UID, including root, is refused before
the repository is opened. The atomic exchange preserves the repository directory's
POSIX mode and group ID; preservation of ACLs is not claimed. GC exclusively owns
the repository. It validates existing objects and the recovery
graph before following typed World, revision DAG, namespace and sparse-content
references. Unknown inode codecs/flags fail closed. Generic object roots retain
their payload without guessing a codec from its bytes. Staging aliases are retained
only for live objects. The new generation is copied, synced, reopened and verified
before an atomic directory exchange. A shared ownership lease inode spans the
DB-close/exchange interval, preventing another engine from opening that gap.

The original repository path always names a complete generation. A process crash
can leave a sibling `STORE.gc-*` directory. The next GC removes bound preparation and completed leftovers
only when their repository/path marker and source ownership inode match, before
checking free-space admission. Unbound or legacy preparation directories are
retained conservatively for manual inspection. This
is offline collection, not background incremental GC. Repacking requires temporary
space for another live copy. Process-crash tests do not establish power-loss safety
of every host filesystem, firmware or device.

If replay logging is enabled, default GC pins all indexed objects and preserves
logs, root aliases and counters so existing backup baselines/deltas remain usable.
It may therefore reclaim only unindexed tails. `--checkpoint` explicitly starts a
new replay generation. GC first creates a durable baseline inside the staged
generation, exchanges directories, syncs the parent, and then publishes the
external baseline without replacing an existing file. A checksummed pending
receipt blocks ordinary repository opens until publication completes. After an
interruption, retry the same `gc STORE --checkpoint ARCHIVE` command; a matching
existing archive is validated and reused, while conflicting user data is never
overwritten. A completion receipt makes retries idempotent within that retained
replay generation, including after subsequent writes. `gc-plan` never resumes
publication. The internal baseline is removed only after durable completion.
Existing external backups remain independently recoverable, but new deltas require
the new baseline. Provision it at backup destinations before relying on the new
incremental chain. GC does not automatically prune revisions or reuse inode numbers.

## Capacity admission

```
forkfsd capacity STORE
forkfsd capacity STORE MAX_DATA_BYTES MIN_HOST_FREE_BYTES
```

Zero disables the corresponding bound. CAPACITY is a checksummed, synced control
file. Writes preflight unique new segment chunks before append; rejection returns
ENOSPC without changing committed metadata. Deduplicated chunks are not charged
again. Segment appends recheck the budget; actual I/O failures retain the existing
fail-closed recovery behavior. FSKit statistics now expose available capacity
bounded by the policy and host free space rather than all-zero statistics.

This is a DATA segment admission budget, **not a kernel-enforced repository quota**.
LevelDB, logs, control files and temporary GC generations consume additional space.
Host free space can change concurrently; a reserve is not a reservation. Configure
policy while the repository is offline. GC applies the policy before opening its
staging store, checks free-space admission for internal and external baseline
copies, and preserves backup-task/FSKit control files. Existing data above a newly configured cap is retained; additional
allocation is rejected until collection or a policy change makes room.

## Validation status

The implementation has regression tests for collection at five process-crash
barriers, retained revision/fork contents, replay compatibility, generation reset,
quota rejection and policy corruption. Harness tests cover fake-mount refusal,
native oracle behavior, actual sandbox denial and overlapping grants. Both suites
use the metadata-regression label in CI.

On the current development machine, no valid signing identity or registered
extension is available. The app builds, but no actual FSKit mount, mounted workload
percentile, mounted permission boundary or 300 ms mount target has been measured.
These three P0s are therefore not all closed by this change.
