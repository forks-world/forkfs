"""Online GC durable phase markers recover across process crashes."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

binary, fault_binary = sys.argv[1:3]

def run(executable, *args, env=None, code=0):
    result = subprocess.run([executable, *map(str, args)], capture_output=True,
                            timeout=20, env=env)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout

def crash_mark_barrier(stage, endpoint_name, root, baseline, payload, binary, fault_binary):
    repository = root / stage
    shutil.copytree(baseline, repository)
    endpoint = root / endpoint_name
    proc = subprocess.Popen([fault_binary, 'serve', str(repository), '--socket', str(endpoint), '--rpc'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, FORKFS_TEST_CRASH=stage))
    try:
        deadline = time.monotonic() + 10
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'{stage} daemon exited before readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                probe = subprocess.run([binary, 'request', str(endpoint), 'gc-status'],
                                       capture_output=True, timeout=2)
                if probe.returncode == 0:
                    break
            if time.monotonic() >= deadline:
                raise AssertionError(f'{stage} daemon RPC readiness timed out')
            time.sleep(.01)

        report = json.loads(run(binary, 'request', endpoint,
                                'gc-step', 65580, 4, 256*1024*1024, 1))
        assert report['mark_work_done'] <= 1 and report['mark_pending'], report
        expected = ('publish survived ' + stage).encode()
        payload.write_bytes(expected)
        # The socket may close before the client receives a response. The
        # repository write must nevertheless have reached its durable point.
        subprocess.run([binary, 'request', str(endpoint), 'fs-write', '/file', str(payload)],
                       capture_output=True, timeout=10)
        try:
            code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.terminate();proc.wait(timeout=5)
            raise AssertionError(f'{stage} hook did not terminate the daemon')
        stdout, stderr = proc.communicate()
        assert code == 86, (stage, code, stdout, stderr)

        scratch_dirs = [path for path in repository.glob('.gc-mark-*') if path.is_dir()]
        assert scratch_dirs, (stage, list(repository.glob('.gc-mark-*')))
        scratch = scratch_dirs[0]
        files = [path for path in scratch.rglob('*') if path.is_file()]
        corrupt = files[0] if files else scratch / 'corrupt-leftover'
        corrupt.write_bytes(b'corrupt mark database after process crash')

        # First open discards the corrupted scratch tree and recovers the
        # already committed namespace transaction.
        run(binary, 'fs-check', repository)
        assert run(binary, 'fs-cat', repository, '/file') == expected, stage
        assert json.loads(run(binary, 'check', repository))['last_committed_seq'] > json.loads(
            run(binary, 'check', baseline))['last_committed_seq']
        assert not list(repository.glob('.gc-mark-*')), stage
        for _ in range(512):
            report = json.loads(run(binary, 'gc-step', repository, 65580, 16))
            assert report['bytes_copied'] <= 65580, (stage, report)
            if not report['mark_pending'] and report['pending_segments'] == 0 and \
               report['objects_deleted'] == 0 and report['chunks_moved'] == 0 and \
               report['segments_deleted'] == 0:
                break
        else:
            raise AssertionError(f'{stage} recovery GC did not converge')
        later = b'write after mark barrier crash recovery'
        payload.write_bytes(later)
        run(binary, 'fs-write', repository, '/file', payload)
        assert run(binary, 'fs-cat', repository, '/file') == later
        run(binary, 'fs-check', repository)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill();proc.wait();raise

with tempfile.TemporaryDirectory(prefix='ff-online-gc-crash-') as temp:
    root = Path(temp)
    baseline = root / 'baseline'
    payload = root / 'payload'
    run(binary, 'init', baseline)
    run(binary, 'fs-init', baseline)
    current = bytes((i * 29 + 11) % 251 for i in range(190000))
    for index in range(9):
        payload.write_bytes(bytes((i * 17 + index * 31) % 251 for i in range(125000)))
        run(binary, 'fs-write', baseline, '/file', payload)
    payload.write_bytes(current)
    run(binary, 'fs-write', baseline, '/file', payload)
    sequence = json.loads(run(binary, 'check', baseline))['last_committed_seq']

    # Each fault runs against an independent copy so every durable boundary is
    # reached from a clean generation and recovery is checked independently.
    stages = ('gc_physical_after_index_row', 'gc_physical_after_index_complete',
              'gc_online_after_copy', 'gc_online_after_sync',
              'gc_online_before_publish', 'gc_online_after_chunk_publish', 'gc_online_after_publish',
              'gc_online_after_unlink', 'gc_online_after_delete_sync',
              'gc_mark_after_begin', 'gc_mark_after_task',
              'gc_mark_before_sweep', 'gc_mark_after_sweep')
    for stage in stages:
        repository = root / stage
        shutil.copytree(baseline, repository)
        env = dict(os.environ, FORKFS_TEST_CRASH=stage)
        # A source segment can need several budgeted passes before this exact
        # durability boundary is reached. Every noncrashing pass is still
        # checked for the byte budget and the unchanged committed namespace.
        for attempt in range(256):
            result = subprocess.run([fault_binary, 'gc-step', str(repository), '65580', '4'],
                                    capture_output=True, timeout=20, env=env)
            if result.returncode == 86:
                break
            assert result.returncode == 0, (stage, attempt, result.stdout, result.stderr)
            progress = json.loads(result.stdout)
            assert progress['bytes_copied'] <= 65580, (stage, progress)
            assert run(binary, 'fs-cat', repository, '/file') == current
            run(binary, 'fs-check', repository)
        else:
            raise AssertionError(f'{stage} fault hook was not reached within bounded GC steps')
        if stage.startswith('gc_mark_'):
            scratch_dirs = [path for path in repository.glob('.gc-mark-*') if path.is_dir()]
            assert scratch_dirs, (stage, list(repository.glob('.gc-mark-*')))
            # Deliberately corrupt the abandoned mark database before the next
            # owner opens the repository. Mark scratch is never recovery data.
            scratch = scratch_dirs[0]
            files = [path for path in scratch.rglob('*') if path.is_file()]
            corrupt = files[0] if files else scratch / 'corrupt-leftover'
            corrupt.write_bytes(b'corrupt abandoned incremental GC mark state')
        assert run(binary, 'fs-cat', repository, '/file') == current
        assert not list(repository.glob('.gc-mark-*')), stage
        run(binary, 'fs-check', repository)
        assert json.loads(run(binary, 'check', repository))['last_committed_seq'] == sequence
        # Recovery may finish the interrupted step, and a later ordinary write
        # must append safely after it without reusing a committed sequence.
        for _ in range(512):
            report = json.loads(run(binary, 'gc-step', repository, 65580, 16))
            assert report['bytes_copied'] <= 65580, report
            run(binary, 'fs-check', repository)
            if not report['mark_pending'] and report['pending_segments'] == 0 and report['objects_deleted'] == 0 and report['chunks_moved'] == 0:
                break
        else:
            raise AssertionError(f'GC retry did not converge after {stage}')
        payload.write_bytes(b'future state after recovered collection')
        run(binary, 'fs-write', repository, '/file', payload)
        assert run(binary, 'fs-cat', repository, '/file') == payload.read_bytes()
        assert json.loads(run(binary, 'check', repository))['last_committed_seq'] > sequence

    for index, stage in enumerate(('gc_mark_before_barrier', 'gc_mark_after_barrier')):
        crash_mark_barrier(stage, f'p{index}.sock', root, baseline, payload, binary, fault_binary)

    # Failed data appends/syncs poison only the current owner. Reopen must keep
    # the committed namespace intact and allow bounded GC retries to finish.
    for failure in ('partial_write', 'sync'):
        repository = root / ('segment-error-' + failure)
        shutil.copytree(baseline, repository)
        env = dict(os.environ, FORKFS_TEST_SEGMENT_ERROR=failure)
        run(fault_binary, 'gc-step', repository, 8*1024*1024, 128, env=env, code=1)
        assert run(binary, 'fs-cat', repository, '/file') == current
        run(binary, 'fs-check', repository)
        assert json.loads(run(binary, 'check', repository))['last_committed_seq'] == sequence
        for _ in range(512):
            report = json.loads(run(binary, 'gc-step', repository, 8*1024*1024, 128))
            assert report['bytes_copied'] <= 8*1024*1024, report
            run(binary, 'fs-check', repository)
            if not report['mark_pending'] and report['pending_segments'] == 0 and report['objects_deleted'] == 0 and report['chunks_moved'] == 0:
                break
        else:
            raise AssertionError(f'GC did not recover from {failure} within bounded retries')
        future = b'write after recovered GC I/O failure'
        payload.write_bytes(future)
        run(binary, 'fs-write', repository, '/file', payload)
        assert run(binary, 'fs-cat', repository, '/file') == future
        assert json.loads(run(binary, 'check', repository))['last_committed_seq'] > sequence

    # An incremental root barrier failure invalidates only the active mark
    # cycle. The foreground publish remains durable and the owner stays healthy.
    repository = root / 'rpc-mark-barrier-error'
    shutil.copytree(baseline, repository)
    endpoint = root / 'b.sock'
    proc = subprocess.Popen([fault_binary, 'serve', str(repository), '--socket', str(endpoint), '--rpc'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, FORKFS_TEST_GC_ERROR='barrier'))
    try:
        deadline = time.monotonic() + 10
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'barrier-fault daemon exited before readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                probe = subprocess.run([binary, 'request', str(endpoint), 'gc-status'],
                                       capture_output=True, timeout=2)
                if probe.returncode == 0:
                    break
            if time.monotonic() >= deadline:
                raise AssertionError('barrier-fault daemon RPC readiness timed out')
            time.sleep(.01)

        def barrier_request(*args):
            return run(binary, 'request', endpoint, *args)

        mark = json.loads(barrier_request('gc-step', 65580, 4, 256*1024*1024, 1))
        assert mark['mark_work_done'] <= 1 and mark['mark_pending'], mark
        future = b'foreground write survives incremental mark barrier failure'
        payload.write_bytes(future)
        barrier_request('fs-write', '/file', payload)
        barrier_request('fs-sync')
        assert barrier_request('fs-cat', '/file') == future

        started = json.loads(barrier_request('gc-start', 65580, 4, 24, 256*1024*1024, 1))
        assert started['state'] in {'running', 'failed'}, started
        deadline = time.monotonic() + 20
        while True:
            status = json.loads(barrier_request('gc-status'))
            if not status['active']:
                assert status['state'] == 'failed', status
                assert 'GC publication barrier failed' in status['error'], status
                break
            assert time.monotonic() < deadline, status
            time.sleep(.01)

        later = b'write after mark barrier error is consumed'
        payload.write_bytes(later)
        barrier_request('fs-write', '/file', payload)
        barrier_request('fs-sync')
        assert barrier_request('fs-cat', '/file') == later
        retry = json.loads(barrier_request('gc-start', 8*1024*1024, 128, 1024, 256*1024*1024, 65536))
        assert retry['state'] in {'running', 'complete', 'budget_exhausted'}, retry
        deadline = time.monotonic() + 30
        while True:
            status = json.loads(barrier_request('gc-status'))
            if not status['active']:
                assert status['state'] == 'complete', status
                assert status['error'] == '', status
                break
            assert time.monotonic() < deadline, status
            time.sleep(.01)
        assert barrier_request('fs-cat', '/file') == later
        barrier_request('fs-check')
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait(); raise

    # The online worker's sync failure poisons the still-running owner. Querying
    # job status remains available, while storage operations must fail until
    # that daemon exits and the repository is reopened.
    repository = root / 'rpc-sync-error'
    shutil.copytree(baseline, repository)
    segments = sorted(repository.glob('DATA-*.seg'))
    assert segments, repository
    # Preserve the valid segment header and indexed records, but add a durable
    # unindexed tail so the online worker must exercise relocation+sync.
    with segments[-1].open('ab') as tail:
        tail.write(bytes([0xA7]) * (1024 * 1024))
    endpoint = root / 'rpc-sync-error.sock'
    proc = subprocess.Popen([fault_binary, 'serve', str(repository), '--socket', str(endpoint), '--rpc'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, FORKFS_TEST_SEGMENT_ERROR='sync'))
    try:
        deadline = time.monotonic() + 10
        last_probe = None
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'fault daemon exited before RPC readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                try:
                    probe = subprocess.run([binary, 'request', str(endpoint), 'gc-status'],
                                           capture_output=True, timeout=2)
                    if probe.returncode == 0:
                        json.loads(probe.stdout)
                        if proc.poll() is not None:
                            stdout, stderr = proc.communicate()
                            raise AssertionError(f'fault daemon exited after binding: {stdout!r} {stderr!r}')
                        break
                    last_probe = (probe.returncode, probe.stdout, probe.stderr)
                except subprocess.TimeoutExpired as error:
                    last_probe = error
            if time.monotonic() >= deadline:
                proc.terminate()
                try:
                    stdout, stderr = proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill();stdout, stderr = proc.communicate()
                raise AssertionError(f'fault daemon RPC readiness timed out; probe={last_probe!r}; logs={stdout!r} {stderr!r}')
            time.sleep(.01)

        def request(*args, ok=True):
            return run(binary, 'request', endpoint, *args, code=0 if ok else 1)

        started = json.loads(request('gc-start', 8*1024*1024, 128, 64))
        assert started['state'] in {'running', 'complete', 'blocked', 'budget_exhausted', 'failed'}, started
        deadline = time.monotonic() + 20
        while True:
            status = json.loads(request('gc-status'))
            if not status['active']:
                assert status['state'] == 'failed' and status['error'], status
                break
            assert time.monotonic() < deadline, status
            time.sleep(.01)
        request('fs-sync', ok=False)
        payload.write_bytes(current)
        request('fs-write', '/file', payload, ok=False)
        request('fs-cat', '/file', ok=False)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait(); raise
    run(binary, 'fs-check', repository)
    assert run(binary, 'fs-cat', repository, '/file') == current

    # A durable retired-segment marker must be reaped before a tight DATA
    # budget attempts relocation into a new segment.
    repository = root / 'quota-pending-retirement'
    shutil.copytree(baseline, repository)
    sequence = json.loads(run(binary, 'check', repository))['last_committed_seq']
    env = dict(os.environ, FORKFS_TEST_CRASH='gc_online_after_publish')
    run(fault_binary, 'gc-step', repository, 8*1024*1024, 4096, env=env, code=86)
    data_files = sorted(repository.glob('DATA-*.seg'), key=lambda path: int(path.stem.split('-')[1]))
    assert len(data_files) >= 2, data_files
    with data_files[-1].open('ab') as tail:
        tail.write(bytes([0xB9]) * (1024 * 1024))
    before_bytes = sum(path.stat().st_size for path in data_files)
    run(binary, 'capacity', repository, before_bytes, 0)
    first = None
    for _ in range(512):
        first = json.loads(run(binary, 'gc-step', repository, 8*1024*1024, 4096))
        assert first['bytes_copied'] <= 8*1024*1024, first
        if first['segments_deleted'] > 0:
            break
    assert first and first['segments_deleted'] > 0, first
    assert sum(path.stat().st_size for path in repository.glob('DATA-*.seg')) < before_bytes
    assert run(binary, 'fs-cat', repository, '/file') == current
    run(binary, 'fs-check', repository)
    assert json.loads(run(binary, 'check', repository))['last_committed_seq'] == sequence
    for _ in range(512):
        report = json.loads(run(binary, 'gc-step', repository, 8*1024*1024, 4096))
        assert report['bytes_copied'] <= 8*1024*1024, report
        run(binary, 'fs-check', repository)
        if not report['mark_pending'] and report['pending_segments'] == 0 and report['objects_deleted'] == 0 and report['chunks_moved'] == 0 and report['segments_deleted'] == 0:
            break
    else:
        raise AssertionError('quota-bounded online GC did not converge')
    assert run(binary, 'fs-cat', repository, '/file') == current
    assert json.loads(run(binary, 'check', repository))['last_committed_seq'] == sequence
    future = b'future write after quota recovery'
    payload.write_bytes(future)
    run(binary, 'fs-write', repository, '/file', payload)
    assert run(binary, 'fs-cat', repository, '/file') == future
    run(binary, 'fs-check', repository)

print('online GC crash recovery across copy, publication, unlink and delete sync passed')
