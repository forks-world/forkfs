# GC tail latency and foreground backpressure — 2026-10-29

Observed development fs-save p99 fell from **39.788 ms** in the instrumented
baseline to **32.641 ms** after batching chunk protection, then **27.483 ms** with
foreground-aware pacing. All three unchanged 600-second workloads completed,
including pre-backup GC draining and independent base/final backup replay.
These are sequential single-run observations, not confidence intervals or
mounted FSKit acceptance.

## Workload and matching

Release AppleClang21, macOS27.0.1 arm64, Python3.9.6, warm OS caches; run order
baseline→batch→pressure. Seed20261028, files16×1024B, rounds10, workers2,
steps350, gc-work128, copy1MiB, objects128, sample0.5s, source256MiB,
artifacts1024MiB, deadline600s. Each run completed10 development rounds and one
backup-overlap round. All scripts/settings, foreground operation counts and
retained-model checkpoints match; binary changes are intentional and recorded.
GC polling/start counts vary with scheduling and are excluded from that count
match. Each save distribution has1000 development samples. The matched model
has3 Worlds,21 revisions,41920 unique payload bytes at sequence7378; final
sequence8114 retains23 revisions.

| Observation | Instrumented baseline | Batched protection | Plus pacing |
| --- | ---: | ---: | ---: |
| Code | `99b6e49` | `1372e21` | `4f9ce13` |
| Complete elapsed | 240.092 s | 230.740 s | 224.618 s |
| Save p50 | 11.097 ms | 10.569 ms | 9.103 ms |
| Save p95 | 25.050 ms | 23.675 ms | 22.048 ms |
| Save p99 | 39.788 ms | 32.641 ms | 27.483 ms |
| Save maximum | 171.307 ms | 114.874 ms | 149.145 ms |
| Cumulative GC barrier | 12.776 s | 7.679 s | 5.043 s |
| Cumulative foreground writer wait | 1.065 s | 1.038 s | 0.024 s |
| Maximum foreground transaction hold | 178.324 ms | 178.427 ms | 190.073 ms |
| Sampled source allocation peak | 127.09 MiB | 122.85 MiB | 138.27 MiB |
| Marker scratch apparent peak | 20.67 MiB | 19.90 MiB | 13.92 MiB |

P99 improved31% from baseline and16% from batching alone; complete elapsed
fell6.4%. This is not a worst-case bound: transaction hold still reached190ms,
and the paced run's barrier maximum was185.368ms. Source allocation peak rose
8.8% against baseline, below256MiB. These are sampled disk allocations, not
process RSS; category maxima need not coincide and must not be summed.

## Measurement and implementation

Bounded counters separate foreground writer wait/hold, GC sweep/relocation/
retirement wait/hold, and relocation read/DATA sync/metadata sync. Hold bands
are non-cumulative:≤1ms,1–2ms,2–5ms,5–10ms,10–20ms,20–50ms,>50ms.
Counters include entered failure scopes and cover one store owner; concurrent
multi-counter snapshots are not atomic. Sweep includes reference marking;
foreground lock timing covers transact, not all backup/lease/pin acquisition.
The baseline used one additional read-only mid-run profile RPC; other runs used
only the harness's final profile.

Foreground maximum writer wait was16.614ms but own hold reached178.324ms.
Relocation hold totaled58.232s, including8.119s reads and45.364s metadata
publication. Pure writer-lock priority therefore would not explain all tails.

Publication now validates and protects at most16384 chunk hashes with one
scratch WriteBatch/mutex acquisition, replacing per-chunk writes. Root barriers
remain in order; the main writer lock spans all protections. DATA and metadata
sync ordering is unchanged, and barrier failure still abandons the cycle.
Duplicate H marks are harmless. Temporary foreground vector/batch allocation
has a fixed bound; no unbounded dedup set is introduced. This staging is not
part of GC's metadata charge.

Foreground demand is registered via RAII before location-pin admission and
held through transaction cleanup after writer unlock. GC checks work boundaries
and tightens remaining mark/physical quanta to at most8, preserving cursors and
original ceilings. Late pressure cannot undo prior work, so pressure=true may
accompany more than8 already-completed units, within the original caller cap.
Idle calls restore original budgets. Synchronous collect_step never sleeps.
Only the async worker waits interruptibly1ms after a pressured step; stop sets
the predicate under the same mutex and notifies before join. Pressure-induced
no-progress is retryable within the existing job ceiling; quiet completion
still takes priority, including blocked replay-retention completion. The
worker continues eligible work under demand rather than disabling GC. Existing
pins can still defer pruning/retirement; collection completion is not guaranteed
under never-ending changes or pins.

Individual traversal, LevelDB stalls and filesystem sync remain indivisible.
This does not establish hard latency, CPU-share or process-RSS ceilings.

## Regression coverage

Marker tests cover duplicate/empty batches, invalid input without partial H
marks, size limits and protection during blocked reads. Demand gates hold a
transaction before publication, verify positive mark/index progress with≤8
units, then idle restoration and cleanup after invalid/conflicting requests.
A late-arrival gate introduces demand after16 units and checks ceilings,
continuation and data. Byte oracles, reopen, base backup and delta replay remain.
The8 targeted tests passed in72.43s after batching and69.26s after pacing,
including GC crash boundaries and three namespace seeds.

The final pressure binary passed all116 local Release CTest targets in571.58s
(105 metadata-labelled cases), including the new gates, crash/fault tests,
backup/replay, resource budgets and development harness. No remote CI or Linux
execution is claimed by these local results.

[Selected variant evidence](GC_TAIL_20261029.json) includes fingerprints,
distributions, timing bands, storage checkpoints and replay results. Raw traces
and exact harness snapshots remain at:

- `build/gc-tail-baseline/seed-20261028-ok0sgzyc`
- `build/gc-tail-batch/seed-20261028-1bl2kioe`
- `build/gc-tail-pressure/seed-20261028-3tw6idwu`

Earlier fixes and failed probes remain in the
[preceding investigation](GC_PUBLICATION_FIX_20261028.md).

## Final-binary no-GC control

The same pressure binary completed the no-GC control in178.315s. Strict comparison
accepted identical binary/script fingerprints, workload, successful operation
counts and retained model. Both complete backups and both replays passed.

| Development save,1000 samples per run | GC disabled | GC with pacing |
| --- | ---: | ---: |
| p50 | 8.575 ms | 9.103 ms |
| p95 | 14.969 ms | 22.048 ms |
| p99 | 16.670 ms | 27.483 ms |
| Complete workload elapsed | 178.315 s | 224.618 s |

GC still adds foreground tail cost (p99 about1.65× this no-GC control), while p50
overhead is about6.1%. This is one sequential warm-cache pair, not a statistical
SLO. No timeout, model oracle or retention requirement was relaxed.

[Strict paired result](GC_TAIL_PAIRED_20261029.json) preserves distributions and
storage checkpoints. Control raw artifacts and harness snapshots:
`build/gc-tail-control/seed-20261028-5t3nm7go`.
