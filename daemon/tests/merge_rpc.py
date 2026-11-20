"""Atomic three-way merge of revision-backed Worlds, including conflict fencing."""
from contextlib import contextmanager
import itertools
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile

BIN, FAULT = sys.argv[1:3]


def run(*args, error=None):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=10)
    if error is None:
        assert result.returncode == 0, (args, result.stderr)
    else:
        assert result.returncode != 0 and error in result.stderr.decode(), (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='ff-merge-', dir='/tmp') as directory:
    root = Path(directory)
    database = root / 'db'
    input_file = root / 'input'
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
            try:
                yield daemon, lambda *args, **options: run('request', address, *args, **options)
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                daemon.wait(timeout=10)

    def write(rpc, path, data, branch=None):
        input_file.write_bytes(data)
        selector = ('--world', branch) if branch else ()
        rpc('fs-write', path, input_file, *selector)

    def read(rpc, path, branch, count):
        return b''.join(
            rpc('fs-read-at', path, offset, min(count - offset, 128 * 1024), '--world', branch)
            for offset in range(0, count, 128 * 1024)
        )

    def plan(rpc, branch='target', source='incoming', base='base'):
        return json.loads(rpc('fs-merge-plan', branch, base, source))

    def seq(rpc):
        return json.loads(rpc('status'))['last_committed_seq']

    source_bytes = bytes(range(256)) * 1300
    with serve() as (_, rpc):
        rpc('fs-init')
        write(rpc, '/shared', b'base')
        rpc('fs-mkdir', '/a')
        rpc('fs-mkdir', '/b')
        rpc('fs-snapshot', 'base')
        for name in ('target', 'source', 'fast', 'conflict', 'cycle-src', 'cycle-dst', 'time-src', 'time-dst', 'crash'):
            rpc('fs-fork', 'base', name)

        for offset in range(0, len(source_bytes), 128 * 1024):
            input_file.write_bytes(source_bytes[offset:offset + 128 * 1024])
            rpc('fs-write-at', '/shared', offset, input_file, '--world', 'source')
        write(rpc, '/source.txt', b'from source', 'source')
        rpc('fs-mkdir', '/source-dir', '--world', 'source')
        rpc('fs-snapshot', 'incoming', '--world', 'source')
        write(rpc, '/target.txt', b'from target', 'target')
        rpc('fs-mkdir', '/target-dir', '--world', 'target')
        rpc('fs-snapshot', 'target-base', '--world', 'target')
        rpc('fs-merge-plan', 'target', 'target-base', 'incoming', error='must be an ancestor')

        before = seq(rpc)
        preview = plan(rpc)
        assert preview['state'] == 'ready' and preview == plan(rpc)
        assert seq(rpc) == before
        session = json.loads(rpc('session-open', '15000'))['session']
        rpc('inode-root', '--world', 'target', '--session', session)
        rpc('fs-merge', 'target', 'base', 'incoming', preview['token'], error='active mount')
        rpc('session-close', session)
        session = json.loads(rpc('session-open', '15000'))['session']
        rpc('fs-open', '/shared', 'r', '--world', 'target', '--session', session)
        rpc('fs-merge', 'target', 'base', 'incoming', preview['token'], error='open file handles')
        rpc('session-close', session)
        assert seq(rpc) == before

        write(rpc, '/late.txt', b'new change after preview', 'target')
        before = seq(rpc)
        rpc('fs-merge', 'target', 'base', 'incoming', preview['token'], error='merge plan is stale')
        assert seq(rpc) == before
        preview = plan(rpc)
        result = json.loads(rpc('fs-merge', 'target', 'base', 'incoming', preview['token']))
        assert result['state'] == 'merged' and result['manifest_id'] == preview['manifest_id']
        assert seq(rpc) == before + 1
        for path, content in (('/shared', source_bytes), ('/source.txt', b'from source'),
                              ('/target.txt', b'from target'), ('/late.txt', b'new change after preview')):
            assert read(rpc, path, 'target', len(content) + 1) == content
        root_inode = json.loads(rpc('fs-stat', '/', '--world', 'target'))
        assert root_inode['links'] == 6
        assert plan(rpc)['state'] == 'unchanged'
        rpc('fs-check', '--world', 'target')
        assert rpc('fs-cat', '/shared', '--revision', 'base') == b'base'

        preview = plan(rpc, 'fast')
        assert json.loads(rpc('fs-merge', 'fast', 'base', 'incoming', preview['token']))['state'] == 'merged'
        assert read(rpc, '/shared', 'fast', len(source_bytes) + 1) == source_bytes

        write(rpc, '/shared', b'competing write', 'conflict')
        write(rpc, '/source.txt', b'competing creation', 'conflict')
        before = seq(rpc)
        conflict = plan(rpc, 'conflict')
        assert conflict['state'] == 'conflict' and 'token' not in conflict
        assert {item['kind'] for item in conflict['conflicts']} == {'inode', 'entry'}
        assert rpc('fs-cat', '/shared', '--world', 'conflict') == b'competing write'
        assert seq(rpc) == before

        write(rpc, '/a/only-source', b'edit a entries', 'cycle-src')
        rpc('fs-rename', '/a', '/b/a', '--world', 'cycle-src')
        rpc('fs-snapshot', 'cycle-source', '--world', 'cycle-src')
        write(rpc, '/b/only-target', b'edit b entries', 'cycle-dst')
        rpc('fs-rename', '/b', '/a/b', '--world', 'cycle-dst')
        before = seq(rpc)
        cycle = plan(rpc, 'cycle-dst', 'cycle-source')
        assert cycle['state'] == 'conflict' and cycle['conflicts'][0]['kind'] == 'namespace', cycle
        assert seq(rpc) == before
        rpc('fs-check', '--world', 'cycle-dst')
        rpc('fs-setmtime', '/a', '1000', '--world', 'time-src')
        rpc('fs-snapshot', 'time-source', '--world', 'time-src')
        write(rpc, '/a/new', b'changes only target entries', 'time-dst')
        conflict = plan(rpc, 'time-dst', 'time-source')
        assert conflict['state'] == 'conflict' and conflict['conflicts'][0]['kind'] == 'inode', conflict
        write(rpc, '/only-target.txt', b'keep through crash', 'crash')
        crash_plan = plan(rpc, 'crash')

    for stage in ('leveldb_before_write', 'leveldb_after_write'):
        with serve(FAULT, crash=stage) as (daemon, rpc):
            rpc('fs-merge', 'crash', 'base', 'incoming', crash_plan['token'], error='RPC')
            assert daemon.wait(timeout=5) == 86
        with serve() as (_, rpc):
            assert rpc('fs-cat', '/only-target.txt', '--world', 'crash') == b'keep through crash'
            if stage == 'leveldb_before_write':
                assert plan(rpc, 'crash') == crash_plan
                assert rpc('fs-cat', '/shared', '--world', 'crash') == b'base'
            else:
                assert plan(rpc, 'crash')['state'] == 'unchanged'
                assert read(rpc, '/shared', 'crash', len(source_bytes) + 1) == source_bytes
            rpc('fs-check', '--world', 'crash')
print('three-way merge, conflict and lease admission, and crash atomicity passed')
