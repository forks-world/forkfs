# Linux: Btrfs native-directory backend

Btrfs uses the Linux `FICLONE` backend for init, fork, checkpoint and pool creation.
Worlds and stored snapshots remain ordinary directories; no Btrfs userspace library,
mount helper or privileged daemon is linked into forkfs. The same Bubblewrap execution
policy as [XFS](LINUX_XFS.md#execution-isolation) applies.

## Filesystem behavior

- Source, store and World may live in different subvolumes of the **same Btrfs
  filesystem**. Subvolumes have different `st_dev` values, so the clone probe compares
  `BTRFS_IOC_FS_INFO` filesystem UUIDs before accepting that case. Actual file cloning
  must still succeed; there is no implicit copy fallback.
- The probe creates temporary files only at the destination. Read-only source
  subvolumes are supported.
- Before populating a new inode, the backend matches the source's NOCOW (`+C`),
  compression (`+c`) and no-compression (`+m`) flags. It clears conflicting inherited
  flags and compression properties first. The normal metadata pass preserves the
  source's `btrfs.compression` property, including its algorithm.
- This also preserves directory policy for subsequently created files. NOCOW files
  still have isolated data after reflinking: a write to shared extents must separate
  those extents. It does not guarantee in-place writes for shared NOCOW data.
- A nested source subvolume is traversed and its contents are cloned into an ordinary
  directory. This avoids the empty nested-subvolume stubs produced by native Btrfs
  subvolume snapshots. Subvolume identity and properties such as `ro` are not copied.
- Pool handout uses rename within a subvolume. Across subvolumes, rename returns
  `EXDEV`; forkfs returns the pool entry and takes the normal file-clone path. Discard
  and restore use the existing cross-volume trash path when needed.
- Different Btrfs filesystem UUIDs are refused by default. `fork --copy` explicitly
  allows a byte-copy fallback, subject to preserving metadata on the destination.

Use Linux 5.18 or later for reflinks across separately mounted subvolumes. Unsupported
kernel operations, incompatible inode settings and metadata errors fail the operation.
The Btrfs CLI is needed by the tests, not by normal forkfs commands.

## Tests

On an existing Btrfs mount writable by the current user:

```bash
cmake -S . -B /path/on/btrfs/build -DCMAKE_BUILD_TYPE=Release \
  -DWFS_FSKIT=OFF -DWFS_LINUX_TEST_FS=btrfs
cmake --build /path/on/btrfs/build --parallel
ctest --test-dir /path/on/btrfs/build --output-on-failure
# Only the Btrfs suite:
ctest --test-dir /path/on/btrfs/build -R '^linux_btrfs_test$' --output-on-failure
```

Install the [Linux build dependencies](LINUX_XFS.md#build-and-validate) plus
`btrfs-progs`. The test mount needs `user_subvol_rm_allowed` so an ordinary user can
delete its private subvolume fixtures. The test itself never formats or mounts a
device. It verifies the scratch filesystem is Btrfs and fails if the required reflink
or sandbox features do not work.

`linux_btrfs_test` runs the shared Linux reflink, metadata, sparse-file, lifecycle and
namespace-security suite, then Btrfs-specific subvolume and inode-policy cases. The
separate `linux_clone_test` and 10k-entry `diff_test` run too. Set `WFS_BTRFS_OTHER` to
a writable directory on a **different Btrfs filesystem** to additionally test UUID
rejection and explicit cross-filesystem copying.

The `Release (Linux Btrfs)` CI job always provides both filesystems: two bounded 2 GiB
loop images with 16 KiB metadata nodes, mounted only during the job. Tests run as the
ordinary runner user. XFS and macOS/APFS retain their own CI jobs.

## Limits

This is per-file reflinking, not a native subvolume-snapshot backend: cold tree clones
remain O(entries). It preserves the three flags above, not arbitrary Linux inode flags,
qgroup assignments, project IDs or subvolume topology. Diff still compares the existing
content/metadata model; inode-flag-only changes are not reported. Quota exhaustion,
Btrfs ENOSPC recovery, send/receive, zoned devices and other mount-option combinations
are not covered by this test matrix. The same namespace security boundaries and trusted
host mutation limits documented for XFS apply.

World identity still uses the existing recorded device/inode pair. Btrfs subvolume
device numbers can change across remounts or reboots; automatic identity recovery in
that case is not implemented or covered here. A filesystem UUID in the clone probe
does not make stored World identities persistent across device-number changes.

References: [Btrfs reflink constraints](https://btrfs.readthedocs.io/en/latest/Reflink.html),
[subvolumes](https://btrfs.readthedocs.io/en/latest/Subvolumes.html),
[inode attributes](https://btrfs.readthedocs.io/en/latest/ch-file-attributes.html).
