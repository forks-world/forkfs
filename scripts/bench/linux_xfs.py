#!/usr/bin/env python3
"""Reproducible XFS lifecycle timings; scratch must be on the volume under test."""
import argparse
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('world', type=Path)
p.add_argument('--scratch', type=Path, default=Path.cwd())
p.add_argument('--entries', type=int, nargs='+', default=[1000, 10000])
p.add_argument('--runs', type=int, default=3)
a = p.parse_args()
if a.runs < 1 or any(n < 1 for n in a.entries):
    p.error('entries and runs must be positive')
world = a.world.resolve()
env = dict(os.environ, WORLD_POOL_TOPUP='0')
print('files,init_ms,fork_ms,pool_fork_ms,diff_clean_ms,exec_ms (median)')
for count in a.entries:
    with tempfile.TemporaryDirectory(prefix='wfs-bench-', dir=a.scratch.resolve()) as temp:
        root = Path(temp)
        src, store = root / 'source', root / 'store'
        src.mkdir()
        for i in range(count):
            directory = src / f'd{i // 100:05}'
            directory.mkdir(exist_ok=True)
            (directory / f'f{i:06}').write_bytes(b'x' * 4096)

        def run(*args):
            start = time.perf_counter()
            result = subprocess.run([str(world), '--store', str(store), *map(str, args)],
                                    env=env, capture_output=True, text=True, check=True)
            return result.stdout, (time.perf_counter() - start) * 1000

        samples = [[] for _ in range(5)]
        for i in range(a.runs):
            out, elapsed = run('fs', 'init', src)
            sid = re.search(r'\bS\d+\b', out).group()
            samples[0].append(elapsed)
            dst = root / 'world'
            out, elapsed = run('fs', 'fork', '--from', sid, '--to', dst, '--no-pool')
            wid = re.search(r'\bW\d+\b', out).group()
            samples[1].append(elapsed)
            samples[3].append(run('fs', 'diff', wid, '--full')[1])
            samples[4].append(run('exec', wid, '--', '/bin/true')[1])
            run('fs', 'discard', wid, '--now')
            run('fs', 'pool', 'fill', sid, '--count', '1')
            out, elapsed = run('fs', 'fork', '--from', sid, '--to', dst)
            assert '(pool)' in out
            samples[2].append(elapsed)
            wid = re.search(r'\bW\d+\b', out).group()
            run('fs', 'discard', wid, '--now')
            run('fs', 'discard', sid, '--now')
        print(str(count) + ',' + ','.join(f'{statistics.median(s):.2f}' for s in samples), flush=True)
