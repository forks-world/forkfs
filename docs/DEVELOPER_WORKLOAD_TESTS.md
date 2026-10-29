# Development regression and stress workload

`python3 scripts/tests/developer_workload.py BINARY --output ARTIFACT_DIRECTORY`
runs a seeded, stateful development workload against a resident `forkfsd`. Every
invocation creates a new private repository and retains its evidence, including on
failure. The script does not mount, format, or delete user directories. It uses
Python's standard library and argument-vector subprocess calls, without a shell.

## Coverage and oracle

- Checkout: unique source files (default mixed 1 KiB/4 KiB), headers, configuration
  and build directories. `--file-bytes` controls the base source size.
- Sixteen forks of one revision, with a segment-growth budget to catch tree copies.
- Concurrent clients on isolated Worlds, repeatedly restoring a common base.
- Atomic editor saves, random offset writes including holes, append, truncate,
  rename over tracked files, recreation, chmod and invalid operations.
- Dependency hashing and synthetic 72 KiB build outputs, repeated build/clean,
  lock-file churn and temporary directory creation/deletion.
- Hardlink identity/link counts, symlink resolution, open-after-unlink, concurrent
  descriptor locks and lock release on close.
- A revision after each client round, checked against independent Python byte and
  namespace models. Directory membership, type, mode, size and complete file bytes
  are checked. Source revisions and all retained branch snapshots remain isolated.
- SIGKILL at barriers after acknowledged writes; every modeled World/revision is
  checked after reopening. An additional kill races a 128 KiB atomic save: an
  acknowledged request must survive; an unacknowledged request may recover the
  exact old or new bytes, never a mixture. Scheduler outcomes are recorded.
- Final backup/replay into a separate repository and complete model verification.
- A read-only phase must neither append segment bytes nor advance the sequence.

The build output is synthetic and determined by source hashes. These are storage
requests that resemble a development workload; this is not a measurement of a real
Git process or compiler operating on an FSKit/FUSE mount. Use the mounted workload
in `scripts/bench/mounted_fs.py` for real mounted Git/file operations. Process
crashes do not establish physical power-loss behavior. Deterministic injected
storage faults and archive-corruption cases remain covered by their existing
regression suites.

## Running

Short regression (two seeds are also registered with CTest):

```sh
python3 scripts/tests/developer_workload.py build/metadata/daemon/forkfsd \
  --output build/developer-results --seed 173 \
  --files 32 --rounds 3 --workers 2 --steps 7 --max-store-mib 64
ctest --test-dir build/metadata -L developer-workload --output-on-failure
```

A larger stress run:

```sh
python3 scripts/tests/developer_workload.py build/metadata/daemon/forkfsd \
  --output build/developer-stress-results --seed 20261018 \
  --files 512 --rounds 20 --workers 4 --steps 21 \
  --restart-every 2 --max-store-mib 512
```

Every run retains `settings.json`, `operations.jsonl`, `daemon.log`,
`summary.json`, `storage.json`, repository, backup and restored repository.
The small private socket directory is removed on exit. Re-run the saved settings
against the same binary to reproduce the input seed; concurrent interleavings and
crash timing remain scheduling-dependent. Failed artifacts are not automatically
removed. Output directories should be on a volume with space for the repository,
its backup, the restored repository and traces; the configured store budget applies
to each repository, not the sum of all artifacts.

## Latency and space interpretation

`summary.json` reports count, p50 and max for successful RPCs per operation and per
workload phase. Rejected and interrupted requests are counted separately.
P95 and p99 are only emitted with at least 1,000 samples per bucket. RPC samples include socket setup,
queueing, daemon execution and the normal durability boundary. Caches are not
flushed; reopening resets daemon caches. Latencies are reported, not gated against
hardware-independent millisecond thresholds. CI correctness failures fail the run.

Each storage checkpoint reports apparent file bytes, allocated `st_blocks * 512`,
segment/main-index/control bytes, logical/physical GC scratch bytes, object count, sequence, logical live-main bytes, unique payload
bytes reachable from modeled Worlds/revisions, growth and bytes per commit. APFS
sharing/compression can make `st_blocks` differ from physical device usage.

The script enforces an absolute allocated-byte budget per repository, a fork
segment-growth budget (8 KiB per probe fork), and a deliberately generous per-round
segment budget (submitted payload bytes plus 256 KiB per mutation request and
1 MiB slack). The latter detects gross copying, not small amplification regressions.
Read-only segment growth is rejected exactly. Detailed growth data should be used
to set tighter workload-specific budgets once multi-platform baselines are available.

The original development regression does not start online GC. Immutable objects,
historical revisions, obsolete metadata and logs can therefore remain retained.
Online reclamation is implemented separately; see [ONLINE_GC.md](ONLINE_GC.md) and
the mixed maintenance soak below. Retained revisions and replay logs remain live
by policy, so a small current workspace does not imply flat backing storage.
Backup/replay size is reported separately and does not imply GC of the source.

## CI

The normal metadata regression matrix runs seeds 42 and 173 on Linux and macOS,
and uploads artifacts on success and failure. Oracle unit tests deliberately feed
wrong content, missing/extra directory entries and invalid metadata to prove the
checker rejects them.

`.github/workflows/developer-stress.yml` provides manual and weekly runs on Linux
and macOS with two seeds, 512 files, 20 rounds and four clients. GitHub schedules
only activate when the workflow exists on the default branch. It retains evidence
for seven days; it does not claim to have passed until those jobs run.


## Mixed maintenance soak

`developer_soak.py` reuses the independent byte/namespace model and seeded worker
from the original regression. Each worker owns a separate World, restores the
common base, performs editor saves/range writes/append/truncate/rename/chmod,
hashes dependencies, writes/verifies/removes synthetic build products, and creates
a revision. All named revisions remain retained: there is no revision-pruning API
in this workload, and history growth is accounted for rather than treated as a
leak. This mode uses graceful shutdown and recovery; crash injection remains in
the original regression and storage-fault suites.

Concurrent mode supervises the background collector during producer rounds,
restarting completed/budget-exhausted/blocked bounded jobs without `gc-stop`.
After producers stop, it requires complete logical and physical convergence before
backup. It records storage immediately before and after that drain, its duration,
and the observed signed net DATA-byte recovery rate (not device bandwidth).

An online base backup then runs against a further producer generation. The
captured base revision is checked by replay into a separate repository/daemon
without changing the source daemon endpoint while producers are running. Backup
enables replay retention: subsequent GC is explicitly reported as logical-sweep
skipped, and its physical continuation must still drain. Finally a quiescent full
backup is replayed and **all** modeled Worlds/revisions are checked. Every daemon
startup and subprocess has a deadline; monitors propagate failures to the next
foreground operation. Cleanup may take additional time after the work deadline.

Short runs (also registered with CTest for concurrent and disabled modes):

```sh
python3 scripts/tests/developer_soak.py build/metadata/daemon/forkfsd \
  --output build/developer-workload-artifacts/soak --seed 173 \
  --files 4 --rounds 1 --workers 1 --steps 7 --gc-work 256 \
  --gc-mode concurrent --max-store-mib 64 --deadline 120
ctest --test-dir build/metadata -L developer-workload --output-on-failure
```

A successful matched stress run accumulates 1,000 `fs-save` samples in the development phase
per mode. Run the modes sequentially on the same machine and binary; caches are
not flushed, and these are diagnostic comparisons rather than causal/SLO proof:

```sh
python3 scripts/tests/developer_soak.py build/metadata/daemon/forkfsd \
  --output build/soak-pairs --seed 20261028 --files 16 --rounds 10 \
  --workers 2 --steps 350 --gc-work 128 --max-store-mib 256 \
  --max-artifact-mib 1024 --gc-mode disabled --deadline 600
python3 scripts/tests/developer_soak.py build/metadata/daemon/forkfsd \
  --output build/soak-pairs --seed 20261028 --files 16 --rounds 10 \
  --workers 2 --steps 350 --gc-work 128 --max-store-mib 256 \
  --max-artifact-mib 1024 --gc-mode concurrent --deadline 600
python3 scripts/bench/compare_developer_soak.py \
  build/soak-pairs/CONTROL/summary.json build/soak-pairs/CONCURRENT/summary.json
```

`--seconds` optionally caps the development phase in addition to `--rounds`;
`--deadline` caps the entire run. A duration cap can produce different generation
counts, which the comparison helper rejects. The helper also checks the binary
and harness hashes, Python version, seed/shape, successful operation counts, and
retained byte-model sizes before comparing development-phase latency. Failures
and rejected operations remain separate from successful RPC timing samples.

Each run captures SHA-256-identified harness sources in `harness-snapshot/` along
with the binary hash and settings. In addition to existing operation traces and
checkpoint storage:

- `jobs.jsonl` retains status and observed job lifetimes. Completed GC steps per
  first-observed terminal job time are reported; last-step copy counts are not
  extrapolated into a total byte throughput.
- `storage-samples.jsonl` classifies DATA, main LevelDB, control files, logical-mark
  scratch, physical-index scratch, and all artifact files (including backups and
  replay copies). It tolerates only disappearance races and never follows links.
  Sampled maxima are labeled as sampled, not exact transient/device peaks.
- GC/backup running observations are intersected with actual client transport
  intervals from foreground worker threads. `overlap_observed=false` can mean a
  short overlap was missed; a positive observation is not a proof of backend CPU
  contention. Status polling and trace recording themselves add load.
- The source and each replay copy enforce `--max-store-mib`; the whole artifact
  tree enforces `--max-artifact-mib` (default eight times the store limit), with
  synchronous checks after replay and before/after the final report. These are
  sample-and-stop guards, not filesystem quotas: an in-flight operation can
  overshoot. Failure artifacts are retained for inspection.
- `--max-rpcs` defaults to one million and stops further requests if the recording
  budget is exhausted. Client latency/job collections use O(recorded operations)
  memory within that request budget; this is not a process-RSS ceiling. Raw traces
  are retained independently on disk.

## Real Git/compiler fixtures

The companion driver runs actual Git status/diff/add/commit, atomic source saves,
full compilation/link/execution of a fixed four-object C program, and incremental
source edit/one-object compilation/link/execution. Program output is checked after
every build. Git configuration and identity are hermetic; generated build outputs
are ignored. Compiler/version, raw durations, per-round workspace storage,
sampled peaks, failures and cleanup outcomes are retained.

Native-only control (used by CI; this establishes no mounted result):

```sh
mkdir -p build/native-fixtures build/native-results
python3 scripts/bench/developer_mount_soak.py \
  --native build/native-fixtures --output build/native-results --rounds 20
```

For a provisioned writable World, add `--world /Volumes/ForkfsWorld`. The driver
requires a mount point on a device distinct from the native root and creates only
its own UUID/marker-protected fixture. Those checks do not certify the filesystem
driver or agent isolation. The driver does not install/sign/mount anything.
Workspace allocated bytes are not ObjectStore/device usage; use the RPC storage
sampler for backing-store costs. At least 1,000 rounds are needed to report native
p95/p99 per operation. The baseline and incremental inputs are identical on both
sides and execution order is seeded/alternated.

The short native test, paired RPC smoke tests and negative oracle tests are in
`metadata-regression`/`developer-workload`. Soak tests run serially within CTest to
avoid imposing their background load on unrelated timeout-sensitive regressions.
All artifacts live under the existing developer-workload CI upload directory.

The first local mixed run and its failed high-load GC profile are recorded in
[evidence/DEVELOPER_SOAK_20261028.md](evidence/DEVELOPER_SOAK_20261028.md). The long
profile exposed publication-path/marking overhead and did not meet its configured
work deadline; passing smoke tests do not override that result. Storage category
maxima are independent sampled maxima and must not be summed as a co-timed peak.
