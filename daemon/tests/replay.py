"""Disaster restore from a detached base plus independently retained commit archives."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

BIN, FAULT = sys.argv[1:3]
def run(*args, binary=BIN, crash=None, ok=True):
    env = dict(os.environ)
    if crash:
        env['FORKFS_TEST_CRASH'] = crash
    p = subprocess.run([binary, *map(str, args)], capture_output=True, env=env, timeout=20)
    expected = 86 if crash else 0
    if ok:
        assert p.returncode == expected, (args, p.returncode, p.stderr)
    else:
        assert p.returncode != 0, (args, p.stdout)
    return p.stdout

def seq(path):
    return json.loads(run('check', path))['last_committed_seq']

def rewrite(data, mutate):
    """Recompute transport checksums to test structural, not just checksum rejection."""
    header = data[:105]
    frames, pos = [], 105
    while pos < len(data):
        length, = struct.unpack_from('<I', data, pos)
        payload = data[pos+4:pos+4+length]
        pos += 4 + length + 32
        if payload[:1] == b'E':
            break
        frames.append(payload)
    header, frames = mutate(header, frames)
    digest = hashlib.sha256(header).digest()
    out = bytearray(header)
    for payload in frames:
        h = hashlib.sha256(payload).digest()
        out += struct.pack('<I', len(payload)) + payload + h
        digest = hashlib.sha256(digest+h).digest()
    footer = b'E'+digest
    return bytes(out + struct.pack('<I', len(footer))+footer+hashlib.sha256(footer).digest())

with tempfile.TemporaryDirectory(prefix='ff-replay-') as temp:
    root = Path(temp)
    source, recovered, base = root/'source', root/'recovered', root/'base.pack'
    payload = root/'input'
    run('init', source)
    run('fs-init', source)
    payload.write_bytes(b'base\0bytes')
    run('fs-write', source, '/file', payload)
    run('fs-link', source, '/file', '/alias')
    run('fs-symlink', source, 'file', '/symbolic')
    run('fs-mkdir', source, '/dir')
    run('fs-snapshot', source, 'r1')
    run('fs-fork', source, 'r1', 'work')
    run('replay-export', source, 0, root/'disabled', ok=False)
    first = int(run('backup', source, base))
    assert first == seq(source)
    assert int(run('replay', base, recovered)) == first
    assert int(run('replay', base, recovered)) == first
    run('fs-check', recovered)
    payload.write_bytes(b'new\xffcontent')
    run('fs-write', source, '/file', payload, '--world', 'work')
    run('fs-chmod', source, '/file', '0600', '--world', 'work')
    run('fs-rename', source, '/dir', '/moved', '--world', 'work')
    run('fs-snapshot', source, 'r2', '--world', 'work')
    part1 = root/'part1.pack'
    second = int(run('replay-export', source, first, part1))
    run('fs-restore', source, 'r1', '--world', 'work')
    run('fs-unlink', source, '/alias')
    run('fs-compact', source)
    part2 = root/'part2.pack'
    third = int(run('replay-export', source, second, part2))
    all_delta = root/'all.pack'
    assert int(run('replay-export', source, first, all_delta)) == third
    # Gap rejection and unchanged destination, including after reopen.
    run('replay', part2, recovered, ok=False)
    assert seq(recovered) == first
    assert int(run('replay', part1, recovered)) == second
    assert int(run('replay', part1, recovered)) == second
    assert run('fs-cat', recovered, '/alias', '--world', 'work') == b'new\xffcontent'
    assert int(run('replay', all_delta, recovered)) == third  # Overlapping prefix.
    assert int(run('replay', part2, recovered)) == third
    assert int(run('replay', base, recovered)) == third
    assert run('fs-cat', recovered, '/file', '--world', 'work') == b'base\0bytes'
    assert run('fs-cat', recovered, '/file', '--revision', 'r2') == b'new\xffcontent'
    assert run('fs-ls', recovered, '/') == run('fs-ls', source, '/')
    assert run('fs-worlds', recovered) == run('fs-worlds', source)
    assert run('fs-revisions', recovered) == run('fs-revisions', source)
    run('fs-check', recovered)
    # A second base starts at a later cursor; no zero-sequence assumption.
    later_base = root/'later-base.pack'
    assert int(run('backup', source, later_base)) == third
    later = root/'later'
    assert int(run('replay', later_base, later)) == third
    run('replay', all_delta, later, ok=False)  # Before its retained base.
    empty = root/'empty.pack'
    assert int(run('replay-export', source, third, empty)) == third
    assert int(run('replay', empty, later)) == third
    # Different lineage and locally diverged destinations must not be overwritten.
    foreign = root/'foreign'
    run('init', foreign)
    run('fs-init', foreign)
    run('backup', foreign, root/'foreign.pack')
    foreign_seq = seq(foreign)
    run('replay', part1, foreign, ok=False)
    run('replay', base, foreign, ok=False)
    assert seq(foreign) == foreign_seq
    divergent = root/'divergent'
    run('replay', base, divergent)
    run('fs-mkdir', divergent, '/local')
    before = seq(divergent)
    run('replay', part1, divergent, ok=False)
    assert seq(divergent) == before
    assert b'local\n' in run('fs-ls', divergent, '/')
    # Truncation, bit corruption, suffixes, missing/reordered/duplicated commits.
    data = all_delta.read_bytes()
    malformed = [data[:cut] for cut in [0, 8, 104, 109, len(data)//2, len(data)-1]]
    corrupt = bytearray(data);corrupt[len(data)//2] ^= 1;malformed += [bytes(corrupt), data+b'x']
    malformed += [rewrite(data, lambda h, f: (h, f[1:])),
                  rewrite(data, lambda h, f: (h, f[::-1])),
                  rewrite(data, lambda h, f: (h, f+[f[-1]]))]
    clean = root/'clean'
    run('replay', base, clean)
    for i, contents in enumerate(malformed):
        bad = root/('bad-'+str(i));bad.write_bytes(contents)
        run('replay', bad, clean, ok=False)
        assert seq(clean) == first
    # Backup corruption must not publish even an empty destination.
    bad_base = root/'bad-base.pack'
    bad_base.write_bytes(base.read_bytes()[:-1])
    absent = root/'absent'
    run('replay', bad_base, absent, ok=False)
    assert not absent.exists()
    link = root/'target-link';link.symlink_to(clean, target_is_directory=True)
    run('replay', part1, link, ok=False)
    # Replay crash before/after a single atomic commit, then resume entire archive.
    for stage, committed in [('replay_before_write', False), ('replay_after_write', True)]:
        target = root/stage
        run('replay', base, target)
        run('replay', all_delta, target, binary=FAULT, crash=stage)
        assert seq(target) == first + int(committed)
        assert int(run('replay', all_delta, target)) == third
        assert run('fs-cat', target, '/file', '--revision', 'r2') == b'new\xffcontent'
        run('fs-check', target)
    # Base publication is all-or-nothing; a lost success acknowledgement is retryable.
    for stage, published in [('replay_base_rows', False), ('replay_base_before_publish', False), ('replay_base_after_publish', True)]:
        target = root/stage
        run('replay', base, target, binary=FAULT, crash=stage)
        assert target.exists() == published
        assert int(run('replay', base, target)) == first
        run('fs-check', target)
    # Original commit and retained replay record share the same durable WriteBatch.
    for stage, committed in [('leveldb_before_write', False), ('leveldb_after_write', True)]:
        writer = root/('writer-'+stage)
        shutil.copytree(source, writer)
        run('fs-mkdir', writer, '/crashed', binary=FAULT, crash=stage)
        delta = root/(stage+'.pack')
        assert int(run('replay-export', writer, third, delta)) == third+int(committed)
        target = root/('reader-'+stage)
        run('replay', later_base, target)
        run('replay', delta, target)
        assert (b'crashed\n' in run('fs-ls', target, '/')) == committed
        run('check', writer)
    # Enabling replay and upgrading the format must survive either crash edge.
    for stage in ['replay_enable_before_write', 'replay_enable_after_write']:
        writer = root/stage
        run('init', writer)
        run('fs-init', writer)
        old_seq = seq(writer)
        run('backup', writer, root/(stage+'.first'), binary=FAULT, crash=stage)
        assert seq(writer) == old_seq
        completed = root/(stage+'.pack')
        assert int(run('backup', writer, completed)) == old_seq
        run('fs-mkdir', writer, '/after-enable')
        delta = root/(stage+'.delta')
        run('replay-export', writer, old_seq, delta)
        target = root/(stage+'-target')
        run('replay', completed, target)
        run('replay', delta, target)
        assert b'after-enable\n' in run('fs-ls', target, '/')
    # Recovery remains usable with the original machine entirely gone.
    shutil.rmtree(source)
    disaster = root/'disaster'
    run('replay', base, disaster)
    run('replay', all_delta, disaster)
    run('fs-fork', disaster, 'r2', 'recovery-work')
    assert run('fs-cat', disaster, '/file', '--world', 'recovery-work') == b'new\xffcontent'
    run('check', disaster)
print('backup, incremental disaster replay, integrity, lineage and crash recovery passed')
