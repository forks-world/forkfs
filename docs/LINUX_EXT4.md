# Linux: ext4 native-directory backend

Ext4 supports init, fork, checkpoint, pool, diff, verify, discard, restore and GC.
The backend automatically copies regular-file data while preserving sparse holes with
SEEK_DATA/SEEK_HOLE. No `--copy` option is needed on the same filesystem. Each World
has independent data; it does not share extents with its baseline. Cold operations
cost O(data + entries), and pool fill pays that cost ahead of a rename-based handout.
Hardlink groups are restored after copying, so temporary space can exceed the final
size when a large file has multiple names.

The shared Linux metadata path preserves ownership (when permitted), permission bits,
POSIX ACLs, xattrs, timestamps, symlinks and FIFOs. Unsupported special files or metadata
errors fail the operation. Regular-file source descriptors enable O_NOATIME before
copying so data reads leave source access times unchanged; copying fails if that flag is not permitted
(normally the caller must own the source or have CAP_FOWNER). Directory traversal
and symlink reads still follow the existing Linux behavior and can update their
access times. Copy writeback is checked before publication, including
delayed-allocation ENOSPC errors. Existing failure cleanup and GC handle partial trees.
The free-space preflight is a metadata floor, not a reservation for all copied data;
a copy can still run out of space after starting.

Different filesystems are refused by default. `fork --copy` permits explicit cross-volume
copying, subject to destination metadata support. The ext4 strategy does not enable
implicit copy fallback on XFS/Btrfs. Ext2/3 share ext4's statfs magic and will enter the
same copy path, but they are **not validated or covered by this support contract**.

Execution uses the same [namespace isolation policy](LINUX_XFS.md#execution-isolation)
as XFS/Btrfs, including hardlink and nested-mount checks. Linux `--hard` remains
unsupported. This is a workspace copier, not a filesystem backup: ext4 encryption,
verity, project IDs/quotas and arbitrary inode flags are not preserved. Device/inode
identity retains the existing limitations across remounts and device-number changes.

## Validation

Install the [Linux build dependencies](LINUX_XFS.md#build-and-validate). On an existing
ext4 mount writable by the current user:

```bash
cmake -S . -B /path/on/ext4/build -DCMAKE_BUILD_TYPE=Release \
  -DWFS_FSKIT=OFF -DWFS_LINUX_TEST_FS=ext4
cmake --build /path/on/ext4/build --parallel
ctest --test-dir /path/on/ext4/build --output-on-failure
```

`linux_ext4_test` verifies the actual mount type and runs the shared lifecycle and
namespace-security suite, requiring unshared extents and sparse copies. Its xattr
fixture fits default ext4 without requiring the optional ea_inode feature. The
standalone clone and full-scan diff regressions also run. `Release (Linux ext4)` CI
creates a bounded ext4 loop image and executes tests as the unprivileged runner.
A second 1.5 GiB ext4 image exercises actual ENOSPC during init, fork, pool fill
and checkpoint, followed by cleanup and successful retries. The reservation fixture
leaves more than the metadata preflight floor, forcing failure in data copying.
Tests never format a device themselves. XFS, Btrfs and macOS retain separate jobs.
