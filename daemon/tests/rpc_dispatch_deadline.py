"""A completed RPC dispatch gets a bounded reply window of its own."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

BIN, FAULT = (str(Path(path).resolve()) for path in sys.argv[1:3])

with tempfile.TemporaryDirectory(prefix='ff-rpc-deadline-') as directory:
    root = Path(directory)
    db, endpoint, source = root/'db', root/'rpc.sock', root/'input'
    initialized = subprocess.run([BIN, 'init', str(db)], capture_output=True, timeout=10)
    assert initialized.returncode == 0, initialized.stderr

    env = dict(os.environ)
    env['FORKFS_TEST_RPC_EXPIRE_DURING_DISPATCH'] = '1'
    proc = subprocess.Popen([FAULT, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def request(*args):
        result = subprocess.run([BIN, 'request', str(endpoint), *map(str, args)],
                                capture_output=True, timeout=10)
        assert result.returncode == 0, (args, result.returncode, result.stderr)
        return result.stdout

    try:
        deadline = time.monotonic() + 10
        last_probe = None
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'daemon exited before RPC readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                try:
                    probe = subprocess.run([BIN, 'request', str(endpoint), 'status'],
                                           capture_output=True, timeout=2)
                except subprocess.TimeoutExpired as error:
                    last_probe = error
                else:
                    if probe.returncode == 0:
                        status = json.loads(probe.stdout)
                        assert status['storage_engine'] == 'leveldb', status
                        break
                    if b'Connection refused' in probe.stderr or b'No such file or directory' in probe.stderr:
                        last_probe = (probe.returncode, probe.stderr)
                    else:
                        raise AssertionError(f'RPC readiness probe failed: {probe.stderr!r}')
            if time.monotonic() >= deadline:
                raise AssertionError(f'RPC readiness timed out; probe={last_probe!r}')
            time.sleep(.01)

        request('fs-init')
        old = bytes((i * 19 + 3) % 251 for i in range(96 * 1024))
        current = bytes((i * 29 + 7) % 251 for i in range(112 * 1024))
        source.write_bytes(old)
        request('fs-write', '/checked', source)
        request('fs-snapshot', 'base')
        source.write_bytes(current)
        request('fs-write', '/checked', source)

        before = json.loads(request('status'))['last_committed_seq']
        request('fs-check')
        request('fs-check', '--revision', 'base')
        assert request('fs-cat', '/checked') == current
        assert request('fs-cat', '/checked', '--revision', 'base') == old
        after = json.loads(request('status'))['last_committed_seq']
        assert after == before, (before, after)
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
        endpoint.unlink(missing_ok=True)
