"""Multi-archive selection and atomic publication under gaps, forks and crashes."""
import json
import os
import random
import shutil
import subprocess
import struct
import sys
import tempfile
from pathlib import Path
from replay_corruption import unpack, pack, base_rows, base_frames

BIN, FAULT = sys.argv[1:3]
def run(*args, ok=True, crash=None):
    env = dict(os.environ)
    if crash:
        env['FORKFS_TEST_CRASH'] = crash
    p = subprocess.run([FAULT if crash else BIN, *map(str, args)], env=env,
                       capture_output=True, timeout=30)
    assert p.returncode == ((86 if crash else 0) if ok else 1), (args, p.returncode, p.stderr)
    return p.stdout

def receipt(db, revision):
    return json.loads(run('fs-revision-point', db, revision))['checkpoint']

def args(p):
    return (p['sequence'], p['repository'], p['chain_hash'])

def plan(p, archives):
    return json.loads(run('restore-plan', *args(p), *archives))['steps']

with tempfile.TemporaryDirectory(prefix='ff-restore-plan-') as directory:
    root = Path(directory)
    for seed in (1, 173, 4294967295):
        folder = root/str(seed);folder.mkdir();rng = random.Random(seed)
        source, base, first, second, middle = [folder/n for n in ('source', 'base', 'first', 'second', 'middle')]
        run('init', source);run('fs-init', source)
        payload = folder/'input';payload.write_bytes(b'base');run('fs-write', source, '/file', payload)
        start = int(run('backup', source, base))
        # Same repository UUID, different lineage at identical sequence numbers.
        branch = folder/'branch';shutil.copytree(source, branch)
        values = []
        for i in range(2):
            value = rng.randbytes(rng.randrange(1, 5000));values.append(value)
            payload.write_bytes(value);run('fs-write', source, '/file', payload)
        run('fs-snapshot', source, 'middle');mid = receipt(source, 'middle')
        run('replay-export', source, start, first);run('backup', source, middle)
        for i in range(2):
            payload.write_bytes(b'branch'+bytes([i]));run('fs-write', branch, '/file', payload)
        run('fs-snapshot', branch, 'middle')
        dead_base, dead_delta = folder/'dead-base', folder/'dead-delta'
        run('backup', branch, dead_base);run('replay-export', branch, start, dead_delta)
        final = rng.randbytes(7000);payload.write_bytes(final);run('fs-write', source, '/file', payload)
        run('fs-snapshot', source, 'target');target = receipt(source, 'target')
        payload.write_bytes(b'past target');run('fs-write', source, '/file', payload)
        run('replay-export', source, mid['sequence'], second)
        # Newest *reachable* base wins. A newer dead branch cannot hide an older path.
        inputs = [base, first, second, dead_base, dead_delta]
        expected = [str(p) for p in (base, first, second)]
        for _ in range(5):
            rng.shuffle(inputs)
            steps = plan(target, inputs)
            assert [s['archive'] for s in steps] == expected
            assert steps[-1]['sequence'] == target['sequence']
            assert steps[-1]['chain_hash'] == target['chain_hash']
        assert [s['archive'] for s in plan(target, inputs+[middle])] == [str(middle), str(second)]
        assert [s['archive'] for s in plan(mid, inputs+[middle])] == [str(middle)]
        recovered = folder/'recovered'
        assert int(run('restore', recovered, *args(target), *inputs)) == target['sequence']
        assert run('fs-cat', recovered, '/file') == final
        assert receipt(recovered, 'target') == target
        run('fs-check', recovered);run('check', recovered)
        run('restore', recovered, *args(target), *inputs, ok=False)
        assert run('fs-cat', recovered, '/file') == final
        # Gaps, wrong branches, wrong receipts and malformed supplied archives fail closed.
        for n, archives in enumerate(([base, second], [dead_base, second], [base, dead_delta, second])):
            absent = folder/('gap'+str(n))
            run('restore-plan', *args(target), *archives, ok=False)
            run('restore', absent, *args(target), *archives, ok=False);assert not absent.exists()
        run('restore-plan', *args(dict(target, chain_hash='00'*32)), *inputs, ok=False)
        corrupt = folder/'corrupt';corrupt.write_bytes(second.read_bytes()[:-1])
        run('restore', folder/'bad', *args(target), *inputs, corrupt, ok=False)
        assert not (folder/'bad').exists()
        foreign = folder/'foreign';run('init', foreign)
        foreign_pack = folder/'foreign-pack';run('backup', foreign, foreign_pack)
        run('restore-plan', *args(target), *inputs, foreign_pack, ok=False)
        # A validly framed base with a missing referenced object fails during staging.
        header, frames = unpack(base.read_bytes());rows = base_rows(frames)
        root_key = next(k for k in rows if k.startswith(b'R/'))
        del rows[b'O/'+rows[root_key]]
        count, = struct.unpack('<Q', rows[b'M/objects'])
        rows[b'M/objects'] = struct.pack('<Q', count-1)
        bad_graph = folder/'bad-graph';bad_graph.write_bytes(pack(header, base_frames(rows)))
        absent = folder/'graph-failed'
        run('restore', absent, *args(target), bad_graph, first, second, ok=False)
        assert not absent.exists() and not list(folder.glob('graph-failed.restore-job-*'))
        # JSON paths and duplicate candidate inputs remain deterministic.
        quoted = folder/'quoted"\nbase';shutil.copyfile(base, quoted)
        assert plan(target, [quoted, quoted, first, second])[0]['archive'] == str(quoted)
        link = folder/'linked-base';link.symlink_to(base)
        run('restore-plan', *args(target), link, first, second, ok=False)
        sentinel = folder/'sentinel';sentinel.write_bytes(b'keep')
        run('restore', sentinel, *args(target), *inputs, ok=False)
        assert sentinel.read_bytes() == b'keep'
        for stage, published in [('restore_after_step', False), ('restore_before_publish', False), ('restore_after_publish', True)]:
            dest = folder/stage
            run('restore', dest, *args(target), *inputs, crash=stage)
            assert dest.exists() == published
            if not published:
                assert int(run('restore', dest, *args(target), *inputs)) == target['sequence']
            assert run('fs-cat', dest, '/file') == final
            assert json.loads(run('check', dest))['last_committed_seq'] == target['sequence']
print('deterministic multi-archive plans, forks/gaps, staging failures and crash publication passed')
