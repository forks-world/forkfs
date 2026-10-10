"""Directory backend, CoW branches, native compaction and atomic recovery."""
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import time

BIN, FAULT = sys.argv[1:3]
def run(*args, ok=True):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=15)
    assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(prefix='ffdb-', dir='/tmp') as tmp:
    root = Path(tmp); db = root/'store'; data = root/'input'
    initial = json.loads(run('init', db))
    assert initial['storage_engine'] == 'leveldb'
    assert db.is_dir() and db.stat().st_mode & 0o777 == 0o700
    assert (db/'CURRENT').is_file()
    run('init', db, ok=False)
    alias = root/'alias'; alias.symlink_to(db)
    run('check', alias, ok=False)
    empty = root/'empty'; empty.mkdir()
    run('check', empty, ok=False)
    run('init', empty, ok=False)
    run('fs-init', db); run('fs-mkdir', db, '/src')
    model = {}; rng = random.Random(17)
    # Shuffled insertion, replacement and deletion exercise treap rotations/merge.
    names = list(range(100)); rng.shuffle(names)
    for i in names:
        data.write_bytes(f'value-{i}'.encode()); run('fs-write', db, f'/src/f{i:03}', data)
        model[f'f{i:03}'] = data.read_bytes()
    run('fs-check', db)
    run('fs-snapshot', db, 'base'); run('fs-fork', db, 'base', 'work')
    assert json.loads(run('fs-layout', db))['layout'] == 'cow-tree'
    for i in names[:50]:
        run('fs-unlink', db, f'/src/f{i:03}', '--world', 'work')
    for i in names[50:]:
        data.write_bytes(b'changed'); run('fs-write', db, f'/src/f{i:03}', data, '--world', 'work')
    run('fs-link', db, '/src/f000', '/alias')
    assert json.loads(run('fs-stat', db, '/alias'))['links'] == 2
    data.write_bytes(b'linked'); run('fs-write', db, '/alias', data)
    assert run('fs-cat', db, '/src/f000') == b'linked'
    assert run('fs-cat', db, '/src/f000', '--revision', 'base') == model['f000']
    run('fs-write', db, '/alias', data, '--revision', 'base', ok=False)
    run('fs-compact', db); run('checkpoint', db)
    for view in [(), ('--world', 'work'), ('--revision', 'base')]:
        run('fs-check', db, *view)
    assert run('fs-ls', db, '/src', '--world', 'work').splitlines() == sorted(
        f'f{i:03}'.encode() for i in names[50:])
    for name, value in model.items():
        assert run('fs-cat', db, '/src/'+name, '--revision', 'base') == value
    baseline = root/'baseline'; shutil.copytree(db, baseline)
    for stage in ['leveldb_before_write', 'leveldb_after_write']:
        for command, args, probe in [
            ('fs-rename', ('/alias', '/moved'), ('fs-stat', '/moved')),
            ('fs-fork', ('base', 'crashfork'), ('fs-stat', '/', '--world', 'crashfork')),
            ('fs-snapshot', ('crashrev',), ('fs-stat', '/', '--revision', 'crashrev')),
        ]:
            shutil.rmtree(db); shutil.copytree(baseline, db)
            p = subprocess.run([FAULT, command, str(db), *args], capture_output=True,
                               env={**os.environ, 'FORKFS_TEST_CRASH': stage}, timeout=15)
            assert p.returncode == 86, (stage, command, p.stderr)
            run(probe[0], db, *probe[1:], ok=stage == 'leveldb_after_write')
            run('fs-check', db); run('fs-check', db, '--revision', 'base')
            if command == 'fs-rename':
                run('fs-stat', db, '/alias', ok=stage == 'leveldb_before_write')
    socket = root/'control.sock'
    proc = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(socket)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+5
        while not socket.exists():
            assert proc.poll() is None, proc.communicate()
            assert time.monotonic() < deadline
            time.sleep(.01)
        run('check', db, ok=False)
        proc.kill(); proc.wait(timeout=5)
        run('fs-check', db)
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
    print('LevelDB directory, CoW tree, branches, compaction, locking and crash recovery passed')
