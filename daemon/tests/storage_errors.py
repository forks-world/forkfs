"""Preserve storage errno, fail closed, and recover whole old/new publications."""
import errno
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

BIN, FAULT = sys.argv[1:3]


def command(*args):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=30)
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='ff-io-storage-', dir='/tmp') as temporary:
    root = Path(temporary)
    db, baseline, data, endpoint, armed = [root / name for name in ['db', 'baseline', 'input', 'control.sock', 'armed']]
    old, new = b'O' * 4096, b'N' * 12000
    data.write_bytes(old)
    command('init', db)
    command('fs-init', db)
    command('fs-write', db, '/file', data)
    command('fs-snapshot', db, 'base')
    shutil.copytree(db, baseline)

    def exact(stream, count):
        result = b''
        while len(result) < count:
            part = stream.recv(count - len(result))
            assert part
            result += part
        return result

    def rpc(*fields, code=0):
        fields = [value if isinstance(value, bytes) else str(value).encode() for value in fields]
        payload = struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(value)) + value for value in fields)
        with socket.socket(socket.AF_UNIX) as stream:
            stream.settimeout(20)
            stream.connect(str(endpoint))
            stream.sendall(b'FFRPC001' + struct.pack('<I', len(payload)) + payload)
            header = exact(stream, 12)
            assert header[:8] == b'FFRSP001'
            response = exact(stream, struct.unpack('<I', header[8:])[0])
            actual = struct.unpack('<I', response[:4])[0]
            assert actual == code, (fields[:3], actual, code, response[4:])
            return response[4:]

    for stage in ['wal-append', 'wal-sync']:
        for storage_errno in [errno.ENOSPC, errno.EDQUOT, errno.EIO]:
            for operation in ['fs-hwrite', 'inode-setattr']:
                shutil.rmtree(db)
                shutil.copytree(baseline, db)
                armed.unlink(missing_ok=True)
                process = subprocess.Popen([FAULT, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           env={**os.environ, 'FORKFS_TEST_IO_ERROR': stage + ':' + str(storage_errno),
                                                'FORKFS_TEST_IO_ERROR_FILE': str(armed)})
                try:
                    assert select.select([process.stdout], [], [], 5)[0]
                    assert json.loads(process.stdout.readline())['state'] == 'leveldb'
                    session = json.loads(rpc('session-open', 60000))['session']
                    selection = ('--session', session)
                    rpc('inode-root', *selection)
                    handle = json.loads(rpc('fs-open', '/file', 'rw', *selection))['handle']
                    inode = json.loads(rpc('fs-hstat', handle, *selection))['inode_id']
                    # Warm verified object/root caches and retained-view reads.
                    assert rpc('fs-cat', '/file') == old
                    assert rpc('fs-cat', '/file', '--revision', 'base') == old
                    assert rpc('fs-hread', handle, 0, len(old), *selection) == old
                    rpc('fs-cat', '/missing', code=1)  # Logical errors keep the existing generic contract.
                    assert not json.loads(rpc('status'))['requires_reopen']
                    armed.touch()
                    if operation == 'fs-hwrite':
                        rpc(operation, handle, 0, new, *selection, code=storage_errno + 1)
                        expected = new
                    else:
                        rpc(operation, inode, '-', 8192, '-', handle, *selection, code=storage_errno + 1)
                        expected = old + bytes(4096)
                    failed = json.loads(rpc('status'))
                    assert failed['requires_reopen']
                    cached = json.loads(rpc('storage-profile'))['storage_metrics']
                    rpc('session-keepalive', session, code=errno.EIO + 1)
                    rpc('inode-sync', *selection, code=errno.EIO + 1)
                    armed.unlink()
                    for request in [
                        ('fs-hwrite', handle, 0, b'retry', *selection),
                        ('fs-hwriteback', handle, 0, b'retry', *selection),
                        ('fs-htruncate', handle, 0, *selection),
                        ('fs-hread', handle, 0, len(old), *selection),
                        ('fs-hstat', handle, *selection),
                        ('fs-cat', '/file'), ('fs-cat', '/file', '--revision', 'base'),
                        ('fs-stat', '/file'), ('fs-ls', '/'), ('fs-check',),
                        ('inode-stat', inode, *selection),
                    ]:
                        response = rpc(*request, code=errno.EIO + 1)
                        assert b'requires reopen' in response
                    after = json.loads(rpc('status'))
                    for key in ['last_committed_seq', 'indexed_objects', 'object_roots']:
                        assert after[key] == failed[key], (key, after, failed)
                    assert after['requires_reopen'] and process.poll() is None
                    profile = json.loads(rpc('storage-profile'))['storage_metrics']
                    assert all(profile[key] == cached[key] for key in ['object_cache_hits', 'root_cache_hits'])
                finally:
                    process.terminate()
                    process.wait(timeout=10)
                    if endpoint.exists():
                        endpoint.unlink()
                # Sync failure may leave a complete WAL record. Accept an
                # entire old or new publication, never mixed inode/data/index.
                recovered = command('fs-cat', db, '/file')
                assert recovered in [old, expected]
                if stage == 'wal-append':
                    assert recovered == old
                assert json.loads(command('fs-stat', db, '/file'))['allocated_size'] == (len(recovered) + 511) // 512 * 512
                assert command('fs-cat', db, '/file', '--revision', 'base') == old
                command('fs-check', db)
                command('check', db)
                data.write_bytes(b'recovered')
                command('fs-write', db, '/file', data)
                assert command('fs-cat', db, '/file') == b'recovered'
                assert not json.loads(command('check', db))['requires_reopen']
    print('12 WAL append/sync scenarios preserve initial errno, return EIO after poison including warm caches, and recover atomic publications')
