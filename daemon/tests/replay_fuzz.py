"""Seeded state-machine and archive mutation fuzzing, with retained repro inputs.

No production filesystem code is used in the state oracle. The independent Python
archive codec is shared with the fixed corruption regressions, not with C++.
"""
import argparse
import copy
from dataclasses import dataclass
import hashlib
import itertools
import json
from pathlib import Path
import random
import secrets
import shutil
import struct
import subprocess
import tempfile
import traceback

from replay_corruption import unpack, pack, base_rows, base_frames, counts, h, rechain

@dataclass
class Node:
    kind: str
    data: bytes = b''
    mode: int = 0o644

class Trial:
    def __init__(self, binary, seed, root, steps, mutations, corpus):
        self.binary, self.seed, self.root = binary, seed, root
        self.rng = random.Random(seed)
        self.steps, self.mutations, self.corpus = steps, mutations, corpus
        self.serial = itertools.count()
        self.worlds = {'main': {'/': Node('dir', mode=0o755)}}
        self.revisions = {}
        self.counts = {}
        self.trace = root/'events.jsonl'
        (root/'seed.json').write_text(json.dumps({'seed': seed, 'steps': steps, 'mutations': mutations, 'binary': str(binary)}, indent=2))

    def call(self, *args, allow_error=False, expected_error=None):
        argv = [str(self.binary), *map(str, args)]
        event = {'argv': argv[1:]}
        try:
            p = subprocess.run(argv, capture_output=True, timeout=15)
            event.update(exit=p.returncode, stdout=p.stdout[:1024].hex(), stderr=p.stderr.decode(errors='replace'))
            if (p.returncode not in ([0, 1] if allow_error else [0])
                    or b'Sanitizer' in p.stderr or b'runtime error:' in p.stderr):
                raise AssertionError(event)  # Signals, sanitizer errors and timeouts are failures.
            if expected_error is not None:
                assert p.returncode == 1 and expected_error.encode() in p.stderr, event
            return p.returncode, p.stdout
        finally:
            with self.trace.open('a') as out:
                out.write(json.dumps(event)+'\n')

    def run(self, *args):
        return self.call(*args)[1]

    def record(self, operation, **args):
        self.counts[operation] = self.counts.get(operation, 0)+1
        with self.trace.open('a') as out:
            out.write(json.dumps({'model_operation': operation, **args})+'\n')

    def bytes(self):
        n = self.rng.choice([0, 1, 2, 7, 31, 255, 512])
        return bytes(self.rng.randrange(256) for _ in range(n))

    def fresh(self, state):
        parent = self.rng.choice([p for p, n in state.items() if n.kind == 'dir'])
        return parent.rstrip('/')+'/n'+str(next(self.serial))

    def operation(self, source):
        world = self.rng.choice(sorted(self.worlds));state = self.worlds[world]
        files = [p for p, n in state.items() if n.kind == 'file']
        choices = ['write']*4+['mkdir', 'snapshot', 'snapshot', 'compact']
        if files:
            choices += ['write', 'append', 'truncate', 'link', 'chmod', 'symlink']
        if len(state)>1:
            choices += ['rename', 'unlink']
        if self.revisions:
            choices += ['restore']
            if len(self.worlds)<4:
                choices += ['fork']
        op = self.rng.choice(choices)
        selector = ['--world', world]
        if op in ['write', 'append', 'truncate', 'chmod', 'link', 'symlink']:
            path = self.rng.choice(files) if files and (op != 'write' or self.rng.random()<0.5) else self.fresh(state)
            if op == 'link':
                new = self.fresh(state);self.run('fs-link', source, path, new, *selector);state[new] = state[path]
                self.record(op, world=world, path=path, target=new)
            elif op == 'symlink':
                new = self.fresh(state);self.run('fs-symlink', source, path, new, *selector);state[new] = Node('symlink', path.encode(), 0o777)
                self.record(op, world=world, path=new, target=path)
            elif op == 'chmod':
                mode = self.rng.randrange(0o1000);self.run('fs-chmod', source, path, format(mode, 'o'), *selector);state[path].mode = mode
                self.record(op, world=world, path=path, mode=mode)
            elif op == 'truncate':
                size = self.rng.randrange(1025);self.run('fs-truncate', source, path, size, *selector);state[path].data = state[path].data[:size].ljust(size, b'\0')
                self.record(op, world=world, path=path, size=size)
            else:
                data = self.bytes();payload = self.root/'input';payload.write_bytes(data)
                self.run('fs-'+op, source, path, payload, *selector)
                if path not in state:
                    state[path] = Node('file')
                state[path].data = state[path].data+data if op == 'append' else data
                self.record(op, world=world, path=path, data=data.hex())
        elif op == 'mkdir':
            path = self.fresh(state);self.run('fs-mkdir', source, path, *selector);state[path] = Node('dir', mode=0o755);self.record(op, world=world, path=path)
        elif op == 'rename':
            path = self.rng.choice([p for p in state if p != '/'])
            parents = [p for p, n in state.items() if n.kind == 'dir' and p != path and not p.startswith(path+'/')]
            new = self.rng.choice(parents).rstrip('/')+'/n'+str(next(self.serial))
            self.run('fs-rename', source, path, new, *selector)
            moved = {new+p[len(path):]: n for p, n in state.items() if p == path or p.startswith(path+'/')}
            for p in list(state):
                if p == path or p.startswith(path+'/'):
                    del state[p]
            state.update(moved);self.record(op, world=world, path=path, target=new)
        elif op == 'unlink':
            paths = [p for p, n in state.items() if p != '/' and (n.kind != 'dir' or not any(q.startswith(p+'/') for q in state))]
            path = self.rng.choice(paths);self.run('fs-rmdir' if state[path].kind == 'dir' else 'fs-unlink', source, path, *selector);del state[path];self.record(op, world=world, path=path)
        elif op == 'snapshot':
            name = 'r'+str(next(self.serial));self.run('fs-snapshot', source, name, *selector);self.revisions[name] = copy.deepcopy(state);self.record(op, world=world, revision=name)
        elif op == 'fork':
            revision = self.rng.choice(sorted(self.revisions));name = 'w'+str(next(self.serial))
            self.run('fs-fork', source, revision, name);self.worlds[name] = copy.deepcopy(self.revisions[revision]);self.record(op, world=name, revision=revision)
        elif op == 'restore':
            revision = self.rng.choice(sorted(self.revisions));self.run('fs-restore', source, revision, *selector);self.worlds[world] = copy.deepcopy(self.revisions[revision]);self.record(op, world=world, revision=revision)
        else:
            self.run('fs-compact', source, *selector);self.record(op, world=world)

    def verify(self, repository):
        assert set(self.run('fs-worlds', repository).decode().splitlines()) == set(self.worlds)
        assert set(self.run('fs-revisions', repository).decode().splitlines()) == set(self.revisions)
        for flag, views in [('--world', self.worlds), ('--revision', self.revisions)]:
            for name, state in views.items():
                selector = [flag, name];self.run('fs-check', repository, *selector)
                identities = {}
                for path, node in sorted(state.items()):
                    info = json.loads(self.run('fs-lstat', repository, path, *selector))
                    assert info['directory'] == (node.kind == 'dir') and info['symlink'] == (node.kind == 'symlink')
                    if id(node) in identities:
                        assert info['inode_id'] == identities[id(node)]
                    else:
                        assert info['inode_id'] not in identities.values()
                        identities[id(node)] = info['inode_id']
                    if node.kind == 'dir':
                        children = {p.rsplit('/', 1)[1] for p in state if p != '/' and (p.rsplit('/', 1)[0] or '/') == path}
                        assert set(self.run('fs-ls', repository, path, *selector).decode().splitlines()) == children
                        assert info['links'] == 2+sum(n.kind == 'dir' for p, n in state.items() if p != '/' and (p.rsplit('/', 1)[0] or '/') == path)
                    elif node.kind == 'file':
                        assert self.run('fs-cat', repository, path, *selector) == node.data
                        assert info['size'] == len(node.data) and info['mode'] == node.mode
                        assert info['links'] == sum(n is node for n in state.values())
                    else:
                        assert self.run('fs-readlink', repository, path, *selector) == node.data
        self.run('check', repository)

    def image(self, repository):
        path = self.root/('image-'+str(next(self.serial))+'.pack')
        sequence = int(self.run('backup', repository, path));data = path.read_bytes();path.unlink()
        return sequence, data

    def mutate(self, original, framed):
        def bytes_mutation(data):
            data = bytearray(data);op = self.rng.randrange(4)
            if op == 0 and data:
                for _ in range(self.rng.randint(1, 4)):
                    i = self.rng.randrange(len(data));data[i] ^= 1<<self.rng.randrange(8)
            elif op == 1 and data:
                a = self.rng.randrange(len(data));del data[a:a+self.rng.randint(1, 32)]
            elif op == 2:
                a = self.rng.randrange(len(data)+1);data[a:a] = self.bytes()[:32] or b'\0'
            else:
                data = data[:self.rng.randrange(len(data)+1)]
            return bytes(data)
        if not framed:
            return bytes_mutation(original)
        header, frames = unpack(original);frames = list(frames)
        if not frames:
            return original
        i = self.rng.randrange(len(frames))
        # Keep sequence/framing intact in most mutations to reach inner parsers.
        frames[i] = frames[i][:41]+bytes_mutation(frames[i][41:])
        return pack(*rechain(header, frames))

    def mutations_run(self, base, delta):
        original = delta.read_bytes();bh, _ = unpack(base.read_bytes());dh, df = unpack(original)
        first = struct.unpack_from('<Q', bh, 33)[0];last = struct.unpack_from('<Q', dh, 33)[0]
        for i in range(self.mutations):
            framed = bool(i % 2);candidate = self.mutate(original, framed)
            path = self.root/'mutant.pack';path.write_bytes(candidate)
            target = self.root/'mutation-target';self.run('replay', base, target);before = self.image(target)
            result, _ = self.call('replay', path, target, allow_error=True)
            after = self.image(target);assert first <= after[0] <= last
            if not framed and candidate != original:
                assert result == 1 and after == before
            elif after[0] == first:
                assert after == before
            else:
                # Publication must be an exact prefix of whole candidate transactions.
                header, frames = unpack(candidate);n = after[0]-first
                prefix_header = header[:33]+struct.pack('<Q', after[0])+header[41:]
                prefix_header, prefix_frames = rechain(prefix_header, frames[:n])
                prefix = self.root/'prefix.pack';prefix.write_bytes(pack(prefix_header, prefix_frames))
                reference = self.root/'prefix-target';self.run('replay', base, reference);self.run('replay', prefix, reference)
                assert self.image(reference) == after
                shutil.rmtree(reference)
            again, _ = self.call('replay', path, target, allow_error=True)
            assert again == result and self.image(target) == after
            if result == 0:
                assert after[0] == last
            key = ('framed' if framed else 'raw')+('-accepted' if result == 0 else '-rejected')
            self.counts[key] = self.counts.get(key, 0)+1
            shutil.rmtree(target)

    def graph_mutations_run(self, full):
        header, frames = unpack(full.read_bytes())
        original = base_rows(frames)
        revisions = [k for k in original if k.startswith(b'R/\0forkfs-history/revision/')]
        unrelated = self.root/'graph-target.restore-unrelated'
        unrelated.mkdir();(unrelated/'sentinel').write_bytes(b'other recovery')
        for i in range(self.mutations):
            rows = dict(original)
            key = self.rng.choice(revisions)
            descriptor = bytearray(rows[b'O/'+rows[key]])
            missing = bytes(self.rng.randrange(256) for _ in range(32))
            while b'O/'+missing in rows or missing == bytes(32):
                missing = bytes(self.rng.randrange(256) for _ in range(32))
            kind = self.rng.choice(['parent', 'sequence', 'manifest', 'tree', 'value'])

            def put(value):
                value = bytes(value);oid = h(value)
                rows[b'O/'+oid] = value
                return oid

            if kind == 'parent':
                descriptor[40:72] = missing
            elif kind == 'sequence':
                # Every descriptor must precede the archive's committed sequence.
                sequence = struct.unpack_from('<Q', header, 33)[0]
                descriptor[72:80] = struct.pack('<Q', self.rng.randint(sequence, 2**64-1))
            elif kind == 'manifest':
                descriptor[8:40] = missing
            else:
                manifest = bytearray(rows[b'O/'+bytes(descriptor[8:40])])
                assert manifest[:8] == b'FFTREE01'
                if kind == 'tree':
                    manifest[8:40] = missing
                else:
                    # Choose a random reachable tree node, then rehash the path
                    # back to the manifest so hash validation cannot mask damage.
                    oid = bytes(manifest[8:40]);ancestors = []
                    while True:
                        node = bytearray(rows[b'O/'+oid])
                        children = [offset for offset in (44, 76) if node[offset:offset+32] != bytes(32)]
                        if not children or self.rng.random()<0.35:
                            break
                        offset = self.rng.choice(children)
                        ancestors.append((node, offset));oid = bytes(node[offset:offset+32])
                    node[12:44] = missing;oid = put(node)
                    for parent, offset in reversed(ancestors):
                        parent[offset:offset+32] = oid;oid = put(parent)
                    manifest[8:40] = oid
                descriptor[8:40] = put(manifest)
            rows[key] = put(descriptor);counts(rows)
            path = self.root/'graph-mutant.pack';path.write_bytes(pack(header, base_frames(rows)))
            target = self.root/'graph-target'
            for _ in range(2):
                error = 'revision ancestry sequence' if kind == 'sequence' else 'read recovery object'
                result, _ = self.call('replay', path, target, allow_error=True, expected_error=error)
                assert result == 1 and not target.exists(), kind
                assert set(self.root.glob('graph-target.restore-*')) == {unrelated}, kind
                assert (unrelated/'sentinel').read_bytes() == b'other recovery'
            self.counts['graph-'+kind] = self.counts.get('graph-'+kind, 0)+1

    def save_corpus(self, full, delta):
        if self.corpus is None:
            return
        self.corpus.mkdir(parents=True, exist_ok=True)
        def save(data):
            if len(data) <= 256*1024:
                (self.corpus/hashlib.sha256(data).hexdigest()).write_bytes(data)
        header, frames = unpack(full.read_bytes());rows = base_rows(frames)
        for frame in frames:
            save(b'\x01'+frame)
        for frame in unpack(delta.read_bytes())[1]:
            save(b'\x00'+frame)
        objects = {k[2:]: v for k, v in rows.items() if k.startswith(b'O/')}
        roots = [(3, v) for k, v in rows.items() if k.startswith(b'R/\0forkfs-history/revision/')]
        roots += [(2, v) for k, v in rows.items() if k.startswith(b'R/\0forkfs-history/world/')]
        roots += [(4, k) for k, v in list(objects.items()) if v.startswith(b'FFTRNO01')][:16]
        sequence = struct.unpack_from('<Q', header, 33)[0]
        for mode, oid in roots:
            values = [objects[oid]]+[v for k, v in objects.items() if k != oid]
            if len(values)<=2048:
                save(bytes([mode])+struct.pack('<QH', sequence, len(values))+b''.join(struct.pack('<I', len(v))+v for v in values))

    def execute(self):
        source = self.root/'source';self.run('init', source);self.run('fs-init', source)
        # One initial revision ensures every history can fork/restore immediately.
        self.run('fs-snapshot', source, 'initial');self.revisions['initial'] = copy.deepcopy(self.worlds['main'])
        base = self.root/'base.pack';first = int(self.run('backup', source, base))
        cuts = {self.steps-1, *[self.rng.randrange(self.steps) for _ in range(3)]}
        parts = [];watermark = first
        for step in range(self.steps):
            self.operation(source)
            if step in cuts:
                part = self.root/('part-'+str(step)+'.pack');watermark = int(self.run('replay-export', source, watermark, part));parts.append((part, watermark))
        delta = self.root/'all.pack';last = int(self.run('replay-export', source, first, delta))
        full = self.root/'full.pack';assert int(self.run('backup', source, full)) == last
        self.verify(source);self.save_corpus(full, delta)
        # Recovery must work without consulting the original machine.
        shutil.rmtree(source)
        target = self.root/'recovered';assert int(self.run('replay', base, target)) == first
        for part, watermark in parts:
            assert int(self.run('replay', part, target)) == watermark
            if self.rng.random()<0.5:
                before = self.image(target);self.run('replay', part, target);assert self.image(target) == before
        self.verify(target)
        full_target = self.root/'full-recovered';self.run('replay', full, full_target)
        self.verify(full_target)
        before = self.image(target);self.run('replay', delta, target);assert self.image(target) == before
        self.mutations_run(base, delta)
        self.graph_mutations_run(full)
        return self.counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--seed', type=int)
    parser.add_argument('--steps', type=int, default=32)
    parser.add_argument('--mutations', type=int, default=24)
    parser.add_argument('--iterations', type=int, default=1)
    parser.add_argument('--forever', action='store_true')
    parser.add_argument('--artifacts', type=Path, default=Path('fuzz-artifacts'))
    parser.add_argument('--corpus', type=Path)
    args = parser.parse_args()
    if args.steps<1 or args.mutations<0 or args.iterations<1:
        parser.error('steps/iterations must be positive; mutations must be nonnegative')
    args.artifacts.mkdir(parents=True, exist_ok=True)
    seed = args.seed if args.seed is not None else secrets.randbits(64)
    generator = random.Random(seed)
    for i in itertools.count():
        if not args.forever and i>=args.iterations:
            break
        trial_seed = seed if i == 0 else generator.getrandbits(64)
        root = Path(tempfile.mkdtemp(prefix='seed-'+str(trial_seed)+'-', dir=args.artifacts.resolve()))
        print(json.dumps({'seed': trial_seed, 'artifacts': str(root)}), flush=True)
        try:
            result = Trial(args.binary.resolve(), trial_seed, root, args.steps, args.mutations, args.corpus).execute()
        except BaseException:
            (root/'failure.txt').write_text(traceback.format_exc())
            print('FUZZ FAILURE: retained seed, trace, base, deltas and mutant in '+str(root), flush=True)
            raise
        else:
            print(json.dumps({'seed': trial_seed, 'coverage': result}), flush=True)
            shutil.rmtree(root)

if __name__ == '__main__':
    main()
