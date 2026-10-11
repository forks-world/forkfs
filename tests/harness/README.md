# Fixed regression harness: first executable slice

```sh
python3 tests/harness/regression.py --binary build/metadata/daemon/forkfsd
python3 tests/harness/regression.py --binary build/metadata/daemon/forkfsd --suite fork
```

The offline CLI adapter runs nine independently reported fixed filesystem cases
(FS001–FS007, FS009, FS011), plus fork isolation and revision-history smoke cases. Each owns a fresh
repository. Use --case FS004 to reproduce one case; incompatible suite/case
selectors are rejected before creating artifacts. Every successful case finishes
with a namespace consistency check. Calls use subprocess argument lists without a shell, have a timeout,
and log return status/output/timing in JSONL. Expected refusals check both the
ordinary CLI error exit and the specific reason; crashes or unrelated failures
cannot satisfy them. Every CLI operation opens/closes the engine, so persisted
state is exercised between operations.

The runner prints an artifact directory with manifest.json, result.json, per-case
events.jsonl and failure.txt on failure. Generated inputs and repositories are
retained for inspection. An explicit --output path must not already exist.
No user repository is accepted or deleted. Artifacts currently require manual
removal; there is no retention manager or automatic replay yet.

These smoke groups are a subset of the fixed case matrix in
[the design](../../docs/TEST_HARNESS_DESIGN.md). They are not full POSIX, resident
RPC or mounted coverage. --profile acceptance reports missing capabilities and
exits nonzero even if these groups pass. Revision restore and generation revocation
remain unavailable. Native workloads, fault injection and trace minimization are
future adapters/profiles; existing deeper CTest regressions remain enabled.

CTest registers forkfsd_basic_harness and forkfsd_harness_runner under the
metadata-regression label, so the existing dedicated metadata CI discovers them.
Runner self-tests cover crash-versus-refusal, wrong error reasons, timeout logging,
missing-capability acceptance, conflicting case selection and refusing existing
output directories. Resident handles (FS008), directory pagination (FS010) and
the complete fork/revision case matrix remain explicitly unimplemented.
