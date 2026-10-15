"""Exact recovery endpoints, randomized file histories, preflight and crash retry."""
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from replay_corruption import unpack, pack, h

BIN, FAULT = sys.argv[1:3]
def run(*args, ok=True, crash=None):
    env = dict(os.environ)
    if crash:
        env['FORKFS_TEST_CRASH'] = crash
    p = subprocess.run([FAULT if crash else BIN, *map(str, args)], capture_output=True,
                       timeout=20, env=env)
    expected = (86 if crash else 0) if ok else 1
    assert p.returncode == expected, (args, p.returncode, p.stderr)
    return p.stdout

def sequence(db):
    return json.loads(run('check', db))['last_committed_seq']

def args(point):
    return ('--until', point['sequence'], point['repository'], point['chain_hash'])

with tempfile.TemporaryDirectory(prefix='ff-replay-until-') as directory:
    root = Path(directory)
    for seed in (1, 173, 4294967295):
        rng = random.Random(seed)
        source, base, delta = [root/(str(seed)+suffix) for suffix in ('-source', '-base', '-delta')]
        run('init', source);run('fs-init', source)
        payload = root/'input';payload.write_bytes(b'initial')
        run('fs-write', source, '/file', payload)
        start = int(run('backup', source, base))
        points, model = [], {start: b'initial'}
        for n in range(6):
            value = rng.randbytes(rng.randrange(0, 12000))
            payload.write_bytes(value);run('fs-write', source, '/file', payload)
            model[sequence(source)] = value
            run('fs-snapshot', source, 'r'+str(n))
            model[sequence(source)] = value
            point = json.loads(run('fs-revision-point', source, 'r'+str(n)))['checkpoint']
            points.append(point)
        end = int(run('replay-export', source, start, delta))
        header, frames = unpack(delta.read_bytes())
        anchor = dict(sequence=start, repository=header[9:25].hex(), chain_hash=header[41:73].hex())
        # Every transaction boundary, including the base, is an exact endpoint.
        boundaries = [anchor]+[dict(sequence=struct.unpack_from('<Q', f, 1)[0],
                                   repository=anchor['repository'], chain_hash=h(f).hex()) for f in frames]
        target = root/(str(seed)+'-target')
        run('replay', base, target, *args(anchor))
        for point in boundaries:
            assert int(run('replay', delta, target, *args(point))) == point['sequence']
            assert sequence(target) == point['sequence']
            assert run('fs-cat', target, '/file') == model[point['sequence']]
            assert int(run('replay', delta, target, *args(point))) == point['sequence']
        run('replay', delta, target, *args(points[0]), ok=False)
        run('replay', base, target, *args(anchor), ok=False)
        assert sequence(target) == end
        # Rejected receipts and malformed suffixes must leave the base untouched.
        clean = root/(str(seed)+'-clean');run('replay', base, clean)
        for bad in [dict(points[0], sequence=start-1), dict(points[0], sequence=end+1),
                    dict(points[0], repository='00'*16), dict(points[0], chain_hash='00'*32),
                    dict(points[0], chain_hash='g'*64), dict(points[0], repository='0')]:
            run('replay', delta, clean, *args(bad), ok=False)
            assert sequence(clean) == start
            assert run('fs-cat', clean, '/file') == b'initial'
        for n, image in enumerate([delta.read_bytes()[:-1], delta.read_bytes()+b'extra',
                                    pack(header, frames[:-1]), pack(header, frames+frames[-1:])]):
            bad_archive = root/(str(seed)+'-bad'+str(n));bad_archive.write_bytes(image)
            run('replay', bad_archive, clean, *args(points[0]), ok=False)
            assert sequence(clean) == start
        absent = root/(str(seed)+'-absent')
        run('replay', base, absent, *args(points[0]), ok=False)
        assert not absent.exists()
        # A crash may publish one complete transaction; retry reaches exactly r0.
        for stage in ('replay_before_write', 'replay_after_write'):
            recovered = root/(str(seed)+stage);run('replay', base, recovered)
            run('replay', delta, recovered, *args(points[0]), crash=stage)
            assert sequence(recovered) == start+int(stage=='replay_after_write')
            assert int(run('replay', delta, recovered, *args(points[0]))) == points[0]['sequence']
            assert run('fs-cat', recovered, '/file') == model[points[0]['sequence']]
            run('fs-check', recovered)
        # Empty increments and an ordinary full-range continuation remain valid.
        empty = root/(str(seed)+'-empty');run('replay-export', source, end, empty)
        assert int(run('replay', empty, target, *args(points[-1]))) == end
        assert int(run('replay', delta, clean, *args(points[0]))) == points[0]['sequence']
        assert int(run('replay', delta, clean)) == end
print('exact recovery endpoints, randomized histories, rejected receipts, corrupt tails and crash retry passed')
