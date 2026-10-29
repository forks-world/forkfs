# Marker scratch memtable and foreground stalls — 2026-10-30

PR #78 passed all remote CI jobs and merged. This follow-up addresses its
remaining foreground writer-hold outliers without changing the durable format,
DATA-before-metadata ordering, publication fence, or GC safety protocol.

## Finding and change

New nested timing counters separate root protection, committed descriptor
lookup/decoding, and batched chunk protection inside the publication barrier.
With the existing 64 KiB requested marker memtable, the first complete probe
reproduced a 187.226 ms writer hold and a 182.680 ms publication barrier; one root
protection call took 179.726 ms. A subsequent 64 KiB confirmation reproduced
204.375 ms writer hold and 199.549 ms barrier; a chunk protection call also
reached 186.977 ms. Both marker insertion paths can suffer long waits or scratch
writes; descriptor decoding was below 0.56 ms in these runs.

The marker scratch write-buffer request is now 1 MiB. The paired result is
consistent with reducing scratch flush/compaction pressure on a path that holds
the main writer fence. These aggregate counters do not distinguish scheduler,
mutex, and LevelDB compaction time inside each call, so this is not a proof of
every individual pause's cause. Scratch writes remain unsynced and scratch is
still discarded on restart; no user data durability is weakened.

## Controlled comparison

The candidate ran before the final 64 KiB confirmation, sequentially with no
concurrent build or second benchmark. Both used seed 20261028, 16 files of 1024
bytes, 10 rounds, 2 workers, 350 steps, GC work 128, copy budget 1 MiB, object
budget 128, and a 256 MiB storage ceiling. Each produced 1000 development save
samples. Harness/Python fingerprints, workload settings, revision counts and
retained models match. Both passed their byte/model, reopen, complete backup and
base/final replay oracles. Caches remained warm and were not flushed.

| Metric | 64 KiB confirmation | 1 MiB candidate |
| --- | ---: | ---: |
| Total elapsed | 237.800 s | 220.207 s |
| Save p50 | 10.438 ms | 8.748 ms |
| Save p95 | 22.787 ms | 16.521 ms |
| Save p99 | 27.882 ms | 21.096 ms |
| Maximum writer hold | 204.375 ms | 20.458 ms |
| Maximum publication barrier | 199.549 ms | 10.833 ms |
| Writer holds above 50 ms | 16 | 0 |
| Sampled repository disk peak | 135.984 MiB | 131.203 MiB |

The observed save p99 reduction is 24.3%; elapsed reduction is 7.4%. These are
single runs, not a statistically established speedup or maximum-latency promise.
The first baseline (245.601 s, save p99 28.030 ms) overlapped candidate compilation
and is retained as diagnostic evidence only, not the controlled comparator.

## Costs and limits

LevelDB can retain both mutable and immutable memtables, plus allocation and
iterator overhead. Raising the request increases potential resident scratch
memory; process RSS was not measured. This memory remains outside the per-step
GC metadata charge. The physical reverse-index scratch DB retains its own 64 KiB
request. Disk peaks are sampled rather than exact, and scratch indexes can grow
with the retained graph. Each run retained the same 3 modeled Worlds, 21
revisions and 41,920 reachable payload bytes at the pre-backup checkpoint.

This is a synthetic resident RPC workload, not mounted Git/build acceptance.
Real mounted workloads, repeated seeds and strict latency/RSS bounds remain
separate work. The evidence includes fingerprints and raw artifact paths in
[GC_MARK_MEMTABLE_20261030.json](GC_MARK_MEMTABLE_20261030.json).

## Regression coverage

The online GC regression checks the new nested counters with GC absent, a new
object publication, immutable-reference reuse and a transaction after collection
cancellation. It checks nested duration accounting and final object bytes without
machine-dependent timing thresholds. The existing GC budget, concurrent-reader,
fault/crash and seeded model regressions exercise the larger marker buffer.

Validation: the Release build passed all 24 selected CTest cases in 490.69 s:

```
ctest --test-dir build/foreground-1m -R 'gc|replay_core|object_store_recovery|save$|lock_granularity' --output-on-failure
```

The cases are already part of the existing CI; the new counter assertions extend
the registered `forkfsd_online_gc` test. No new remote CI execution is claimed
for this follow-up.
