"""Seeded, independent namespace model for offline ObjectStore collection."""
import json
import copy
import hashlib
import os
import random
from pathlib import Path
import subprocess
import sys
import tempfile

BIN = str(Path(sys.argv[1]).resolve())
SEED = int(sys.argv[2])


def run(*args, code=0):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=30)
    assert p.returncode == code, (BIN, SEED, args, p.returncode, p.stderr)
    return p.stdout


def bytes_for(rng, n):
    return bytes(rng.randrange(256) for _ in range(n))


def filesystem_snapshot(repo):
    """Capture DATA and GC control/staging state, excluding LevelDB internals."""
    captured = {}

    def add(path):
        st = path.lstat()
        key = str(path)
        captured[key] = (st.st_ino, st.st_mode, st.st_size)
        if path.is_symlink():
            captured[key] += (os.readlink(path),)
        elif path.is_file():
            captured[key] += (hashlib.sha256(path.read_bytes()).digest(),)
        elif path.is_dir():
            for child in sorted(path.iterdir()):
                add(child)

    for path in repo.iterdir():
        if path.name.startswith(('DATA-', 'GC-')) or path.name == 'CAPACITY' or path.name.startswith('.forkfs-fskit'):
            add(path)
    for path in repo.parent.glob(repo.name + '.gc-*'):
        add(path)
    return captured


def gc_plan(repo):
    before = filesystem_snapshot(repo)
    plan = json.loads(run('gc-plan', repo))
    after = filesystem_snapshot(repo)
    assert after == before, (SEED, 'gc-plan mutated repository state', before, after)
    assert plan['applied'] is False, plan
    assert plan['data_bytes_after'] is None, plan
    assert plan['temporary_data_bytes_required'] == plan['data_bytes_estimated_after'], plan
    assert plan['reclaimable_data_bytes'] == max(
        0, plan['data_bytes_before'] - plan['data_bytes_estimated_after']), plan
    return plan


with tempfile.TemporaryDirectory(prefix=f'ff-gc-model-{SEED}-') as td:
    root = Path(td)
    db = root / 'store'
    scratch = root / 'input'
    run('init', db)
    run('fs-init', db)
    rng = random.Random(SEED)
    worlds = {'main': {'paths': {}, 'data': {}}}
    revisions = {}
    serial = 0

    def write(world, path, data):
        model = worlds[world]
        token = model['paths'].get(path)
        if token is None:
            token = f'inode-{nonlocal_serial[0]}'
            nonlocal_serial[0] += 1
            model['paths'][path] = token
        scratch.write_bytes(data)
        run('fs-write', db, path, scratch, '--world', world)
        model['data'][token] = data

    # Keep the helper's counter mutable in this module scope.
    nonlocal_serial = [0]

    # Distinct initial contents ensure chunk and dedup paths both occur.
    for i in range(5):
        write('main', f'/f{i}', bytes_for(rng, 900 + i * 113))
    write('main', '/large', bytes_for(rng, 390000))
    # Materialize a short extent in a large logical file and verify its hole.
    write('main', '/sparse', b'')
    run('fs-truncate', db, '/sparse', 360000, '--world', 'main')
    sparse_patch = b'sparse-end'
    scratch.write_bytes(sparse_patch)
    run('fs-write-at', db, '/sparse', 359990, scratch, '--world', 'main')
    sparse_token = worlds['main']['paths']['/sparse']
    worlds['main']['data'][sparse_token] = bytes(359990) + sparse_patch
    # A hard link aliases the same inode, including through writes.
    run('fs-link', db, '/f0', '/alias', '--world', 'main')
    worlds['main']['paths']['/alias'] = worlds['main']['paths']['/f0']

    def capture(name, world):
        run('fs-snapshot', db, name, '--world', world)
        revisions[name] = copy.deepcopy(worlds[world])

    capture('seeded-base', 'main')
    run('fs-fork', db, 'seeded-base', 'branch')
    worlds['branch'] = copy.deepcopy(revisions['seeded-base'])

    # Force a large file through shrink then sparse regrowth. Its former data
    # is unreachable from every retained view and should be collected.
    shrink_path = '/shrink-regrow'
    write('main', shrink_path, bytes_for(rng, 380000))
    shrink_token = worlds['main']['paths'][shrink_path]
    run('fs-truncate', db, shrink_path, 19, '--world', 'main')
    run('fs-truncate', db, shrink_path, 350000, '--world', 'main')
    model = worlds['main']
    model['data'][shrink_token] = model['data'][shrink_token][:19] + bytes(350000 - 19)

    def verify_view(repo, selector, model):
        expected_paths = model['paths']
        actual_names = run('fs-ls', repo, '/', *selector).splitlines()
        assert actual_names == sorted(path[1:].encode() for path in expected_paths), (SEED, selector, actual_names, expected_paths)
        inode_ids = {}
        for path, token in sorted(expected_paths.items()):
            expected = model['data'][token]
            assert run('fs-cat', repo, path, *selector) == expected, (SEED, selector, path)
            stat = json.loads(run('fs-stat', repo, path, *selector))
            inode_id = stat['inode_id']
            if token in inode_ids:
                assert inode_ids[token] == inode_id, (SEED, selector, path, token, inode_id, inode_ids)
            else:
                assert inode_id not in inode_ids.values(), (SEED, selector, path, token, inode_id, inode_ids)
                inode_ids[token] = inode_id
        run('fs-check', repo, *selector)

    def verify_all():
        for name, model in worlds.items():
            verify_view(db, ('--world', name), model)
        for name, model in revisions.items():
            verify_view(db, ('--revision', name), model)

    verify_all()
    shrink_plan = gc_plan(db)
    shrink_gc = json.loads(run('gc', db))
    assert shrink_gc['data_bytes_after'] == shrink_plan['data_bytes_estimated_after'], (SEED, shrink_plan, shrink_gc)
    verify_all()

    for step in range(36):
        world = 'branch' if rng.randrange(4) == 0 else 'main'
        model = worlds[world]
        op = rng.choice(('create', 'write', 'range', 'truncate', 'rename', 'unlink', 'link', 'revision'))
        present = list(model['paths'])
        if op == 'create' or not present:
            path = f'/n{nonlocal_serial[0]}'
            # Repeated values exercise object sharing across independent files.
            data = (b'shared-block-' * rng.randrange(1, 40) if rng.randrange(4) == 0
                    else bytes_for(rng, rng.randrange(1, 2400)))
            if step % 9 == 0:
                data = bytes_for(rng, 390000 + rng.randrange(12000))
            write(world, path, data)
        elif op == 'write':
            path = rng.choice(present)
            data = bytes_for(rng, rng.randrange(1, 7000))
            write(world, path, data)
        elif op == 'range':
            path = rng.choice(present)
            token = model['paths'][path]
            old = model['data'][token]
            offset = rng.randrange(max(1, len(old) + 1))
            patch = bytes_for(rng, rng.randrange(1, 65))
            scratch.write_bytes(patch)
            run('fs-write-at', db, path, offset, scratch, '--world', world)
            model['data'][token] = old[:offset] + bytes(max(0, offset - len(old))) + patch + old[offset + len(patch):]
        elif op == 'truncate':
            path = rng.choice(present)
            token = model['paths'][path]
            old = model['data'][token]
            size = rng.randrange(0, min(max(len(old), 1), 5000) + 1)
            run('fs-truncate', db, path, size, '--world', world)
            model['data'][token] = old[:size] + bytes(max(0, size - len(old)))
        elif op == 'rename' and len(present) > 1:
            src = rng.choice(present)
            dst = rng.choice([p for p in present if p != src]) if rng.randrange(3) == 0 else f'/m{serial}'
            serial += 1
            run('fs-rename', db, src, dst, '--world', world)
            if dst in model['paths'] and model['paths'][dst] == model['paths'][src]:
                pass  # POSIX rename of one hard link over another is a no-op.
            else:
                if dst in model['paths']:
                    del model['paths'][dst]
                model['paths'][dst] = model['paths'].pop(src)
        elif op == 'unlink':
            src = rng.choice(present)
            run('fs-unlink', db, src, '--world', world)
            del model['paths'][src]
        elif op == 'link':
            src = rng.choice(present)
            dst = f'/h{nonlocal_serial[0]}'
            nonlocal_serial[0] += 1
            run('fs-link', db, src, dst, '--world', world)
            model['paths'][dst] = model['paths'][src]
        else:
            name = f'rev-{step}'
            capture(name, world)

        if step % 6 == 5:
            verify_all()
            plan = gc_plan(db)
            packed = plan['data_bytes_estimated_after']
            assert isinstance(packed, int) and packed >= 0, plan
            # The plan predicts the resulting packed DATA footprint exactly.
            report = json.loads(run('gc', db))
            assert report['data_bytes_after'] == packed, (SEED, step, plan, report)
            again = json.loads(run('gc', db))
            assert again['data_bytes_after'] == report['data_bytes_after'], again
            assert again['data_bytes_before'] == again['data_bytes_after'], again
            verify_all()

    # Unindexed bytes after the committed prefix must be reclaimed by GC.
    before = sum(p.stat().st_size for p in db.glob('DATA-*.seg'))
    tail = sorted(db.glob('DATA-*.seg'))[-1]
    committed_tail_size = tail.stat().st_size
    # Appending after the indexed prefix models a partial tail and a complete-
    # looking but uncommitted chunk record.
    with tail.open('ab') as f:
        f.write(b'partial-tail')
        f.write(b'FFCHNK01' + bytes(range(256)) * 128)
    orphan_bytes = tail.stat().st_size - committed_tail_size
    assert orphan_bytes > 1000
    plan = gc_plan(db)
    assert plan['reclaimable_data_bytes'] >= orphan_bytes, plan
    assert plan['data_bytes_estimated_after'] < before + orphan_bytes, plan
    report = json.loads(run('gc', db))
    assert report['data_bytes_after'] == plan['data_bytes_estimated_after'], (plan, report)
    assert sum(p.stat().st_size for p in db.glob('DATA-*.seg')) < before + orphan_bytes
    verify_all()

    # Replay retention pins every indexed object while a delta base is live.
    # An explicit checkpoint establishes a detached baseline and permits the
    # next generation to collect objects that are no longer namespace roots.
    baseline = root / 'baseline.pack'
    replay_point = int(run('backup', db, baseline))
    churn_path = '/checkpoint-churn'
    churn_data = bytes_for(rng, 1800)
    write('main', churn_path, churn_data)
    baseline_worlds = copy.deepcopy(worlds)
    baseline_revisions = copy.deepcopy(revisions)
    delta = root / 'after-baseline.pack'
    delta_end = int(run('replay-export', db, replay_point, delta))
    assert delta_end > replay_point
    pinned_plan = gc_plan(db)
    assert pinned_plan['replay_pins_all_objects'] is True, pinned_plan
    pinned_gc = json.loads(run('gc', db))
    assert pinned_gc['replay_pins_all_objects'] is True, pinned_gc
    assert pinned_gc['objects_after'] == pinned_gc['objects_before'], pinned_gc
    assert pinned_gc['data_bytes_after'] == pinned_plan['data_bytes_estimated_after'], (pinned_plan, pinned_gc)

    checkpoint = root / 'checkpoint.pack'
    checkpoint_report = json.loads(run('gc', db, '--checkpoint', checkpoint))
    assert checkpoint_report['new_replay_generation'] is True, checkpoint_report
    assert checkpoint_report['data_bytes_after'] < pinned_gc['data_bytes_after'], checkpoint_report
    assert checkpoint.is_file()
    checkpoint_seq = json.loads(run('check', db))['last_committed_seq']
    post_data = bytes_for(rng, 777)
    write('branch', '/after-checkpoint', post_data)
    post_delta = root / 'post-checkpoint.pack'
    post_end = int(run('replay-export', db, checkpoint_seq, post_delta))
    assert post_end > checkpoint_seq

    # Lose access to the source repository before replaying the checkpoint and
    # its delta, so this checks detached recovery rather than a local copy.
    offline = root / 'offline-source'
    db.rename(offline)
    recovered_delta = root / 'recovered-delta'
    run('replay', baseline, recovered_delta)
    run('replay', delta, recovered_delta)
    assert json.loads(run('check', recovered_delta))['last_committed_seq'] == delta_end
    for world, model in baseline_worlds.items():
        verify_view(recovered_delta, ('--world', world), model)
    for revision, model in baseline_revisions.items():
        verify_view(recovered_delta, ('--revision', revision), model)

    recovered_checkpoint = root / 'recovered-checkpoint'
    run('replay', checkpoint, recovered_checkpoint)
    run('replay', post_delta, recovered_checkpoint)
    assert json.loads(run('check', recovered_checkpoint))['last_committed_seq'] == post_end
    for world, model in worlds.items():
        verify_view(recovered_checkpoint, ('--world', world), model)
    for revision, model in revisions.items():
        verify_view(recovered_checkpoint, ('--revision', revision), model)

print(f'seeded offline GC model passed (seed={SEED})')
