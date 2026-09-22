# Linux: XFS native-directory backend

Linux support starts with reflink-enabled XFS. Snapshot, fork, checkpoint, pool, diff,
verify, discard/restore and GC use the existing lifecycle and C ABI. The backing tree is
an ordinary directory; applications read and write it through XFS directly.

## Build and validate

Requirements: C++23 compiler (GCC tested), CMake, SQLite development headers, static
libstdc++, Python 3 for tests, and the header-only Git submodules. Sandboxed `world exec`
also requires `/usr/bin/bwrap` (Bubblewrap 0.8 or newer), unprivileged user namespaces,
seccomp, and `close_range(CLOSE_RANGE_CLOEXEC)` support. The sandbox filter supports
x86-64 and AArch64; other architectures refuse sandbox startup.

```bash
# Fedora
sudo dnf install gcc-c++ cmake sqlite-devel libstdc++-static bubblewrap python3
# Ubuntu: sudo apt-get install g++-14 cmake libsqlite3-dev bubblewrap python3

git submodule update --init --recursive
# The build directory must be on reflink-enabled XFS for these tests.
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
ctest --test-dir build/Release --output-on-failure
scripts/check-deps.sh build/Release
python3 scripts/bench/linux_xfs.py build/Release/cli/world --scratch build/Release
```

The Linux XFS integration and diff tests put fixtures under the build directory, because
Linux `/tmp` is often tmpfs. The small `linux_clone_test` uses its own `/tmp` fixture to
exercise restrictive umask handling. The XFS suite fails if reflink or namespace isolation
is unavailable; it does not turn these checks into silent skips. CI provisions a bounded
2 GiB XFS loop volume, builds there, and runs the tests as the ordinary runner user.

The default store is `$XDG_DATA_HOME/world/fs`, or `~/.local/share/world/fs`.
Use `--store` to place it on the project's filesystem if necessary.

## Storage strategy

| Operation | Linux implementation |
|---|---|
| Capability probe | Check source/destination devices and execute FICLONE on private temporary files in the destination. Every actual file clone also checks its result. |
| File clone | `ioctl(FICLONE)`, sharing XFS extents; never a silent byte copy. |
| Tree clone | Four workers create entries; a final pass restores directory metadata, deepest first. |
| Cross-filesystem fork | Refused by default; explicit `--copy` permits a sparse-aware byte copy. |
| Metadata | Mode, uid/gid, nanosecond atime/mtime, xattrs including POSIX ACLs; metadata errors fail the clone. |
| Links | Symlinks are recreated without following them; the existing manifest machinery restores internal hardlink groups. |
| Sparse files | Reflink retains their layout; byte-copy fallback uses SEEK_DATA/SEEK_HOLE. |
| Special files | FIFOs supported; sockets and device nodes rejected rather than omitted. |
| Snapshot protection | Existing root gate (`0000`); `--hard` is rejected before creating a snapshot. |
| Diff | Full tree comparison, including xattrs. Linux xattr names are sorted before comparison. `--events` falls back to the scan. |
| Pool | Same pre-clone/rename path as macOS. |

The backend dispatch is separated in CMake: Darwin uses `platform_darwin.cpp`; Linux
uses `platform_linux.cpp`. Selection inside Linux is capability-based, not a filesystem
name allowlist. [Btrfs](LINUX_BTRFS.md) has its own subvolume/inode-policy adaptation and
test job. Other reflink filesystems have not been certified. XFS inode flags,
project IDs/quotas and birthtime are not
round-tripped by this first backend. This is a workspace clone, not a volume backup.

There is no privileged Linux immutable-flag implementation, persistent change journal or
schema-2 store migration implementation yet. Legacy schema migration fails closed.
The Darwin-specific `core_test` suite remains on macOS; Linux has its own lifecycle and
sandbox integration suite plus the shared 10k-file diff suite. APFS disk-full tests are
not evidence of Linux ENOSPC coverage.

## Execution isolation

Reflink isolates file contents after a write. It does not prevent a process from opening
another World or the store. Likewise, the snapshot gate prevents accidental traversal,
but the owning user can change its permissions outside a sandbox.

Linux `world exec` therefore defaults to a namespace sandbox. The CLI invokes the
system Bubblewrap executable directly (no shell); the core library does not link or
invoke Bubblewrap. The filesystem lifecycle commands themselves need no namespace or
privilege.

The policy in `cli/linux_sandbox.cpp`:

- Requires user, mount, PID, IPC and UTS namespaces; adds a cgroup namespace when available.
- Makes the host filesystem read-only, then exposes the selected World as writable.
- Hides the entire store behind an empty read-only mount.
- Provides private `/tmp`, `/var/tmp`, `/run`, a minimal `/dev` and a new `/proc` for the PID namespace; preserves the host resolver file read-only for DNS.
- Drops capabilities, uses Bubblewrap's `no_new_privs`, and disables further user namespaces.
- Keeps the host network for TCP/UDP development tools. A seccomp filter denies Unix socket creation, datagram socketpairs, io_uring setup and alternate syscall ABIs. Stream socketpairs remain available for child-process IPC.
- Marks inherited descriptors above stderr close-on-exec before launching Bubblewrap. Standard input/output/error remain caller-authorized handles.
- Before a sandboxed launch, the core performs one parallel O(entries) preflight over the selected
  World. On Linux it reads each entry's `statx(2)` mount ID, including files, symlinks and
  directories, and rejects any nested bind or other mount. This uses `STATX_MNT_ID` because
  same-filesystem bind mounts can share `st_dev`; failure to obtain that field fails closed.
  The same pass rejects nested `.world` markers and non-directory hardlinks whose inode also has
  a name outside the World; hardlinks fully contained in the World remain usable.
- Uses a new session and `--die-with-parent`; the existing exec lock covers the runner's lifetime.

Setup failures return an error without starting the command. `--require-sandbox` is
consistent with this default. Only explicit `--no-sandbox` runs unconfined.

Host HOME, including agent caches/configuration, is read-only in this first policy.
Tools needing writable state should be configured to place it in the World or private
`/tmp`. Unix-socket services such as Docker, SSH agents and session D-Bus are unavailable.
The host network is intentionally shared, including localhost services; this policy is
not network isolation, confidentiality of readable host files, or CPU/memory/disk quota
control. The preflight is a point-in-time check while `world exec` starts; trusted host changes
to the tree between or during execution are outside this guarantee. Running a command by
manually entering the directory bypasses `world exec`.
Overlayfs is not needed for these execution namespaces.

References: [FICLONE API](https://man7.org/linux/man-pages/man2/ioctl_ficlone.2.html),
[Bubblewrap security model](https://github.com/containers/bubblewrap#sandbox-security),
[namespace options](https://github.com/containers/bubblewrap/blob/v0.8.0/bwrap.xml).

## Local validation and initial timings

2026-09-22: Fedora 44, Linux 6.19.10, x86-64 Intel i7-10700, XFS, GCC 16.2.1,
Bubblewrap 0.12.0. All three Linux CTest tests and the linked-dependency check passed.
The integration suite checks shared extents with FIEMAP, distinct clone inodes,
COW isolation, ACL/xattr/mode/time preservation, xattr-only diff, hardlinks, sparse files,
symlinks, FIFO, checkpoint, pool hits, discard/restore, protected baselines, cross-volume
refusal and explicit sparse copy, and unsupported special-file refusal.

Sandbox checks cover writes within the World, denial of writes/chmod outside it,
symlink traversal, a hidden store, a separate PID namespace, capability removal,
`no_new_privs`, nested namespace refusal, Unix sockets, leaked inherited descriptors,
setup failure without fallback, command exit status, exec locks and signal termination.

Medians of three runs, 4 KiB files with 100 files per directory; milliseconds including
CLI startup. Source construction, pool fill and deletion are outside the corresponding
latency sample. These are warm local microbenchmarks, not a claim of optimal concurrency
or native-workload throughput. The exec column predates the O(entries) sandbox preflight and
is retained only as a pre-preflight baseline.

| Files | Init | Fork without pool | Pool fork | Clean full diff | Sandboxed exec (`true`, pre-preflight) |
|---|---:|---:|---:|---:|---:|
| 1,000 | 26.06 | 25.07 | 3.86 | 4.80 | 4.71 |
| 10,000 | 208.20 | 193.44 | 3.70 | 29.66 | 4.76 |

The planned overlayfs backend remains a separate optimization for lazy namespace
creation and changed-set tracking. The XFS backend establishes tested lifecycle and
execution semantics first; it does not claim O(1) cold forks or O(changes) diff.
