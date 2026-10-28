# Mixed maintenance soak — 2026-10-28

The short correctness regressions passed. The long concurrent-GC profile **failed
its 600-second work budget**. It is preserved as a performance finding, not a
successful long-run or mounted-filesystem acceptance result.

Environment: macOS 27.0.1 arm64, Release core at `45fe0fa` (binary SHA-256
prefix `732b20e232d6`), Python 3.9.6. Two sequential
runs used seed 20261028, 16 source files, 10 development rounds, two workers,
350 steps per worker/round, `gc-work=128`, 1 MiB copy allowance, 128 objects per
step, 1,024 steps per background job, 0.5-second storage sampling, 256 MiB
per-repository and 1,024 MiB per-artifact limits. Caches were not flushed.
No latency threshold or timeout was relaxed after observing the failure.

| Observation | GC disabled | Concurrent GC |
| --- | ---: | ---: |
| Outcome | passed | failed at work deadline |
| Elapsed | 190.945 s | 600.020 s |
| Completed development generations | 10 | 7 |
| Completed additional backup generation | 1 | 0 |
| Development `fs-save` samples | 1,000 | 736 |
| Development `fs-save` p50 | 8.614 ms | 120.596 ms |
| Development `fs-save` p95 / p99 | 15.098 / 17.720 ms | insufficient samples |
| Development `fs-cat` samples | 7,420 | 5,434 |
| Development `fs-cat` p95 / p99 | 10.067 / 12.258 ms | 181.122 / 251.416 ms |

The concurrent trace is incomplete and covers fewer generations, so these rows
are observations, not a completed matched latency ratio. The comparison tool
correctly refuses the failed trace. Both successful short modes were separately
compared to exercise its matching/count checks and missing-tail behavior.

The disabled run verified its online base backup and complete final backup by
replay. The concurrent run did not reach the backup stage. Its already completed
generations passed the byte/namespace oracle. After timeout and graceful cleanup,
independent `fs-check` and `check` on the reopened source passed, with committed
sequence 5417 and no uncommitted tail. This does not establish a complete final
byte-model check for the interrupted generation or power-loss behavior.

## Bottleneck evidence

During the concurrent run, GC remained in marking at the observed epoch, with no
physical copy work in the captured status. An example observation around 219 s
had 350,907 processed mark tasks and epoch sequence 25. Foreground overlap with an
observed active GC job was recorded in every completed generation.

The [mid-run storage profile](DEVELOPER_SOAK_20261028_PROFILE.json) recorded:

- 2,778 transactions; transaction publication time 209.888 s.
- Main WAL sync time 13.958 s; metadata preparation time 0.719 s.
- Approximately 196 s of publication time is therefore not explained by that WAL
  sync metric. DATA preparation/fsync and the GC insertion barrier are not split
  by the existing metrics, so attributing the remainder solely to a lock would
  be unsupported.

A concrete contention path exists: marker advancement holds `GcMark::mu_` while
processing tasks, reading objects and updating scratch. Foreground publication
calls `seed_root`/`protect_chunk` through the same mutex while retaining its writer
lock. The next investigation should split DATA preparation/sync, main metadata
write, publication-barrier work, and marker mutex wait/hold time before selecting
a fix. The physical reverse-index optimization alone does not address this path.

Sampled logical-marker scratch peaked at 73,436,987 apparent bytes (70.04 MiB),
with 59,751 apparent bytes of physical-index scratch. Category maxima need not
occur in the same sample and cannot be added to infer a device peak. Temporary
mark work is a material part of the observed space cost.

## Validation and artifacts

Seven developer-workload CTest targets passed (including 22 negative/oracle unit
cases, original workload seeds 42/173, paired maintenance smoke and native Git/C
build smoke) in the final 36.43-second run. The final short maintenance inputs
were concurrent 24.65 s and disabled 1.26 s; native smoke took 4.57 s. Earlier short
runs varied with scheduling. CI exercises functional maintenance/recovery; it does
not mark the failed long profile as performance-ready.

Raw artifacts are retained locally under:

- `build/developer-soak-paired/seed-20261028-14ieqf2_`: successful disabled run.
- `build/developer-soak-paired/seed-20261028-ffnl3tpk`: failed concurrent run,
  including operation/job/storage traces, summary, mid-run profile and post-timeout
  check. SHA-256-verified copies of both measured harness sources are retained in
  each run's `harness-snapshot/`; final cleanup error reporting/source snapshot
  capture was subsequently improved without changing the measured workload. Failure artifacts were not deleted.
- `build/gc-budget/developer-workload-artifacts/`: CI-shaped regressions and the
  successful short-run comparison. This directory is covered by existing CI
  artifact uploads.

The native-only driver completed 20 rounds of Git edits/commits and full plus
incremental C compilation/link/execution. Native p50 samples were 75.23 ms for the
Git phase, 406.52 ms for full build/run and 389.11 ms for incremental build/run.
Each bucket had 20 samples, so native p95/p99 were intentionally not emitted.
These small-project command timings do not isolate filesystem overhead. This machine had zero valid code-signing identities and no
mounted forkfs volume. No mounted Git/compiler result or agent-isolation acceptance
is claimed by this run; the driver accepts a provisioned World for that later
validation.
