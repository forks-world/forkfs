# P0 mount, workload acceptance and storage lifecycle

The implementation is local first. LevelDB indexes metadata; immutable objects live
in DATA segment files. A filesystem mount, rather than the backing directory, is
the application interface. No remote acknowledgement is required by this work.

## Mount and access boundary

The optional `ForkfsRevision.app` now requests a mount through FSClient on macOS
27+ when built with a macOS 27+ SDK. SDK 26 builds retain a disabled mount entry.
Select a repository containing exactly one existing FSKit World/revision
marker. A signed, provisioned and enabled extension is required. The host retains
the user-selected read/write security scope until it exits or changes selection.
The extension owns the engine; another daemon cannot own the repository at once.

Run the read-only preflight:

```
python3 scripts/fskit/mount_probe.py --app build/p0/ForkfsRevision.app
```

Use the actual app path emitted by the build. An unsigned build is not installable
proof. `--mount PATH` separately checks kernel statfs: FS short name
`forkrevision`, FSKit flag, exact mount root and writable status must all match.
Neither a directory with a marker nor a child directory of a mount passes.

For a local workload process tree:

```
python3 scripts/fskit/restricted_workload.py --mount MOUNT --backing STORE -- TOOL ARGS
```

This launcher uses a deny-by-default macOS sandbox. Mount and private temporary
storage are writable; system/toolchain directories are readable. Additional
read-only toolchains require explicit `--read-only PATH` grants. Overlapping
backing grants are refused. At launch, the harness walks the backing tree and
rejects symlinks, special files, and any non-directory inode with a link count
other than one. This catches hardlink aliases hidden anywhere in an allowed
read-only toolchain. It also requires the real `OWNERSHIP` file to be a
single-link regular file whose nonblocking exclusive `flock` reports contention
from another process; an unlocked lease is rejected. This is a liveness check,
not proof of the lock holder's identity or its association with the mounted volume.

`close_fds` closes extra descriptors, while inherited standard streams are
checked separately. Socket and unsupported stdio types are rejected. Pipes,
terminals, and `/dev/null` are allowed; regular files are allowed only when
their resolved path is outside backing and their device/inode identity is absent
from the backing inventory.
Concurrent same-UID host changes can invalidate these startup checks, and the
harness does not prevent backing replacement during the mount lifetime. This
admission check therefore depends on the trusted-host assumption below.
Network access and arbitrary Mach service access are not granted. Some toolchains
will need further scoped permissions; failures must remain failures, without an
unrestricted retry. The sandbox is tested with real denied reads/writes on macOS.

The launcher builds a fresh environment instead of filtering the host environment.
Only a fixed system/CLT PATH, private HOME/TMPDIR, locale, and noninteractive Git
configuration are passed. If installed, the fixed system Command Line Tools
directory is selected explicitly; full Xcode installations need separately scoped
toolchain support. API/cloud tokens, proxy credentials, SSH agent sockets,
control endpoints, shell startup hooks and language-runtime injection variables
are not inherited. Tools requiring credentials currently fail; no secret forwarding
or automatic unrestricted retry is supported. Executables from extra toolchains
must use an absolute path and an explicit read-only toolchain grant.
Relative executable paths are interpreted under the selected mount. Apple tool
shims that require process inspection are not supported; use actual CLT binaries.
On the tested macOS 27 toolchain, the linker also fails under the unconditional
process-info denial. Restricted compilation/linking is therefore not accepted
yet; selecting CLT binaries alone does not resolve it. Do not re-enable process
inspection as a fallback. A separate UID/VM execution boundary is required before
claiming both host-credential isolation and ordinary developer build support.

The next acceptance gate is `scripts/tests/uid_boundary.py`, an opt-in root-only
experiment for disposable macOS hosts. CI runs it on macOS 15 and 26 with the
runner account as host and the existing `_www` account as worker. It installs no
accounts/services and changes no production launcher permissions. Children drop
all supplementary groups, primary GID and UID before exec, and receive fresh
environments and private HOME/TMPDIR. Probes also require attempts to regain
UID 0, GID 0 or supplementary group 0 to fail. The experiment alone permits process-info
inside its sandbox; the ordinary launcher retains its unconditional denial.
It requires a readable host marker from the host UID, a readable worker marker
from the worker UID inside the experimental sandbox, and EPERM/EACCES/EINVAL for the
host marker from the worker both with and without the sandbox. This positive
control distinguishes real cross-UID protection from a broken query. It also
requires compiling, linking and executing C and C++20 programs inside that same sandbox,
including a C++ standard-library program that writes a workspace file,
and rejects backing reads under both POSIX permissions and the sandbox.
EINVAL is intentional: Apple's published
[XNU `sysctl_procargsx` implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_sysctl.c)
returns it for a UID mismatch. Probes use `SC_ARG_MAX`-sized buffers and require
same-query positive controls and live markers, rather than accepting arbitrary
query failure as isolation evidence.

On a disposable Mac, run with a real non-root host account and a separate
non-root worker account/group (the example uses existing `_www`):

```sh
sudo python3 scripts/tests/uid_boundary.py --host-user "$(id -un)" --worker-user _www
```

A pass is a UID/toolchain prerequisite only: the fixture is an ordinary native
directory, not a mounted acceptance run. It does not enable a production UID
launcher, attest an FSKit mount, or validate management/frontend/service RPC
separation. The proposed deployment still needs a trusted identity broker,
per-agent identities/lifecycle, workspace ownership and mount access, and real
mounted build and escape tests. Agents sharing a worker UID can inspect each
other's process arguments; do not reuse it across mutually untrusted agents.

### Dedicated workload entry point

`scripts/fskit/dedicated_workload.py` is the post-drop entry point. A trusted
supervisor must first select a provisioned agent identity, remove supplementary
groups and drop real/effective/saved UID/GID, then invoke a trusted system Python
with `-I -S`. Before switching identities it must discard host credentials,
descriptors and secrets in argv, and exec the launcher with exactly
`PATH=/usr/bin:/bin:/usr/sbin:/sbin`, `HOME=/var/empty`, `LANG=C`, `LC_ALL=C`.
The launcher rejects any other entry environment, except macOS Python's automatic
`__CF_USER_TEXT_ENCODING` with exactly the current UID's C-locale value
(`0x46:0x0:0x0` or Xcode Python's `0x46:0:0` for UID 70). No arbitrary value is permitted.
Admission scans the live native `_NSGetEnviron` array with entry/count/byte
bounds and rejects duplicate or malformed entries before comparing the complete
environment. Python's `os.environ` mapping alone loses duplicate `execve` keys.
The fresh isolated interpreter remains trusted: this check cannot reconstruct
an original exec stack after arbitrary earlier native environment mutation.
CI passes actual duplicate PATH, HOME, LANG, LC_ALL and CoreFoundation entries
through native `execve`, requiring rejection before any workload starts.
Cleaning only the eventual
workload environment would leave the same-UID launcher process's original
exec-stack environment exposed to process queries; changing `os.environ` does
not erase that stack. The launcher refuses root, identity/group mismatches and retained
root recovery. It does not itself provide a privileged identity broker, create
accounts, install sudo rules, or accept requests from agents as root.

Install this file, `restricted_workload.py`, `mount_probe.py` and the per-mount
JSON policy under a root-owned installation directory. The launcher checks all
code/policy ancestors, rejects symlinks, multiple hardlinks, special modes,
group/world writes and extended ACLs. Temporary world-writable ancestors are
not admitted. Installation and interpreter authenticity remain responsibilities
of the trusted supervisor. The policy has this exact versioned schema:

```json
{
  "version": 1,
  "host_uid": 501,
  "service_uid": 502,
  "frontend_uid": 503,
  "worker_uid": 504,
  "worker_gid": 504,
  "mount": "/Volumes/AgentWorld",
  "mount_fsid": [123, 456],
  "backing": "/Library/Forkfs/store",
  "read_only": []
}
```

These are example IDs; the supervisor must obtain actual identities and the
`statfs` fsid from its authenticated mount lifecycle. All four UIDs must be
distinct and non-root. Unknown/duplicate policy keys, invalid IDs and parent
traversal are rejected. The worker does not inventory the private backing;
it checks the root-owned parent, service ownership, mode 0700 and absence of
extended ACLs. Both mount attestations must identify a writable `forkrevision`
FSKit root with the provisioned fsid, including the final check before exec.
There is no native-directory bypass option. A trusted supervisor must also
prevent concurrent unmount/remount and ensure the underlying mountpoint does
not expose writable native storage if the volume disappears.

After admission the dedicated policy enables process-info, while retaining the
ordinary filesystem/network restrictions and explicit backing denial. The
original same-UID launcher remains unchanged. The child receives a fresh
HOME/TMPDIR/environment, mount cwd, no extra descriptors, stdin `/dev/null`,
and umask 077. Supervisor output must be pipes or `/dev/null`; regular files,
sockets and terminals are rejected. The supervisor remains responsible for
agent process-tree cleanup and exclusive identity lifetime; this entry point
is not a process/container lifecycle manager.

The `dedicated_launcher_acceptance.py` CI stages root-owned code/policy on a
disposable runner, tests real UID/group/config denials and the production
profile/spawn path, including C++ linking, environment scrubbing and host argv
denial. Its success driver explicitly mocks only mount attestation for the
native fixture; a separate real CLI test requires rejection of that fixture.
These tests never count as real mounted FSKit acceptance.

The sandbox explicitly denies process-info operations and limits sysctl grants
to basic system/CPU metadata. The process-info denial is necessary to block
argument/environment queries such as kern.procargs2; sysctl filters alone do
not block that query on the tested macOS version. A runtime test
checks both permitted CPU discovery and denial of an external marker process.
The parent and its permitted stdio streams remain trusted: a cooperating external
process can relay commands or data through pipes, so this is not protection from
a malicious host supervisor.

This is **not isolation from other processes owned by the host user**. An agent
launched outside this sandbox still has the host user's access. Global protection
requires an independently enforced launcher/service identity boundary. The
FSKit host's own app sandbox does not sandbox arbitrary application clients.

## Mounted acceptance

```
python3 scripts/tests/mounted_acceptance.py --mount MOUNT --native NATIVE_DIR --output RESULT_DIR
```

The harness refuses native directories before creating workload files. Its native
reference must be on a different filesystem device; results live outside both
workspaces. It verifies mmap/flush/fsync contents, old descriptors after replacement,
hardlinks, truncate/regrow zeroes and cross-process flock conflict errno. It runs
Git commits/checkout and a C build/executable, then records raw stat and 4 KiB
save/fsync/rename durations. Default 1000 samples provide p50/p95/p99; smaller runs
omit tail percentiles. OS caches are not dropped. Rename does not include a parent
directory fsync and is not a crash-durable editor-save assertion.

Native runs of the workload functions test the harness only. They are not forkfs
performance measurements. Real mount cache coherency, dirty buffer writeback,
permissions, large project item limits and mount latency remain acceptance gates.
The adapter assigns monotonic mount-local item identities without a 4096-item
limit. Numeric identities remain stable across reclaim and are not reused during
a mount. The identity map grows with distinct observed inodes until unmount;
this is not a bounded-memory cache or large-repository performance acceptance.

## Offline GC

```
forkfsd gc-plan STORE
forkfsd gc STORE
forkfsd gc STORE --checkpoint NEW_BASELINE_ARCHIVE
```

Run GC as the repository owner; another UID, including root, is refused before
the repository is opened. The atomic exchange preserves the repository directory's
POSIX mode and group ID; preservation of ACLs is not claimed. GC exclusively owns
the repository. It validates existing objects and the recovery
graph before following typed World, revision DAG, namespace and sparse-content
references. Unknown inode codecs/flags fail closed. Generic object roots retain
their payload without guessing a codec from its bytes. Staging aliases are retained
only for live objects. The new generation is copied, synced, reopened and verified
before an atomic directory exchange. A shared ownership lease inode spans the
DB-close/exchange interval, preventing another engine from opening that gap.

The original repository path always names a complete generation. A process crash
can leave a sibling `STORE.gc-*` directory. The next GC removes bound preparation and completed leftovers
only when their repository/path marker and source ownership inode match, before
checking free-space admission. Unbound or legacy preparation directories are
retained conservatively for manual inspection. This
is offline collection, not background incremental GC. Repacking requires temporary
space for another live copy. Process-crash tests do not establish power-loss safety
of every host filesystem, firmware or device.

If replay logging is enabled, default GC pins all indexed objects and preserves
logs, root aliases and counters so existing backup baselines/deltas remain usable.
It may therefore reclaim only unindexed tails. `--checkpoint` explicitly starts a
new replay generation. GC first creates a durable baseline inside the staged
generation, exchanges directories, syncs the parent, and then publishes the
external baseline without replacing an existing file. A checksummed pending
receipt blocks ordinary repository opens until publication completes. After an
interruption, retry the same `gc STORE --checkpoint ARCHIVE` command; a matching
existing archive is validated and reused, while conflicting user data is never
overwritten. A completion receipt makes retries idempotent within that retained
replay generation, including after subsequent writes. `gc-plan` never resumes
publication. The internal baseline is removed only after durable completion.
Existing external backups remain independently recoverable, but new deltas require
the new baseline. Provision it at backup destinations before relying on the new
incremental chain. GC does not automatically prune revisions or reuse inode numbers.

## Capacity admission

```
forkfsd capacity STORE
forkfsd capacity STORE MAX_DATA_BYTES MIN_HOST_FREE_BYTES
```

Zero disables the corresponding bound. CAPACITY is a checksummed, synced control
file. Writes preflight unique new segment chunks before append; rejection returns
ENOSPC without changing committed metadata. Deduplicated chunks are not charged
again. Segment appends recheck the budget; actual I/O failures retain the existing
fail-closed recovery behavior. FSKit statistics now expose available capacity
bounded by the policy and host free space rather than all-zero statistics.

This is a DATA segment admission budget, **not a kernel-enforced repository quota**.
LevelDB, logs, control files and temporary GC generations consume additional space.
Host free space can change concurrently; a reserve is not a reservation. Configure
policy while the repository is offline. GC applies the policy before opening its
staging store, checks free-space admission for internal and external baseline
copies, and preserves backup-task/FSKit control files. Existing data above a newly configured cap is retained; additional
allocation is rejected until collection or a policy change makes room.

## Validation status

The implementation has regression tests for collection at five process-crash
barriers, retained revision/fork contents, replay compatibility, generation reset,
quota rejection and policy corruption. Harness tests cover fake-mount refusal,
native oracle behavior, actual sandbox denial and overlapping grants. Both suites
use the metadata-regression label in CI.

On the current development machine, no valid signing identity or registered
extension is available. The app builds, but no actual FSKit mount, mounted workload
percentile, mounted permission boundary or 300 ms mount target has been measured.
These three P0s are therefore not all closed by this change.

## Separate service and management identities

The standalone RPC daemon accepts an explicit management UID:

```
forkfsd serve STORE --socket RUNTIME/control.sock --rpc --control-uid ADMIN_UID
```

The daemon must already run as the dedicated service UID; this option does not
create users, change identity, install a privileged helper or move the FSKit
engine. The service owns a private (0700) STORE. For a different management UID,
RUNTIME must be service-owned mode 0711, with non-writable trusted ancestors.
Only the socket becomes 0666, so another UID can connect; kernel peer credentials
are checked against ADMIN_UID before request bytes are read or dispatched. No
UID asserted inside the request is trusted. Without a configured frontend UID, other UIDs, including the service UID
when it differs from ADMIN_UID, receive no RPC response. The default remains
same-UID control with a private runtime directory. A matching UID has all RPC
management privileges; there is no per-operation capability delegation yet.

Agents must run as a third, unprivileged UID or inside the restricted launcher.
If an unrestricted agent shares ADMIN_UID, it still has management authority.
Do not share the backing service UID with agents. The actual FSKit extension
currently opens its engine under the host identity; migrating it to an
independently authenticated service transport remains necessary before claiming
whole-product identity isolation. No accounts or system services are installed
by this change. Dedicated-account installation and a real cross-UID permitted
client require deployment-level validation; local ordinary-user regressions
verify a real matching peer and rejection when the configured UID differs.

### Generate the macOS service configuration

```
python3 scripts/fskit/service_plan.py --service-user _forkfs --admin-uid 501 \
  --binary /Library/PrivilegedHelperTools/forkfsd \
  --backing /Library/Forkfs/store --runtime /Library/Forkfs/runtime \
  --output ./world.forks.forkfsd.plist --preflight
```

Replace 501 with the actual management UID. This writes only a new, mode-0600
output file and prints a machine-readable report. It never creates an account,
changes owners, installs a plist, or calls launchctl. A missing service account
or unsafe POSIX layout produces a blocked result and exit status 2; the generated
plist remains available for review. Without --preflight the result is plan_only,
not evidence that a configuration can be installed.

The launchd plan runs the executable directly as the dedicated non-root user,
with a fixed PATH, private umask, core dumps disabled and restart throttling.
Preflight requires a non-login shell, a service UID distinct from management,
a root-owned single-link executable, service-owned STORE (0700), service-owned
RUNTIME (0711), and non-writable ancestors owned by root/the relevant owner.
Symlink paths and ancestors are rejected. It does not audit ACLs or create a
privileged installation channel. A reviewed production installation must also
verify ACLs, protect the launchd plist as root-owned/non-writable by agents,
verify the executable signature, and test actual permitted and rejected UIDs.
The legacy in-process FSKit adapter cannot use this daemon concurrently: exclusive
repository ownership intentionally prevents that unsupported combination.

### Authenticate the service before sending a request

```
forkfsd request RUNTIME/control.sock --server-uid SERVICE_UID status
```

Clients pin the daemon UID independently of the endpoint path and verify kernel
peer credentials immediately after connect, before transmitting any request
bytes, session tokens or file payloads. The default expected server UID is the
caller's effective UID; cross-UID services require an explicit --server-uid from
trusted deployment configuration. A value supplied by an agent, a socket file's
owner or an unauthenticated reply is not an identity authority. Both checks are
required: server authenticates management UID and client authenticates service
UID. This verifies an OS identity, not an executable signature or service instance;
processes sharing the service UID must therefore all be trusted. Root is outside
this isolation boundary.

The FSKit remote adapter must consume the inode, mount-session and generation
contracts below. Path commands must not emulate inode operations by rebuilding
paths: rename and open-unlink would make that unsafe. The current FSKit adapter
continues using its local engine until the remote adapter is implemented.

### Inode-addressed RPC frontend contract

These are management-authorized operations; inode IDs are addresses, not access
capabilities. They accept the existing --world/--revision selectors. No host path
is resolved by these operations.

| Operation | Arguments after operation | Result |
| --- | --- | --- |
| fs-iroot | none | inode attributes |
| fs-iparent | directory inode | parent inode attributes |
| fs-ilookup | parent inode, component | inode attributes |
| fs-iread | inode, offset, count | bounded binary payload |
| fs-ireadlink | inode | link target |
| fs-icreate | parent, component, file/directory, attribute patch | inode plus handle (null for directory) |
| fs-isymlink | parent, component, target, attribute patch | inode attributes |
| fs-ilink | source inode, destination parent, component | inode attributes |
| fs-iremove | parent, component, expected inode, file/directory | status |
| fs-irename | source parent, source component, destination parent, destination component, expected source, expected target | status |
| fs-iopen | inode, existing handle access/flags | session handle |
| fs-dir-iopen | directory inode | session directory cursor |

fs-icreate, fs-iopen and directory operations require --session. File creation
publishes an atomic read/write handle together with the namespace change;
clients release it using fs-close. Existing handle operations retain inode
identity after rename/unlink. Directory cursors retain their existing view and
session binding. Mutations of immutable revision views fail read-only.
An empty expected target in fs-irename requires the destination to be absent;
otherwise it must identify the exact existing target. Remove and rename identity
mismatches fail ESTALE. Paths reconstructed from cached names must not substitute
for these inode operations. Attribute patches use the existing bounded codec.

These endpoints enable transport implementation; they do not switch the FSKit
adapter to remote service mode, authenticate a filesystem extension executable,
or provide a mount-scoped authorization/revocation capability.

### Mount session lifecycle and service generation

After verifying the service UID, management calls service-hello to obtain the
protocol version and a random generation for this daemon process. It then calls
`mount-open world NAME LEASE_MS --generation GENERATION` (or revision) to obtain
a session bound to that exact view. Leases retain the existing 100..300000 ms
range. Every mounted filesystem operation supplies both --session and
--generation; omitted, stale or mismatched generations fail before dispatch.
Explicit view selectors, if present, must exactly match the bound view.

Mounted sessions allow only an explicit inode/handle/directory operation list.
Backup, restore, snapshot, generic session creation and status operations cannot
be invoked with such a session. A revision binding is read-only. Use
`mount-keepalive TOKEN --generation GENERATION` to renew and
`mount-revoke TOKEN --generation GENERATION` to close retained handles and drop
cursors. Failed cleanup leaves the session closing and unusable until cleanup
succeeds. Lease expiry also cleans it up. Management restore revokes affected
World mount sessions after the engine revokes their handles. Generation changes
on daemon restart; clients must revoke their local volume rather than silently
reopening old handles against a new instance. An uncertain mutation must not be
retried automatically after reconnect.

By default this remains view/lifecycle scoping on a management-only endpoint:
the management UID can omit a session and retains full authority. For enforced
frontend admission configure `--rpc --control-uid ADMIN_UID --frontend-uid FRONTEND_UID`.
These two UIDs must differ. The server derives the role from kernel peer
credentials, never from request fields. Frontend peers may obtain service-hello;
all filesystem operations require a valid, generation-bound mount session.
They cannot create generic or mount sessions, call management operations without
credentials, or use a legacy management session as a mount session. Holders may
renew/revoke their mounted session. Management creates and securely delivers the
mount token; delivery to FSKit has not yet been implemented. A shared Unix socket
does not imply shared privileges.

Deployment must also keep frontend and backing-service UIDs distinct; the
service planner rejects that overlap. The standalone CLI permits it for protocol
tests but it provides no backing-file isolation. An untrusted agent must not
share ADMIN_UID. FSKit transport integration and real three-identity deployment
validation remain necessary for end-to-end isolation. Generation identifies a
process, not a durable repository identity or a signature on its executable.
