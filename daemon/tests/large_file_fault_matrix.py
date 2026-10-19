"""Large-file operation atomicity at storage barriers, with source-free replay."""
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
BIN, FAULT = sys.argv[1:3]

def run(*args, binary=BIN, env=None, code=0):
    p = subprocess.run([binary, *map(str, args)], capture_output=True, timeout=30, env=env)
    assert p.returncode == code, (args, p.returncode, p.stderr)
    return p.stdout

def read_model(prefix, size, at, count, overlay=None):
    if at >= size: return b''
    out = bytearray(min(count, size-at))
    stop = min(len(prefix), at+len(out))
    if at < stop: out[:stop-at] = prefix[at:stop]
    if overlay:
        offset, value = overlay
        start, end = max(at, offset), min(at+len(out), offset+len(value))
        if start < end: out[start-at:end-at] = value[start-offset:end-offset]
    return bytes(out)

with tempfile.TemporaryDirectory(prefix='ff-large-matrix-') as directory:
    root = Path(directory); baseline = root/'baseline'; payload = root/'input'
    rng = random.Random(8173); big = rng.randbytes(700000); raw = rng.randbytes(262140); patch = rng.randbytes(8192)
    run('init', baseline); run('fs-init', baseline)
    for path, data in (('/file', big), ('/raw', raw)):
        payload.write_bytes(data); run('fs-write', baseline, path, payload)
    run('fs-link', baseline, '/file', '/alias'); run('fs-snapshot', baseline, 'base'); run('fs-fork', baseline, 'base', 'work')
    baseline_status = json.loads(run('check', baseline))
    payload.write_bytes(patch); far = (1 << 40) + 65535
    modified = bytearray(big); modified[65535:65535+len(patch)] = patch
    # name, command, arguments, target, expected prefix/size/optional distant overlay
    operations = [
        ('promote', 'fs-write-at', ['/raw', len(raw), payload], '/raw', raw+patch, len(raw)+len(patch), None),
        ('cross-block', 'fs-write-at', ['/file', 65535, payload], '/file', bytes(modified), len(big), None),
        ('sparse', 'fs-write-at', ['/file', far, payload], '/file', big, far+len(patch), (far, patch)),
        ('shrink', 'fs-truncate', ['/file', 65537], '/file', big[:65537], 65537, None),
        ('grow', 'fs-truncate', ['/file', 1 << 40], '/file', big, 1 << 40, None),
        ('zero', 'fs-truncate', ['/file', 0], '/file', b'', 0, None),
        ('replace', 'fs-save', ['/file', payload], '/file', patch, len(patch), None),
        ('append', 'fs-append', ['/file', payload], '/file', big+patch, len(big)+len(patch), None),
    ]
    barriers = [(name, 'FORKFS_TEST_CRASH', name, 86) for name in
                ('segment_after_append', 'segment_after_sync', 'segment_before_metadata', 'segment_after_metadata')]
    barriers += [(name, 'FORKFS_TEST_SEGMENT_ERROR', name, 1) for name in ('partial_write', 'sync', 'metadata')]

    def check(db, operation, committed):
        name, _, _, path, prefix, size, overlay = operation
        original = raw if path == '/raw' else big
        if not committed: prefix, size, overlay = original, len(original), None
        selectors = ['--world', 'work']
        for target in ([path, '/alias'] if path == '/file' and name != 'replace' else [path]):
            assert json.loads(run('fs-stat', db, target, *selectors))['size'] == size
            for at, count in ((0, len(prefix)), (65534, 12), (262139, 12), (max(0, size-9), 20), (far-2, len(patch)+4)):
                assert run('fs-read-at', db, target, at, count, *selectors) == read_model(prefix, size, at, count, overlay), (name, target, at)
        # Atomic replacement must detach only the replaced directory entry.
        if name == 'replace': assert run('fs-cat', db, '/alias', *selectors) == big
        assert run('fs-cat', db, '/file') == big
        assert run('fs-cat', db, '/raw', '--revision', 'base') == raw
        assert run('fs-cat', db, '/file', '--revision', 'base') == big
        run('fs-check', db, *selectors)

    for operation in operations:
        name, command, args, path, prefix, size, overlay = operation
        for barrier, key, value, code in barriers:
            db = root/'case'; shutil.copytree(baseline, db)
            env = {**os.environ, key: value}
            run(command, db, *args, '--world', 'work', binary=FAULT, env=env, code=code)
            committed = barrier == 'segment_after_metadata'
            state = json.loads(run('check', db))
            assert state['last_committed_seq'] == baseline_status['last_committed_seq'] + int(committed)
            if not committed:
                for field in ('indexed_objects', 'object_roots'):
                    assert state[field] == baseline_status[field], (name, barrier, field)
            # Repeated opens must retain the same committed result.
            check(db, operation, committed); check(db, operation, committed)
            if not committed:
                run(command, db, *args, '--world', 'work'); check(db, operation, True)
            shutil.rmtree(db)
        # Full+delta replay, without the source, followed by new writes and forks.
        db = root/'live'; shutil.copytree(baseline, db); base = root/(name+'.base.pack'); delta = root/(name+'.delta.pack')
        point = int(run('backup', db, base)); run(command, db, *args, '--world', 'work'); run('replay-export', db, point, delta)
        shutil.rmtree(db); recovered = root/'restored'; run('replay', base, recovered); run('replay', delta, recovered); check(recovered, operation, True)
        payload.write_bytes(b'after-recovery'); at = size+3
        run('fs-write-at', recovered, path, at, payload, '--world', 'work')
        assert run('fs-read-at', recovered, path, at-1, 99, '--world', 'work') == b'\0after-recovery'
        run('fs-snapshot', recovered, 'continued', '--world', 'work'); run('fs-fork', recovered, 'continued', 'next')
        assert run('fs-read-at', recovered, path, at, 99, '--world', 'next') == b'after-recovery'
        run('fs-check', recovered, '--world', 'next'); shutil.rmtree(recovered); payload.write_bytes(patch)
print(f'{len(operations)*len(barriers)} operation/fault combinations and {len(operations)} writable replay recoveries passed')
