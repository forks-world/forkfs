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
A complete OS mount/syscall acceptance run remains required.

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

In a second terminal, prepare the mount resource after the service is ready:

```sh
python3 scripts/fskit-config.py build/forkfs/daemon/forkfsd "$runtime" --world main
# For an immutable snapshot, use --revision NAME instead.
```

The helper validates the selected view and pins its repository identity in the
JSON descriptor. Keep this directory private. Do not edit a descriptor for an
active mount. Socket peers must have the same OS UID as the daemon. This local
control interface is not an agent broker, multi-user authorization system, or
replacement for a sandbox boundary.

For an OS mount test, install/register the built application and enable its file
system extension in System Settings first. These are explicit host setup steps;
the bundle script does not perform them. Once enabled:

```sh
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

Current limits are inherited from the namespace prototype: 256 KiB per regular
file, printable ASCII names, mode bits through 0777, no xattrs/ACLs, no uid/gid
changes, and no special files. Setattr supports mode, size and mtime atomically;
unsupported attributes fail before mutation. The owner/group are synthesized
from the service identity. The mounted root is always exposed as mode 0700, and
requests to broaden that mode fail before mutation; this mount policy does not
rewrite the root metadata of a read-only revision. The frontend is intended for a single-user, private
mount point; multi-user permissions and real syscall mappings are unqualified.

Directory enumeration uses captured daemon cursors, retains at most 64 bounded
streams and rejects directories exceeding 65,536 entries. Old/foreign cookies
fail with ESTALE. FSItem numbers are stable only for the mount lifetime;
persistent object IDs are not advertised. Inode requests currently traverse the
revision metadata to locate a linked inode, so large-tree performance is not
qualified. File data always goes through daemon handles. Sparse-file, 2 TB-file,
xattr and native-directory capabilities are not advertised.

Linux continues to build the daemon without FSKit; enabling the option there is
an explicit configuration error. CI builds the frontend and runs callback tests
on macOS 26, alongside the daemon suite on Linux.


## OS acceptance still required

Validate real `open/read/write/fsync`, atomic-save rename, hardlinks, symlinks,
open-unlink, truncate, directory cookies and dot entries, statfs, read-only
revision mounts, daemon loss, kernel writeback/mmap, and ownership/access checks.
The callback suite and signed bundle do not replace those OS-level checks.


If mount reports `Module world.forks.forkfs.extension is disabled!`, plugin
registration alone is insufficient. Enable ForkFS in System Settings →
General → Login Items & Extensions → File System Extensions. The public FSKit
client exposes a settings launcher and the module enabled state is read-only;
the scripts do not alter private preferences or bypass this authorization.
