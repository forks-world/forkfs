"""Explicit offline retention changes preserve ancestry and release abandoned branches."""
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile

BIN, LEGACY, FAULT = sys.argv[1:4]
for binary in [BIN, LEGACY]:
    def run(*args, ok=True, program=binary, env=None):
        p = subprocess.run([program, *map(str, args)], capture_output=True, timeout=30, env=env)
        assert (p.returncode == 0) == ok, (args, p.returncode, p.stderr)
        return p.stdout

    with tempfile.TemporaryDirectory(prefix='ff-retention-', dir='/tmp') as tmp:
        root = Path(tmp); db = root/'db'; data = root/'input'
        run('init', db); run('fs-init', db)
        data.write_bytes(b'base'); run('fs-write', db, '/f', data)
        def snapshot(name, *view):
            return json.loads(run('fs-snapshot', db, name, *view))['revision_id']
        def seq():
            return json.loads(run('check', db))['last_committed_seq']
        base = snapshot('base'); run('fs-fork', db, 'base', 'discard')
        data.write_bytes(b'branch-only'*4096); run('fs-write', db, '/f', data, '--world', 'discard')
        branch = snapshot('branch', '--world', 'discard')
        data.write_bytes(b'kept'); run('fs-write', db, '/f', data); kept = snapshot('kept')
        before = seq()
        for args in [('fs-drop-world', 'main'), ('fs-drop-world', 'absent'),
                     ('fs-forget-revision', 'absent'), ('fs-drop-world', '../bad'),
                     ('fs-forget-revision', 'bad/name')]:
            run(args[0], db, *args[1:], ok=False)
        for command, name in [('fs-drop-world', 'discard'), ('fs-forget-revision', 'branch')]:
            for selector in ['--world', '--revision']:
                run(command, db, name, selector, 'base', ok=False)
        assert seq() == before

        # Removing an ancestor label cannot rewrite the immutable parent chain.
        result = json.loads(run('fs-forget-revision', db, 'base'))
        assert result['revision_id'] == base and seq() == before+1
        assert b'base' not in run('fs-revisions', db).splitlines()
        run('fs-cat', db, '/f', '--revision', 'base', ok=False)
        history = json.loads(run('fs-history', db, 'kept'))
        assert [e['revision_id'] for e in history['entries']] == [kept, base]
        assert run('fs-cat', db, '/f', '--world', 'discard') == b'branch-only'*4096
        # An unlabelled World base remains pinned even without any named child.
        run('fs-forget-revision', db, 'branch')
        if binary == BIN:
            run('gc', db)
        next_branch = snapshot('branch-next', '--world', 'discard')
        assert [e['revision_id'] for e in json.loads(run('fs-history', db, 'branch-next'))['entries']] == [next_branch, branch, base]
        # Labels can be reused without changing IDs in an older history.
        replacement = snapshot('base'); assert replacement != base
        assert [e['revision_id'] for e in json.loads(run('fs-history', db, 'kept'))['entries']] == [kept, base]
        before = seq(); dropped = json.loads(run('fs-drop-world', db, 'discard'))
        assert seq() == before+1 and dropped['base_revision_id'] == next_branch
        assert b'discard' not in run('fs-worlds', db).splitlines()
        run('fs-cat', db, '/f', '--world', 'discard', ok=False)
        assert run('fs-cat', db, '/f', '--revision', 'branch-next') == b'branch-only'*4096
        before = seq(); run('fs-drop-world', db, 'discard', ok=False); assert seq() == before
        if binary == BIN:
            run('gc', db)
        run('fs-check', db, '--revision', 'branch-next')
        run('fs-forget-revision', db, 'branch-next')
        if binary == BIN:
            assert json.loads(run('gc', db))['collected_objects'] > 0
            assert json.loads(run('gc', db))['collected_objects'] == 0
        assert run('fs-cat', db, '/f') == b'kept'
        run('fs-check', db); run('fs-check', db, '--revision', 'kept')
        # Recreating the World is explicit and uses the newly selected base.
        run('fs-fork', db, 'kept', 'discard')
        assert run('fs-cat', db, '/f', '--world', 'discard') == b'kept'

        # Restore's safety revision stays pinned by the World base after its
        # label is forgotten; later snapshots continue through that exact ID.
        data.write_bytes(b'before restore'); run('fs-write', db, '/f', data)
        safety = json.loads(run('fs-restore', db, 'kept', 'restore-safety'))['safety_revision_id']
        run('fs-forget-revision', db, 'restore-safety')
        if binary == BIN:
            run('gc', db)
        after_restore = snapshot('after-restore')
        ancestry = json.loads(run('fs-history', db, 'after-restore'))['entries']
        assert [e['revision_id'] for e in ancestry[:3]] == [after_restore, safety, replacement]
        assert run('fs-cat', db, '/f') == b'kept'

        if binary == BIN:
            # Both head/base deletion and label deletion use one synchronous batch.
            baseline = root/'baseline'; shutil.copytree(db, baseline)
            for command, name in [('fs-drop-world', 'discard'), ('fs-forget-revision', 'base')]:
                for stage in ['leveldb_before_write', 'leveldb_after_write']:
                    shutil.rmtree(db); shutil.copytree(baseline, db)
                    p = subprocess.run([FAULT, command, str(db), name], capture_output=True, timeout=30,
                                       env={**os.environ, 'FORKFS_TEST_CRASH':stage})
                    assert p.returncode == 86, p.stderr
                    committed = stage == 'leveldb_after_write'
                    listing = 'fs-worlds' if command == 'fs-drop-world' else 'fs-revisions'
                    assert (name.encode() not in run(listing, db).splitlines()) == committed
                    if command == 'fs-drop-world' and committed:
                        run('fs-fork', db, 'kept', 'discard') # no stranded base root
                    run('gc', db); run('fs-check', db); run('fs-check', db, '--revision', 'kept')
            # A resident daemon owns the repository; retention has no RPC entry.
            endpoint = root/'control.sock'
            proc = subprocess.Popen([binary, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                assert select.select([proc.stdout], [], [], 5)[0]
                assert json.loads(proc.stdout.readline())['state'] == 'leveldb'
                for command, name in [('fs-drop-world', 'discard'), ('fs-forget-revision', 'kept')]:
                    run(command, db, name, ok=False)
                    run('request', endpoint, command, name, ok=False)
                assert b'discard' in run('request', endpoint, 'fs-worlds').splitlines()
                assert b'kept' in run('request', endpoint, 'fs-revisions').splitlines()
            finally:
                proc.terminate(); proc.wait(timeout=10)
print('offline retention, ancestry, unlabelled bases, label reuse, survivors, GC, crash atomicity and legacy compatibility passed')
