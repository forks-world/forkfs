# Local development workload evidence — 2026-10-18

Apple M1, macOS 27.0.1, local APFS, Release daemon with the subtree-height cache fix. These are local RPC stress observations; Linux and mounted FSKit results are not established here. The first two trials used minimal source stubs while the harness was being developed; the final trial used the final mixed-size fixture and telemetry. Four clients ran in every stress case.

| Seed | Files | Source fixture | Rounds | RPC requests | SIGKILLs | Allocated start → final checkpoint | Reachable unique payload at final | Result |
| --- | ---: | --- | ---: | ---: | ---: | --- | ---: | --- |
| 173 | 256 | tiny C stubs (~42 B) | 12 | 478,937 | 12 | 6.57 → 37.75 MiB | 165.8 KiB | passed |
| 20261018 | 512 | tiny C stubs (~42 B) | 8 | 496,513 | 8 | 12.02 → 34.25 MiB | 39.3 KiB | passed |
| 20261019 | 256 | mixed 1 KiB/4 KiB | 6 | 166,229 | 6 | 6.66 → 21.05 MiB | 637.7 KiB | passed |

Total: 1,141,679 RPC requests and 26 process kills across these three trials. Half of the kills were at acknowledged-write barriers; half raced an atomic save. All Worlds and retained revisions passed content/metadata checks after recovery and after final backup/replay. In-flight-save observations were 11 unacknowledged old-state recoveries and two unacknowledged new-state recoveries. No in-flight response was acknowledged before those kills; acknowledged-write durability was exercised by the separate barrier kills.

The final mixed-size trial retained 21 Worlds (five actively modeled plus sixteen immutable fork probes) and 25 revisions. Atomic-save p50 with four clients was 14.77 ms (72 successful samples); offset-write p50 was 14.30 ms (72 samples). These include queueing and synchronous publication. No write p95 is claimed. The earlier single-client fork/write microbenchmark is not a comparable workload.

## Storage finding

The mixed-size repository grew from 6.66 MiB allocated after checkout to 21.05 MiB at the final read-only checkpoint, with about 637.7 KiB of unique reachable payload. Segment apparent bytes were 10,529,602 and index/other apparent bytes were 11,337,717. Source build artifacts were repeatedly deleted and working Worlds restored, yet the store continued accumulating immutable history and metadata. Source ObjectStore GC is not implemented. The tests confirm fork sharing and reject read-only append growth, but do not establish bounded long-term space use. Retention/GC and write amplification remain follow-up work.

Backup/replay also passed, but did not materially shrink the backing store. Replay preserves stored objects and is not a source GC mechanism. The reported allocated bytes use st_blocks and are not a physical device accounting of APFS sharing.

## Regression validation

- CTest developer-workload suite: 3/3 passed (seeds 42 and 173, plus oracle checks).
- Oracle unit suite: 6/6 passed, including rejected corrupt contents, directory discrepancies, invalid metadata and lost acknowledged/torn-save outcomes.
- Python byte compilation, workflow YAML parsing and git diff whitespace checks passed.
- No production code changed in this test branch; prior daemon regression results remain separate. The new Linux/macOS CI matrix and longer scheduled workflow have not yet run remotely.

## Local artifacts

- `/private/tmp/fw49-83uw91l6/dev-workload-results/seed-173-dogwzn69`: settings, trace, summary, storage checkpoints, backup and recovered repository.
- `/private/tmp/fw49-83uw91l6/dev-workload-results/seed-20261018-ngs83b7s`: settings, trace, summary, storage checkpoints, backup and recovered repository.
- `/private/tmp/fw49-83uw91l6/dev-workload-results/seed-20261019-1ypar_gp`: settings, trace, summary, storage checkpoints, backup and recovered repository.

Latest short regression log: `/private/tmp/fw49-83uw91l6/dev-regressions-final.log`.
Daemon SHA-256: `21119d2fbb407a97c0163354f123a4f6e08cf8180d9c9b327032256fedcee1db`.

The first two artifact schemas were produced before successful-response latency filtering and separate barrier/in-flight crash counters were finalized. Their process-kill totals above include both types. The final mixed-size trial uses the final schema. The default mixed-size fixture and commands are documented in `docs/DEVELOPER_WORKLOAD_TESTS.md`.
