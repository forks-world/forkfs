"""Deterministic crash/IO fault matrix over isolated copies of a test repository."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import traceback

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True, type=Path)
parser.add_argument('--fault-binary', required=True, type=Path)
parser.add_argument('--output', type=Path)
case_names = ['-'.join([operation, point, action])
              for operation in ['write', 'rename', 'fork', 'restore']
              for point in ['leveldb_before_write', 'wal_append', 'wal_sync', 'wal_after_sync', 'leveldb_after_write']
              for action in ['crash', 'eio', 'enospc']]
case_names += [point + '-' + action for point in ['compaction_before_publish', 'compaction_published', 'compaction_stage_committed']
               for action in ['crash', 'eio', 'enospc']]
case_names += ['resident-' + point for point in ['wal_append', 'wal_sync', 'wal_after_sync', 'leveldb_after_write', 'leveldb_before_write']]
case_names += ['production-ignores-controls', 'invalid-unknown-crash-1', 'invalid-wal_sync-invalid-1', 'invalid-wal_sync-eio-0']
parser.add_argument('--case', choices=case_names, help='repeat one deterministic fault scenario')
args = parser.parse_args()
binary, fault_binary = args.binary.resolve(), args.fault_binary.resolve()
root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='ff-faults-', dir='/tmp'))
if args.output:
    root.mkdir()
clean_env = {key: value for key, value in os.environ.items() if not key.startswith('FORKFS_TEST_FAULT') and key != 'FORKFS_TEST_CRASH'}
(root / 'manifest.json').write_text(json.dumps({'schema': 1, 'binary': str(binary),
    'fault_binary': str(fault_binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'fault_binary_sha256': hashlib.sha256(fault_binary.read_bytes()).hexdigest()}, indent=2))

def run(*fields, executable=binary, environment=clean_env, expected=0):
    event = {'argv': list(map(str, fields)), 'binary': str(executable),
             'fault': {key: value for key, value in environment.items() if key.startswith('FORKFS_TEST_FAULT')}}
    started = time.monotonic_ns()
    try:
        result = subprocess.run([str(executable), *map(str, fields)], env=environment, capture_output=True, timeout=15)
        event.update(exit=result.returncode, stdout_hex=result.stdout[:256].hex(), stderr=result.stderr.decode(errors='replace'))
        assert result.returncode == expected, (fields, result.returncode, result.stderr)
        return result
    except subprocess.TimeoutExpired:
        event['failure'] = 'timeout'
        raise
    finally:
        event['duration_ns'] = time.monotonic_ns() - started
        with (root / 'events.jsonl').open('a') as stream:
            stream.write(json.dumps(event) + '\n')


def environment(point, action, hit=1):
    return {**clean_env, 'FORKFS_TEST_FAULT': point, 'FORKFS_TEST_FAULT_ACTION': action, 'FORKFS_TEST_FAULT_HIT': str(hit)}

def injected(result, point, action, hit=1, require_error=True):
    expected = {'fault_point': point, 'action': action, 'hit': hit}
    events = []
    for line in result.stderr.decode(errors='replace').splitlines():
        if line.startswith('{'):
            events.append(json.loads(line))
    assert events == [expected], (events, result.stderr)
    if action != 'crash' and require_error:
        assert b'injected' in result.stderr, result.stderr


def signature(repository):
    def tree(view=()):
        output = {}
        def walk(path):
            attrs = json.loads(run('fs-lstat', repository, path, *view).stdout)
            output[path] = {key: attrs[key] for key in ['inode_id', 'directory', 'symlink', 'mode', 'links', 'size']}
            if attrs['directory']:
                for name in run('fs-ls', repository, path, *view).stdout.decode().splitlines():
                    walk(path.rstrip('/') + '/' + name)
            else:
                output[path]['data'] = run('fs-cat', repository, path, *view).stdout.hex()
        walk('/')
        run('fs-check', repository, *view)
        return output
    worlds = run('fs-worlds', repository).stdout.decode().splitlines()
    revisions = run('fs-revisions', repository).stdout.decode().splitlines()
    return {'sequence': json.loads(run('check', repository).stdout)['last_committed_seq'],
            'worlds': {name: tree(('--world', name)) for name in worlds},
            'revisions': {name: tree(('--revision', name)) for name in revisions}}

baseline = root / 'baseline'
payload = root / 'input'
run('init', baseline)
run('fs-init', baseline)
payload.write_bytes(b'old\0bytes')
run('fs-write', baseline, '/a', payload)
run('fs-link', baseline, '/a', '/alias')
run('fs-snapshot', baseline, 'base')
payload.write_bytes(b'live')
run('fs-write', baseline, '/a', payload)
payload.write_bytes(b'new')
old = signature(baseline)
operations = [('write', ('fs-write', '/a', str(payload))), ('rename', ('fs-rename', '/a', '/moved')),
              ('fork', ('fs-fork', 'base', 'child')), ('restore', ('fs-restore', 'base'))]
results = []

def case(name, action):
    if args.case and args.case != name:
        return
    result = {'case': name, 'status': 'pass'}
    try:
        action()
    except Exception as error:
        result.update(status='fail', error=str(error))
        (root / (name + '.failure.txt')).write_text(traceback.format_exc())
    results.append(result)

for name, operation in operations:
    reference = root / (name + '-reference')
    shutil.copytree(baseline, reference)
    run(operation[0], reference, *operation[1:])
    new = signature(reference)
    for point in ['leveldb_before_write', 'wal_append', 'wal_sync', 'wal_after_sync', 'leveldb_after_write']:
        for action in ['crash', 'eio', 'enospc']:
            label = '-'.join([name, point, action])
            def scenario(label=label, operation=operation, new=new, point=point, action=action):
                repository = root / label
                shutil.copytree(baseline, repository)
                result = run(operation[0], repository, *operation[1:], executable=fault_binary,
                             environment=environment(point, action), expected=86 if action == 'crash' else 1)
                injected(result, point, action)
                actual = signature(repository)
                # WAL append may have emitted a partial record; Sync failure is
                # unacknowledged, and process-crash recovery can retain the write.
                if point == 'leveldb_before_write':
                    assert actual == old
                elif point in ['wal_after_sync', 'leveldb_after_write']:
                    assert actual == new
                else:
                    assert actual in [old, new], 'mixed transaction state after recovery'
            case(label, scenario)

# A flat, initialized repository forces real compaction publication (not a no-op).
flat = root / 'flat'
run('init', flat)
run('fs-init', flat)
flat_state = signature(flat)
for point in ['compaction_before_publish', 'compaction_published', 'compaction_stage_committed']:
    for action in ['crash', 'eio', 'enospc']:
        label = point + '-' + action
        def scenario(label=label, point=point, action=action):
            repository = root / label
            shutil.copytree(flat, repository)
            operation = 'test-compact-staged'
            result = run(operation, repository, executable=fault_binary, environment=environment(point, action),
                         expected=86 if action == 'crash' else 1)
            injected(result, point, action)
            recovered = signature(repository)
            assert recovered['sequence'] == flat_state['sequence'] + (2 if point == 'compaction_published' else 1)
            recovered['sequence'] = flat_state['sequence']
            assert recovered == flat_state
        case(label, scenario)

written = signature(root / 'write-reference')

# Resident errors must not be mistaken for an ordinary safe refusal when the WAL
# may have changed: subsequent reads/writes require reopening the poisoned store.
for point in ['wal_append', 'wal_sync', 'wal_after_sync', 'leveldb_after_write', 'leveldb_before_write']:
    label = 'resident-' + point
    def scenario(label=label, point=point):
        repository = root / label
        shutil.copytree(baseline, repository)
        endpoint = root / (label + '.sock')
        log = root / (label + '.stderr')
        with log.open('wb') as stderr:
            process = subprocess.Popen([str(fault_binary), 'serve', str(repository), '--socket', str(endpoint), '--rpc'],
                                       env=environment(point, 'eio', 2), stdout=subprocess.DEVNULL, stderr=stderr)
            try:
                deadline = time.monotonic() + 5
                while not endpoint.exists():
                    assert process.poll() is None
                    assert time.monotonic() < deadline
                    time.sleep(.01)
                # Startup must not consume the hit counter. A second transaction
                # is refused exactly once; use the single per-commit boundary for
                # counting, while WAL append hits count physical records.
                first = run('request', endpoint, 'fs-write', '/a', payload,
                            expected=1 if point == 'wal_append' else 0)
                if point != 'wal_append':
                    first = run('request', endpoint, 'fs-write', '/a', payload, expected=1)
                assert b'injected' in first.stderr, first.stderr
                if point == 'leveldb_before_write':
                    run('request', endpoint, 'fs-write', '/a', payload)
                    assert run('request', endpoint, 'fs-cat', '/a').stdout == b'new'
                else:
                    refused = run('request', endpoint, 'fs-cat', '/a', expected=1)
                    assert b'requires reopen after write failure' in refused.stderr
                    refused = run('request', endpoint, 'fs-write', '/a', payload, expected=1)
                    assert b'requires reopen after write failure' in refused.stderr
                assert process.poll() is None
            finally:
                process.kill()
                process.wait(timeout=5)
        injected(subprocess.CompletedProcess([], 1, b'', log.read_bytes()), point, 'eio', 2, require_error=False)
        actual = signature(repository)
        twice_written = json.loads(json.dumps(written))
        twice_written['sequence'] += 1
        if point == 'wal_append':
            assert actual in [old, written]
        elif point == 'wal_sync':
            assert actual in [written, twice_written]
        else:
            assert actual == twice_written

    case(label, scenario)

# Production must ignore all fault controls, including deliberately invalid configuration.
def production():
    assert b'FORKFS_TEST_FAULT' not in binary.read_bytes(), 'fault controls leaked into production binary'
    repository = root / 'production'
    shutil.copytree(baseline, repository)
    run('test-compact-staged', repository, expected=1)
    run('fs-write', repository, '/a', payload, environment=environment('invalid', 'invalid', 0))
    assert run('fs-cat', repository, '/a').stdout == b'new'
case('production-ignores-controls', production)

# Invalid configuration is rejected before the test executable opens or mutates data.
for point, action, hit in [('unknown', 'crash', 1), ('wal_sync', 'invalid', 1), ('wal_sync', 'eio', 0)]:
    def invalid(point=point, action=action, hit=hit):
        run('fs-write', baseline, '/a', payload, executable=fault_binary,
            environment=environment(point, action, hit), expected=1)
        assert signature(baseline) == old
    case('invalid-' + point + '-' + action + '-' + str(hit), invalid)

report = {'schema': 1, 'status': 'fail' if any(r['status'] == 'fail' for r in results) else 'pass',
          'results': results, 'artifacts': str(root)}
(root / 'result.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
raise SystemExit(1 if report['status'] == 'fail' else 0)
