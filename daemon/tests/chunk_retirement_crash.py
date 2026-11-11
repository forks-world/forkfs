"""Crash publication and recovery with more than 4096 allocated chunks."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BIN, FIXTURE, FAULT = sys.argv[1:4]


def run(binary, *args):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, timeout=60)
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='ff-io-retire-crash-', dir='/tmp') as temporary:
    root = Path(temporary)
    baseline, db = root / 'baseline', root / 'db'
    run(FIXTURE, '--fixture', baseline)
    for action in ['unlink', 'replace', 'truncate', 'orphan-write', 'orphan-truncate', 'last-close']:
        for stage in ['leveldb_before_write', 'leveldb_after_write']:
            if db.exists():
                shutil.rmtree(db)
            shutil.copytree(baseline, db)
            result = subprocess.run([FAULT, str(db), action, stage], capture_output=True, timeout=60,
                                    env={key: value for key, value in os.environ.items() if key != 'FORKFS_TEST_CRASH'})
            assert result.returncode == 86, (action, stage, result.stderr)
            committed = stage == 'leveldb_after_write'
            orphan = json.loads(run(FIXTURE, '--inspect', db))
            roots = 0 if action == 'truncate' or (action == 'last-close' and committed) else (
                2 if action.startswith('orphan-') or action == 'last-close' or committed else 0)
            assert orphan['roots'] == roots, (action, stage, orphan)
            if roots:
                assert orphan['size'] == (17 if action == 'orphan-truncate' and committed else 320 * 1024 * 1024)
                assert orphan['first'] == (0x99 if action == 'orphan-write' and committed else 0x5a)
            # Opening the repository releases stale handles and both retention
            # roots. It never retargets an old handle to a reused pathname.
            run(BIN, 'fs-check', db)
            assert json.loads(run(FIXTURE, '--inspect', db))['roots'] == 0
            if action == 'truncate':
                assert json.loads(run(BIN, 'fs-stat', db, '/dense'))['size'] == (17 if committed else 320 * 1024 * 1024)
            elif action == 'replace':
                assert run(BIN, 'fs-read-at', db, '/dense', 0, 1) == (b'S' if committed else b'Z')
            else:
                result = subprocess.run([BIN, 'fs-stat', str(db), '/dense'], capture_output=True, timeout=60)
                assert (result.returncode == 0) == (action == 'unlink' and not committed)
            run(BIN, 'gc', db)
            run(BIN, 'check', db)
            run(BIN, 'fs-check', db, '--revision', 'base')
            assert run(BIN, 'fs-read-at', db, '/dense', 320 * 1024 * 1024 - 4, 4, '--revision', 'base') == b'ZZZZ'
    print('5120-chunk unlink/replace/truncate/orphan-write/last-close crash publication and recovery passed')
