# GC publication contention investigation — 2026-10-28

Final code at `d591394` **passed the unchanged 600-second workload in 248.769 s**:
10 development generations, one backup-overlap generation, online base and final
backup replay, and the byte/namespace/revision oracles. Earlier failed attempts
are retained below, including two correctness/lifecycle bugs exposed only after
logical marking became fast enough to finish.

## Accepted concurrent run

Same parameters as the original failed profile: seed 20261028, 16 files of 1,024
bytes, 10 rounds, two workers, 350 steps each, mark-work128, copy1MiB,
objects128, storage sampling0.5s, per-source256MiB, per-artifact1024MiB,
deadline600s. Release AppleClang21, macOS27.0.1 arm64, Python3.9.6, warm OS
caches. No deadline, workload count or oracle was weakened.

- `fs-save`, development phase only: 1,000 samples; p50 **11.287 ms**, p95
  **26.144 ms**, p99 **47.419 ms**. Tail latency remains a maintenance cost.
- Source sampled allocated peak **137,490,432 bytes (131.12 MiB)**. Marker scratch
  apparent peak20,794,366 bytes; physical-index scratch21,637,771 bytes. Category
  maxima need not coincide; they must not be summed into a device peak.
- Pre-backup draining took95.339s. Segments shrank34,159,089→878,032 bytes;
  allocated source shrank109,977,600→47,185,920 bytes at these checkpoints.
  Final source allocation was23,535,616 bytes after the extra backup generation.
  Retained revisions grew to23, so this is not a flat-space guarantee.
- Both `base-replayed` and `final-replayed` passed their independent models.
  Replay-retained GC stopped as `blocked` after363 steps, with physical/mark
  pending false and pending_segments0. Logical sweep was explicitly skipped;
  this reports physical completion, not full logical reclamation under retention.
- Final storage profile:8,114 transactions,62.821s cumulative publication,
  DATA preparation9.513s, DATA sync1.473s, metadata write38.146s, barrier13.660s;
  marker mutex hold46.740s. Phase counts include one metadata-only replay-enable
  publication, so they are not expected to equal transaction counts.

[Accepted raw evidence](GC_FIX_ACCEPTED_20261028.json) retains exact binary and
harness fingerprints, counters, model checkpoints and backup/replay evidence.
Binary SHA-256: `6faf28add26d29b166bde7dd31b656c392d550aef6089189f7aea038604ca66f`.
Full artifacts: `build/developer-soak-gc-complete/seed-20261028-3drrqez7`.
No extra diagnostic RPC was added during this measured run. This is synthetic
resident Unix RPC evidence; Linux, remote CI and signed mounted FSKit/Git/compiler
performance are not claimed.

## Matched no-GC control

The same final binary and exact harness fingerprints completed a disabled-GC
control in **179.336 s**. The strict comparison tool accepted matching settings,
actual operation/generation counts and retained model. Both runs completed all
backup/replay checks. Order was concurrent then disabled, sequentially on the
same host with warm OS caches; this is one pair, not a repeated statistical
estimate or mounted performance result.

| Observation | GC disabled | Concurrent GC |
| --- | ---: | ---: |
| Complete workload elapsed | 179.336 s | 248.769 s |
| Development fs-save samples | 1,000 | 1,000 |
| fs-save p50 | 8.114 ms | 11.287 ms |
| fs-save p95 | 14.897 ms | 26.144 ms |
| fs-save p99 | 18.265 ms | 47.419 ms |
| Source allocated at matched development checkpoint | 73,728,000 B | 47,185,920 B |
| Segments at that checkpoint | 30,586,339 B | 878,032 B |
| Sampled source allocated peak | 133,025,792 B | 137,490,432 B |
| Online base archive | 61,306,360 B | 2,134,251 B |

The matched checkpoint retains21 revisions and the same41,920 modeled unique
payload bytes in three Worlds. The concurrent checkpoint follows logical/physical
GC draining, so its reclamation time is included in complete workload elapsed.
GC reduced stored garbage and backup size, but added temporary scratch space and
measurable foreground tail cost: p50 +39.1%, p95 +75.5%, p99 2.60×. No claim of
maintenance-free latency or flat-space growth is made.

[Strict paired comparison](GC_FIX_PAIRED_20261028.json) preserves full distributions,
matching fields and storage checkpoints. Control artifacts:
`build/developer-soak-gc-control/seed-20261028-qlal68bg`.

## Earlier attempts

The original long mixed-maintenance profile failed at 600 seconds with only
7/10 development generations complete. The publication-lock fix completed all
10 development generations, but **also failed the unchanged 600-second deadline**
while draining logical marking. Neither failed run is a full backup/replay or
performance acceptance result.

## Changes

`GcMark::advance` now serializes advancement separately, retains the selected Q
row, and performs immutable object reads/interpretation outside the publication
mutex. It commits semantic records, staged children, dedup and queue advancement
atomically after reacquiring that mutex. Foreground seeds never access staging.
Encoded children are capped at 65 and freed as the commit batch grows.

Cumulative publication phase and marker mutex counters expose the remaining
cost. Their shared lifetime spans GC completion/cancellation in one store owner.
The harness saves a final storage profile on successful completion.

A second experiment added a 10-bit Bloom filter to scratch LevelDB. It did not
produce a material improvement in this workload and was removed. The final
implementation instead coalesces publication-origin mutable World roots in a
disk-backed P row per World key, preserving baseline, revision and lease tasks
independently. A selected P value stays visible during processing and is deleted
only when its value still matches, so concurrent updates cannot be lost.

## Lock-scope-only run

Same machine and workload parameters as
[the original profile](DEVELOPER_SOAK_20261028.md): seed 20261028, files16,
rounds10, workers2, steps350, gc-work128, copy1MiB, objects128, sample0.5s,
store256MiB, artifacts1024MiB, deadline600s. Release AppleClang21,
macOS27.0.1 arm64, Python3.9.6; OS caches not flushed.
Binary SHA-256: `a59e6fef4fdd677d8b6de68063675f53ce18fab37e057039256d3f55b00e1400`.

- Failed at600.020s, all10 development rounds completed; no backup generation.
- `fs-save`:1000 samples, p50 10.629ms, p95 25.143ms, p99 30.332ms.
  The old failed trace had736 samples and p50 120.596ms over fewer generations;
  this is an observational improvement, not a completed matched ratio.
- `fs-cat`:10120 samples, p99 18.941ms; this includes verification traffic.
- At599.83s,474435 mark tasks processed, still the original epoch sequence25.
  No logical/physical reclamation was reached in that observation.
- Sampled marker scratch peak88,976,751 apparent bytes. Total sampled allocated
  source peak163,180,544 bytes, below the unchanged256MiB limit. Category maxima
  are not necessarily simultaneous.
- A pre-deadline profile had7378 transactions:55.541s cumulative publication,
  DATA preparation5.091s, DATA sync0.740s, metadata write32.672s, GC barrier17.021s.
  Seed mutex wait10.377s and hold6.140s; marker mutex hold285.183s. Seed metrics
  include collector-origin seeds. These sums are not latency percentiles.
- Post-timeout graceful cleanup and independent reopened `fs-check` passed at
  sequence7378, no uncommitted tail. This is not a power-loss result or completed
  final backup/replay verification.

[Selected raw evidence](GC_PUBLICATION_FIX_20261028.json) includes counters and the
last GC observation. Full artifacts remain in
`build/developer-soak-gc-fix/seed-20261028-0dvadwoj`, including exact harness
snapshots. Three additional read-only diagnostic RPCs captured two profiles and
one GC status; these are not included in the harness operation counts.

## Bloom-only trial and task amplification

The Bloom-only run also failed at 600.015 s after all 10 development rounds,
with 486,172 tasks processed and marking still pending. `fs-save` had 1,000
samples: p50 11.226 ms, p95 26.546 ms, p99 34.557 ms. Sampled marker scratch
peaked at 88,550,359 apparent bytes. The pre-deadline profile had 7,378
transactions, 56.490 s publication, 18.299 s barrier, and 306.340 s marker mutex
hold. Different scheduling and graph expansion order prevent treating its task
count as a strictly matched throughput comparison. This trial did not establish
a material benefit; it does not imply Bloom filters are generally ineffective.

[Selected Bloom trial evidence](GC_BLOOM_TRIAL_20261028.json) preserves settings,
raw counters and the final observation. Full artifacts:
`build/developer-soak-gc-bloom/seed-20261028-ynzwg4rr`. One additional diagnostic
profile RPC was used; these metrics do not include a successful backup/replay.

Each mutable World publication previously queued a new manifest. TREE/INODE task
dedup includes that manifest context, so shared CoW nodes were traversed separately
for many intermediate Worlds, even with only 21 named revisions. The P queue
coalesces only mutable publication roots before expansion. Baseline roots,
retained revisions, leases, and already-expanded immutable tasks are preserved.
This avoids unsafe cancellation of manifests still needed elsewhere. It does not
promise bounded completion under indefinitely sustained mutations.

## Completed GC exposed a LevelDB boundary bug

With World coalescing but the old LevelDB pin, the next full workload reached
physical reclamation and then failed at 269.581 s during backup-overlap `fs-check`.
Pre-backup draining reduced segments from 38,576,102 to 865,472 bytes and sampled
allocated storage by 67,063,808 bytes, but the later consistency failure makes
this a failed correctness run, not an accepted space/performance result.

A copied repository contained 21,935 R entries against `M/roots=21,934`, while
O entries matched their counter at 20,331. Exactly one canonical alias pointed to
a missing O entry. Neither that alias nor object appeared in the checksummed
base backup at sequence 7,378 or the 736 retained transactions ending at 8,114.
Independent point Gets confirmed the inconsistency. Original files were never
opened for forensic writes; all DB scans used copies.

Read-only MANIFEST/SST dumps found the alias's tombstone at internal sequence
623,059 at the end of SST 373, and its old value at 210,821 at the start of SST
374. A later compaction moved the old value to SST 409, then a different
compaction consumed the tombstone without including 409. The old alias became
visible after the base snapshot without a new logical publication.

This matches the upstream
[LevelDB boundary fix](https://github.com/google/leveldb/commit/13e3c4efc66b8d7317c7648766a930b5d7e48aa7).
The submodule advances from 1.23's `99b3c03` to that exact commit: two compaction
boundary additions plus its preceding Apple C-header correction. No vendor patch
is carried. A deterministic real-SST fixture demonstrates old-pin deletion
resurrection and fixed-pin preservation, checking point reads, iteration,
reopening and unrelated surviving keys across ASCII and NUL-prefixed keys.

[Selected boundary failure evidence](GC_WORLD_LEVELDB_FAILURE_20261028.json)
preserves the old-pin trace, forensic counts and MANIFEST excerpts. Complete
failed artifacts remain at
`build/developer-soak-gc-world/seed-20261028-2ddpmzdm`. Updating the binary does not
repair this preserved damaged fixture. The new regression is included in the
existing metadata CI label.

## Replay-retained asynchronous GC completion

After fixing LevelDB, all 116 local CTest targets passed (105 metadata-labelled
cases in 547.45 s and 11 remaining cases in 22.46 s). The next large workload
passed the previously failing backup-overlap checks and replayed the online base,
but **still hit 600.026 s** while draining replay-retained physical maintenance.

The job repeatedly finished a clean physical round and immediately started a new
index. Cleanup reports positive physical work even when no pending/reclaimed
work remains. The coordinator required `!logical_sweep_skipped` before stopping,
so replay retention made it continue until each job's 1,024-step ceiling. The
harness then resumed because the last sampled report usually showed a new index.

The terminal condition now stops when neither pending work nor fresh reclamation
remains, reporting `blocked` if logical sweep was skipped, otherwise `complete`.
No harness deadline or pending-work assertion changed. The added RPC regression
enables replay via a durable base, starts a 32-step GC budget and requires blocked
physical completion before exhaustion. With the old binary it failed at exactly
32 steps despite all pending fields being false. The fixed version passes.

[Selected retained-loop evidence](GC_RETAINED_LOOP_20261028.json) contains the
failed run and repeated complete→index transitions. Full artifacts:
`build/developer-soak-gc-final/seed-20261028-umc8jv68`. After the coordinator change,
all 9 directly affected GC RPC/crash and development-harness targets passed in
64.72 s; the short concurrent soak took 2.25 s versus 17.51 s in the previous full
suite. These individual timings are observations, not a repeated statistical
performance estimate.

## Regression validation

The lock fix passed20 relevant CTest targets covering storage/online GC models,
crash recovery, concurrency, RPC, namespace/extent walks, save metrics, segment
indexing and physical/memory budgets. A combined parallel heavy run hit the
memory test120-second timeout; the same final implementation passed in39.48s
when run alone without changing limits. Physical budget passed in158.70s.
Additional marker cases cover a blocked Reader, foreground seeds/chunks, duplicate
children, Reader exception/retry, competing advance callers, counter lifetime
across marker recreation, and maximum64-parent revision fanout. The reader-gate
test fails against the original marker at its10-second deadlock guard and passes
with the reduced lock scope. No remote CI, Linux or signed mounted result is
claimed here.

The P queue regression additionally covers distinct World keys, independent
baseline/revision/lease closures, A→B replacement during a blocked Reader,
A→B→A, invalid manifests and Reader retry. An isolated mutant that deletes P
unconditionally fails the A→B test; the unmodified implementation passes.
