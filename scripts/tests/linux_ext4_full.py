#!/usr/bin/env python3
"""ENOSPC rollback/retry on a dedicated bounded ext4 CI volume; never mounts/formats."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('world', type=Path)
parser.add_argument('--scratch', required=True, type=Path)
args = parser.parse_args()
world = args.world.resolve()
assert subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(args.scratch)], text=True).strip() == 'ext4'
env = dict(os.environ, WORLD_POOL_TOPUP='0', WORLD_GC_CREATING_MIN_AGE='0')
with tempfile.TemporaryDirectory(prefix='wfs-ext4-full-', dir=args.scratch) as tmp:
    root = Path(tmp)
    store, src, dst, reserve = (root / n for n in ('store', 'source', 'world', 'reserve'))
    src.mkdir()
    # Written data, not unwritten fallocate extents that SEEK_HOLE could skip.
    with (src / 'data').open('wb') as f:
        block = b'x' * (1024 * 1024)
        for _ in range(320):
            f.write(block)
        f.flush()
        os.fsync(f.fileno())

    def run(*argv, ok=True):
        p = subprocess.run([str(world), '--store', str(store), 'fs', *map(str, argv)],
                           env=env, text=True, capture_output=True)
        assert (p.returncode == 0) == ok, (argv, p.stdout, p.stderr)
        return p

    def ident(text, prefix):
        return re.search(rf'\b{prefix}\d+\b', text).group()

    def exhausted(*argv):
        # Stay above the 256 MiB preflight floor but below one file's copy size.
        available = shutil.disk_usage(root).free
        assert available > 300 * 1024 * 1024, available
        try:
            with reserve.open('wb') as f:
                os.posix_fallocate(f.fileno(), 0, available - 280 * 1024 * 1024)
                os.fsync(f.fileno())
            p = run(*argv, ok=False)
            assert 'No space left' in p.stderr, (argv, p.stderr)
        finally:
            reserve.unlink(missing_ok=True)
        run('gc', '--now', '--retention', '0')
        assert not list(root.glob('.wfs-*')), list(root.iterdir())

    try:
        exhausted('init', src)
        sid = ident(run('init', src).stdout, 'S')
        exhausted('fork', '--from', sid, '--to', dst, '--no-pool')
        assert not dst.exists()
        run('verify', sid)
        exhausted('pool', 'fill', sid, '--count', '1')
        run('verify', sid)
        run('pool', 'fill', sid, '--count', '1')
        run('pool', 'drain', sid)
        run('gc', '--now', '--retention', '0')
        wid = ident(run('fork', '--from', sid, '--to', dst, '--no-pool').stdout, 'W')
        shutil.rmtree(src)
        exhausted('checkpoint', wid)
        run('verify', sid)
        run('verify', wid)
        sid2 = ident(run('checkpoint', wid).stdout, 'S')
        run('verify', sid2)
        assert (dst / 'data').stat().st_size == 320 * 1024 * 1024
        with (dst / 'data').open('rb') as f:
            while chunk := f.read(1024 * 1024):
                assert chunk == block
        print('linux_ext4_full: init/fork/pool/checkpoint ENOSPC rollback and retry: PASS')
    finally:
        for parent, dirs, _ in os.walk(root):
            for name in dirs:
                path = Path(parent) / name
                if not path.is_symlink():
                    path.chmod(0o700)
