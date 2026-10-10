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

No signed extension bundle, resource loader, installation or mount command is
provided yet. Lifecycle revocation, stable persistent IDs, accurate statfs,
complete errno mapping, large-directory performance, actual mount/data-cache
semantics and broker/backing isolation remain prerequisites for deployment.
Do not treat this adapter or its callback tests as a usable mounted filesystem
or an agent isolation boundary. The existing passthrough extension is unchanged.
