# Fixed regression harness: first executable slice

```sh
python3 tests/harness/regression.py --binary build/metadata/daemon/forkfsd
python3 tests/harness/regression.py --binary build/metadata/daemon/forkfsd --suite fork
```

The runner provides eleven basic filesystem scenarios (FS001–FS011), plus
seven fork scenarios (FK001–FK007) and seven revision scenarios (RV001–RV007). FS008, FS010, FK005, FK006,
RV003, RV006 and RV007 use a resident RPC service; the others use the offline CLI adapter. Each owns a fresh
repository. Use --case FS004 to reproduce one case; incompatible suite/case
selectors are rejected before creating artifacts. Every successful case finishes
with a namespace consistency check. Calls use subprocess argument lists without a shell, have a timeout,
and log return status/output/timing in JSONL. Expected refusals check both the
ordinary CLI error exit and the specific reason; crashes or unrelated failures
cannot satisfy them. Every CLI operation opens/closes the engine, so persisted
state is exercised between offline operations. Resident cases test open/duplicate
handles surviving rename/unlink, path recreation, handle/session closure, bounded
directory pages pinned across mutations, rewind and stable EOF. Services have
bounded startup/shutdown, file-backed logs and forced cleanup on shutdown timeout.
After shutdown, the engine is reopened for a persisted namespace consistency check.
These scenarios are initial coverage of each case ID, not every design-matrix invariant.

The runner prints an artifact directory with manifest.json, result.json, per-case
events.jsonl and failure.txt on failure. Generated inputs and repositories are
retained for inspection. An explicit --output path must not already exist.
No user repository is accepted or deleted. Artifacts currently require manual
removal; there is no retention manager or automatic replay yet.

These scenarios are a subset of the fixed case matrix in
[the design](../../docs/TEST_HARNESS_DESIGN.md). They are not full POSIX or mounted coverage. Resident cases also check World-scoped handles/locks, refusal publication
counters, revision mutation refusals and history after service restart.
Fork baselines compare directory trees, inode attributes, hardlink identities,
symlink targets and bytes. These checks establish functional isolation; they do
not prove physical CoW sharing or performance. --profile acceptance reports missing capabilities and
exits nonzero even if these groups pass. CLI/RPC revision restore and service access revocation are covered; mounted
generation revocation remains unavailable. Native workloads, fault injection and trace minimization are
future adapters/profiles; existing deeper CTest regressions remain enabled.

CTest registers forkfsd_basic_harness and forkfsd_harness_runner under the
metadata-regression label, so the existing dedicated metadata CI discovers them.
Runner self-tests cover crash-versus-refusal, wrong error reasons, timeout logging,
missing-capability acceptance, conflicting case selection and refusing existing
output directories, service crashes and shutdown timeout cleanup.
RV005 checks complete-tree restore and continued writes; RV006 checks revoked
handles/cursors, lock release and unaffected views; RV007 checks restored state
and retained history after service restart. Physical CoW sharing, mounted behavior
and the complete POSIX matrix remain explicit acceptance gaps.
