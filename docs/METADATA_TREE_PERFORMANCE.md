# Metadata tree height validation

Each namespace publication creates a fresh `MetadataTree` editor. Previously,
`apply()` checked the final candidate's depth by walking every persisted node;
the memoized heights lived only for that editor. A small file overwrite therefore
included an O(N) validation walk even though the CoW edit changed only a path.

Editors now share an in-memory, repository-local LRU of subtree heights, keyed by
immutable object ID. Its capacity is 65,536 entries. A cached subtree still checks
`height <= 256 - current_depth`, so rotations cannot bypass the depth limit. A
miss walks and validates the subtree before recording its height. Entries from
pending, unpublished nodes stay local to the editor: a failed plan must not seed
hints for objects that were never committed. Explicit tree verification continues
to read and walk the tree independently.

The cache is synchronized for concurrent planning, bounded, and discarded on
repository close. There is no disk-format or replay-ID change. Eviction affects
performance rather than correctness. A restart or a subtree larger than the
available cache can still require a full validation walk. This change removes
repeated walks for resident working sets; it does not eliminate cold-start cost.

## Local measurement, 2026-10-17

Apple M1, macOS 27.0.1, APFS, Release. Each repository contains 100, 1,000 or
10,000 files of 1 KiB, with 100 files per directory and identical payloads
(content deduplication applies). A resident daemon serves binary Unix-socket
RPC; each request opens a connection. Timings include RPC and synchronous
commit, and exclude process startup and mounting. OS caches are not flushed.

The baseline is commit `304a6fa`. The modified build uses the same repositories
and source revision, with distinct new World names; previous runs' Worlds remain.
These are sequential local observations, not a controlled cross-machine result.
The reported modified run had no simultaneous compilation or test suite running.

Each scale has 1,000 serial forks plus 1,000 forks from eight concurrent clients.
The first 100 serial Worlds each read and then overwrite one shared source file;
read/write timing is separate from fork timing. Source revision bytes are checked
unchanged after branch writes. First-write p50 below includes the first cold
height validation, which occurs once per reopened daemon.

| Files | Baseline write p50 | Modified write p50 | Modified first cold write | Modified fork p50 / p95 |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 6.86 ms | 5.93 ms | 32.66 ms | 3.96 / 5.02 ms |
| 1,000 | 10.82 ms | 6.63 ms | 119.14 ms | 3.95 / 5.02 ms |
| 10,000 | 897.90 ms | 6.62 ms | 993.02 ms | 3.96 / 5.08 ms |

The 10,000-file write p50 improved about 136 times. Eight-client fork throughput
in the modified run was 283–301 forks/second. Only 100 writes were sampled per
scale; no write p95 acceptance claim is made. The cold first write remains a
visible limitation.

The metadata-tree regression counts object reads rather than asserting timing:
a 4,096-file tree requires a full initial walk, then fresh editors and branch
edits require fewer than 256 loads. Tests compare byte-identical plans against
uncached traversal, exercise disabled/tiny caches, and ensure unpublished nodes
do not leak into shared hints. Existing depth-boundary and rotation rejection
cases remain active.
