"""Independently encoded corruption fixtures with VALID transport/object hashes.

Every negative case starts from real revisions. Mutations are re-encoded with
correct frame/footer hashes, so revision and graph checks must actually run.
"""
import hashlib
import json
import itertools
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BIN = None
HISTORY = b'\0forkfs-history/'
VICTIM = b'R/'+HISTORY+b'revision/base'

def run(*args, error=None):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=20)
    if error is None:
        assert p.returncode == 0, (args, p.stderr)
    else:
        assert p.returncode != 0 and error.encode() in p.stderr, (args, error, p.stderr)
    return p.stdout

state_number = itertools.count()
def logical_state(path):
    status = json.loads(run('check', path))
    stable = {key: status[key] for key in ['repository_id', 'last_committed_seq', 'indexed_objects', 'object_roots']}
    image = path.parent/('state-'+str(next(state_number))+'.pack')
    run('backup', path, image)
    return stable, image.read_bytes()  # All roots/objects, not volatile IO timings.

def h(data):
    return hashlib.sha256(b'forkfs.object.v0'+struct.pack('<HHQ', 4, 0, len(data))+data).digest()

def unpack(data):
    header, pos, frames = data[:105], 105, []
    chain = h(header)
    while pos < len(data):
        n, = struct.unpack_from('<I', data, pos)
        payload = data[pos+4:pos+4+n]
        assert h(payload) == data[pos+4+n:pos+4+n+32]
        pos += n+36
        if payload[:1] == b'E':
            assert payload == b'E'+chain and pos == len(data)
            return header, frames
        frames.append(payload)
        chain = h(chain+h(payload))
    raise AssertionError('missing footer')

def pack(header, frames):
    out, chain = bytearray(header), h(header)
    for payload in frames:
        digest = h(payload)
        out += struct.pack('<I', len(payload))+payload+digest
        chain = h(chain+digest)
    end = b'E'+chain
    return bytes(out+struct.pack('<I', len(end))+end+h(end))

def base_rows(frames):
    rows = {}
    for f in frames:
        assert f[:1] == b'B'
        kn, vn = struct.unpack_from('<II', f, 1)
        assert len(f) == 9+kn+vn
        rows[f[9:9+kn]] = f[9+kn:]
    return rows

def base_frames(rows):
    return [b'B'+struct.pack('<II', len(k), len(v))+k+v for k, v in sorted(rows.items())]

def put_object(rows, value):
    oid = h(value)
    rows[b'O/'+oid] = value
    return oid

def descriptor(rows):
    return rows[b'O/'+rows[VICTIM]]

def replace_revision(rows, value):
    rows[VICTIM] = put_object(rows, value)

def revision_source(b):
    return struct.unpack_from('<Q', b, 72 if b[:8]==b'FFREV001' else 40)[0]

def revision_with_source(b, sequence):
    at = 72 if b[:8]==b'FFREV001' else 40
    return b[:at]+struct.pack('<Q', sequence)+b[at+8:]

def revision_with_parents(b, parents):
    return b'FFREV002'+b[8:40]+struct.pack('<QI', revision_source(b), len(parents))+b''.join(parents)

def patch_revision(rows, offset, value):
    b = bytearray(descriptor(rows));b[offset:offset+len(value)] = value
    replace_revision(rows, bytes(b))

def patch_manifest(rows, mutate):
    d = descriptor(rows)
    manifest = rows[b'O/'+d[8:40]]
    patch_revision(rows, 8, put_object(rows, mutate(manifest)))

def patch_tree(rows, mutate):
    def manifest(m):
        assert m[:8] == b'FFTREE01'
        node = rows[b'O/'+m[8:40]]
        return m[:8]+put_object(rows, mutate(node))
    patch_manifest(rows, manifest)

def tree_entries(rows):
    manifest = rows[b'O/'+descriptor(rows)[8:40]]
    def walk(oid):
        if oid == bytes(32):
            return []
        node = rows[b'O/'+oid]
        return walk(node[44:76])+[(node[108:], node[12:44])]+walk(node[76:108])
    return walk(manifest[8:40])

def full_manifest(entries):
    return b'FFMAN001'+struct.pack('<I', len(entries))+b''.join(struct.pack('<I', len(k))+k+v for k, v in entries)

def remove_transitive_content(rows):
    oid = h(b'old content\0')
    del rows[b'O/'+oid]
    for key, value in list(rows.items()):
        if key.startswith(b'R/') and value == oid:
            del rows[key]

def counts(rows):
    rows[b'M/objects'] = struct.pack('<Q', sum(k.startswith(b'O/') for k in rows))
    rows[b'M/roots'] = struct.pack('<Q', sum(k.startswith(b'R/') for k in rows))

def operations(frame):
    assert frame[:1] == b'T'
    count, = struct.unpack_from('<I', frame, 41)
    pos, result = 45, []
    for _ in range(count):
        op, kn, vn = struct.unpack_from('<BII', frame, pos);pos += 9
        k, v = frame[pos:pos+kn], frame[pos+kn:pos+kn+vn];pos += kn+vn
        result.append((op, k, v))
    assert pos == len(frame)
    return result

def encode_ops(ops):
    return struct.pack('<I', len(ops))+b''.join(struct.pack('<BII', op, len(k), len(v))+k+v for op, k, v in ops)

def corrupt_revision_tx(frame, key, mutate):
    ops = operations(frame)
    original_id = next(v for op, k, v in ops if k == key)
    original = next(v for op, k, v in ops if k == b'O/'+original_id)
    bad = mutate(original);oid = h(bad)
    changed = [(op, b'O/'+oid, bad) if k == b'O/'+original_id else
               (op, k, oid) if k == key else (op, k, v) for op, k, v in ops]
    # Keep the original descriptor for base/main's independent reference.
    changed.append((1, b'O/'+original_id, original))
    changed = [(op, k, struct.pack('<Q', struct.unpack('<Q', v)[0]+1)) if k == b'M/objects' else (op, k, v) for op, k, v in changed]
    return frame[:41]+encode_ops(changed)

def rechain(header, frames):
    previous, result = header[41:73], []
    for f in frames:
        f = f[:9]+previous+f[41:]
        result.append(f);previous = h(f)
    return header[:73]+previous, result

def main():
    global BIN
    BIN = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='ff-replay-corruption-') as temp:
        root = Path(temp);source = root/'source';payload = root/'payload'
        run('init', source);run('fs-init', source)
        payload.write_bytes(b'old content\0')
        run('fs-write', source, '/file', payload);run('fs-snapshot', source, 'base')
        payload.write_bytes(b'new content')
        run('fs-write', source, '/file', payload);run('fs-snapshot', source, 'child')
        archive = root/'base.pack';first = int(run('backup', source, archive))
        data = archive.read_bytes();header, frames = unpack(data);rows = base_rows(frames)
        # Positive controls: independent encoders produce byte-identical valid archives.
        assert pack(header, frames) == data
        assert pack(header, base_frames(rows)) == data
        control = root/'control';run('replay', archive, control)
        assert run('fs-cat', control, '/file', '--revision', 'base') == b'old content\0'
        cases = []
        def add(name, change, error):
            altered = dict(rows);change(altered);counts(altered)
            cases.append((name, pack(header, base_frames(altered)), error))
        add('revision-magic', lambda r: replace_revision(r, b'BROKEN00'+descriptor(r)[8:]), 'invalid replay revision descriptor')
        add('revision-short', lambda r: replace_revision(r, descriptor(r)[:-1]), 'invalid replay revision descriptor')
        add('revision-long', lambda r: replace_revision(r, descriptor(r)+b'x'), 'invalid replay revision descriptor')
        add('revision-zero-manifest', lambda r: patch_revision(r, 8, bytes(32)), 'zero revision/World manifest')
        add('revision-missing-manifest', lambda r: patch_revision(r, 8, b'\xff'*32), 'read recovery object')
        add('revision-missing-parent', lambda r: replace_revision(r, revision_with_parents(descriptor(r), [b'\xff'*32])), 'read recovery object')
        add('revision-parent-not-descriptor', lambda r: replace_revision(r, revision_with_parents(descriptor(r), [put_object(r, b'not a revision')])), 'invalid replay revision descriptor')
        add('revision-excess-parents', lambda r: replace_revision(r, revision_with_parents(descriptor(r), [b'x'*32]*65)), 'invalid replay revision descriptor')
        add('revision-invalid-name', lambda r: r.__setitem__(b'R/'+HISTORY+b'revision/../escape', r[VICTIM]), 'invalid World/revision name')
        add('world-base-length', lambda r: r.__setitem__(b'R/'+HISTORY+b'base/main', put_object(r, b'x')), 'invalid replay World base')
        add('manifest-unknown-format', lambda r: patch_manifest(r, lambda m: b'BADMAGIC'+m[8:]), 'unsupported namespace manifest')
        add('manifest-short', lambda r: patch_manifest(r, lambda m: m[:-1]), 'unsupported namespace manifest')
        add('manifest-zero-tree', lambda r: patch_manifest(r, lambda m: m[:8]+bytes(32)), 'empty metadata tree root')
        add('manifest-missing-tree', lambda r: patch_manifest(r, lambda m: m[:8]+b'\xff'*32), 'read recovery object')
        add('tree-bad-magic', lambda r: patch_tree(r, lambda n: b'BADMAGIC'+n[8:]), 'invalid metadata tree node')
        add('tree-short', lambda r: patch_tree(r, lambda n: n[:107]), 'invalid metadata tree node')
        add('tree-length', lambda r: patch_tree(r, lambda n: n[:8]+struct.pack('<I', 1025)+n[12:]), 'invalid metadata node length')
        add('tree-zero-value', lambda r: patch_tree(r, lambda n: n[:12]+bytes(32)+n[44:]), 'zero metadata value reference')
        add('tree-missing-value', lambda r: patch_tree(r, lambda n: n[:12]+b'\xff'*32+n[44:]), 'read recovery object')
        add('tree-missing-child', lambda r: patch_tree(r, lambda n: n[:44]+b'\xff'*32+n[76:]), 'read recovery object')
        add('tree-invalid-key', lambda r: patch_tree(r, lambda n: n[:108]+b'X'+n[109:]), 'invalid metadata node key')
        add('transitive-content-missing', remove_transitive_content, 'read recovery object')
        add('manifest-run-count-zero', lambda r: patch_manifest(r, lambda m: b'FFMAN002'+bytes(4)), 'invalid manifest run count')
        add('manifest-run-zero', lambda r: patch_manifest(r, lambda m: b'FFMAN002'+struct.pack('<I', 1)+bytes(32)), 'duplicate or zero run reference')
        add('manifest-run-missing', lambda r: patch_manifest(r, lambda m: b'FFMAN002'+struct.pack('<I', 1)+b'\xff'*32), 'read recovery object')
        add('run-unordered', lambda r: patch_manifest(r, lambda m: full_manifest(tree_entries(r)[::-1])), 'unordered run keys')
        add('run-duplicate', lambda r: patch_manifest(r, lambda m: full_manifest(tree_entries(r)+[tree_entries(r)[-1]])), 'unordered run keys')
        add('run-zero-value', lambda r: patch_manifest(r, lambda m: full_manifest([(k, bytes(32)) for k, v in tree_entries(r)])), 'zero PUT reference')
        add('revision-object-bitflip', lambda r: r.__setitem__(b'O/'+r[VICTIM], b'X'+descriptor(r)[1:]), 'invalid replay object hash')
        add('revision-object-missing', lambda r: r.pop(b'O/'+r[VICTIM]), 'verify root reference')
        add('root-short-reference', lambda r: r.__setitem__(VICTIM, r[VICTIM][:-1]), 'invalid replay root')
        add('world-manifest-malformed', lambda r: r.__setitem__(b'R/'+HISTORY+b'world/main', put_object(r, b'BADMAGIC')), 'unsupported namespace manifest')
        for name, encoded, error in cases:
            damaged, target = root/(name+'.pack'), root/name
            damaged.write_bytes(encoded);run('replay', damaged, target, error=error)
            assert not target.exists(), name
        # Older full-map and sorted-run revision formats remain readable.
        for kind in ['full', 'runs']:
            changed = dict(rows)
            entries = tree_entries(changed)
            if kind == 'full':
                manifest = full_manifest(entries)
            else:
                run_data = b'FFRUN001'+struct.pack('<I', len(entries))+b''.join(struct.pack('<II', 1, len(k))+k+v for k, v in entries)
                manifest = b'FFMAN002'+struct.pack('<I', 1)+put_object(changed, run_data)
            patch_revision(changed, 8, put_object(changed, manifest));counts(changed)
            legacy = root/(kind+'.pack');legacy.write_bytes(pack(header, base_frames(changed)))
            target = root/kind;run('replay', legacy, target)
            assert run('fs-cat', target, '/file', '--revision', 'base') == b'old content\0'
            run('fs-check', target, '--revision', 'base')
        # Exercise a hash-valid malformed revision arriving in an incremental commit.
        run('fs-snapshot', source, 'new-revision')
        delta = root/'delta.pack';last = int(run('replay-export', source, first, delta))
        dh, df = unpack(delta.read_bytes());assert len(df) == 1
        assert pack(*rechain(dh, df)) == delta.read_bytes()
        ops = operations(df[0]);revision_key = b'R/'+HISTORY+b'revision/new-revision'
        rev_id = next(v for op, k, v in ops if k == revision_key)
        original = next(v for op, k, v in ops if k == b'O/'+rev_id)
        replacements = [('short', original[:-1], 'invalid replay revision descriptor'),
                        ('parent', revision_with_parents(original, [b'\xff'*32]), 'missing replay graph object'),
                        ('manifest', original[:8]+b'\xff'*32+original[40:], 'missing replay graph object')]
        for name, bad, error in replacements:
            bad_frame = corrupt_revision_tx(df[0], revision_key, lambda original: bad)
            damaged = root/('delta-'+name+'.pack');damaged.write_bytes(pack(*rechain(dh, [bad_frame])))
            target = root/('delta-'+name);run('replay', archive, target)
            before = logical_state(target)
            run('replay', damaged, target, error=error)
            assert logical_state(target) == before
            assert run('fs-cat', target, '/file', '--revision', 'base') == b'old content\0'
            assert int(run('replay', delta, target)) == last
            run('fs-check', target, '--revision', 'new-revision')
        # Record-level corruption reaches exact structural checks after re-encoding.
        for name, altered, error in [
            ('duplicate-key', ops+[ops[0]], 'duplicate replay key'),
            ('unknown-metadata', ops+[(1, b'M/unknown', bytes(8))], 'invalid replay metadata operation'),
            ('counter', [(op, k, bytes(8) if k == b'M/roots' else v) for op, k, v in ops], 'replay root count mismatch'),
            ('missing-object', [(op, k, b'\xff'*32 if k == revision_key else v) for op, k, v in ops], 'replay root references missing object')]:
            damaged = root/('record-'+name+'.pack')
            damaged.write_bytes(pack(*rechain(dh, [df[0][:41]+encode_ops(altered)])))
            target = root/('record-'+name);run('replay', archive, target)
            before = logical_state(target);run('replay', damaged, target, error=error)
            assert logical_state(target) == before
            assert int(run('replay', delta, target)) == last
        # A bad later transaction leaves only the valid committed prefix; retry resumes.
        run('fs-snapshot', source, 'next-revision')
        two = root/'two.pack';end = int(run('replay-export', source, first, two))
        th, tf = unpack(two.read_bytes());assert len(tf) == 2
        tf[-1] = corrupt_revision_tx(tf[-1], b'R/'+HISTORY+b'revision/next-revision', lambda value: value[:-1])
        damaged = root/'bad-second.pack';damaged.write_bytes(pack(*rechain(th, tf)))
        target = root/'valid-prefix';run('replay', archive, target)
        run('replay', damaged, target, error='invalid replay revision descriptor')
        assert json.loads(run('check', target))['last_committed_seq'] == last
        run('fs-check', target, '--revision', 'new-revision')
        assert b'next-revision\n' not in run('fs-revisions', target)
        assert int(run('replay', two, target)) == end
        run('fs-check', target, '--revision', 'next-revision')
        # Bounded parser and file-type failures must reject before opening a target.
        for n in [0, 24*1024*1024+1, 0xffffffff]:
            bad = root/('length-'+str(n));bad.write_bytes(data[:105]+struct.pack('<I', n)+data[109:])
            target = root/('length-target-'+str(n));run('replay', bad, target, error='invalid replay frame length')
            assert not target.exists()
        fifo = root/'fifo';os.mkfifo(fifo)
        run('replay', fifo, root/'fifo-target', error='regular file')
        directory = root/'directory';directory.mkdir()
        run('replay', directory, root/'directory-target', error='regular file')
        symlink = root/'archive-link';symlink.symlink_to(archive)
        run('replay', symlink, root/'link-target', error='open replay file')
    print('revision ancestry, CoW references and structurally valid corrupt replay archives rejected')


if __name__ == '__main__':
    main()
