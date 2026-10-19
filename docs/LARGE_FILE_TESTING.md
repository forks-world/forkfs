# Large-file testing and oracle limits

These tests use three different oracles. Passing generated cases is evidence for
those operations and seeds, not proof that the filesystem is correct.

## Independent oracles

- `sparse_content_model.cpp` records individual nonzero bytes and logical length.
  It does not implement blocks, radix nodes or descriptor decoding. It covers
  address carries through bits 16/24/32/40/48/56/62, shrink/regrow, immutable
  history branches and a full 256-slot leaf. Three deterministic seeds run in CI.
- `native_file_differential.py` applies the same actions to actual OS files and
  the resident daemon. After each action it compares complete file contents,
  size/mode/link counts, hardlink identity relationships, retained descriptors,
  cursor positions, return values and access errors. Separate read descriptors
  verify complete contents even after a file is unlinked or replaced, including
  when the descriptor under test is write-only. Duplicated handles are compared
  against native `dup` cursor sharing. It combines positional and
  sequential I/O, append, replacement, unlink, restart, revision/fork and restore.
  Native copies preserve hardlink groups and independently retain revision trees.
  Three fixed seeds plus a fresh logged random seed run in CI.
- `large_file_fault_matrix.py` checks eight operations at seven storage fault
  boundaries. It compares committed bytes, sizes, aliases and history, transaction
  sequence, and unpublished object/root counts. Each operation also undergoes
  source-free baseline+delta replay followed by further writes and a new fork.

Native `ftruncate` on a read-only descriptor returns EINVAL on macOS (documented
by the installed `ftruncate(2)` manual), while the forkfs handle API uses EBADF.
Only that specific operation/access/error combination is normalized. Ordinary
read/write access errors must match EBADF exactly; an unclassified RPC error is
never accepted as equivalent. This is a documented API difference, not a claim
that every error code has identical host behavior.

## Reproduce and reduce

```text
python3 daemon/tests/native_file_differential.py BUILD/daemon/forkfsd --seed 173 --steps 300 --output ARTIFACTS
python3 daemon/tests/native_file_differential.py BUILD/daemon/forkfsd --replay ARTIFACTS/FAILED/trace.json --output ARTIFACTS
python3 daemon/tests/native_file_differential.py BUILD/daemon/forkfsd --minimize ARTIFACTS/FAILED/trace.json --output ARTIFACTS
```

Failures retain the action trace, seed, daemon log and repositories. Successful
runs retain operation/access-error counts and World/revision counts. CI uploads
these alongside CTest logs. The reducer deletes action chunks and shrinks byte
payloads while preserving the observed mismatch; it is not guaranteed to find a
globally minimal sequence. Invalid action dependencies do not count as a matching
failure. The reducer does not minimize arbitrary crashes or timeouts.

## Bug found by the native oracle

`fixtures/readonly-handle-write.json` is a three-action reproducer: create a file,
open it read-only, then attempt a sequential write. Before the fix, the native
file returned EBADF while forkfs returned a generic RPC error. Existing tests
only required an exception and therefore missed this distinction. Handle read
and write access checks now return EBADF, including zero-length operations.
The fixture failed against the previous executable and passes after the fix.
No content corruption was observed in this reproducer.

## Mutation experiment

The sparse model was also compiled against four intentionally altered copies of
`file_content.h`: omit truncate-tail zeroing; select the wrong radix slot; fill
raw extension bytes with nonzero data; discard a retained prefix during shrink.
The unchanged copy passed seed 1 and all four altered copies failed the same test.
This was a local compiler experiment, not four bugs found in the shipped code or
a mutation-score claim. It demonstrates sensitivity to these particular faults.

Additional coverage includes 54 rehashed archive-corruption cases across every
index level, descriptor and data block; concurrent disjoint writes to shared
blocks with unique per-round records; concurrent append to retained unlinked
files; atomic replacement retaining old handles; and large-file handle crash
recovery. Storage growth checks count DATA segments so unrelated LevelDB
compaction does not cause false failures.

Remaining gaps include mounted kernel buffers/mmap, real power loss, broad POSIX
error conformance, networked durability and exhaustive operation schedules.
The native differential test is sequential; the explicit concurrency suite uses
coordinated threads but does not exhaust all thread interleavings.
