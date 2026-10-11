"""Restore publishes the whole tree and generation in one durable transaction."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary, fault_binary = sys.argv[1:3]
def run(*args):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr
    return result.stdout
with tempfile.TemporaryDirectory(prefix='ff-restore-crash-') as temp:
    root = Path(temp)
    baseline = root / 'baseline'
    payload = root / 'input'
    run('init', baseline)
    run('fs-init', baseline)
    payload.write_bytes(b'old')
    run('fs-write', baseline, '/old', payload)
    run('fs-snapshot', baseline, 'r1')
    run('fs-unlink', baseline, '/old')
    payload.write_bytes(b'new')
    run('fs-write', baseline, '/new', payload)
    run('fs-snapshot', baseline, 'r2')
    before = json.loads(run('check', baseline))['last_committed_seq']
    for stage, committed in [('leveldb_before_write', False), ('leveldb_after_write', True)]:
        repository = root / stage
        shutil.copytree(baseline, repository)
        result = subprocess.run([fault_binary, 'fs-restore', str(repository), 'r1'],
                                env={**os.environ, 'FORKFS_TEST_CRASH': stage}, capture_output=True, timeout=10)
        assert result.returncode == 86, result.stderr
        assert run('fs-ls', repository, '/') == (b'old\n' if committed else b'new\n')
        assert run('fs-cat', repository, '/old' if committed else '/new') == (b'old' if committed else b'new')
        assert json.loads(run('check', repository))['last_committed_seq'] == before + int(committed)
        run('fs-check', repository)
        assert run('fs-cat', repository, '/old', '--revision', 'r1') == b'old'
        assert run('fs-cat', repository, '/new', '--revision', 'r2') == b'new'
print('restore before/after durable publication crash recovery passed')
