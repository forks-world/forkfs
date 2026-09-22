#!/usr/bin/env python3
"""Btrfs lifecycle, cross-subvolume and inode-policy regression. Never formats a device.

Run as an ordinary user on a Btrfs mount with user_subvol_rm_allowed for fixture cleanup.
The shared reflink/security suite runs first, without skips.
"""
import argparse
import array
import fcntl
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('world', type=Path)
parser.add_argument('--scratch', type=Path, default=Path.cwd())
args = parser.parse_args()
world, scratch = args.world.resolve(), args.scratch.resolve()
subprocess.run([sys.executable, str(Path(__file__).with_name('linux_reflink.py')),
                str(world), '--scratch', str(scratch), '--filesystem', 'btrfs'], check=True)

# Linux FS_IOC_{GET,SET}FLAGS encode sizeof(long), although their payload is an int.
GETFLAGS = 0x80006601 | (struct.calcsize('l') << 16)
SETFLAGS = 0x40006602 | (struct.calcsize('l') << 16)
NOCOW, COMPRESS, NOCOMPRESS = 0x800000, 0x4, 0x400
POLICY = NOCOW | COMPRESS | NOCOMPRESS


def flags(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        value = array.array('i', [0])
        fcntl.ioctl(fd, GETFLAGS, value)
        return value[0]
    finally:
        os.close(fd)


def set_policy(path, value):
    fd = os.open(path, os.O_RDONLY)
    try:
        old = flags(path)
        fcntl.ioctl(fd, SETFLAGS, array.array('i', [old & ~POLICY]))
        fcntl.ioctl(fd, SETFLAGS, array.array('i', [(old & ~POLICY) | value]))
    finally:
        os.close(fd)


def shared(path):
    request = bytearray(32 + 56)
    struct.pack_into('=QQIIII', request, 0, 0, 2**64 - 1, 1, 0, 1, 0)
    with path.open('rb') as f:
        fcntl.ioctl(f.fileno(), 0xC020660B, request, True)
    return struct.unpack_from('=I', request, 20)[0] and struct.unpack_from('=I', request, 72)[0] & 0x2000


def btrfs(*argv):
    return subprocess.run(['btrfs', *map(str, argv)], check=True, text=True, capture_output=True)


def ident(text, kind):
    return re.search(rf'\b{kind}\d+\b', text).group()


root = Path(tempfile.mkdtemp(prefix='wfs-btrfs-subvol-', dir=scratch))
subvolumes = []
try:
    source, storage, workspace = (root / p for p in ('source', 'storage', 'workspace'))
    for p in (source, storage, workspace):
        btrfs('subvolume', 'create', p)
        subvolumes.append(p)
    assert len({p.stat().st_dev for p in (source, storage, workspace)}) == 3

    # The destination parent deliberately conflicts with the source root's COW policy.
    set_policy(workspace, NOCOW)
    btrfs('property', 'set', storage, 'compression', 'zstd')
    data = b'Btrfs reflink contents\n' * 8192
    (source / 'cow').write_bytes(data)
    (source / 'nocow-dir').mkdir()
    set_policy(source / 'nocow-dir', NOCOW)
    (source / 'nocow-dir' / 'data').write_bytes(data)
    os.link(source / 'nocow-dir' / 'data', source / 'nocow-link')
    (source / 'compressed').touch()
    btrfs('property', 'set', source / 'compressed', 'compression', 'zstd')
    (source / 'compressed').write_bytes(data)
    (source / 'no-compression').touch()
    set_policy(source / 'no-compression', NOCOMPRESS)
    (source / 'no-compression').write_bytes(data)
    nested = source / 'nested-subvolume'
    btrfs('subvolume', 'create', nested)
    subvolumes.append(nested)
    (nested / 'included').write_text('nested subvolume contents\n')

    store = storage / 'store'
    env = dict(os.environ, WORLD_POOL_TOPUP='0', WORLD_GC_CREATING_MIN_AGE='0')

    def run(*argv, ok=True):
        result = subprocess.run([str(world), '--store', str(store), *map(str, argv)],
                                env=env, text=True, capture_output=True)
        assert (result.returncode == 0) == ok, (argv, result.stdout, result.stderr)
        return result

    def fs(*argv, **kwargs):
        return run('fs', *argv, **kwargs)

    # A read-only source must work: neither probe nor cloning writes to it.
    btrfs('property', 'set', source, 'ro', 'true')
    sid = ident(fs('init', source).stdout, 'S')
    a = workspace / 'a'
    wid = ident(fs('fork', '--from', sid, '--to', a, '--no-pool').stdout, 'W')
    assert flags(a) & POLICY == flags(source) & POLICY
    for rel in ('cow', 'nocow-dir', 'nocow-dir/data', 'compressed', 'no-compression'):
        assert flags(a / rel) & POLICY == flags(source / rel) & POLICY, rel
    for rel in ('cow', 'nocow-dir/data', 'compressed'):
        assert (a / rel).read_bytes() == data
        assert shared(a / rel), rel
    assert (a / 'nocow-dir/data').stat().st_ino == (a / 'nocow-link').stat().st_ino
    assert os.getxattr(a / 'compressed', 'btrfs.compression') == b'zstd'
    assert 'btrfs.compression' not in os.listxattr(a / 'cow')
    assert (a / 'nested-subvolume/included').read_text() == 'nested subvolume contents\n'
    # Subvolume contents are flattened into ordinary directories, so normal lifecycle works.
    assert (a / 'nested-subvolume').stat().st_dev == a.stat().st_dev
    assert not fs('diff', wid, '--full').stdout.strip()
    fs('verify', sid)
    run('exec', wid, '--require-sandbox', '--', '/bin/true')

    (a / 'nocow-dir/data').write_text('private write\n')
    assert (source / 'nocow-dir/data').read_bytes() == data
    sid2 = ident(fs('checkpoint', wid).stdout, 'S')
    fs('pool', 'fill', sid2, '--count', '1')
    # The pool cannot rename across subvolumes; it must fall back to a real reflink clone.
    cross = workspace / 'cross-pool'
    out = fs('fork', '--from', sid2, '--to', cross).stdout
    cross_id = ident(out, 'W')
    assert '(pool)' not in out
    assert (cross / 'nocow-dir/data').read_text() == 'private write\n'
    assert flags(cross / 'nocow-dir/data') & NOCOW
    same = storage / 'same-pool'
    out = fs('fork', '--from', sid2, '--to', same).stdout
    same_id = ident(out, 'W')
    assert '(pool)' in out

    # Discard/restore also crosses the store's subvolume boundary, exercising EXDEV trash.
    fs('discard', cross_id)
    assert not cross.exists()
    fs('restore', cross_id)
    fs('verify', cross_id)
    assert not fs('diff', cross_id, '--full').stdout.strip()

    # Different Btrfs filesystems must still be refused (the CI supplies a second mount).
    other_mount = os.environ.get('WFS_BTRFS_OTHER')
    if other_mount:
        with tempfile.TemporaryDirectory(prefix='wfs-other-', dir=other_mount) as other:
            target = Path(other) / 'world'
            failure = fs('fork', '--from', sid, '--to', target, '--no-pool', ok=False)
            assert 'volume' in failure.stderr.lower(), failure.stderr
            assert not target.exists()
            copied = ident(fs('fork', '--from', sid, '--to', target, '--no-pool', '--copy').stdout, 'W')
            assert (target / 'nocow-dir/data').read_bytes() == data
            assert flags(target / 'nocow-dir/data') & NOCOW
            fs('verify', copied)
            fs('discard', copied, '--now')

    for w in (cross_id, same_id, wid):
        fs('discard', w, '--now')
    fs('pool', 'drain', sid2)
    fs('discard', sid2, '--now')
    fs('discard', sid, '--now')
    fs('gc', '--now', '--retention', '0')
    print('linux_btrfs: subvolumes, readonly source, NOCOW/compression, pool and trash: PASS')
finally:
    # All subvolumes are created inside this private fixture; leave caller mounts untouched.
    for p in reversed(subvolumes):
        if p.exists():
            # Parent read-only state prevents deleting a nested subvolume.
            if source.exists():
                btrfs('property', 'set', source, 'ro', 'false')
            btrfs('subvolume', 'delete', p)
    shutil.rmtree(root)
