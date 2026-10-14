"""Portable append-only catalogs, independent receipt codec and crash publication."""
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from replay_corruption import h, unpack

BIN, FAULT = sys.argv[1:3]
def run(*args, ok=True, crash=None):
    env = dict(os.environ)
    if crash:
        env['FORKFS_TEST_CRASH'] = crash
    p = subprocess.run([FAULT if crash else BIN, *map(str, args)], capture_output=True,
                       env=env, timeout=30)
    assert p.returncode == ((86 if crash else 0) if ok else 1), (args, p.returncode, p.stderr)
    return p.stdout

def entries(catalog):
    return json.loads(run('catalog-list', catalog))['archives']

def point_args(point):
    return point['sequence'], point['repository'], point['chain_hash']

with tempfile.TemporaryDirectory(prefix='ff-catalog-') as directory:
    root = Path(directory)
    source, base, catalog = root/'source', root/'base', root/'catalog'
    run('init', source);run('fs-init', source)
    start = int(run('backup', source, base));archives = [base]
    rng = random.Random(173);payload = root/'input'
    for i in range(4):
        value = rng.randbytes(rng.randrange(1, 8000));payload.write_bytes(value)
        run('fs-write', source, '/file', payload);run('fs-snapshot', source, 'r'+str(i))
        delta = root/('delta'+str(i));start = int(run('replay-export', source, start, delta));archives.append(delta)
    point = json.loads(run('fs-revision-point', source, 'r3'))['checkpoint']
    # Writers serialize without losing entries, including simultaneous duplicates.
    procs = [subprocess.Popen([BIN, 'catalog-add', str(catalog), str(p)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE) for p in archives+archives]
    for proc in procs:
        out, err = proc.communicate(timeout=30);assert proc.returncode == 0, (out, err)
    saved = entries(catalog);assert len(saved) == len(archives)
    for entry in saved:
        image = Path(entry['archive']).read_bytes();header, frames = unpack(image)
        digest = h(header)
        for frame in frames:
            digest = h(digest+h(frame))
        expected = h(b'forkfs.catalog.package.v1'+struct.pack('<Q', len(image))+digest)
        assert entry['digest'] == expected.hex()
        record = b'FFCAT001'+header+struct.pack('<Q', len(image))+expected
        record += h(record)
        assert (catalog/(h(record[:-32]).hex()+'.entry')).read_bytes() == record
        assert entry['size'] == len(image)
        assert entry['checkpoint']['sequence'] == struct.unpack_from('<Q', header, 33)[0]
        assert entry['checkpoint']['chain_hash'] == header[73:105].hex()
    # Catalog owns immutable copies; recovery works after losing the source machine.
    shutil.rmtree(source)
    for p in archives:
        p.unlink()
    moved = root/'moved';catalog.rename(moved)
    plan = json.loads(run('restore-plan', *point_args(point), '--catalog', moved))['steps']
    assert len(plan) == 5 and all(str(moved) in s['archive'] for s in plan)
    restored = root/'restored'
    assert int(run('restore', restored, *point_args(point), '--catalog', moved)) == point['sequence']
    assert run('fs-cat', restored, '/file') == value
    # Reimporting a catalog's own package is idempotent.
    package = Path(entries(moved)[0]['archive'])
    run('catalog-add', moved, package);assert len(entries(moved)) == 5
    for stage, visible in [('catalog_before_package', 0), ('catalog_after_package', 0),
                           ('catalog_before_entry', 0), ('catalog_after_entry', 1)]:
        dest = root/stage
        run('catalog-add', dest, package, crash=stage)
        assert len(entries(dest)) == visible
        run('catalog-add', dest, package);assert len(entries(dest)) == 1
    # A foreign repository is rejected without changing published records.
    foreign = root/'foreign';run('init', foreign)
    foreign_pack = root/'foreign.pack';run('backup', foreign, foreign_pack)
    mixed = root/'mixed'
    writers = [subprocess.Popen([BIN, 'catalog-add', str(mixed), str(p)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE) for p in (package, foreign_pack)]
    codes = []
    for writer in writers:
        writer.communicate(timeout=30);codes.append(writer.returncode)
    assert sorted(codes) == [0, 1] and len(entries(mixed)) == 1
    # Parent symlinks (e.g. macOS /tmp) are allowed; the catalog itself is not a symlink.
    alias = root/'parent-alias';alias.symlink_to(root, target_is_directory=True)
    run('catalog-add', alias/'nested', package);assert len(entries(root/'nested')) == 1
    previous = entries(moved)
    run('catalog-add', moved, foreign_pack, ok=False);assert entries(moved) == previous
    receipt = next(moved.glob('*.entry'));original = receipt.read_bytes()
    damaged_images = [original[:cut] for cut in (0, 7, 112, len(original)-1)]
    damaged_images += [original+b'x', bytes([original[0]^1])+original[1:]]
    for i, image in enumerate(damaged_images):
        bad = root/('bad-record-'+str(i));shutil.copytree(moved, bad)
        (bad/receipt.name).write_bytes(image)
        run('catalog-list', bad, ok=False)
        dest = root/('bad-restored-'+str(i))
        run('restore', dest, *point_args(point), '--catalog', bad, ok=False);assert not dest.exists()
    # A checksum-valid receipt with altered package size must still be rejected.
    bad = root/'wrong-size';shutil.copytree(moved, bad)
    altered = bytearray(original[:-32]);struct.pack_into('<Q', altered, 113, 1)
    altered = bytes(altered);(bad/receipt.name).unlink()
    (bad/(h(altered).hex()+'.entry')).write_bytes(altered+h(altered))
    run('catalog-list', bad, ok=False)
    for damage in ('missing', 'corrupt', 'symlink'):
        bad = root/('bad-package-'+damage);shutil.copytree(moved, bad)
        victim = bad/package.name
        if damage == 'missing':
            victim.unlink()
        elif damage == 'corrupt':
            victim.write_bytes(victim.read_bytes()[:-1])
        else:
            victim.unlink();victim.symlink_to(package)
        run('catalog-list', bad, ok=False)
        run('restore-plan', *point_args(point), '--catalog', bad, ok=False)
    link = root/'linked-catalog';link.symlink_to(moved, target_is_directory=True)
    run('catalog-list', link, ok=False)
    broken = root/'broken';broken.write_bytes(b'not an archive')
    dest = root/'new-catalog';run('catalog-add', dest, broken, ok=False)
    assert entries(dest) == [] and not list(dest.glob('.catalog-add-*'))
print('catalog identity, concurrency, relocation, corruption and crash publication passed')
