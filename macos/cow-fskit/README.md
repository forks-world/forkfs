# Experimental CoW revision FSKit adapter

Build on macOS with `-DWFS_BUILD_LEGACY=OFF -DWFS_COW_FSKIT=ON` and run
`ctest --test-dir build/namespace-view -R 'forkfsd_(fskit_callbacks|inode_frontend)' --output-on-failure`.
The optional target links FSKit directly to `forkfs_namespace`; it performs no
host-file passthrough and no per-operation RPC. The volume owns a shared
Container and selects a named immutable revision. A live World is not accepted.

Lookup, attributes, symlink reads, regular-file reads and directory enumeration
use logical inodes. All mutation callbacks return EROFS. Hardlinks reuse the same
FSItem. Numeric item IDs last only for this volume lifetime, and persistent IDs
are explicitly disabled. At most 4096 items are retained; quota exhaustion
currently returns EIO. Directory cookies are entry ordinals in the immutable
revision; resumed enumeration currently scans earlier entries again.

This uses the Operations compatibility protocols, deprecated by the installed
macOS 27 SDK. Deprecation warnings remain visible but are not build errors for
these optional targets. The Handler API remains future work. Callback tests run
without installing an extension; they exercise lookup/attributes, hardlink
identity, missing names, symlinks and write refusal. Kernel-managed read buffers
and directory packing have not been tested end to end.

An unsigned extension bundle and resource loader are now built; installation
and a mount command are not provided yet. Stable persistent IDs, accurate statfs,
complete errno mapping, large-directory performance, actual mount/data-cache
semantics and broker/backing isolation remain prerequisites for deployment.
Do not treat this adapter or its callback tests as a usable mounted filesystem
or an agent isolation boundary. The existing passthrough extension is unchanged.


## Resource loader and revocation

The build emits `ForkRevisionExtension.appex` with a separate bundle identifier
and FS short name `forkrevision`. It remains unsigned and is not registered with
macOS. The loader requires a security-scoped FSPathURLResource for the repository
directory. A regular `.forkfs-fskit-revision` file in that directory contains only
a revision name (1..64 bytes, optional trailing newline); symlinks, nonregular
files, invalid names and oversized descriptors are rejected. It cannot redirect
the loader to a different repository. Probe does not open the database. Load
acquires exclusive repository ownership, so another forkfsd holding its LevelDB
lock must be stopped first. Concurrent multi-process ownership is unsupported.

Unload/deactivation/unmount revoke the volume. Revocation drains synchronous
callbacks using the same monitor, clears item identities and releases its engine
references. Subsequent inode reads/lookup fail with ENOTCONN; a revoked instance
cannot be reactivated. Unload releases the security-scoped grant after revocation.
Tests cover descriptor parsing/rejection and old-item access after revocation;
OS grant delivery and extension installation have not been exercised.

This environment reports zero valid code-signing identities. A containing app,
compatible signing/provisioning and user-enabled extension registration are still
needed before an actual FSKit mount can be validated. No system installation or
signing was attempted.

## Containing app

The optional build now produces `ForkfsRevision.app` containing
`Contents/PlugIns/ForkRevisionExtension.appex`. Its small native window can select
a repository and validate the revision descriptor; it explicitly reports that
nothing has been mounted. The app does not create descriptors, install the
extension, open the repository engine or change repository data.

`forkfsd_fskit_bundle` validates the unsigned host/extension layout, identifiers,
executables and declared sandbox entitlements. It is not a signature test.
The host requests only user-selected read access; the extension still relies on
FSKit's granted resource. Repeated probes derive the same container identifier
from the resource path and revision, without opening LevelDB. These identifiers
are path-based and are not persistent repository or inode identities.

With an appropriate real signing identity, `sign_bundle.py --app APP --output NEW_APP
--identity IDENTITY` copies the build app to a new destination, signs the embedded
extension before the host and verifies the resulting signature. Existing output
is refused. Ad-hoc signing is refused; the tool does not install or register
anything. Signing and provisioning have not been exercised here. Profile/team
requirements, OS registration and real mounts still need validation on a configured
machine. The unsigned preview window is not evidence that FSKit can load the bundle.
