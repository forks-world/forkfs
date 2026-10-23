# Experimental CoW FSKit adapter

Build with a macOS 26+ SDK and runtime, with `-DWFS_BUILD_LEGACY=OFF -DWFS_COW_FSKIT=ON` and run
`ctest --test-dir build/namespace-view -R 'forkfsd_(fskit_callbacks|inode_frontend)' --output-on-failure`.

The optional build defaults its deployment target to 26.0 and rejects explicit
older targets because the path-resource loader uses FSKit V2. This matches the
host and extension plist minimum versions.
The in-process compatibility mode links FSKit directly to `forkfs_namespace`
and owns a shared Container. The resource loader selects a named immutable
revision or live writable World. The isolated session-resource mode below
uses per-operation RPC and opens no backing Container in the frontend.
Neither mode performs host-file passthrough.

Lookup, attributes, symlink reads, regular-file reads and directory enumeration
use logical inodes. Revision mutation callbacks return EROFS. Hardlinks reuse the same
FSItem. Numeric item IDs last only for this volume lifetime, and persistent IDs
are explicitly disabled. IDs are allocated monotonically without a 4096-item
quota and never reused during a mount; exhaustion of the 64-bit counter fails
with EOVERFLOW. The identity table grows with distinct observed inodes until
revocation; this is not bounded-memory identity management. Directory cookies are entry ordinals in the immutable
revision; resumed enumeration currently scans earlier entries again.

This uses the Operations compatibility protocols, deprecated by the installed
macOS 27 SDK. Deprecation warnings remain visible but are not build errors for
these optional targets. The Handler API remains future work. Callback tests run
without installing an extension; they exercise lookup/attributes, dot/parent lookup,
hardlink identity, missing/long names, symlinks, read offsets/EOF, xattrs and write
refusal. Directory operations on regular files return ENOTDIR, regular reads of
directories return EISDIR and foreign items return ESTALE. Directory attributes
include the parent item ID; hardlinked regular files do not claim a unique parent.

Get/list read versioned inode xattrs, missing names return ENOATTR,
and all revision set/delete policies return EROFS;
the pathconf maximum xattr size is 64 KiB. The adapter conforms to the general
xattr protocol and intentionally omits the limited-name hook. These callbacks
never inspect backing-file xattrs. Revocation takes precedence over item/cookie
validation and read-only errors for requests against revoked volumes (ENOTCONN).
Item types are cached from the immutable revision, so argument/type validation
does not add extra metadata-tree lookups.
Metadata CI retains macOS 15 and Linux core coverage with the adapter disabled,
and adds macOS 26 with `configure_metadata.py --fskit on`. Callback and unsigned-
bundle regressions are included in the metadata-regression label on that job.
Kernel-managed read buffers
and directory packing have not been tested end to end.

An unsigned extension bundle and resource loader are built. The containing app
requests a mount on macOS 27+; installation and provisioning remain external.
Stable persistent IDs, complete statfs accounting,
complete errno mapping, large-directory performance, actual mount/data-cache
semantics and broker/backing isolation remain prerequisites for deployment.
Do not treat this adapter or its callback tests as a usable mounted filesystem
or an agent isolation boundary. The existing passthrough extension is unchanged.


## Resource loader and revocation

The build emits `ForkRevisionExtension.appex` with a separate bundle identifier
and FS short name `forkrevision`. It remains unsigned and is not registered with
macOS. The loader requires a security-scoped FSPathURLResource for the repository
directory. A regular `.forkfs-fskit-revision` file in that directory contains only
a revision name; alternatively `.forkfs-fskit-world` selects a live World. Exactly
one marker must exist, with a name of 1..64 bytes and an optional trailing newline.
Conflicting markers (even a dangling symlink), symlinks, nonregular
files, invalid names and oversized descriptors are rejected. It cannot redirect
the loader to a different repository. Probe does not open the database. Load
opens revisions in RevisionPreview mode: orphan recovery is skipped and Journal
put/transact/checkpoint/compaction are rejected. Logical roots and sequence are
unchanged, including when revision selection fails. Native LevelDB open/recovery
and background maintenance can still write backing files; the loader therefore
requires a writable path-resource grant. Physically read-only backing resources
are unsupported. It acquires exclusive repository ownership, so another forkfsd holding its LevelDB
lock must be stopped first. Concurrent multi-process ownership is unsupported.
Missing views currently report EIO through the volume constructor.
Worlds open in ReadWrite mode, including normal orphan recovery. A failed World
selection may therefore perform recovery; only revision selection promises to
leave logical roots unchanged. Changing the marker after load does not retarget
the active volume; unload and load again to select another view.

Unload/deactivation/unmount revoke the volume. Revocation drains synchronous
callbacks using the same monitor, clears item identities and releases its engine
references. Subsequent inode reads/lookup fail with ENOTCONN; a revoked instance
cannot be reactivated. Unload releases the security-scoped grant only after successful revocation.
If cleanup fails, it returns the error, marks the container not ready, retains the
volume and grant, and rejects another load with EBUSY until unload succeeds.
Final object destruction balances the grant even when cleanup cannot be retried.
Tests cover descriptor parsing/rejection and old-item access after revocation;
OS grant delivery and extension installation have not been exercised.

This environment reports zero valid code-signing identities. A containing app,
compatible signing/provisioning and user-enabled extension registration are still
needed before an actual FSKit mount can be validated. No system installation or
signing was attempted.

## Containing app

The optional build now produces `ForkfsRevision.app` containing
`Contents/PlugIns/ForkRevisionExtension.appex`. Its small native window can select
a repository and validate the World or revision descriptor. Its Mount button uses
FSClient mountSingleVolumeForResource on macOS 27+, retaining the selected
security-scoped grant. It reports the actual completion result and mount path.
On older systems this mount entry is unavailable. The app does not create
descriptors, install the extension or open the repository engine.

`forkfsd_fskit_bundle` validates the unsigned host/extension layout, identifiers,
executables and declared sandbox entitlements. It is not a signature test.
The host requests user-selected read/write access and the FSKit mount entitlement;
the extension still relies on
FSKit's granted resource. Repeated probes derive the same container identifier
from the resource path and selection, without opening LevelDB. World and revision
identities use separate hash domains; existing revision identities are preserved. These identifiers
are path-based and are not persistent repository or inode identities.

With an appropriate real signing identity, `sign_bundle.py --app APP --output NEW_APP
--identity IDENTITY` copies the build app to a new destination, signs the embedded
extension before the host and verifies the resulting signature. Existing output
is refused. Ad-hoc signing is refused; the tool does not install or register
anything. Signing and provisioning have not been exercised here. Profile/team
requirements, OS registration and real mounts still need validation on a configured
machine. The unsigned host window is not evidence that FSKit can load the bundle.

## World file I/O callback stage

`initWithStore:world:error:` accepts a writable LevelDB Container and selects a
live World. The resource loader uses this constructor for a World descriptor.
A RevisionPreview Container is rejected with EROFS.

World callbacks now implement writes to existing regular files, atomic
size/mode/uid/gid/mtime/atime/birthtime updates, and all four xattr policies.
Attributes report the stored ownership and timestamps, including zero values;
identity mapping and permission enforcement remain frontend work. Unsupported
set attributes (including flags and ctime), malformed timestamps and invalid
modes are rejected before mutation. Only successfully applied fields are marked
consumed. Regular files use the [sparse large-file format](../../docs/LARGE_FILES.md); xattrs remain limited to 64 KiB.

Each retained regular FSItem pins a backend read/write handle. This is an internal
lifetime pin, not an authorization decision or a POSIX open mode. Hardlinks share
that pin. Reads, attributes and xattrs continue to work after unlink until reclaim
or revocation closes it. Reclaim invalidates the old FSItem and retains its numeric
identity for subsequent lookup, without a 4096 lifetime identity limit.
Revocation closes pins before releasing engine references and returns cleanup
errors; failed cleanup can be retried, while further callbacks remain revoked.
Deactivate propagates cleanup failure. Restore must happen after revoking the
World adapter because backend restore invalidates all World handles.

World callbacks also create files/directories/symlinks, link, remove, rename and
enumerate directories. The loader can now construct this volume; actual mounted
filesystem behavior remains unvalidated.
`forkfsd_fskit_world` exercises writes, atomic rejection, timestamp conversion,
xattr policies, hardlink identity, fork/revision isolation, open-unlink retention,
reclaim, revocation and reopen persistence. It joins metadata-regression on the
macOS FSKit CI job. Actual kernel buffers, mounting, authorization and fault-time
cleanup still need integration validation.

## World namespace and enumeration

Creation validates requested attributes before publication. Initial mode, size,
ownership and timestamps are stored with the directory entry and zero-filled
content in one transaction. Regular-file creation also reserves its backend
handle before committing; handle quota failure cannot leave a created file.
Directory and symlink size requests are rejected. FSKit request type attributes
must match the requested creation type. Other unsupported attributes fail without
creating anything. The existing inode identity quota is checked before creation.

Link/remove/rename use inode-relative backend operations, including expected
source and replacement inode IDs. A stale source or target returns ESTALE rather
than deleting a replacement entry. Overwritten/unlinked regular FSItems keep their
handle until reclaim. Directory moves update parent lookup through the backend
parent index. Revisions continue to reject these mutations with EROFS.

World directory enumerations use immutable backend cursors, with attributes from
the same captured view. The adapter assigns numeric IDs without opening live
files, so removed entries can still be enumerated from a captured view. Each
(directory, attributes-mode) has one enumeration: a fresh initial cookie and
verifier starts a new snapshot and invalidates the previous verifier for that
pair. Cookies returned by a successful pack may be replayed, including earlier
cookies. A full packer does not consume its rejected entry. Name-only enumeration
includes dot entries; attribute enumeration omits them as required by FSKit.

At most 128 enumeration contexts are retained. Starting another evicts the oldest
context; stale, evicted, wrong-mode and unissued cookies return
FSErrorInvalidDirectoryCookie so the caller can restart. Reclaim and revoke clear
contexts. Sequential resumption retains a backend cursor checkpoint and replays at most
one 128-entry batch; it does not rescan the directory prefix for every small page.
Replaying earlier cookies can still scan from the captured root. Backend
restore still requires revoking the adapter first.

`forkfsd_fskit_namespace` tests atomic creation rejection, hardlinks/symlinks,
stale mutation arguments, rename-over-open-file, directory moves/cycles, snapshot
pagination across deletion and compaction, rejected pack entries, replay/EOF,
128-entry boundaries, cursor eviction, and persistence/isolation. The portable
`forkfsd_inode_create` regression additionally verifies creation attributes on
both storage engines and all-or-nothing creation at the backend handle limit.
Both suites are in the appropriate metadata-regression CI groups.


## Resource lifecycle regression

`forkfsd_fskit_resource` is included in metadata-regression on macOS with FSKit.
It tests both descriptors, ambiguous/invalid selectors, symlinks and FIFO rejection,
identity separation, denied/read-only grants, probe with a live database owner,
duplicate load, missing views, World write isolation and persistence, revision
write rejection, cleanup failure/retry, and balanced grants on final teardown.
Descriptors, the database and volume callbacks are real; security-scoped URL
access and the cleanup failure are deterministic test doubles. These tests do not
establish OS grant delivery, signing, mounted permissions or kernel cache behavior.

### Isolated daemon transport

The resource loader also supports a frontend-only `.forkfs-fskit-session`
credential in the daemon's control directory. This route owns no Container:
existing callbacks use authenticated RPC with a pinned service UID, token and
generation. See [provisioning and limitations](../../docs/p0-mount-storage.md#fskit-session-transport).
The older in-process resource and constructor paths above remain compatibility
fixtures; separate-service security requires the session resource and distinct
service/frontend/worker identities.

`forkfsd_fskit_transport` runs actual RPC callback/lease/restart tests in
metadata-regression. Disposable-root CI additionally validates credential
admission and backing/management denial across UIDs. These tests simulate only
the security-scope grant and do not establish a real mounted FSKit sandbox grant.
