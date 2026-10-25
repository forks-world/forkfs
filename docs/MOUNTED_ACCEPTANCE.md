# Real mounted acceptance

This gate needs an actual `forkrevision` FSKit mount. Callback tests, a native
directory, mocked mount attestation, and a valid code signature alone cannot
complete it. The lifecycle controller is described in
[MOUNT_LIFECYCLE.md](MOUNT_LIFECYCLE.md).

## Deployment prerequisites

The host and extension must use the expected bundle identifiers, have valid
signatures from the same team, and contain the required signed entitlements.
The host needs `com.apple.developer.fskit.mount`; the extension needs
`com.apple.developer.fskit.fsmodule`. Both need App Sandbox and outbound client
permission for the session RPC transport. These are declarations that the signing
and provisioning environment must authorize; adding a plist key is not proof of
authorization. Apple documents the requirements in its
[FSKit sample](https://developer.apple.com/documentation/FSKit/building-a-passthrough-file-system)
and [mount API](https://developer.apple.com/documentation/fskit/fsclient/mountsinglevolume(resource:bundleid:options:completionhandler:)).

After building and signing a disposable copy of the app, run:

```
python3 scripts/fskit/mount_probe.py --app /path/ForkfsRevision.app --output /outside/workspaces/preflight.json
```

The probe verifies each bundle's signature, actual signed entitlements, signed and
plist bundle identifiers, matching team identifiers, and extension registration.
It invokes system tools by absolute path with bounded deadlines. It does not
sign, register, enable, install, or mount anything. An already signed app can be
checked without its private signing key installed on the test machine.
`prerequisites_available` still means `mounted=false` and
`isolation_verified=false`.

Provision distinct service/frontend/worker identities and a frontend-only session
resource using the lifecycle instructions. Mount the resource from the signed
host app under the frontend identity. Bind its root-only receipt to the worker
policy. Binding must read a nonzero kernel `ATTR_VOL_UUID` matching the
session-specific `FSVolumeIdentifier`; missing or mismatched UUIDs block the gate.
Only an actual FSKit mount can establish that this publication works.

## Data-plane correctness and performance

Choose an empty native reference directory on a different filesystem device,
then run the existing standalone acceptance harness:

```
python3 scripts/tests/mounted_acceptance.py \
  --mount /Volumes/WORLD --native /path/native-reference \
  --output /outside/both/workspaces/results --samples 1000
```

The harness pins the original mount root fd and records its filesystem ID and
kernel volume UUID before/after the mounted workload and cleanup. An identity
change fails the run. Generated workspaces are private and uniquely named;
cleanup uses fd-relative operations, verifies ownership/inode identity, refuses
other filesystem devices and nonregular owner markers, and never follows
symlinks into user data. A workload failure still produces a failure receipt.

The mounted workload requires:

- mmap/flush/fsync, retained open-but-unlinked inodes, hardlink replacement,
  truncate/regrow zeroes, and conflicting cross-process flock behavior.
- Exact CJK, emoji and control-character names through create/read/rename/readdir;
  distinct NFC/NFD spellings and case-sensitive names. Native reference results
  characterize that filesystem rather than imposing forkfs name semantics on it.
- A reader that pins an old fd before a separate writer fsyncs a temporary file,
  atomically replaces the pathname and fsyncs its parent. A pipe barrier orders
  the final observations: the old fd must retain the old bytes, while a fresh
  pathname lookup sees the new complete bytes and hash.
- Git commits/checkout, C compilation and execution, plus raw stat and 4-KiB
  save/fsync/rename timings. Child environments exclude host credentials and
  inherited Git configuration. OS caches are not dropped.

The native and mounted results retain the same correctness/development/timing
structure and add metadata and cross-process observations. Default 1,000 samples
permit p50/p95/p99 reporting; short smoke runs do not establish tail latency.
Neither native-directory unit oracles nor this data-plane harness establish
agent isolation by themselves.

## Lifecycle and isolation gate

On a fresh managed mount, run the trusted supervisor with its root-owned policy
and lifecycle record. Exercise ordinary exit, session revoke, daemon restart,
supervisor SIGKILL with detached worker descendants, and an unmount failure.
Each scenario needs its own fresh session/mount; successful teardown does not
silently remount the previous one.

Verify all worker real/effective-UID processes are gone, the original filesystem
ID is absent, and the exact durable registration is retired before restore can
succeed. A blocked unmount must retain quarantine and continue blocking restore.
Try a replacement mount at the same path: its different session volume UUID must
prevent recovery from unmounting it. Include dirty-page and old-fd/mmap clients,
and verify backing data, session tokens and management RPC remain inaccessible
to the worker. Record mount-ready/teardown latency and raw scenario results.

The disposable-root CI fixture validates real UID/process/pipe/record handling
with mocked kernel and RPC boundaries. Its output explicitly reports
`mounted_acceptance=false`. The real-mounted results above remain mandatory
before describing the new protected mode as end-to-end validated.

## Current environment

The development host reports macOS 27.0.1, zero valid code-signing identities,
and no `forkrevision` mount. Actual signed mounting and the kernel volume-UUID
publication gate have not run here. Provide a compatible signed/provisioned and
enabled installation (or configure the signing environment), then run the above
gate; preflight/unit/CI fixture success must not be substituted for it.
