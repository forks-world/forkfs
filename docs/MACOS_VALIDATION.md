# macOS support and validation

The supported main path is the C++ CLI/core using ordinary APFS directories, with
`WFS_FSKIT=OFF`. M1 workspace lifecycle and M2 background GC, snapshot disposal,
reconciliation, and hardlink preservation are implemented. This describes a
development-workspace provider; passing the suites below is not a claim of
protection from hostile code, power loss, or every macOS/storage configuration.

## Environments and boundaries

| Area | Evidence or limitation |
| --- | --- |
| Automated environment | CI builds Release and runs tests on macOS 15 arm64; the workflow checks `uname -m` explicitly. |
| Local environment | Development and benchmark reports include Apple M1 on macOS 26.6.2 and 27.0. These are observations, not a complete compatibility matrix. |
| Filesystem | Native clonefile Worlds require compatible APFS storage. Source and destination must support cloning together; `--copy` is an explicit fallback, not the same performance guarantee. |
| Toolchain | CMake, Apple Command Line Tools, C++23, system SQLite, and checked-out submodules. Python 3 drives the disk-image test. Full Xcode is unnecessary for the default build. |
| Other configurations | Intel Macs, every macOS release, case-sensitive/encrypted/external APFS volumes, and network filesystems are not comprehensively covered by current CI. |
| FSKit | Optional frozen frontend. Default CI does not validate signing, extension activation, mounting, or its performance. |
| Distribution | The documented path is a source build. A signed installer, notarization, automatic updates, and a supported upgrade/rollback release matrix are not part of this validation. |

## Reproduce validation

Use the SDK/SQLite configure commands in the README's Continuous integration
section. Then run from the repository root:

```bash
ctest --test-dir build/ci --output-on-failure --timeout 600 --no-tests=error
scripts/check-deps.sh build/ci
validation_tmp="$(mktemp -d /private/tmp/forkfs-validation.XXXXXX)"
scripts/tests/safety.sh build/ci "$validation_tmp/m1test"
python3 -m unittest discover -s scripts/tests -p 'test_disk_full_wrapper.py'
python3 scripts/tests/disk_full.py build/ci
```

Check each command's exit status; in an automated shell use `set -euo pipefail`.
Run as an ordinary user. The safety script deletes its supplied scratch target
on startup, so give it a fresh private directory ending in `m1test`. Do not give
it a workspace or an existing directory containing useful data. If overriding
`TMPDIR`, create that directory first and retain a trailing `/`.

| Check | What it establishes |
| --- | --- |
| `core_test` | Store/world lifecycle, identity and metadata invariants, concurrent operations, and targeted crash/interleaving regressions. |
| `diff_test` | Exact diff results, metadata/xattr handling, event candidates and scan fallback behavior. |
| `check-deps.sh` | Built binaries link only allowed system libraries. |
| `safety.sh` | CLI behavior and refusal paths, snapshot protection, execution locks, pool/GC behavior, and recovery regressions. |
| `disk_full.py` | Actual low-space and kernel ENOSPC behavior within a bounded APFS image, preservation of existing data/store state, and recovery after freeing space. |
| `test_disk_full_wrapper.py` | Simulated partial-attach, timeout, unknown mount state, and detach-failure paths preserve mounted data and propagate errors without attaching a real image. |

Test counts in `M1_RESULTS.md` and older sections of `TASKS.md` are historical
measurements. Read current command summaries and CI logs for current totals;
do not use a fixed number of passing cases as an acceptance criterion. Every
registered test must run, and every suite must return success with zero failures.

## Real disk-full regression

`scripts/tests/disk_full.py` creates and mounts its own 512 MiB APFS disk image
under a private temporary directory. It checks host headroom and mount identity
before running `core/disk_full_test`. The binary is intentionally not a default
CTest entry: the wrapper supplies the bounded volume required for safe execution.
Do not invoke the binary against an arbitrary directory or physical volume.

The test exercises the 256 MiB-plus-entry-estimate admission check, then uses
the existing snapshot test hook to fill the image after that check has passed.
It exhausts data space and then metadata space using bounded directory creation:
APFS can still complete a small clone after data writes encounter `ENOSPC`.
It requires an actual `ENOSPC` from filesystem writes and an operation failure;
API errors may be mapped to a different code, so a generic failure alone is not
proof that the disk was full. After removing the filler it verifies existing
World and snapshot content, identity, database integrity, and successful new
checkpoint/fork operations. A failed operation must not publish a usable partial
snapshot or alter existing data. This does not promise rollback of arbitrary
application writes that partially succeed before ENOSPC.

The last phase reopens the store on a volume that is genuinely full, which is the
one condition that used to be reported as damage. `wfs_store_open()` must return
`-ENOSPC` and never `WFS_E_STORE_DAMAGED`, `metadata3.db` must be byte-identical
before and after that failed open, `world fs status` (the wrapper hands the test
the `world` binary from the same build for this) must exit `1` and must not
suggest `mv`-ing the store aside or restoring `metadata3.db`, and after the
filler is removed every snapshot and world row must still be there. Observed on
2026-09-21, macOS 27 arm64: SQLite `rc=10`, extended `4618`
(`SQLITE_IOERR_SHMOPEN` -- the store is WAL, so the `-shm` has to be written even
to read it), `sqlite3_system_errno()` `3` (`ESRCH`, no help), and `statfs(2)`
still reporting 11,247,616 bytes available on a volume where a 4 KiB write and a
`mkdir(2)` both failed with `ENOSPC`. These are observations, not fixed expected
thresholds; what the test pins down is the verdict, not the codes.

The wrapper detaches the image on success and failure. If detach fails, it leaves
the temporary directory in place and prints the location; it must not recursively
remove a directory while that image is mounted. Inspect the reported image and
mountpoint, detach that test image, and only then remove its temporary files.
An unsupported environment or failed image setup is an error, not a passing test.
CI retains its test log when a run fails.

Local validation on macOS 27 arm64 passed CTest (2/2), the safety suite
(298 cases), wrapper cleanup tests (12/12), dependency checks, and the real
disk-full regression. The latter observed data `ENOSPC` after 523,239,424 bytes
and metadata exhaustion after 996 directories; these are observations, not
fixed expected thresholds. Pass `safety.sh` a scratch path with no doubled
slash in it: `TMPDIR` as the CI recipe sets it ends in `/`, so `"$TMPDIR/m1test"`
makes one, and twelve cases that compare a printed path against the one they
passed in fail on the difference alone. The new disk-full CI step still requires its first
GitHub run; local success does not establish a macOS 15 result for this test.

## Isolation and performance limits

`world exec` protects the store and other Worlds from specified accidental
operations. Its profile starts with `(allow default)`; it is not a credential-free
or network-isolated sandbox for untrusted agents. Snapshot mode/flag protection
also does not stop the owner from deliberately changing permissions. Use a
separate suitable execution boundary for hostile code. `--require-sandbox` makes
the command refuse execution when the current sandbox cannot be applied; it does
not turn that sandbox into a stronger policy.

Sub-10 ms fork results refer to a prefilled pool hit in the reported benchmark.
Pool misses clone a directory tree, and each World consumes APFS metadata even
while data blocks are shared. `diff` normally scans the tree; FSEvents narrows
candidates when usable and falls back when it cannot establish completeness.
Background GC removes foreground deletion latency, not the physical cost of
eventually deleting every entry. See `M1_RESULTS.md` and the M2 task records for
the workloads and measurement conditions.

## Remaining release validation

- Whole-machine power loss and storage I/O failure are not established by the
  existing process-crash hooks or the disk-full test.
- Long-duration multi-agent stress, repeated low-space/recovery cycles, and
  large real repositories need environment-specific evidence beyond the short CI run.
- Expand OS, hardware, volume, and upgrade/rollback coverage before claiming a
  broad supported product matrix; keep that matrix tied to reproducible results.
- Keep FSKit validation separate from the default clonefile path. Linux/Windows
  implementation and generic merging of divergent Worlds are outside this macOS scope.
