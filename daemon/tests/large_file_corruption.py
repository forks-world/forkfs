"""Rehash corrupted extent graphs and archives: transport checksums stay valid."""
import copy
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from replay_corruption import unpack, pack, base_rows, base_frames, h

BIN = sys.argv[1]
HISTORY = b'R/\0forkfs-history/'

def run(*args, success=True):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=30)
    assert (p.returncode == 0) == success, (args, p.returncode, p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(prefix='ff-large-corrupt-') as directory:
    root = Path(directory); source = root/'source'; payload = root/'input'
    run('init', source); run('fs-init', source)
    original = random.Random(173).randbytes(700000); payload.write_bytes(original)
    run('fs-write', source, '/file', payload); run('fs-snapshot', source, 'base')
    archive = root/'base.pack'; run('backup', source, archive)
    header, frames = unpack(archive.read_bytes()); baseline = base_rows(frames)
    leaves = [(key[2:], value) for key, value in baseline.items()
              if key.startswith(b'O/') and value[:9] == b'FFEXT001\0']
    assert len(leaves) == 1
    leaf_id, leaf = leaves[0]; count = int.from_bytes(leaf[9:11], 'little'); assert count > 1
    last_data = leaf[12+(count-1)*33:44+(count-1)*33]

    def rewritten(target, change):
        rows = copy.deepcopy(baseline); memo = {}
        def rewrite(ident, base=False):
            if (ident, base) in memo: return memo[ident, base]
            data = baseline[b'O/'+ident]
            if ident == target:
                data = change(data)
            else:
                offsets = []
                if base:
                    assert len(data) == 32; offsets = [0]
                elif data[:8] == b'FFTREE01': offsets = [8]
                elif data[:8] == b'FFTRNO01': offsets = [12, 44, 76]
                elif data[:8] == b'FFREV001': offsets = [8, 40]
                elif data[:8] == b'FFREV002': offsets = [8] + list(range(52, len(data), 32))
                elif data[:8] == b'FFFILE01': offsets = [16]
                elif data[:8] == b'FFEXT001': offsets = list(range(12, len(data), 33))
                data = bytearray(data)
                for at in offsets:
                    child = bytes(data[at:at+32])
                    if any(child): data[at:at+32] = rewrite(child)
                data = bytes(data)
            new = h(data); rows[b'O/'+new] = data; memo[ident, base] = new; return new
        for key, ident in baseline.items():
            if key.startswith(HISTORY+b'world/') or key.startswith(HISTORY+b'revision/'):
                rows[key] = rewrite(ident)
            elif key.startswith(HISTORY+b'base/'):
                rows[key] = rewrite(ident, True)
        rows[b'M/objects'] = struct.pack('<Q', sum(k.startswith(b'O/') for k in rows))
        return pack(header, base_frames(rows))

    cases = {
        'level': (leaf_id, lambda b: b[:8]+b'\x01'+b[9:]),
        'count': (leaf_id, lambda b: b[:9]+bytes(2)+b[11:]),
        'duplicate-slot': (leaf_id, lambda b: b[:44]+b[11:12]+b[45:]),
        'zero-child': (leaf_id, lambda b: b[:12]+bytes(32)+b[44:]),
        'missing-child': (leaf_id, lambda b: b[:12]+b'\xff'*32+b[44:]),
        'beyond-eof': (leaf_id, lambda b: b[:11+(count-1)*33]+b'\xff'+b[12+(count-1)*33:]),
        'short-data': (last_data, lambda b: b[:-1]),
        'nonzero-tail': (last_data, lambda b: b[:-1]+b'\xff'),
    }
    for seed in (1, 173, 4294967295):
        rng = random.Random(seed)
        level, slot, cut, tail, missing = rng.randrange(1, 256), rng.randrange(11, 256), rng.randrange(1, 65536), rng.randrange(1, 256), rng.randbytes(32)
        cases.update({
            f'level-{seed}': (leaf_id, lambda b, n=level: b[:8]+bytes([n])+b[9:]),
            f'missing-{seed}': (leaf_id, lambda b, n=missing: b[:12]+n+b[44:]),
            f'eof-{seed}': (leaf_id, lambda b, n=slot: b[:11+(count-1)*33]+bytes([n])+b[12+(count-1)*33:]),
            f'truncated-{seed}': (last_data, lambda b, n=cut: b[:n]),
            f'tail-{seed}': (last_data, lambda b, n=tail: b[:-1]+bytes([n])),
        })
    # A byte-identical logical rewrite is the positive control for the encoder.
    valid = root/'valid.pack'; valid.write_bytes(rewritten(leaf_id, lambda b: b))
    restored = root/'valid'; run('replay', valid, restored)
    assert run('fs-cat', restored, '/file') == original; run('fs-check', restored)
    for name, (target, change) in cases.items():
        bad = root/(name+'.pack'); bad.write_bytes(rewritten(target, change))
        destination = root/name; run('replay', bad, destination, success=False)
        assert not destination.exists(), (name, 'failed replay published destination')
    # Source-free restore remains valid after all rejected attempts.
    shutil.rmtree(source); second = root/'second'; run('replay', archive, second)
    assert run('fs-cat', second, '/file', '--revision', 'base') == original
print(f'{len(cases)} rehashed extent corruption cases rejected before publication')
