"""Exercise standalone control against real offline and resident repositories."""
import json
from pathlib import Path
import socket
import subprocess
import struct
import threading
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
script = Path(__file__).resolve().parents[1] / 'forkfs.py'
with tempfile.TemporaryDirectory(prefix='ff-control-', dir='/tmp') as temp:
    root = Path(temp)
    repository = root / 'repo with spaces'
    endpoint = root / 'rpc.sock'
    payload = root / 'input with spaces'
    prefix = [sys.executable, str(script), '--binary', str(binary)]
    def call(*args, endpoint_mode=False, branch=None, revision=None, expected=0):
        target = ['--socket', str(endpoint)] if endpoint_mode else ['--repo', str(repository)]
        selector = ['--branch', branch] if branch else ['--revision', revision] if revision else []
        result = subprocess.run(prefix + target + selector + list(args), capture_output=True, timeout=40)
        assert result.returncode == expected, (args, result.returncode, result.stderr)
        return result.stdout
    call('init')
    call('mkdir', '/dir')
    original = b'\x00binary\xff\n'
    payload.write_bytes(original)
    call('write', '/dir/a', str(payload))
    assert call('read', '/dir/a') == original
    for selector in ['--branch', '--revision']:
        result = subprocess.run(prefix + ['--repo', str(repository), selector, '', 'unlink', '/dir/a'], capture_output=True, timeout=40)
        assert result.returncode == 2 and b'selectors must not be empty' in result.stderr, result.stderr
        assert call('read', '/dir/a') == original
    call('snapshot', 'base')
    call('fork', 'base', 'child')
    payload.write_bytes(b'child')
    call('write', '/dir/a', str(payload), branch='child')
    assert call('read', '/dir/a') == original
    assert call('read', '/dir/a', branch='child') == b'child'
    call('restore', 'base', branch='child')
    assert call('read', '/dir/a', branch='child') == original
    assert call('branches').splitlines() == [b'main', b'child']
    call('write', '/dir/a', str(payload), revision='base', expected=1)
    mountpoint = root / 'never-created'
    call('mount', str(mountpoint), expected=3)
    call('unmount', str(mountpoint), expected=3)
    assert not mountpoint.exists()
    assert call('read', '/dir/a', revision='base') == original
    call('init', expected=1)  # No overwrite of an existing repository.
    call('check')
    shared = root / 'shared-runtime'
    shared.mkdir(mode=0o755)
    shared.chmod(0o755)
    rejected = subprocess.run(prefix + ['--repo', str(repository), 'serve', '--listen', str(shared / 'control.sock')], capture_output=True, timeout=10)
    assert rejected.returncode == 1 and b'socket parent must be a private directory' in rejected.stderr, rejected.stderr
    assert not (shared / 'control.sock').exists()
    proc = subprocess.Popen(prefix + ['--repo', str(repository), 'serve', '--listen', str(endpoint)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while True:
            assert proc.poll() is None, proc.communicate()
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(.1)
                    connection.connect(str(endpoint))
                break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(.01)
        assert json.loads(call('status', endpoint_mode=True))['storage_engine'] == 'leveldb'
        call('rename', '/dir/a', '/dir/b', endpoint_mode=True)
        assert call('read', '/dir/b', endpoint_mode=True) == original
        call('snapshot', 'later', endpoint_mode=True)
        call('restore', 'base', endpoint_mode=True)
        assert call('list', '/dir', endpoint_mode=True) == b'a\n'
        assert call('read', '/dir/b', revision='later', endpoint_mode=True) == original
        call('compact', endpoint_mode=True)
        call('check', endpoint_mode=True)
        proc.terminate()
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.communicate()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        proc.communicate(timeout=5)
    assert call('read', '/dir/a') == original
    # A real RPC client must classify its own 10-second deadline as timeout.
    stalled_endpoint = root / 'stalled.sock'
    received, errors = [], []
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(stalled_endpoint))
        listener.listen(1)
        listener.settimeout(15)
        def stall():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(15)
                    def exact(count):
                        data = b''
                        while len(data) < count:
                            chunk = connection.recv(count - len(data))
                            assert chunk, 'request truncated'
                            data += chunk
                        return data
                    header = exact(12)
                    assert header[:8] == b'FFRPC001', header
                    size = struct.unpack('<I', header[8:])[0]
                    assert size <= 1024, size
                    received.append(exact(size))
                    assert connection.recv(1) == b'', 'client did not close after timeout'
            except Exception as error:
                errors.append(error)
        server = threading.Thread(target=stall, daemon=True)
        server.start()
        try:
            result = subprocess.run(prefix + ['--socket', str(stalled_endpoint), 'status'], capture_output=True, timeout=20)
            assert result.returncode == 124, (result.returncode, result.stderr)
            assert b'forkfsd: RPC timeout\n' in result.stderr, result.stderr
            assert b'commit outcome' in result.stderr, result.stderr
        finally:
            server.join(timeout=16)
        assert not server.is_alive() and received and not errors, errors
print('standalone CLI/RPC control, branch isolation, restore and unavailable mount passed')
