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
P95 is only emitted with at least 1,000 samples. RPC samples include socket setup,
queueing, daemon execution and the normal durability boundary. Caches are not
flushed; reopening resets daemon caches. Latencies are reported, not gated against
hardware-independent millisecond thresholds. CI correctness failures fail the run.

Each storage checkpoint reports apparent file bytes, allocated `st_blocks * 512`,
segment/index bytes, object count, sequence, logical live-main bytes, unique payload
bytes reachable from modeled Worlds/revisions, growth and bytes per commit. APFS
sharing/compression can make `st_blocks` differ from physical device usage.

The script enforces an absolute allocated-byte budget per repository, a fork
segment-growth budget (8 KiB per probe fork), and a deliberately generous per-round
segment budget (submitted payload bytes plus 256 KiB per mutation request and
1 MiB slack). The latter detects gross copying, not small amplification regressions.
Read-only segment growth is rejected exactly. Detailed growth data should be used
to set tighter workload-specific budgets once multi-platform baselines are available.

Repeated build/clean and World restore do **not** currently promise physical
reclamation. Immutable objects, historical revisions, obsolete metadata and logs
can remain retained; ObjectStore GC is not implemented. A small live workspace
with substantial backing storage is possible and must be reported, rather than
mistaken for a flat-storage guarantee. Backup/replay size is reported separately
and does not imply GC of the source.

## CI

The normal metadata regression matrix runs seeds 42 and 173 on Linux and macOS,
and uploads artifacts on success and failure. Oracle unit tests deliberately feed
wrong content, missing/extra directory entries and invalid metadata to prove the
checker rejects them.

`.github/workflows/developer-stress.yml` provides manual and weekly runs on Linux
and macOS with two seeds, 512 files, 20 rounds and four clients. GitHub schedules
only activate when the workflow exists on the default branch. It retains evidence
for seven days; it does not claim to have passed until those jobs run.
