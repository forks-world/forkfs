# Filesystem contract harness

This directory holds a fixed, reviewable black-box contract for common filesystem behavior. It supplements the existing daemon and mounted-filesystem suites. The cases cover path resolution and creation, rename/link/unlink, binary and positioned I/O, sparse extension, descriptor identity and offsets, metadata and xattrs, and the daemon's snapshot/restart extensions.

The suite is a bounded acceptance inventory. Passing it does not prove all POSIX behavior. In particular, mmap coherence, advisory locks, statfs details, descriptor flags, FIFOs, sockets, and broader crash/power-loss behavior remain outside this contract. The trailing-slash `mkdir` cases were added to expose and guard the regression fixed by PR #137; their normative expectations remain mandatory after that fix. `coverage.json` records implemented cases, existing suites that are inventoried but not run by this harness, and missing contracts. An `existing_external` entry always has `status: not_run` and never counts as a pass. `conformance_complete` therefore remains false while any frontier remains.

## Profiles

| Profile | Adapter | What the gate establishes |
| --- | --- | --- |
| `reference` | Native POSIX calls in an owned APFS scratch directory | The literal expectations are supported by the selected APFS environment. |
| `mounted` | The same POSIX calls in an owned directory below a verified forkfs mount | Runs each selected case on APFS first, then independently on the actual forkfs mount. The mount root must be identified by the `forkfs` mount tag and have mode `0700`. |
| `daemon` | FFRPC001 over a real Unix socket with a fresh daemon, store, and session per case | Checks the daemon's RPC operations and wire behavior. It does not stand in for a mounted POSIX result. |

Example strict daemon run:

```sh
python3 scripts/tests/fs_contract.py \
  --profile daemon \
  --daemon build/ci/daemon/forkfsd \
  --report build/fs-contract-daemon.json
```

Example native pair run on macOS:

```sh
python3 scripts/tests/fs_contract.py \
  --profile mounted \
  --reference-root /private/tmp \
  --mountpoint /Volumes/ForkFS \
  --report build/fs-contract-mounted.json
```

The mountpoint must be the exact mount root. The harness does not use a plain directory as a mounted candidate. If native preflight cannot verify APFS and forkfs, the report records blocked executions and exits nonzero.

## Fixed expectations and DSL

`cases.json` uses `schema_version: 1` and a closed operation vocabulary in `model.py`. Each case has an ID, family inventory, applicable profiles/layers, setup fixture, ordered steps, literal expected results, and optional known-gap annotation. `coverage.json` binds every case ID to each family it claims. Unknown fields, operations, duplicate IDs or keys, dangling coverage IDs, invalid references, empty case sets, and unsupported expectation forms fail closed.

Payloads are represented as hexadecimal bytes. The harness does not stringify Python byte objects. References only use `{ "ref": "saved_name", "field": "handle" }` (or another declared normalized result field); there is no interpolation or evaluation. A saved result must come from an earlier successful step.

Every step declares `errno: 0` for success or a symbolic errno for failure. Platform maps name both Darwin and Linux values. Expected failures require `unchanged: true`; the runner captures a complete pre/post fingerprint automatically. POSIX fingerprints include the owned root and its directory tree, inode/type/mode/size/link counts, mtime/ctime, xattrs, symlink targets, file bytes, and live handle identity/data/offset. Atime is deliberately excluded from atomicity fingerprints because observing file contents can update it. RPC fingerprints additionally include commit sequence, indexed objects, object roots, revisions, and live handle state. Unreadable fingerprint state is unsupported and non-passing.

The native worker sets and restores `umask(0)` around each private case so explicit creation-mode expectations compare directly with the daemon's stored modes. In the DSL, omitted `mkdir` mode means `0755`, omitted regular-file create/open mode means `0644`, and `mkdir_at` defaults to `0777`; the adapters pass these modes directly to the relevant syscall or RPC operation. The chosen umask is included in every report. Setup errors, unsupported operations, timeouts, untyped RPC errors, and known gaps are not passes. FFRPC001 response code `1` remains an untyped failure; the adapter never guesses an errno from its message.

The report is written even when cases fail. It records each case/role and step, expected versus observed errors, binary result evidence, atomicity fingerprints, cleanup errors, `profile_passed`, `native_verified`, and the per-family coverage frontier. Any mandatory result other than `pass` makes the selected profile command exit nonzero. A `known_gap` annotation describes a currently expected failure but does not waive the gate.

Each case runs in its own worker process with a hard `--timeout` deadline (60 seconds by default). The supervisor terminates the worker process group, including its owned daemon child, on timeout. It never tries to remove a possibly mounted scratch tree from the supervisor process; a timed out root is reported as `cleanup_pending` for safe follow-up. Native mount preflight has a separate bounded worker as well. Set `TMPDIR` to choose local control and daemon scratch storage.

## Infrastructure checks

The standard-library selftests exercise schema validation and gate behavior without launching a daemon or mounting a filesystem:

```sh
python3 scripts/tests/fs_contract.py --selftest
```

CMake registers this infrastructure check as `fs_contract_harness_selftest`. The existing daemon tests remain registered separately and continue to run in full.
