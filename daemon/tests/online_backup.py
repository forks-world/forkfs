"""Online export with a live owner, concurrent mutations and independent recovery."""
import json
import random
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BIN = str(Path(sys.argv[1]).resolve())

def cli(*args, ok=True):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=20)
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(prefix='ff-online-') as directory:
    root = Path(directory)
    db, endpoint = root/'db', root/'rpc.sock'
    cli('init', db)

    def request(*args, ok=True):
        return cli('request', endpoint, *args, ok=ok)

    def obj(*args):
        return json.loads(request(*args))

    def start():
        proc = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic()+10
        while not endpoint.exists():
            assert proc.poll() is None, proc.communicate()
            assert time.monotonic()<deadline
            time.sleep(.01)
        return proc

    def finish(name, state='durable'):
        deadline = time.monotonic()+20
        while True:
            result = obj('backup-status', name)
            if result['state'] != 'running':
                assert result['state'] == state, result
                assert (result['sequence'] is not None) == (state == 'durable'), result
                return result
            assert time.monotonic()<deadline, result
            time.sleep(.01)

    proc = start()
    try:
        request('fs-init')
        request('backup-start', 'relative', 'relative.pack', ok=False)
        request('backup-status', 'unknown', ok=False)
        # Same daemon owns LevelDB throughout; an offline backup must not open it.
        cli('backup', db, root/'offline.pack', ok=False)
        for seed in (1, 173, 4294967295):
            rng = random.Random(seed)
            payload = root/'input'
            before = bytes(rng.randrange(256) for _ in range(8192))
            payload.write_bytes(before);request('fs-write', '/file', payload)
            revision = 'r'+str(seed);request('fs-snapshot', revision)
            request_id = 'base'+str(seed);base = root/(request_id+'.pack')
            obj('backup-start', request_id, base)
            # Writes race the snapshot capture/export. A base must contain one
            # complete file version, while the later delta must recover the last.
            versions = [before]
            for i in range(8):
                value = bytes(rng.randrange(256) for _ in range(rng.randrange(1,8193)))
                versions.append(value);payload.write_bytes(value)
                request('fs-write', '/file', payload)
            status = finish(request_id)
            image = base.read_bytes()
            assert obj('backup-start', request_id, base) == status
            assert base.read_bytes() == image
            request('backup-start', request_id, root/'other.pack', ok=False)
            recovered = root/('recovered'+str(seed))
            assert int(cli('replay', base, recovered)) == status['sequence']
            assert cli('fs-cat', recovered, '/file') in versions
            assert cli('fs-cat', recovered, '/file', '--revision', revision) == before
            delta = root/('delta'+str(seed)+'.pack');delta_id='delta'+str(seed)
            obj('replay-export-start', delta_id, status['sequence'], delta)
            end = finish(delta_id)
            assert int(cli('replay', delta, recovered)) == end['sequence']
            assert cli('fs-cat', recovered, '/file') == versions[-1]
            cli('fs-check', recovered);cli('check', recovered)
            assert obj('replay-export-start', delta_id, status['sequence'], delta) == end
            request('replay-export-start', delta_id, status['sequence']+1, delta, ok=False)
            cli('replay', delta, recovered)  # Idempotent replay remains supported.

        occupied = root/'occupied.pack';occupied.write_bytes(b'keep this file')
        obj('backup-start', 'occupied', occupied)
        failed = finish('occupied', 'failed');assert failed['error']
        assert occupied.read_bytes() == b'keep this file'
        assert obj('backup-start', 'occupied', occupied) == failed
        obj('replay-export-start', 'future', 2**64-1, root/'future.pack')
        assert finish('future', 'failed')['error']
        # Failed jobs release their slot; graceful shutdown joins an active worker.
        final = root/'final.pack';obj('backup-start', 'final', final)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill();proc.wait();raise
    cli('replay', final, root/'shutdown-recovered')
    proc = start()
    try:
        request('backup-status', 'final', ok=False)
        # Restart loses only task bookkeeping; never overwrites the published pack.
        saved = final.read_bytes();obj('backup-start', 'final', final)
        finish('final', 'failed');assert final.read_bytes() == saved
        assert request('fs-cat', '/file') == versions[-1]
    finally:
        proc.terminate();proc.wait(timeout=20)
print('online backup/replay, concurrent writes, retries, failure and shutdown passed')
