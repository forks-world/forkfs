"""Online branch discard fences live users, stale plans, and interrupted commits."""
from contextlib import contextmanager
import errno
import itertools
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile

BIN, FAULT = sys.argv[1:3]


def run(*args):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    return result.stdout


def read_exact(channel, size):
    data = bytearray()
    while len(data) < size:
        chunk = channel.recv(size - len(data))
        if not chunk:
            raise EOFError('RPC connection ended before the reply')
        data.extend(chunk)
    return bytes(data)


with tempfile.TemporaryDirectory(prefix='ff-discard-', dir='/tmp') as directory:
    root = Path(directory)
    database = root / 'db'
    endpoints = itertools.count()
    run('init', database)

    @contextmanager
    def serve(program=BIN, *, crash=None):
        address = root / f's{next(endpoints)}'
        env = dict(os.environ)
        if crash:
            env['FORKFS_TEST_CRASH'] = crash
        with subprocess.Popen(
            [program, 'serve', str(database), '--socket', str(address), '--rpc'],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as daemon:
            assert select.select([daemon.stdout], [], [], 5)[0], 'forkfsd startup timed out'
            assert json.loads(daemon.stdout.readline())['state'] == 'leveldb'

            def rpc(*args, error=None):
                fields = [arg.encode() if isinstance(arg, str) else arg for arg in args]
                data = struct.pack('<I', len(fields)) + b''.join(
                    struct.pack('<I', len(arg)) + arg for arg in fields
                )
                with socket.socket(socket.AF_UNIX) as conn:
                    conn.settimeout(5)
                    conn.connect(str(address))
                    conn.sendall(b'FFRPC001' + struct.pack('<I', len(data)) + data)
                    magic, size = struct.unpack('<8sI', read_exact(conn, 12))
                    assert magic == b'FFRSP001'
                    code, = struct.unpack('<I', read_exact(conn, 4))
                    reply = read_exact(conn, size - 4)
                    assert code == (0 if error is None else error + 1), (args, code, reply)
                    return reply

            try:
                yield daemon, rpc
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                daemon.wait(timeout=10)

    def plan(rpc):
        return json.loads(rpc('fs-discard-plan', 'work'))

    work = ('--world', 'work')
    other = ('--world', 'other')
    with serve() as (_, rpc):
        rpc('fs-init')
        rpc('fs-write', '/file', b'base')
        rpc('fs-snapshot', 'base')
        rpc('fs-fork', 'base', 'work')
        rpc('fs-fork', 'base', 'other')
        rpc('fs-write', '/file', b'keep this revision', *work)
        rpc('fs-snapshot', 'saved', *work)

        initial = plan(rpc)
        sequence = json.loads(rpc('status'))['last_committed_seq']
        assert plan(rpc) == initial and json.loads(rpc('status'))['last_committed_seq'] == sequence
        rpc('fs-discard-plan', 'main', error=errno.EPERM)
        rpc('fs-discard', 'main', initial['token'], error=errno.EPERM)
        rpc('fs-discard', 'other', initial['token'], error=errno.ESTALE)
        rpc('fs-write', '/file', b'later uncheckpointed edit', *work)
        rpc('fs-discard', 'work', initial['token'], error=errno.ESTALE)

        session = json.loads(rpc('session-open', '15000'))['session']
        select_session = ('--session', session)
        handle = json.loads(rpc('fs-open', '/file', 'rw', *work, *select_session))['handle']
        rpc('fs-discard', 'work', plan(rpc)['token'], error=errno.EBUSY)
        rpc('fs-close', handle, *work, *select_session)
        cursor = json.loads(rpc('fs-dir-open', '/', *work, *select_session))['cursor']
        rpc('fs-discard', 'work', plan(rpc)['token'], error=errno.EBUSY)
        rpc('fs-dir-close', cursor, *work, *select_session)
        rpc('inode-root', *work, *select_session)
        rpc('fs-discard', 'work', plan(rpc)['token'], error=errno.EBUSY)
        rpc('session-close', session)

        foreign = json.loads(rpc('session-open', '15000'))['session']
        rpc('inode-root', *other, '--session', foreign)
        planned = plan(rpc)
        removed = json.loads(rpc('fs-discard', 'work', planned['token']))
        expected = {key: value for key, value in planned.items() if key != 'token'}
        expected['collected_objects'] = 0
        assert removed == expected
        rpc('fs-discard', 'work', planned['token'], error=errno.ENOENT)
        assert rpc('fs-cat', '/file', *other) == b'base'
        assert rpc('fs-cat', '/file', '--revision', 'saved') == b'keep this revision'
        rpc('session-close', foreign)

        # Recreate an identical branch state and prove the older preview is fenced.
        rpc('fs-fork', 'base', 'work')
        same_tree = plan(rpc)
        rpc('fs-discard', 'work', same_tree['token'])
        rpc('fs-fork', 'base', 'work')
        current = plan(rpc)
        assert current['manifest_id'] == same_tree['manifest_id']
        assert current['base_revision_id'] == same_tree['base_revision_id']
        rpc('fs-discard', 'work', same_tree['token'], error=errno.ESTALE)
        rpc('fs-check', *work)

    for stage in ('leveldb_before_write', 'leveldb_after_write'):
        with serve(FAULT, crash=stage) as (daemon, rpc):
            try:
                rpc('fs-discard', 'work', current['token'])
            except (EOFError, ConnectionResetError):
                assert daemon.wait(timeout=5) == 86
            else:
                raise AssertionError('discard returned before the injected crash')
        with serve() as (_, rpc):
            worlds = rpc('fs-worlds').splitlines()
            if stage == 'leveldb_before_write':
                assert b'work' in worlds and plan(rpc)['token'] == current['token']
            else:
                assert b'work' not in worlds
                rpc('fs-discard', 'work', current['token'], error=errno.ENOENT)
                rpc('fs-fork', 'base', 'work')  # The base reference was removed too.
            assert rpc('fs-cat', '/file', '--revision', 'saved') == b'keep this revision'
            rpc('fs-check', *work)
print('online discard admission, stale token fencing, retained roots, and crash recovery passed')
