# Managed mount lifecycle

A daemon session revocation prevents subsequent callbacks. It does not remove
kernel name/data caches, revoke every application mapping, or unmount a volume.
Managed agents therefore stop before an existing World can be restored. Restoring
a mounted World in place is unsupported: stop, unmount, restore, then provision a
new session and mount.

## Durable restore barrier

`mount-open` records a registration before returning its session. Registration
roots use the reserved `\0forkfs-history/mount/world/VIEW/GENERATION/TOKEN` or
`mount/revision/VIEW/GENERATION/TOKEN` namespace. Their versioned `FFMOUNT01`
payload records read-only state. Registrations are separate from expiring runtime
sessions. Session expiration, revocation, clean daemon shutdown, and daemon restart
do not remove them. The total registration limit is 128, including stale records.

`Namespace::restore` checks these records under its transaction mutex and returns
POSIX `EBUSY` for a registered mount of the target World. The same check applies
to direct CLI calls and RPC. Mounts of immutable revisions and other Worlds do not
block that World. Invalid registration encodings fail closed.

Only management connections can use:

```
mount-check TOKEN --generation CURRENT_GENERATION
mount-list --generation CURRENT_GENERATION
mount-retire ORIGINAL_GENERATION TOKEN --generation CURRENT_GENERATION
```

`mount-check` inspects a live session without extending its lease. `mount-list`
includes persistent registrations from older daemon generations, allowing an
administrator to inspect interrupted provisioning or a lost response.
`mount-retire` is idempotent and removes only the original generation/token
registration. It also closes a corresponding current live session.

The daemon cannot independently attest an OS mount's disappearance. Calling
`mount-retire` is a trusted administrator assertion that cleanup has completed;
agents and the FSKit frontend cannot make that assertion. Never retire a live
mount merely because its token expired or its daemon restarted.

Registrations survive full backup, replay, and offline GC. A recovered repository
can consequently contain stale registrations from its original host. Inspect them
and establish that the original mount and worker identity are retired before
explicitly removing them. Backup recovery never silently clears this protection.

## Root controller and worker

Install the Python controller and its sibling modules under a root-owned directory
without writable ancestors or extended ACLs. Invoke privileged entry points with
isolated Python (`-I -S`) and the documented fresh environment. Service, frontend,
host, and worker identities remain separate.

Provisioning can write two different credentials:

- The frontend-owned mode-0400 `.forkfs-fskit-session` resource marker is for FSKit.
- `mount_session.py --receipt PATH` additionally writes a root-owned mode-0600
  receipt for the lifecycle controller. Tokens are not printed or put in argv.

After mounting that resource and creating the root-owned worker policy, bind the
receipt to that policy:

```
python3 -I -S /trusted/mount_lifecycle.py bind \
  --receipt /Library/Forkfs/state/session.receipt \
  --policy /Library/Forkfs/policies/worker.json \
  --output /Library/Forkfs/state/world.lifecycle
```

The administrator selects the policy-to-resource association. Binding verifies
the actual mount type, FSKit flag, filesystem ID, session-specific volume UUID
and read-only state, and checks
the service session. The kernel volume UUID must match the UUID derived from the root receipt’s
generation and token; unsupported or mismatched UUID attributes block admission.
The root-owned record stores the policy hash, filesystem ID, volume UUID, original
daemon generation and session token.

Run the agent through the existing supervisor with
`--mount-lifecycle /Library/Forkfs/state/world.lifecycle`. Its health checks do
not renew the FSKit session. The teardown order is:

1. Persist stopping state and terminate/reap all real/effective-worker-UID
   processes, including detached descendants.
2. Verify its session-specific volume UUID and unmount through the pinned root
   directory fd, keeping the
   backend session available for normal kernel writeback.
3. Verify that the original filesystem ID disappeared from the kernel mount list.
4. Retire the original registration against the current daemon generation.
5. Mark the lifecycle retired, remove owned scratch, and release the UID marker.

Before a managed workload starts, a separate root deathwatch completes a readiness
handshake. It inherits only a parent-liveness pipe; the agent cannot inherit its
writer. Parent EOF after a crash invokes the same recovery protocol under the UID
and lifecycle locks. The live supervisor also monitors deathwatch liveness.

An error retains the lifecycle/UID quarantine; it never permits identity reuse or
claims that caches were purged. There is no automatic forced unmount and no
automatic remount or replay of uncertain mutations. The fd-based `funmount`
interface targets the filesystem referenced by that fd rather than resolving a
possibly replaced pathname; see Apple's
[XNU implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/vfs/vfs_syscalls.c).

Explicit recovery uses the same policy, UID lock, lifecycle record, process
cleanup, mount disappearance proof and retirement sequence:

```
python3 -I -S /trusted/agent_supervisor.py \
  --policy /Library/Forkfs/policies/worker.json \
  --state /Library/Forkfs/identities \
  --mount-lifecycle /Library/Forkfs/state/world.lifecycle --recover
```

Legacy supervisor invocations without a lifecycle record retain their existing
identity cleanup behavior; they do not establish managed mount teardown.

## Validation boundary

Daemon RPC tests establish persistence, nonrenewing inspection, restore rejection,
and exact retirement across restart. Controller unit tests inject kernel and RPC
failures to check ordering and quarantine. They are not real-mounted acceptance.

Signed FSKit installation, kernel exposure of the FSKit volume identifier through
`ATTR_VOL_UUID`, actual unmount behavior with dirty pages and old
fd/mmap users, App Sandbox delivery, and end-to-end agent isolation must also run
on a real mounted World. A native directory or mocked mount attestation cannot
substitute for those results. A missing signing identity is a deployment blocker,
not a passing or skipped mounted test.
