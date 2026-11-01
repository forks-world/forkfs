"""Black-box durability-header and daemon ownership tests; no mounted FS needed."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]

def run(*args, ok=True):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, timeout=5)
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout, p.stderr)
    return p

def crc32c(data):
    v = 0xffffffff
    for b in data:
        v ^= b
        for _ in range(8):
            v = (v >> 1) ^ (0x82f63b78 if v & 1 else 0)
    return v ^ 0xffffffff

def repair_crc(buf, start):
    buf[start+4092:start+4096] = crc32c(buf[start:start+4092]).to_bytes(4, 'little')

with tempfile.TemporaryDirectory(prefix='ffd-', dir='/tmp') as tmp:
    root = Path(tmp)
    image = root / 'project.forkfs'
    initial = json.loads(run('init', image).stdout)
    assert initial['state'] == 'object-journal' and initial['transactions_supported']
    original = image.read_bytes()
    assert len(original) == 65536 and original[:8] == b'FORKFS00'
    assert int.from_bytes(original[4092:4096], 'little') == crc32c(original[:4092])
    assert image.stat().st_mode & 0o777 == 0o600
    run('init', image, ok=False)
    assert image.read_bytes() == original
    assert json.loads(run('check', image).stdout)['repository_id'] == initial['repository_id']
    link = root / 'alias'
    link.symlink_to(image)
    run('check', link, ok=False)
    run('init', link, ok=False)
    # Single torn anchor is recoverable; neither inspection nor serving repairs it silently.
    torn = bytearray(original)
    torn[100] ^= 1
    image.write_bytes(torn)
    assert json.loads(run('check', image).stdout)['degraded_anchor']
    assert image.read_bytes() == torn
    torn[4096+100] ^= 1
    image.write_bytes(torn)
    run('check', image, ok=False)
    # A valid future format must not be bypassed via the older anchor.
    future = bytearray(original)
    future[8:10] = (1).to_bytes(2, 'little')
    repair_crc(future, 0)
    image.write_bytes(future)
    run('check', image, ok=False)
    # Same generation with contradictory valid anchors is corruption.
    conflict = bytearray(original)
    conflict[4096+128] ^= 1
    repair_crc(conflict, 4096)
    image.write_bytes(conflict)
    run('check', image, ok=False)
    for bad in (original[:8000], original+bytes(4096)):
        image.write_bytes(bad)
        run('check', image, ok=False)
        assert image.read_bytes() == bad
    image.write_bytes(original)
    endpoint = root / 'control.sock'
    proc = subprocess.Popen([BIN, 'serve', str(image), '--socket', str(endpoint)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None, proc.communicate()
            assert time.monotonic() < deadline
            time.sleep(.01)
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(str(endpoint))
            data = b''
            while chunk := client.recv(1024):
                data += chunk
            assert json.loads(data)['repository_id'] == initial['repository_id']
        run('check', image, ok=False)
        # A different inode path (hard link) still shares the exclusive lock.
        hard = root / 'hardlink'
        os.link(image, hard)
        run('check', hard, ok=False)
        other = root / 'other.forkfs'
        run('init', other)
        run('serve', other, '--socket', endpoint, ok=False)
        assert endpoint.exists()
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=3) == 0
        assert not endpoint.exists()
        run('check', image)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=3)
        proc.communicate()
    assert image.read_bytes() == original
    # SIGKILL releases the kernel lock but leaves the socket path untouched.
    crashed = subprocess.Popen([BIN, 'serve', str(image), '--socket', str(endpoint)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+5
        while not endpoint.exists():
            assert crashed.poll() is None
            assert time.monotonic() < deadline
            time.sleep(.01)
        crashed.kill()
        crashed.wait(timeout=3)
        run('check', image)
        run('serve', image, '--socket', endpoint, ok=False)
        assert endpoint.exists()
        endpoint.unlink()
    finally:
        if crashed.poll() is None:
            crashed.kill()
            crashed.wait(timeout=3)
        crashed.communicate()
    # Never erase an unrelated/stale socket path, even on startup failure.
    endpoint.write_text('keep me')
    run('serve', image, '--socket', endpoint, ok=False)
    assert endpoint.read_text() == 'keep me'
    print('forkfsd lifecycle, ownership and format rejection tests passed')
