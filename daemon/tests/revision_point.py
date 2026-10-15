"""Compare revision receipts with independently decoded retained transactions."""
import json
import random
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from replay_corruption import unpack, operations, h

binary = str(Path(sys.argv[1]).resolve())
def run(*args, ok=True):
    p = subprocess.run([binary, *map(str, args)], capture_output=True, timeout=20)
    assert (p.returncode == 0) == ok, (args, p.stderr)
    return p.stdout

def point(db, name):
    return json.loads(run('fs-revision-point', db, name))

with tempfile.TemporaryDirectory(prefix='ff-revision-point-') as directory:
    root = Path(directory)
    db, base, delta = root/'db', root/'base', root/'delta'
    run('init', db);run('fs-init', db)
    run('fs-snapshot', db, 'old')
    assert point(db, 'old')['reason'] == 'retention_disabled'
    start = int(run('backup', db, base))
    assert point(db, 'old')['reason'] == 'creation_not_retained'
    expected = {}
    rng = random.Random(173)
    payload = root/'input'
    for n in range(12):
        data = rng.randbytes(rng.randrange(0, 8193))
        payload.write_bytes(data);run('fs-write', db, '/file', payload)
        name = 'r'+str(n);run('fs-snapshot', db, name)
        expected[name] = data
    run('replay-export', db, start, delta)
    header, frames = unpack(delta.read_bytes())
    receipts = {}
    for name in expected:
        result = point(db, name)
        key = b'R/\0forkfs-history/revision/'+name.encode()
        matches = [f for f in frames if any(k == key and v.hex() == result['revision_id']
                   for kind, k, v in operations(f))]
        assert len(matches) == 1
        event = matches[0]
        assert result['available']
        assert result['checkpoint'] == dict(repository=header[9:25].hex(),
            sequence=struct.unpack_from('<Q', event, 1)[0], chain_hash=h(event).hex())
        receipts[name] = result
    target = root/'target'
    run('replay', base, target);run('replay', delta, target)
    for name, data in expected.items():
        assert point(target, name) == receipts[name]
        assert run('fs-cat', target, '/file', '--revision', name) == data
    full = root/'full';run('backup', db, full)
    latest = root/'latest';run('replay', full, latest)
    assert point(latest, 'r11')['reason'] == 'creation_not_retained'
    run('fs-fork', latest, 'r11', 'restored')
    assert run('fs-cat', latest, '/file', '--world', 'restored') == expected['r11']
    endpoint = root/'rpc.sock'
    proc = subprocess.Popen([binary, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+10
        while not endpoint.exists():
            assert proc.poll() is None, proc.communicate()
            assert time.monotonic()<deadline
            time.sleep(.01)
        for name in expected:
            for _ in range(2):
                assert json.loads(run('request', endpoint, 'fs-revision-point', name)) == receipts[name]
        run('request', endpoint, 'fs-revision-point', 'missing', ok=False)
    finally:
        proc.terminate();proc.wait(timeout=20)
    assert point(db, 'r11') == receipts['r11']
print('revision receipts match retained events across restart, RPC, backup and replay')
