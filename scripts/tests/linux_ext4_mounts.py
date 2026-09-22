#!/usr/bin/env python3
"""Ext4 nested-filesystem refusal, including mount points with no regular files."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('world', type=Path)
parser.add_argument('--scratch', type=Path, required=True)
args = parser.parse_args()
world = args.world.resolve()
env = dict(os.environ, WORLD_POOL_TOPUP='0')
with tempfile.TemporaryDirectory(prefix='wfs-ext4-mounts-', dir=args.scratch) as temp:
    root = Path(temp).resolve()
    src, store, live = (root / n for n in ('source', 'store', 'live'))
    src.mkdir()
    (src / 'mount').mkdir()

    def run(*argv):
        p = subprocess.run([str(world), '--store', str(store), 'fs', *map(str, argv)],
                           env=env, text=True, capture_output=True)
        assert p.returncode == 0, (argv, p.stderr)
        return p.stdout

    try:
        sid = re.search(r'\bS\d+\b', run('init', src)).group()
        wid = re.search(r'\bW\d+\b', run('fork', '--from', sid, '--to', live, '--no-pool')).group()
        # Execute trusted fixture operations inside a child namespace with private tmpfs
        # mounts. No privileged mount helpers, persistent mounts or external data needed.
        for kind in ('empty', 'symlink', 'fifo'):
            code = '''
import os, pathlib, subprocess, sys
world, store, src, live, wid, dst, kind = sys.argv[1:]
for parent in (src, live):
    mount = pathlib.Path(parent) / 'mount'
    assert mount.stat().st_dev != pathlib.Path(parent).stat().st_dev
    if kind == 'symlink': (mount / 'link').symlink_to('missing')
    if kind == 'fifo': os.mkfifo(mount / 'fifo')
for argv in [('init', src), ('checkpoint', wid),
             ('fork', '--from', wid, '--to', dst, '--no-pool')]:
    p = subprocess.run([world, '--store', store, 'fs', *argv], capture_output=True, text=True)
    assert p.returncode != 0, (kind, argv, p.stdout, p.stderr)
    assert 'cross-device' in p.stderr.lower() or 'cross-volume' in p.stderr.lower(), p.stderr
p = subprocess.run([world, '--store', store, 'fs', 'fork', '--from', wid,
                    '--to', dst, '--no-pool', '--copy'], capture_output=True, text=True)
assert p.returncode == 0, p.stderr
mount = pathlib.Path(dst) / 'mount'
assert mount.is_dir()
if kind == 'symlink': assert (mount / 'link').is_symlink()
if kind == 'fifo':
    import stat
    assert stat.S_ISFIFO((mount / 'fifo').stat().st_mode)
'''
            p = subprocess.run(['/usr/bin/bwrap', '--unshare-user', '--unshare-pid',
                                '--ro-bind', '/', '/', '--proc', '/proc',
                                '--bind', str(root), str(root),
                                '--tmpfs', str(src / 'mount'), '--tmpfs', str(live / 'mount'),
                                sys.executable, '-c', code, str(world), str(store), str(src),
                                str(live), wid, str(root / ('copy-' + kind)), kind],
                               env=env, text=True, capture_output=True)
            assert p.returncode == 0, p.stderr
        print('linux_ext4_mounts: nested empty/symlink/FIFO mount refusal and explicit copy: PASS')
    finally:
        for parent, dirs, _ in os.walk(root):
            for name in dirs:
                path = Path(parent) / name
                if not path.is_symlink():
                    path.chmod(0o700)
