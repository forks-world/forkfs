# Revision-backed FSKit frontend

`WFS_FSKIT=ON` now builds `macos/forkfs`, a new frontend for macOS 26+.
The native-directory passthrough in `macos/fskit` remains retired and cannot be
built. The extension links only the RPC client, Foundation and FSKit; it never
links `worldfs_core` or opens a native World as a file-data source.

The frontend forwards FSKit callbacks to the resident `forkfsd` service, using
session-scoped inode requests and file handles. World mutations synchronously
publish immutable content/metadata objects and a new head before replying to the
FSKit callback. Explicit revision mounts are read-only. File size/mtime equality
never stands in for object identity.

## Prepare a reviewable bundle

Requires macOS 26+ and its SDK. The macOS 26 Operations protocols are used for
compatibility; a context-aware Handler frontend is not implemented yet.

```sh
git submodule update --init third_party/leveldb
FORKFS_SIGN_IDENTITY="Apple Development: YOUR NAME (CERTIFICATE ID)" \
FORKFS_MODULE_PROFILE="/absolute/path/ForkFS.provisionprofile" \
  scripts/bundle-forkfs.sh build/forkfs
cmake --build build/forkfs --parallel 4
ctest --test-dir build/forkfs --output-on-failure --no-tests=error
```

The helper embeds a local provisioning profile and signs
`build/forkfs/ForkFS.app` using the selected identity in the macOS Keychain.
It validates the FSKit Module entitlement, exact extension App ID, profile
expiry, and the signing certificate's inclusion in the profile. Missing signing
inputs and ad-hoc identities are rejected before building.

For local testing, create an Apple Development certificate and a macOS App
Development profile for `world.forks.forkfs.extension`, with FSKit Module enabled
and the test Mac's Provisioning UDID registered. Pass an absolute profile path;
the identity may be a certificate name or its SHA-1 fingerprint. Both the host
and extension use that identity. The script does not export or read private-key
material; Keychain authorization may prompt locally when `codesign` uses it.
Provisioning profiles, certificates, private keys, signed bundles, and signing
logs belong outside source control. Temporary profile/entitlement files are
removed when the script exits. The generated bundle embeds the profile, not a
private key.

Self-signed/ad-hoc signatures cannot authorize the restricted FSKit Module
entitlement. Successful signing verifies the bundle's signature; OS enablement,
sandbox-compatible daemon communication, and mount acceptance still need
separate validation. The helper does not install the app, register an extension,
enable it in System Settings, or mount anything. Application/extension IDs are `world.forks.forkfs` and
`world.forks.forkfs.extension`; the filesystem short name is `forkfs`.

The callback test drives real FSKit `FSVolume`/`FSItem` callbacks against a
separate resident daemon, covering binary I/O, identity after rename,
open-unlink, atomic mode/size updates, read-only revisions and daemon loss.
This is not evidence that the OS has accepted, activated or mounted the extension.
The local OS mount/syscall acceptance evidence and remaining qualification
limits are recorded below.

## Start and stop a mount

After installing and enabling the signed module, start the daemon, authenticated
bridge and mount together from the repository root:

```sh
python3 scripts/mount-forkfs.py build/forkfs/daemon/forkfsd \
  "$HOME/project.forkfs" "$HOME/Worlds/demo" \
  --runtime "$HOME/.forkfs/run/demo" --world main --initialize
```

`--initialize` explicitly creates a new repository and its `main` World. It
refuses an existing repository. For subsequent starts, omit `--initialize`.
Use `--revision NAME` instead of `--world main` for an immutable revision. The
repository, private runtime directory and empty private mountpoint must be
separate paths; choose a short runtime path for the daemon's Unix socket.
This foreground launcher owns one daemon and one mount per repository.

The command stays in the foreground and reports readiness only after the OS
mount succeeds. Ctrl-C or SIGTERM unmounts first, then stops the bridge and
daemon. If unmounting fails because applications still use the volume, it keeps
the services running; close those files and press Ctrl-C again. Startup failure
cleans up processes it created. An unexpected daemon/bridge exit triggers
unmounting rather than silently reconnecting a stale mount. If normal unmount
fails after a backend process exits, the launcher attempts a forced detach of
that failed mount. It never force-unmounts a busy volume while both services are
healthy. Unsynced kernel writes are not a recovery guarantee; callers must use
fsync before relying on a revision checkpoint. Separate runtime
and mountpoint locks reject duplicate launchers. No pre-existing control socket
is removed automatically. After its own daemon exits, the launcher removes an
owned leftover socket only if its device, inode and change timestamp
still match the endpoint it observed at startup. A replacement is preserved.
Private `daemon.log` and `bridge.log` files are stored
in the runtime directory; no key material is printed.

The following manual setup remains useful when debugging the individual services.

## Set up a runtime resource

Choose a short private directory owned by the service user. It must contain the
RPC socket `control.sock` and a private `forkfs.json`; no repository file data
lives there. The daemon keeps exclusive ownership of the actual repository.

```sh
forkfsd="$PWD/build/forkfs/daemon/forkfsd"
repo="$HOME/project.forkfs"
runtime="$HOME/.forkfs/run/demo"
mkdir -p "$runtime"
chmod 700 "$runtime"
$forkfsd init "$repo"
$forkfsd fs-init "$repo"
$forkfsd serve "$repo" --socket "$runtime/control.sock" --rpc
```

In a second terminal, start the sandbox-compatible bridge after the service is ready:

```sh
runtime="$HOME/.forkfs/run/demo"
python3 scripts/fskit-bridge.py build/forkfs/daemon/forkfsd "$runtime" --world main
# For an immutable snapshot, use --revision NAME instead.
```

The bridge validates the selected view and pins its repository identity in the
JSON descriptor. Keep it running until after unmounting. A security-scoped URL
grants descriptor access but does not permit the sandboxed extension to connect
to an arbitrary Unix socket. The bridge listens only on `127.0.0.1` and forwards
to the fixed same-UID daemon socket. Fresh client/server nonces and HMAC-SHA256
with a random 256-bit key authenticate both sides before any RPC is forwarded.
The key is published only in the private descriptor and is never sent over the
wire. Each bridge start rotates the endpoint/key, so stale mounts fail closed.
Connections have a ten-second deadline, a 1 MiB frame limit and a sixteen-client
concurrency limit. `scripts/fskit-config.py` remains available for unsandboxed
callback tests that connect directly to the Unix socket. Keep this directory private. Do not edit a descriptor for an
active mount. The bridge and Unix socket peers must have the same OS UID as the daemon;
loopback peers must prove possession of the private descriptor key. This local
control interface is not an agent broker, multi-user authorization system, or
replacement for a sandbox boundary.

For an OS mount test, install/register the built application and enable its file
system extension in System Settings first. These are explicit host setup steps;
the bundle script does not perform them. Use System Settings → General → Login Items & Extensions → **By Category** →
File System Extensions → ForkFS. The **By Application** → FSKit Modules toggle
can ignore clicks due to a macOS settings defect, as investigated in
[macFUSE issue 1194](https://github.com/macfuse/macfuse/issues/1194).
After replacing an ad-hoc
build with a development-signed build, macOS may also request authorization to
reuse the extension's existing data container.

Once enabled, use a third terminal:

```sh
forkfsd="$PWD/build/forkfs/daemon/forkfsd"
runtime="$HOME/.forkfs/run/demo"
mountpoint="$HOME/Worlds/demo"
mkdir -p "$mountpoint"
chmod 700 "$mountpoint"
mount -F -t forkfs "$runtime" "$mountpoint"
# Use only a disposable repository for this acceptance script:
python3 scripts/tests/forkfs_mount.py "$mountpoint" \
  --daemon "$forkfsd" --socket "$runtime/control.sock" --world main --checkpoint
# The optional checkpoint remains retained; unmount before stopping daemon.
umount "$mountpoint"
```

## Coherence, lifetime and limits

A mutable World has one mount session. Other sessions cannot retain file handles
when the lease is acquired or mutate the World while it is mounted; this avoids
unannounced external writes behind the kernel cache. Read queries and creation
of snapshot revisions remain available. Snapshots capture the daemon's committed
root: applications must flush/fsync their writes before requesting a checkpoint;
unsent kernel writeback is not part of that root.

The frontend renews its 60-second session every 15 seconds. Daemon restart,
session expiry, failed heartbeat or repository mismatch fails closed; there is
no transparent reconnection that could retarget a stale inode. This fail-closed
behavior is verified at the callback/RPC boundary. On the macOS 26 Operations
surface, the kernel may still serve cached or mapped data without a callback;
immediate kernel cache revocation on lease loss is not implemented or claimed.
Unmount a failed mount before reusing its World. Real mount acceptance must
qualify cache/writeback behavior, permissions and mmap separately. Handles survive
rename and unlink through daemon orphan roots and are released on final close,
unmount or session expiry. This is normal FSKit callback-level lifetime, not a
claim that a live mount survives service restart.

Names and symlink targets preserve their UTF-8 bytes without normalization or
case folding. Chinese, emoji and combining sequences are supported; NFC and NFD
spellings are distinct names. Malformed UTF-8 and C0/DEL/C1 controls are rejected.
World and revision labels continue to use the separate ASCII identifier grammar.
Actual macOS 27.2 World and read-only revision mounts verified Chinese/emoji
names, links, rename, NFC/NFD distinctions and the byte limit; see
[UTF-8 acceptance evidence](evidence/2026-11-09-fskit-utf8.json).

Current limits are inherited from the namespace prototype: 1 TiB per regular
file with 256 KiB per daemon I/O request, UTF-8 names (255 bytes per component, 4096 per path), mode bits through
0777, native xattrs (262,143 bytes per value, 128 per inode, 8 MiB total values), no ACLs or uid/gid
changes, and no special files. Setattr supports mode, size, mtime, atime, birth time and hidden/nodump/user-immutable flags atomically. Other unsupported flags and
unsupported attributes fail before mutation. The owner/group are synthesized
from the service identity. The mounted root is always exposed as mode 0700, and
requests to broaden that mode fail before mutation; this mount policy does not
rewrite the root metadata of a read-only revision. The frontend is intended for a single-user, private
mount point; multi-user permissions and syscall cases beyond the local
acceptance suite remain unqualified.

Directory enumeration uses captured daemon cursors and retains at most 64 streams,
with one page of at most 512 captured entries per stream (plus two dot entries).
It no longer imposes a 65,536-entry ceiling. Mount-lifetime file ID mappings still
grow with the number of observed inodes; page bounds are not a total-memory bound. Old/foreign cookies
fail with ESTALE. FSItem numbers are stable only for the mount lifetime;
persistent object IDs are not advertised. Inode requests currently traverse the
revision metadata to locate a linked inode, so large-tree performance is not
qualified. File data always goes through daemon handles. Sparse files are supported through zero-filled holes and bounded chunk I/O.
2 TB-file and native-directory capabilities are not advertised.
FSKit callbacks can exceed the advertised I/O size; the frontend completes them
using bounded RPCs. Actual macOS 27.2 mounts verified a 4 MiB transfer, 64 MiB
sparse growth, shrink/extend zero fill, chunked open-unlink and a full 4 MiB
read-only revision read after service restart and physical GC; see
[chunked-file acceptance evidence](evidence/2026-11-09-fskit-chunked-files.json).

The logical file limit is now 1 TiB, shared between daemon validation and the
FSKit frontend. Large-file acceptance verifies a 65 MiB + 17 byte dense file
with full SHA-256 readback, sparse markers across 64 MiB and 4 GiB through the
last byte of 1 TiB, mmap across the old limit, open-unlink/path reuse,
shrink/extend zero fill, and revision isolation. Use a disposable repository:

```sh
python3 scripts/tests/large_file_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/large.json
# After unmount, offline GC, and mounting the reported revision read-only:
python3 scripts/tests/large_file_mount.py /path/to/mount --verify-report /tmp/large.json
```

The script retains a named revision and removes its live scratch directory.
It checks immediate EFBIG on oversized truncate and O_SYNC/F_NOCACHE writes;
buffered writes may reach the kernel cache before a writeback error is reported.
Applications must check write/fsync/close errors. Allocation size now counts
stored data payloads rounded to 512 bytes per position, excluding holes. It is
not exclusive physical repository usage, preallocation or quota enforcement. Dense retirement now uses CoW range
removal and two orphan retention roots, rather than per-chunk root mutations;
see [daemon limits](../daemon/README.md).
Large inode records use FFINOD03; old daemons reject them. Full local evidence
is in [large-file acceptance](evidence/2026-11-11-large-files.json).

The dense retirement suite additionally exercises two actual 320 MiB files,
each with 5,120 allocated chunks (beyond the old 4,096-mutation batch limit).
It covers full SHA-256 readback, open-unlink/dup, orphan writes/truncation,
partial-chunk zero fill, path reuse, final close, closed-file truncate and
revision isolation. Its retained revision is read again after offline GC and
service restart with a read-only mount:

```sh
python3 scripts/tests/chunk_retirement_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/retirement.json
# After unmount, offline GC, and mounting the reported revision read-only:
python3 scripts/tests/chunk_retirement_mount.py /path/to/mount --verify-report /tmp/retirement.json
```

Use a disposable repository: the live scratch directory is removed, but one
explicitly named revision is retained. The suite reports object-root deltas;
unsupported/failed operations return nonzero. It does not qualify a full 1 TiB
dense workload. See [retirement evidence](evidence/2026-11-11-chunk-retirement.json).

Allocation acceptance checks native `st_blocks`, `du`, hardlinks, orphan edits,
zero writes and shrink/extend, then rechecks the retained sparse revision after
GC and read-only remount. The disk-full reproducer fills only its own bounded
256 MiB APFS image, checks ENOSPC and fail-closed keepalive/sync, frees reserve
space, reopens the daemon and verifies atomic data/revision recovery:

```sh
python3 scripts/tests/allocation_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/allocation.json
# After unmount, offline GC, and mounting the reported revision read-only:
python3 scripts/tests/allocation_mount.py /path/to/mount --verify-report /tmp/allocation.json
python3 scripts/tests/revision_disk_full.py --run --daemon build/Release/daemon/forkfsd
```

Use disposable repositories. LevelDB `FFLDB002` stores object-length indexes;
old `FFLDB001` repositories upgrade on their first successful ordinary transaction
and older daemons then reject them. See [allocation and recovery details](../daemon/README.md#backed-data-allocation-and-storage-errors)
and [acceptance evidence](evidence/2026-11-11-allocation-enospc.json).

Linux continues to build the daemon without FSKit; enabling the option there is
an explicit configuration error. CI builds the frontend and runs callback tests
on macOS 26, alongside the daemon suite on Linux.



### Large directory streams

Enumeration keeps a captured daemon cursor open and reads pages as the native
packer accepts entries. Cookies retain their snapshot across page boundaries,
including attributes of subsequently unlinked entries. Backward cookies rewind
and replay that same immutable cursor; they do not reopen the live directory.
At most 64 streams remain; starting another evicts and closes the oldest cursor,
and its old verifier fails with ESTALE. Session shutdown releases all cursors.
This uses existing RPCs and does not change storage formats.

```sh
cmake --build build/Release --target forkfsd_directory_fixture
# Must be a NEW disposable repository; count is bounded to 100000.
build/Release/daemon/forkfsd_directory_fixture /private/tmp/owned-dir-test/repo 66000
# Mount the repository with mount-forkfs.py, then:
python3 scripts/tests/directory_scale_mount.py /path/to/mount --count 66000
python3 scripts/tests/directory_scale_mount.py /private/tmp --count 66000 --reference-apfs
# After unmount + offline GC, remount revision directory_base read-only and repeat.
```

The fixture retains `directory_base`; it never opens an existing repository.
It seeds empty-file records in bounded metadata batches for enumeration testing;
it does not qualify ordinary per-file create throughput.
The native suite checks complete/unique readdir and scandir results, file types,
backward telldir/seekdir and sampled metadata beyond the old ceiling. Direct
callback tests separately cover captured later-page deletion, backward/forward
cookies, EOF, invalid cookies and cursor eviction. See
[acceptance evidence](evidence/2026-11-12-directory-streams.json).

## OS acceptance and remaining qualification

On 2026-11-09, a development-signed arm64 module on macOS 27.2 mounted a
disposable repository through the authenticated bridge. The syscall acceptance
script passed binary I/O/fsync with daemon readback, hardlink identity, symlink
traversal, revision checkpoint isolation, rename/open-unlink/truncate/path reuse,
directory enumeration/dot entries and atomic-save replacement. A separate actual
revision mount advertised `read-only`, preserved the original snapshot bytes and
rejected creation with `EROFS`.

Native xattrs now use the daemon's immutable revision objects, including Finder
tags, binary/empty values, create/replace/delete policy and open-unlink lifetime.
The actual macOS 27.2 acceptance run verified pathname and descriptor xattr
syscalls, hardlinks, snapshot isolation, and read-only metadata after restart and
physical GC. It produced zero AppleDouble `._` sidecars; see
[xattr acceptance evidence](evidence/2026-11-10-fskit-xattrs.json). The acceptance
script still validates any AppleDouble headers before excluding sidecars from
its user-entry comparison. Such files, when present, remain ordinary
revision-backed entries.

Each value is limited to 262,143 bytes, including resource-fork values; larger values
fail with E2BIG. Logical xattr names support up to 255 valid UTF-8 bytes without
normalization, with the namespace's control-character exclusions; macOS can
apply tighter native name limits. Xattrs do not add ACL or multi-user permission
enforcement. Final symlinks refer to their own attributes. The daemon marks
inodes that have xattrs so older readers reject them; after the last attribute
is removed, that marker clears. Attributes are retained by immutable revisions,
not inferred from external files.

The foreground launcher was additionally exercised with normal Ctrl-C and
SIGTERM shutdown, busy-unmount preservation/retry, externally terminated daemon,
abrupt daemon SIGKILL, forced detach, reuse of the same runtime after the crash,
and snapshot readback plus `fs-check` after restarting. Fourteen automated
lifecycle scenarios cover startup cleanup, duplicate ownership, busy unmount,
backend loss, and cleanup/preservation of stale/replaced socket paths.

Native macOS 27.2 acceptance additionally covers shared/private/read-only mmap,
mapping/pread/pwrite coherence, mapped open-unlink, cross-process lockf/flock,
SQLite DELETE/WAL transactions and abrupt application exit recovery, and a local
Git checkout/merge/repack/fsck workflow. Mapped writes flushed with msync/fsync
were read directly from the daemon and retained in an isolated revision.
See the [API matrix and reproducible suite](FILESYSTEM_API_COMPATIBILITY.md)
for the exact scope and APFS reference results. This does not restore the retired
native-directory Git integration.

The native suite also verifies selected open flags and exact failure errno,
umask, dangling-link creation, truncate identity, four-process append using
300 KiB records, Darwin positional writes on append descriptors, single-user
mode/access/search checks, existing-fd access after chmod, and directory-relative
syscalls after the directory is renamed. These are tested scenarios, not a claim
of complete permission/flag conformance. Native rename extensions remain limited
by FSKit: SWAP and EXCL with an absent target return ENOTSUP; EXCL with an existing
target fails early with EEXIST. Both Operations and Handler document that flags
are not forwarded to rename callbacks. See the matrix for primary documentation
and the separate capability probe results.

Broader kernel writeback/mmap under failures, long-running recovery, daemon-loss cache behavior,
multi-user ownership/access checks and other macOS versions remain unqualified.
The local mount evidence does not establish those behaviors.


If mount reports `Module world.forks.forkfs.extension is disabled!`, plugin
registration alone is insufficient. Enable ForkFS in System Settings →
General → Login Items & Extensions → File System Extensions. The public FSKit
client exposes a settings launcher and the module enabled state is read-only;
the scripts do not alter private preferences or bypass this authorization.

The timestamp adapter accepts pathname and descriptor utime requests and explicit
birth-time updates, validating all fields before mutation. Times use unsigned
nanoseconds in 0..INT64_MAX; reads do not update atime. The daemon reads old
FFINOD01 records and writes FFINOD02 with stored atime/birth time. Old records
initially expose mtime/ctime as fallback atime/birth values; no historical creation
time is reconstructed. Older daemons reject upgraded inode records. See
[mode and timestamps](../daemon/README.md#mode-and-timestamps).

Actual macOS 27.2 acceptance verified pathname/fd nanosecond utime, native birth
time, open-unlinked fd updates, `touch` and `cp -p`. A read-only revision mount
after daemon restart and physical GC preserved the recorded times and rejected
utime with EROFS; see [timestamp evidence](evidence/2026-11-10-fskit-times.json).

The inode attribute RPCs also support mode and timestamps on the symlink itself,
including dangling links. They retain the target text and never modify the
referenced file. Symlink size requests fail with EINVAL before any other field
changes. The CLI/RPC `fs-lchmod` and `fs-lsettimes` variants provide the same
nofollow-final behavior; prefix directory links continue to resolve normally.

Native nofollow utime, lchmod, birth-time updates, `touch -h` and `cp -PpX` are
covered by the symlink-attribute acceptance run. Symlink metadata handles now
also support native O_SYMLINK and the xattr-copy path used by plain `cp -Pp`,
including binary xattrs. The source and destination descriptors refer to the
symlink objects, independently of whether the target exists.

After daemon restart and physical GC, a read-only revision mount preserved the
symlink's mode, target text and all stored times, rejecting nofollow utime and
lchmod with EROFS. See [symlink attribute evidence](evidence/2026-11-10-fskit-symlink-attributes.json).

Symlink descriptors support fstat, xattr and mode/time operations, including
open-unlinked descriptors and path reuse. File data read/write operations return
EPERM, and size changes return EINVAL; the adapter rejects empty data requests
too. The daemon retains orphan link text and metadata until the last descriptor
closes or its session expires. Writer-to-reader downgrade preserves the orphan
through a duplicated daemon handle. A read-only revision may open a symlink for
metadata reads and rejects metadata mutations with EROFS. Restart invalidates
all descriptors; it does not resume an old mount or preserve kernel caches.

The native macOS 27.2 run verified ordinary `cp -Pp` with a binary xattr on a
dangling link, O_SYMLINK fstat/fchmod/utime, rejected data I/O, and open-unlink/dup
identity. After restart and GC, a read-only revision fd retained its binary xattr
and rejected metadata writes. See [symlink handle evidence](evidence/2026-11-10-fskit-symlink-handles.json).

Namespace type-conflict errors are also validated directly by inode RPC: rename
file-over-directory reports EISDIR, directory-over-file reports ENOTDIR, rmdir
on a file/symlink reports ENOTDIR, and directory hardlinks report EPERM. Unlinking
a directory reports EPERM on macOS (EISDIR in the Linux daemon). The native
acceptance matrix matches APFS and verifies that rejected operations publish no
repository changes. These checks supplement the kernel's prevalidation.

See [namespace error evidence](evidence/2026-11-10-namespace-posix-errors.json)
for the APFS comparison, native matrix and direct-RPC regression results.

### Shared and blocking native locks

```sh
python3 scripts/tests/locking_mount.py /path/to/private/forkfs/mount
python3 scripts/tests/locking_mount.py /private/tmp --reference-apfs
```

This creates and removes one owned scratch tree. Separate child processes use
readiness/acquisition/release handshakes and deadlines to verify shared lock
coexistence, exclusive conflicts, disjoint record ranges, and a blocked waiter
awakening after unlock. It distinguishes flock's last-open-description-close
behavior from classic lockf/fcntl's release on any close by the owning process.
A raw ctypes F_SETLKW call (Darwin ABI, SA_RESTART disabled) verifies SIGUSR1
interruption with EINTR and that the other process's owner lock remains held.
Python wrappers may automatically retry EINTR, so they are not used for that
single interrupted call. Signal handling does not change normal lock behavior.

These are kernel advisory locks on a native FSKit mount, not daemon RPC locks.
No fairness, deadlock detection, OFD locking, distributed locking, arbitrary load
or multi-user qualification is claimed. See [acceptance evidence](evidence/2026-11-13-native-locking.json)
and [Apple's fcntl semantics](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html).

### Native vnode notifications

The kernel already provides live kqueue `EVFILT_VNODE` notifications for the
qualified mounted operations; this acceptance adds no notification RPC or
persistent event log. Revision history does not replay these events. See
[Apple's kqueue guide](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/KernelQueues/KernelQueues.html).

```sh
python3 scripts/tests/vnode_events_mount.py /path/to/private/forkfs/mount
python3 scripts/tests/vnode_events_mount.py /private/tmp --reference-apfs
```

The suite creates and removes an owned scratch directory. Each case runs in a
bounded worker. The append child uses an owned host temporary HOME/TMPDIR so
interpreter/xcrun startup caches do not add unrelated I/O to the watched mount.
It tests cross-process append and overwrite (WRITE/EXTEND),
chmod (ATTRIB), hardlink count changes (LINK), rename (RENAME), unlink (DELETE),
parent-directory creation/rename/removal, and an unlinked fd whose watch remains
attached to its original inode after pathname reuse. Assertions allow coalescing
and extra documented flags; they do not demand event count/order. The watched
fd remains open for the watch lifetime.

Native macOS 27.2.1 arm64 and APFS reference passed on small disposable
repositories. FSEvents, recursive watching, NOTE_REVOKE/unmount, daemon-death
notification and delivery latency under load remain unqualified. An exploratory
run on the 66,000-entry fixture exceeded worker deadlines amid known metadata
scan costs; it is not a successful large-repository notification stress result.
See [vnode notification evidence](evidence/2026-11-12-vnode-events.json).

### Large xattr acceptance

The daemon and FSKit share a 262,143-byte whole-value limit. The frontend declares
`maximumXattrSizeInBits = 18`, matching native `pathconf(_PC_XATTR_SIZE_BITS)`.
Native macOS 27.2 testing rejected exactly 262,144 bytes before the callback even
when the callback allowed it; the common bound therefore excludes that endpoint.
The callback rejects larger values with E2BIG; the kernel
may reject them earlier with EFBIG. Failure must preserve the prior value.
This remains bounded whole-value xattr support, not unlimited named-fork stream
support. The per-inode limits are 128 attributes and 8 MiB of raw value bytes, including
system-added attributes.

```sh
python3 scripts/tests/large_xattr_mount.py /private/tmp --reference-apfs
python3 scripts/tests/large_xattr_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/xattrs.json
# After unmount, offline GC and mounting the reported revision read-only:
python3 scripts/tests/large_xattr_mount.py /path/to/mount --verify-report /tmp/xattrs.json
```

Use a disposable repository. The test retains one revision and checks native
pathname/fd xattrs, hardlinks, ordinary xattr copy preservation, open-unlink, failed replacement,
and readback after GC/restart. Position-based resource-fork reads are reported
separately. On macOS 27.2, ResourceFork `cp -p` and position reads return EINVAL;
APFS completes both. These paths remain unsupported, despite whole-value support.
See [large xattr evidence](evidence/2026-11-12-large-xattrs.json).

### Xattr count/byte budget and ResourceFork diagnosis

```sh
python3 scripts/tests/xattr_capacity_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/capacity.json
# After unmount, offline GC and mounting the reported revision read-only:
python3 scripts/tests/xattr_capacity_mount.py /path/to/mount --verify-report /tmp/capacity.json
python3 scripts/tests/resource_fork_probe.py /path/to/mount
python3 scripts/tests/resource_fork_probe.py /private/tmp --reference-apfs
```

Capacity acceptance checks 128 total names, exactly 8 MiB of values, ENOSPC
without mutation, slot/byte reuse, orphan edits, revision isolation and GC/restart.
The scripts retain a named revision or use only private scratch directories.
The ResourceFork probe reports path/fd full reads, partial reads, in-range offsets,
EOF and beyond-EOF reads separately; unsupported results remain failures to qualify.

On the tested mount, a full read succeeds, a short buffer returns ERANGE and
nonzero positions return EINVAL, including exactly EOF. APFS instead returns the
requested slice or zero bytes at EOF. The failed `cp -p` can already have copied
the destination payload before its final EOF probe fails. Apple's
[copyfile implementation](https://github.com/apple-oss-distributions/copyfile/blob/main/copyfile.c)
loops resource-fork reads until EOF. Do not treat matching payload bytes as a
successful copy. Changing `supportsLimitedXAttrs` alone produced identical results;
that experimental change is not included. Current xattr callbacks carry no offset,
so this needs further frontend/platform work rather than a daemon range-read stub.
See [capacity and ResourceFork evidence](evidence/2026-11-12-xattr-capacity-resource-fork.json).

### BSD metadata flags

The frontend reports `FSItemAttributes.flags` and atomically consumes supported
flags with mode/size/times through `inode-setattr-flags`. Older setattr RPCs
remain supported; requests without flags keep using the existing times command.
Supported bits include hidden/nodump and user immutable (see below). Kernel-visible rejection is ENOTSUP for
unsupported flags, or EPERM when the kernel rejects a privileged mask first.
Use the matching new daemon/module pair for flag support. `FFINOD04` records
require the updated daemon, including when only historical roots retain flags.

```sh
python3 scripts/tests/file_flags_mount.py /private/tmp --reference-apfs
python3 scripts/tests/file_flags_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/flags.json
# After unmount, offline GC and mounting the reported revision read-only:
python3 scripts/tests/file_flags_mount.py /path/to/mount --verify-report /tmp/flags.json
```

Use a disposable repository. The suite retains a revision and verifies file/fd,
directory/link/hardlink flags, cp preservation, unsupported-mask rejection,
open-unlink/dup/path reuse and read-only GC/restart readback. It qualifies metadata
hints, not Finder UI behavior, backup-tool policy or protection enforcement.
See [flag acceptance evidence](evidence/2026-11-12-file-flags.json).

### Sparse seek regions

When built with SDK 27+, `RevisionVolume` also conforms to
[`FSVolumeSeekRegionHandler`](https://developer.apple.com/documentation/fskit/fsvolume/seekregionhandler).
Native `lseek(SEEK_DATA/SEEK_HOLE)` uses `inode-seek-region` with the open inode
handle when present, retaining open-unlink identity. It does not require a
migration of the other Operations callbacks; the combination is qualified on
macOS 27.2.1. SDK 26 builds omit this protocol and retain conservative seeking.
The updated daemon and module are required together.

```sh
python3 scripts/tests/seek_regions_mount.py /private/tmp --reference-apfs
python3 scripts/tests/seek_regions_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/seek.json
# After unmount, offline GC and mounting the reported revision read-only:
python3 scripts/tests/seek_regions_mount.py /path/to/mount --verify-report /tmp/seek.json
```

Use a disposable repository. All-hole, empty and 1 TiB sparse files, chunk key
boundaries, EOF, fd position sharing, orphan writes and revision/GC readback are
covered. Stored chunks conservatively report data for 64 KiB; inline files
report all bytes as data. APFS can choose different interval boundaries. This
adds neither preallocation nor hole punching, and does not describe physical
storage extents. See [acceptance evidence](evidence/2026-11-12-seek-regions.json).

### User immutable acceptance

The module accepts UF_IMMUTABLE and the daemon enforces new write admission,
metadata mutation and namespace constraints. Existing writable fd data/size
rights are retained as on Darwin; this does not claim mmap revocation or a
multi-user security boundary. Immutable directories prevent child entry changes,
while independently mutable existing children can still be written. Flag-only
clear is supported; unsupported system/append-only bits remain rejected.

```sh
python3 scripts/tests/immutable_mount.py /private/tmp --reference-apfs
python3 scripts/tests/immutable_mount.py /path/to/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/immutable.json
# After unmount, offline GC and readonly mounting the reported revision:
python3 scripts/tests/immutable_mount.py /path/to/mount --verify-report /tmp/immutable.json
```

Use a disposable repository. The script retains an immutable revision and clears
live scratch flags before cleanup. Existing FFINOD04 layout is reused; older
binaries reject its new bit, including historical records. Updated daemon/module
must be deployed together. See [evidence](evidence/2026-11-13-user-immutable.json).

### Link-count capacity

`maximumLinkCount` and daemon link/directory-link capacity now use one shared
32,767 constant. Raw inode RPC and native link operations report EMLINK at the
file-link ceiling. Namespace tests also cover parent-directory growth rejection
and a net-zero replacement of an empty directory at the ceiling. No directory
entry limit is implied for ordinary distinct files.

Use `forkfsd_link_limits_test --fixture NEW_DISPOSABLE_REPOSITORY` and
`scripts/tests/link_limits_mount.py` as documented in the daemon README. The
fixture contains actual 32,767 references; native acceptance checks pathconf,
failure immutability, capacity reuse and GC/readonly revision readback. The
same APFS probe reports that its next link succeeds despite pathconf=32,767,
so capacity equality is not claimed. See [evidence](evidence/2026-11-13-link-limits.json).

### User append-only acceptance

UF_APPEND is supported alongside immutable/hidden/nodump. The kernel admits
O_APPEND opens; the frontend uses positioned RPC writers, preserving callback
offsets. Its separate writeback RPC verifies existing prefix bytes before
allowing an append across a partial cached page. The daemon still rejects a
changed old byte, gaps, size changes and protected namespace/metadata mutations.
Xattrs remain editable, matching the APFS probe. Ordinary RPC writes are EOF-only.

Install matching new daemon/module versions: the new frontend RPCs are not
accepted by old daemons. Old daemons also reject retained FFINOD04 append bits.
The manual suite uses a 332804-byte append, fsync, daemon revision readback,
directory entry constraints and flag clear, plus GC/readonly revision readback.
This does not qualify append-only mmap, arbitrary concurrent writers, power loss,
system protection flags or multi-user security. See
[append evidence](evidence/2026-11-13-append-flags.json) and
[Apple file flags](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemDetails/FileSystemDetails.html).

### Revision-backed volume rename

The macOS 26 RenameOperations protocol now handles native volume-name changes.
The loaded FSVolume name comes from the selected World/revision; successful
rename replies and the framework name update only follow the daemon commit.
An invalid name does not change either value. Readonly/revision mounts return
EROFS. Naming is independent of inode identity, mount path and the descriptor's
repository/view identifier. It does not rename the repository or World.

Use matching daemon/module versions: older daemons lack the label RPC, and
older namespace verifiers reject trees with the optional volume-name metadata
leaf. Legacy trees without the record retain their default `ForkFS` label.
The native suite uses an owned forkfs repository and a separate newly created
APFS reference image; it verifies UTF-8 names, EINVAL, open inode identity,
revision isolation and GC/readonly remount. macOS27.2.1 was measured; this does
not qualify Finder/diskutil workflows or arbitrary concurrent mounts. See
[evidence](evidence/2026-11-13-volume-name.json) and
[Apple RenameOperations](https://developer.apple.com/documentation/fskit/fsvolume/renameoperations).

### Native copyfile and sendfile qualification

```sh
python3 scripts/tests/copy_transfer_mount.py /path/to/private/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock
python3 scripts/tests/copy_transfer_mount.py /private/tmp --reference-apfs
# After unmount, offline GC and readonly mounting the report's revision:
python3 scripts/tests/copy_transfer_mount.py /path/to/mount --verify-report REPORT.json
```

The script owns one unique scratch directory and retains one forkfs revision. CTest
uses `/private/tmp` so its APFS scratch stays independent of the build filesystem.
It calls libc copyfile/fcopyfile/sendfile directly. It checks a 502000-byte binary
payload, copyfile DATA|STAT|XATTR with mode/mtime/hidden/nodump and a 20 KiB binary
ordinary xattr, fd data copy including unlinked source and destination with
reused paths, and an EEXIST destination retaining sentinel data and listed
metadata. EXCL may update ctime on both tested filesystems; full-call atomicity
is not asserted. Each socket receiver has deadlines and exact byte checks;
Darwin sendfile short counts are accumulated, including EAGAIN/EINTR. The
source offset remains unchanged for range, count-zero and beyond-EOF cases.

COPYFILE_CLONE succeeds via ordinary-copy fallback on forkfs and reports
WAS_CLONED=false; CLONE_FORCE returns ENOTSUP. APFS reports actual clones.
Neither fallback success nor the sendfile API implies CoW/zero-copy performance.
ACLs, ResourceFork, recursive copy, sparse allocation, header/trailer sending
and arbitrary load are outside this qualification. The daemon/frontend runtime
is unchanged; macOS CTest adds the APFS reference without requiring a mount.
See [evidence](evidence/2026-11-13-copy-transfer.json),
[Apple copyfile](https://github.com/apple-oss-distributions/copyfile/blob/main/copyfile.3)
and [Darwin sendfile](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sendfile.2.html).

### Volume capacity and pathname limits

```sh
python3 scripts/tests/volume_capacity_mount.py /private/tmp --reference-apfs
python3 scripts/tests/volume_capacity_mount.py /path/to/private/mount \
  --daemon build/Release/daemon/forkfsd --socket /path/to/runtime/control.sock > /tmp/capacity.json
# Unmount, stop services, run offline GC, then mount the reported revision readonly:
python3 scripts/tests/volume_capacity_mount.py /path/to/private/mount --verify-report /tmp/capacity.json
```

Use a disposable repository: the script retains one revision. Native statfs and
statvfs queries cover path/fd geometry, flags and count bounds through rename,
unlink and path reuse. Free counts come from the shared host volume and can
change between calls; they provide neither a quota nor a reservation. Readonly
revisions still report that capacity, while exposing readonly flags and EROFS.
The script checks the actual inclusive 1 TiB limit and failed oversize truncate.
The signed representation requires PC_FILESIZEBITS=42; it is not a 4 TiB promise.

Overlong namespace components/absolute paths and symlink targets now return
ENAMETOOLONG through RPC, including FSKit lookup/create requests. Other invalid
names still return EINVAL. No frontend format or signing update is required for
this daemon correction. PC_NAME_MAX=255 describes forkfs's UTF-8 byte bound;
APFS accepts the tested 256-byte multibyte name. PC_PATH_MAX returns EINVAL for
the tested child directory/file fd, and is recorded as a capability limitation.
The public PathConfOperations protocol has no total-path-length property.
See [evidence](evidence/2026-11-13-volume-capacity.json) and
[Apple PathConfOperations](https://developer.apple.com/documentation/fskit/fsvolume/pathconfoperations).
