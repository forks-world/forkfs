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
0777, native xattrs (64 KiB per value, 32 per inode), no ACLs or uid/gid
changes, and no special files. Setattr supports mode, size, mtime, atime and birth time atomically;
unsupported attributes fail before mutation. The owner/group are synthesized
from the service identity. The mounted root is always exposed as mode 0700, and
requests to broaden that mode fail before mutation; this mount policy does not
rewrite the root metadata of a read-only revision. The frontend is intended for a single-user, private
mount point; multi-user permissions and syscall cases beyond the local
acceptance suite remain unqualified.

Directory enumeration uses captured daemon cursors, retains at most 64 bounded
streams and rejects directories exceeding 65,536 entries. Old/foreign cookies
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

Each value is limited to 64 KiB, including resource-fork values; larger values
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
