#!/usr/bin/env python3
"""Shared Linux lifecycle regression; run on XFS, Btrfs or ext4.
All fixtures live under --scratch (default: current directory), never /tmp by default.
"""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import sys
import time
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('world', type=Path)
parser.add_argument('--scratch', type=Path, default=Path.cwd())
parser.add_argument('--filesystem', choices=('xfs', 'btrfs', 'ext4'), default='xfs')
args = parser.parse_args()
world = args.world.resolve()
actual_fs = subprocess.check_output(['stat', '-f', '-c', '%T', str(args.scratch)], text=True).strip()
if args.filesystem == 'ext4':
    actual_fs = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(args.scratch)], text=True).strip()
assert actual_fs == args.filesystem, f'expected {args.filesystem}, got {actual_fs}'


def shared(path):
    # FS_IOC_FIEMAP, one extent; FIEMAP_FLAG_SYNC ensures delalloc is resolved.
    request = bytearray(32 + 56)
    struct.pack_into('=QQIIII', request, 0, 0, 2**64 - 1, 1, 0, 1, 0)
    with path.open('rb') as f:
        fcntl.ioctl(f.fileno(), 0xC020660B, request, True)
    mapped = struct.unpack_from('=I', request, 20)[0]
    flags = struct.unpack_from('=I', request, 32 + 40)[0]
    return mapped == 1 and bool(flags & 0x2000)  # FIEMAP_EXTENT_SHARED


with tempfile.TemporaryDirectory(prefix=f'wfs-{args.filesystem}-', dir=args.scratch.resolve()) as temp:
    root = Path(temp)
    store, src, a, b, c = (root / name for name in ('store', 'source', 'a', 'b', 'c'))
    src.mkdir()
    env = dict(os.environ, WORLD_POOL_TOPUP='0', WORLD_GC_CREATING_MIN_AGE='0')

    def run(*argv, ok=True):
        result = subprocess.run([str(world), '--store', str(store), *map(str, argv)],
                                env=env, text=True, capture_output=True)
        if ok and result.returncode:
            raise AssertionError(f'{argv}: {result.returncode}\n{result.stdout}\n{result.stderr}')
        if not ok:
            assert result.returncode != 0, argv
        return result

    def fs(*argv, **kw):
        return run('fs', *argv, **kw)

    def ident(text, prefix):
        return re.search(rf'\b{prefix}\d+\b', text).group()

    (src / 'data').write_bytes(b'original\n' * 8192)
    (src / 'data').chmod(0o640)
    os.setxattr(src / 'data', 'user.world-test', b'original')
    # Default ext4 keeps xattrs in an inode plus one block (without ea_inode).
    os.setxattr(src / 'data', 'user.large', b'x' * (512 if args.filesystem == 'ext4' else 5000))
    order_count = 8 if args.filesystem == 'ext4' else 40
    for i in range(order_count):
        os.setxattr(src / 'data', f'user.order{i:02}', b'x' * 128)
    # Linux POSIX ACL xattr format: version, then tag/permissions/id entries.
    acl_uid = os.getuid()
    acl = struct.pack('<I', 2) + b''.join(struct.pack('<HHI', tag, perm, uid) for tag, perm, uid in (
        (1, 6, 0xffffffff), (2, 4, acl_uid), (4, 4, 0xffffffff),
        (16, 4, 0xffffffff), (32, 0, 0xffffffff)))
    os.setxattr(src / 'data', 'system.posix_acl_access', acl)
    os.link(src / 'data', src / 'linked')
    (src / 'sym').symlink_to('data')
    (src / 'dangling').symlink_to('missing')
    os.mkfifo(src / 'fifo', 0o640)
    (src / 'empty').mkdir()
    os.setxattr(src / 'empty', 'user.world-dir', b'directory')
    (src / 'readonly').mkdir()
    (src / 'readonly' / 'file').write_text('read only')
    (src / 'readonly').chmod(0o550)
    with (src / 'sparse').open('wb') as f:
        f.seek(64 * 1024 * 1024)
        f.write(b'end')
    os.utime(src / 'data', ns=(1234567890123456789, 1234567890123456789))

    # Native strategy must work without --copy: reflinks or independent ext4 copies.
    source_atime = (src / 'data').stat().st_atime_ns
    sid = ident(fs('init', src).stdout, 'S')
    if args.filesystem == 'ext4':
        assert (src / 'data').stat().st_atime_ns == source_atime
    fs('init', src, '--hard', ok=False)
    wid = ident(fs('fork', '--from', sid, '--to', a, '--no-pool').stdout, 'W')
    assert shared(src / 'data') == (args.filesystem != 'ext4')
    assert shared(a / 'data') == (args.filesystem != 'ext4')
    if args.filesystem == 'ext4':
        assert (src / 'data').stat().st_atime_ns == source_atime
        assert (a / 'data').stat().st_atime_ns == source_atime
    assert (a / 'data').read_bytes() == (src / 'data').read_bytes()
    assert (a / 'data').stat().st_ino != (src / 'data').stat().st_ino
    assert (a / 'data').stat().st_ino == (a / 'linked').stat().st_ino
    assert (a / 'data').stat().st_mtime_ns == (src / 'data').stat().st_mtime_ns
    assert stat.S_IMODE((a / 'data').stat().st_mode) == 0o640
    assert stat.S_IMODE((a / 'readonly').stat().st_mode) == 0o550
    assert (a / 'sym').readlink() == Path('data')
    assert (a / 'dangling').readlink() == Path('missing')
    assert stat.S_ISFIFO((a / 'fifo').stat().st_mode)
    assert (a / 'sparse').stat().st_size == 64 * 1024 * 1024 + 3
    assert (a / 'sparse').stat().st_blocks * 512 < 1024 * 1024
    assert os.getxattr(a / 'data', 'user.world-test') == b'original'
    assert os.getxattr(a / 'data', 'system.posix_acl_access') == acl
    assert os.getxattr(a / 'empty', 'user.world-dir') == b'directory'
    assert not fs('diff', wid, '--full').stdout.strip()
    fs('verify', sid)
    fs('verify', wid)

    # Attribute insertion order must not create a false diff (XFS inline/leaf order differs).
    for i in range(order_count):
        os.removexattr(a / 'data', f'user.order{i:02}')
    for i in reversed(range(order_count)):
        os.setxattr(a / 'data', f'user.order{i:02}', b'x' * 128)
    assert not fs('diff', wid, '--full').stdout.strip()
    os.setxattr(a / 'data', 'user.world-test', b'changed')
    diff = fs('diff', wid, '--full').stdout
    assert 'T data' in diff and 'T linked' in diff, diff
    assert os.getxattr(src / 'data', 'user.world-test') == b'original'
    (a / 'data').write_text('world changed\n')
    assert (a / 'linked').read_text() == 'world changed\n'
    assert (src / 'data').read_bytes().startswith(b'original\n')
    assert 'M data' in fs('diff', wid, '--events').stdout
    fs('verify', sid)
    world_stat = (a / 'data').stat()
    os.utime(a / 'data', ns=(1234567890123456789, world_stat.st_mtime_ns))
    sid2 = ident(fs('checkpoint', wid).stdout, 'S')
    if args.filesystem == 'ext4':
        assert (a / 'data').stat().st_atime_ns == 1234567890123456789
    fs('pool', 'fill', sid2, '--count', '1')
    out = fs('fork', '--from', sid2, '--to', b).stdout
    assert '(pool)' in out, out
    wid2 = ident(out, 'W')
    assert (b / 'data').read_text() == 'world changed\n'
    assert (b / 'data').stat().st_ino == (b / 'linked').stat().st_ino
    assert (b / 'data').stat().st_ino != (a / 'data').stat().st_ino
    assert not fs('diff', wid2).stdout.strip()
    fs('discard', sid2, ok=False)  # a live world still needs this baseline
    fs('discard', wid2)
    assert not b.exists()
    fs('restore', wid2)
    assert (b / 'data').read_text() == 'world changed\n'
    assert run('exec', wid, '--no-sandbox', '--', '/bin/pwd').stdout.strip() == str(a)
    run('exec', wid, '--require-sandbox', '--', '/bin/true')
    # Verify the policy, not just whether a namespace launcher exits successfully.
    (a / 'escape').symlink_to(src, target_is_directory=True)
    outside = root / 'outside'
    outside.write_text('untouched')

    # Sandbox preflight rejects aliases whose inode also has a name outside the World, before
    # the payload can run. Internal hardlinks remain valid (data/linked above).
    external_alias = root / 'external-data'
    os.link(a / 'data', external_alias)
    preflight_marker = a / 'preflight-ran'
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'hardlinked outside' in failed.stderr and not preflight_marker.exists()
    assert external_alias.read_text() == 'world changed\n'
    external_alias.unlink()

    singleton = a / 'singleton-hardlink'
    singleton.write_text('singleton\n')
    singleton_alias = root / 'singleton-alias'
    os.link(singleton, singleton_alias)
    # Leave only this one recorded group so a singleton group with nlink > 1 cannot be hidden
    # behind the fully internal data/linked group.
    (a / 'linked').unlink()
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'hardlinked outside' in failed.stderr and not preflight_marker.exists()
    singleton_alias.unlink()
    singleton.unlink()
    os.link(a / 'data', a / 'linked')

    symlink_alias = root / 'external-symlink'
    os.link(a / 'sym', symlink_alias, follow_symlinks=False)
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'hardlinked outside' in failed.stderr and not preflight_marker.exists()
    symlink_alias.unlink()

    # A bind of the whole fixture root is still one mount from the selected World's point of
    # view. The enclosing namespace therefore remains a valid way to run world exec.
    root_bind_marker = a / 'root-bind-ran'
    root_bound = subprocess.run([
        '/usr/bin/bwrap', '--unshare-user', '--unshare-pid', '--ro-bind', '/', '/',
        '--proc', '/proc',
        '--bind', str(root), str(root), str(world), '--store', str(store),
        'exec', wid, '--', '/usr/bin/touch', str(root_bind_marker)],
        env=env, text=True, capture_output=True)
    assert root_bound.returncode == 0, root_bound.stderr
    assert root_bind_marker.exists()
    root_bind_marker.unlink()

    # Nested directory and file binds have a new mount ID even when their source is on the same
    # filesystem as the World. The preflight must refuse both before the payload can run.
    mounted_dir = a / 'mounted-dir'
    mounted_dir.mkdir()
    bind_source_dir = root / 'bind-source-dir'
    bind_source_dir.mkdir()
    (bind_source_dir / 'outside').write_text('directory bind untouched\n')
    dir_mount_marker = a / 'dir-mount-preflight-ran'
    failed = subprocess.run([
        '/usr/bin/bwrap', '--unshare-user', '--unshare-pid', '--ro-bind', '/', '/',
        '--proc', '/proc',
        '--bind', str(root), str(root), '--bind', str(bind_source_dir), str(mounted_dir),
        str(world), '--store', str(store), 'exec', wid, '--', '/usr/bin/touch', str(dir_mount_marker)],
        env=env, text=True, capture_output=True)
    assert failed.returncode != 0 and 'nested mount' in failed.stderr, failed.stderr
    assert not dir_mount_marker.exists()
    assert (bind_source_dir / 'outside').read_text() == 'directory bind untouched\n'

    mounted_file = a / 'mounted-file'
    mounted_file.write_text('world file target\n')
    bind_source_file = root / 'bind-source-file'
    bind_source_file.write_text('file bind untouched\n')
    file_mount_marker = a / 'file-mount-preflight-ran'
    failed = subprocess.run([
        '/usr/bin/bwrap', '--unshare-user', '--unshare-pid', '--ro-bind', '/', '/',
        '--proc', '/proc',
        '--bind', str(root), str(root), '--bind', str(bind_source_file), str(mounted_file),
        str(world), '--store', str(store), 'exec', wid, '--', '/usr/bin/touch', str(file_mount_marker)],
        env=env, text=True, capture_output=True)
    assert failed.returncode != 0 and 'nested mount' in failed.stderr, failed.stderr
    assert not file_mount_marker.exists()
    assert bind_source_file.read_text() == 'file bind untouched\n'

    # A verified nested World and an unregistered nested marker are both refused by the same
    # tree walk. Move the verified fixture back so the rest of the lifecycle can use its row.
    inner = a / 'inner'
    b.rename(inner)
    fs('verify', inner)
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'nested .world marker' in failed.stderr and not preflight_marker.exists()
    failed = run('exec', wid2, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'protected' in failed.stderr and not preflight_marker.exists()
    inner.rename(b)
    fs('verify', b)
    unverified = a / 'unverified'
    unverified.mkdir()
    (unverified / '.world').write_text('{}')
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert 'nested .world marker' in failed.stderr and not preflight_marker.exists()
    (unverified / '.world').unlink()
    unverified.rmdir()

    unreadable = a / 'unreadable'
    unreadable.mkdir()
    unreadable.chmod(0)
    failed = run('exec', wid, '--', '/usr/bin/touch', preflight_marker, ok=False)
    assert not preflight_marker.exists()
    unreadable.chmod(0o755)
    unreadable.rmdir()

    # A separate trashed world is outside this walk and does not block the selected world.
    trashed = ident(fs('fork', '--from', sid, '--to', c, '--no-pool').stdout, 'W')
    fs('discard', trashed)
    run('exec', wid, '--require-sandbox', '--', '/bin/true')

    private_name = f'world-private-{root.name}'
    probe = r"""
import errno, hashlib, os, pathlib, socket, subprocess, sys
root, store, other, source, outside, parent_ns, private_name, resolver_digest = sys.argv[1:]
def denied(fn):
    try: fn()
    except OSError as e:
        assert e.errno in (errno.EACCES, errno.EPERM, errno.EROFS, errno.ENOENT), e
    else: raise AssertionError('sandbox allowed forbidden operation')
p = pathlib.Path(root)
(p / 'sandbox-write').write_text('allowed')
pathlib.Path('/tmp', private_name).write_text('temporary')
assert not pathlib.Path(store, 'VERSION').exists()
assert hashlib.sha256(pathlib.Path('/etc/resolv.conf').read_bytes()).hexdigest() == resolver_digest
denied(lambda: pathlib.Path(store, 'intruder').write_text('bad'))
denied(lambda: pathlib.Path(other, 'data').write_text('bad'))
denied(lambda: pathlib.Path(source, 'data').chmod(0o777))
denied(lambda: (p / 'escape' / 'data').write_text('bad'))
denied(lambda: pathlib.Path(outside).write_text('bad'))
denied(lambda: socket.socket(socket.AF_UNIX))
denied(lambda: socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM))
# Subprocess IPC and network sockets still work; no external network requests needed.
x, y = socket.socketpair(); x.close(); y.close()
x = socket.socket(socket.AF_INET); x.close()
assert os.readlink('/proc/self/ns/pid') != parent_ns
status = pathlib.Path('/proc/self/status').read_text()
assert 'NoNewPrivs:\t1' in status and 'CapEff:\t0000000000000000' in status
assert subprocess.run(['unshare', '--user', '--map-root-user', 'true'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0
"""
    run('exec', wid, '--', sys.executable, '-c', probe, a, store, b, src, outside,
        os.readlink('/proc/self/ns/pid'), private_name,
        hashlib.sha256(Path('/etc/resolv.conf').read_bytes()).hexdigest())
    assert not Path('/tmp', private_name).exists()
    assert outside.read_text() == 'untouched'
    assert (b / 'data').read_text() == 'world changed\n'
    assert run('exec', wid, '--', '/bin/sh', '-c', 'exit 42', ok=False).returncode == 42
    # Extra inherited descriptors must not provide a writable path around readonly mounts.
    with outside.open('r+') as inherited:
        check_fd = ('import os,sys; dev,ino=map(int,sys.argv[1:]); '
                    'assert all((os.fstat(int(f)).st_dev,os.fstat(int(f)).st_ino)!=(dev,ino) '
                    'for f in os.listdir("/proc/self/fd") '
                    'if int(f)>2 and os.path.exists("/proc/self/fd/"+f))')
        st = os.fstat(inherited.fileno())
        result = subprocess.run([str(world), '--store', str(store), 'exec', wid, '--',
                                 sys.executable, '-c', check_fd, str(st.st_dev), str(st.st_ino)],
                                env=env, pass_fds=(inherited.fileno(),), capture_output=True)
        assert result.returncode == 0, result.stderr


    # Setup failure must not fall back to an unconfined command. An enclosing namespace
    # explicitly disables nested user namespaces to exercise the kernel refusal path.
    marker = a / 'must-not-run'
    failed = subprocess.run(['/usr/bin/bwrap', '--unshare-user', '--disable-userns',
                             '--ro-bind', '/', '/', '--bind', str(root), str(root),
                             str(world), '--store', str(store), 'exec', wid, '--',
                             '/usr/bin/touch', str(marker)], env=env, capture_output=True)
    assert failed.returncode != 0 and not marker.exists(), failed.stderr

    # Exec lock lasts for the runner; signal termination must not leave a live sandbox.
    ready = a / 'ready'
    proc = subprocess.Popen([str(world), '--store', str(store), 'exec', wid, '--',
                             sys.executable, '-c',
                             'import pathlib,time,sys; pathlib.Path(sys.argv[1]).touch(); time.sleep(30)',
                             str(ready)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        assert ready.exists(), proc.poll()
        fs('discard', wid, ok=False)
        fs('checkpoint', wid, ok=False)
        proc.terminate()
        assert proc.wait(timeout=5) == 143
    finally:
        if proc.poll() is None:
            proc.kill(); proc.wait()
        proc.stderr.close()
    fs('verify', wid)

    fs('fork', '--from', sid, '--to', a, ok=False)  # cannot overwrite a world

    # Unsupported special files must fail instead of silently vanishing in a snapshot.
    with socket.socket(socket.AF_UNIX) as sock:
        sock.bind(str(src / 'socket'))
        fs('init', src, ok=False)
    (src / 'socket').unlink()

    # Cross-volume --copy is explicit and retains sparse layout; /dev/shm is tmpfs.
    if Path('/dev/shm').is_dir() and os.stat('/dev/shm').st_dev != root.stat().st_dev:
        with tempfile.TemporaryDirectory(prefix='wfs-copy-', dir='/dev/shm') as other:
            dst = Path(other) / 'world'
            fs('fork', '--from', sid, '--to', dst, '--no-pool', ok=False)
            copied = ident(fs('fork', '--from', sid, '--to', dst, '--no-pool', '--copy').stdout, 'W')
            assert (dst / 'data').read_bytes() == (src / 'data').read_bytes()
            assert (dst / 'sparse').stat().st_blocks * 512 < 1024 * 1024
            fs('verify', copied)
            fs('discard', copied, '--now')

    fs('discard', wid2, '--now')
    fs('discard', wid, '--now')
    fs('pool', 'drain', sid2)
    fs('discard', sid2, '--now')
    fs('discard', sid, '--now')
    fs('gc', '--now', '--retention', '0')
    # Failed snapshot attempts can leave closed gates: open only our own fixture for cleanup.
    for parent, dirs, _ in os.walk(root):
        for name in dirs:
            path = Path(parent) / name
            if not path.is_symlink():
                path.chmod(0o700)
    print(f'linux_{args.filesystem}: native duplication, isolation, metadata, hardlinks, sparse copy, lifecycle, pool: PASS')
