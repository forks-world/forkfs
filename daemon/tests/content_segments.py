"""Filesystem-level segment crash, recovery, and independent byte-model tests."""
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
BIN, FAULT = map(str, map(Path, sys.argv[1:3]))
def run(*args, binary=BIN, env=None, code=0):
    result = subprocess.run([binary, *map(str, args)], env=env, capture_output=True, timeout=30)
    assert result.returncode == code, (args, result.returncode, result.stderr)
    return result.stdout
with tempfile.TemporaryDirectory(prefix='ff-content-segments-') as name:
    root = Path(name); source = root/'source'; payload = root/'input'
    run('init', source); run('fs-init', source)
    rng = random.Random(8173); original = bytes(rng.randrange(256) for _ in range(int(sys.argv[3]) if len(sys.argv)>3 else 180000))
    payload.write_bytes(original); run('fs-write', source, '/file', payload)
    run('fs-snapshot', source, 'original'); run('fs-fork', source, 'original', 'branch')
    assert list(source.glob('DATA-*.seg'))
    for stage in ['segment_after_append', 'segment_after_sync', 'segment_before_metadata', 'segment_after_metadata']:
        db = root/stage; shutil.copytree(source, db)
        new = bytes(x ^ 0x55 for x in original); payload.write_bytes(new)
        env = dict(os.environ, FORKFS_TEST_CRASH=stage)
        run('fs-write', db, '/file', payload, binary=FAULT, env=env, code=86)
        expected = new if stage == 'segment_after_metadata' else original
        assert run('fs-cat', db, '/file') == expected
        assert run('fs-cat', db, '/file', '--revision', 'original') == original
        assert run('fs-cat', db, '/file', '--world', 'branch') == original
        run('fs-check', db)
        # New writes after a crash must not overwrite any previously referenced data.
        run('fs-write', db, '/file', payload)
        assert run('fs-cat', db, '/file') == new
        assert run('fs-cat', db, '/file', '--revision', 'original') == original
    for stage in ['partial_write', 'sync', 'metadata']:
        db = root/('io-' + stage); shutil.copytree(source, db)
        payload.write_bytes(bytes(x ^ 0xaa for x in original))
        env = dict(os.environ, FORKFS_TEST_SEGMENT_ERROR=stage)
        run('fs-write', db, '/file', payload, binary=FAULT, env=env, code=1)
        assert run('fs-cat', db, '/file') == original
        run('fs-check', db)
        run('fs-write', db, '/file', payload)
        assert run('fs-cat', db, '/file') == payload.read_bytes()
        assert run('fs-cat', db, '/file', '--revision', 'original') == original
    # An unindexed partial tail is garbage, not a reason to lose committed bytes.
    segment = next(source.glob('DATA-*.seg'))
    with segment.open('ab') as f: f.write(b'FFCHNK01-partial')
    # Random partial edits compared against an independent bytearray, including
    # chunk boundaries, EOF, and retention of the original revision.
    model = bytearray(original)
    for i in range(32):
        at = rng.choice([65535, 65536, 131071, len(model)-3, rng.randrange(len(model))])
        data = bytes(rng.randrange(256) for _ in range(min(37, len(model)-at)))
        payload.write_bytes(data); run('fs-write-at', source, '/file', at, payload)
        model[at:at+len(data)] = data
        assert run('fs-cat', source, '/file') == bytes(model)
    assert run('fs-cat', source, '/file', '--revision', 'original') == original
    base = root/'base.pack'; point = int(run('backup', source, base))
    payload.write_bytes(b'changed'); run('fs-write-at', source, '/file', 65536, payload)
    model[65536:65543] = b'changed'
    delta = root/'delta.pack'; run('replay-export', source, point, delta)
    # Force rollover without constructing millions of unrelated test objects.
    with segment.open('r+b') as f: f.truncate(64*1024*1024)
    payload.write_bytes(b'rollover'); run('fs-write-at', source, '/file', 1, payload)
    assert len(list(source.glob('DATA-*.seg'))) == 2
    shutil.rmtree(source)
    recovered = root/'recovered'; run('replay', base, recovered); run('replay', delta, recovered)
    assert run('fs-cat', recovered, '/file') == bytes(model)
    assert run('fs-cat', recovered, '/file', '--revision', 'original') == original
    assert run('fs-cat', recovered, '/file', '--world', 'branch') == original
    run('fs-check', recovered)
print('segment crash boundaries, random partial writes, rollover and source-free recovery passed')
