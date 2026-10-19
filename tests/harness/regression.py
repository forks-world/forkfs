"""Fixed CLI regressions; each case owns a new repository and JSONL event log."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import traceback


class Fixture:
    def __init__(self, binary, root):
        self.binary, self.root = binary, root
        self.repository = root / 'repository'
        self.step = 0
        self.endpoint = None
        self.process = None
        self.service_logs = []

    def call(self, operation, *arguments, view=None, error=False):
        self.step += 1
        argv = ([str(self.binary), 'request', str(self.endpoint), operation] if self.endpoint
                else [str(self.binary), operation, str(self.repository)]) + list(map(str, arguments))
        if view:
            argv += ['--revision' if view[0] == 'revision' else '--world', view[1]]
        started = time.monotonic_ns()
        event = {'step': self.step, 'op': operation, 'args': list(map(str, arguments)), 'view': view}
        try:
            result = subprocess.run(argv, capture_output=True, timeout=15, check=False)
            event.update(returncode=result.returncode, duration_ns=time.monotonic_ns() - started,
                         stdout_hex=result.stdout[:256].hex(), stderr=result.stderr[:4096].decode(errors='replace'))
            # Expected refusal is the CLI's ordinary error exit, not a signal/crash.
            expected = 1 if error else 0
            if result.returncode != expected:
                raise AssertionError(f'{operation}: exit {result.returncode}, expected {expected}: {event["stderr"]}')
            if isinstance(error, str) and error.encode() not in result.stderr:
                raise AssertionError(f'{operation}: wrong refusal: {event["stderr"]}')
            if len(result.stdout) > 1024 * 1024:
                raise AssertionError('response exceeds harness limit')
            return result.stdout
        except subprocess.TimeoutExpired:
            event['failure'] = 'timeout'
            raise
        finally:
            with (self.root / 'events.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(event) + '\n')

    def start_service(self):
        # Keep Unix socket paths short even when the artifact directory is long.
        self.socket_root = tempfile.TemporaryDirectory(prefix='ff-h-', dir='/tmp')
        endpoint = Path(self.socket_root.name) / 'rpc.sock'
        self.service_logs = [(self.root / name).open('ab') for name in ('service.stdout', 'service.stderr')]
        self.process = subprocess.Popen([str(self.binary), 'serve', str(self.repository),
                                         '--socket', str(endpoint), '--rpc'],
                                        stdout=self.service_logs[0], stderr=self.service_logs[1])
        deadline = time.monotonic() + 5
        while True:
            if self.process.poll() is not None:
                raise AssertionError(f'service exited during startup: {self.process.returncode}')
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(.1)
                    connection.connect(str(endpoint))
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('service startup timed out')
                time.sleep(.01)
        self.endpoint = endpoint
        self.call('status')

    def close(self):
        try:
            if self.process is not None:
                if self.process.poll() is not None:
                    raise AssertionError(f'service exited unexpectedly: {self.process.returncode}')
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
                    raise AssertionError('service shutdown timed out')
                if self.process.returncode != 0:
                    raise AssertionError(f'service shutdown exit: {self.process.returncode}')
        finally:
            for stream in self.service_logs:
                stream.close()
            if hasattr(self, 'socket_root'):
                self.socket_root.cleanup()

    def initialize(self):
        self.call('init')
        self.call('fs-init')

    def write(self, path, data, view=None):
        payload = self.root / f'input-{self.step}.bin'
        payload.write_bytes(data)
        self.call('fs-write', path, payload, view=view)

    def read(self, path, expected, view=None):
        actual = self.call('fs-cat', path, view=view)
        if actual != expected:
            raise AssertionError(f'{path}: content {actual!r}, expected {expected!r}')
        attributes = json.loads(self.call('fs-stat', path, view=view))
        if attributes['size'] != len(expected):
            raise AssertionError(f'{path}: inode size differs from content')


def stat(f, path):
    return json.loads(f.call('fs-stat', path))


def initialization(f):
    f.initialize()
    if f.call('fs-ls', '/') != b'' or not stat(f, '/')['directory']:
        raise AssertionError('invalid empty root')
    f.write('/keep', b'preserved')
    f.call('fs-init', error='filesystem already initialized')
    f.read('/keep', b'preserved')


def directories(f):
    f.initialize()
    f.call('fs-mkdir', '/one')
    f.call('fs-mkdir', '/one/two')
    if f.call('fs-ls', '/') != b'one\n' or f.call('fs-ls', '/one') != b'two\n':
        raise AssertionError('directory contents mismatch')
    if stat(f, '/')['links'] != 3 or stat(f, '/one')['links'] != 3 or stat(f, '/one/two')['mode'] != 0o755:
        raise AssertionError('directory attributes mismatch')


def file_io(f):
    f.initialize()
    f.write('/a', b'first')
    identity = stat(f, '/a')['inode_id']
    f.write('/a', b'second')
    payload = f.root / 'append.bin'
    payload.write_bytes(b'!')
    result = json.loads(f.call('fs-append', '/a', payload))
    if result != {'offset': 6, 'written': 1} or stat(f, '/a')['inode_id'] != identity:
        raise AssertionError('append result or inode identity mismatch')
    f.read('/a', b'second!')


def sparse_and_truncate(f):
    f.initialize()
    f.write('/a', b'abc')
    payload = f.root / 'offset.bin'
    payload.write_bytes(b'Z')
    f.call('fs-write-at', '/a', 8, payload)
    f.read('/a', b'abc' + b'\0' * 5 + b'Z')
    f.call('fs-truncate', '/a', 2)
    f.call('fs-truncate', '/a', 5)
    f.read('/a', b'ab\0\0\0')
    if f.call('fs-read-at', '/a', 4, 9) != b'\0' or f.call('fs-read-at', '/a', 5, 9) != b'' or f.call('fs-read-at', '/a', 0, 0) != b'':
        raise AssertionError('EOF/partial/zero read mismatch')


def rename(f):
    f.initialize()
    f.call('fs-mkdir', '/dir')
    f.write('/dir/a', b'source')
    f.write('/target', b'replaced')
    identity = stat(f, '/dir/a')['inode_id']
    f.call('fs-rename', '/dir/a', '/target')
    f.read('/target', b'source')
    if stat(f, '/target')['inode_id'] != identity or f.call('fs-ls', '/dir') != b'':
        raise AssertionError('rename changed identity or retained old name')
    f.write('/dir/f', b'moved')
    f.call('fs-rename', '/dir', '/moved')
    f.read('/moved/f', b'moved')
    f.call('fs-stat', '/dir', error='path not found')


def hardlinks(f):
    f.initialize()
    f.write('/a', b'base')
    f.call('fs-link', '/a', '/b')
    if stat(f, '/a')['inode_id'] != stat(f, '/b')['inode_id'] or stat(f, '/a')['links'] != 2:
        raise AssertionError('hardlink identity/count mismatch')
    f.write('/b', b'changed')
    f.read('/a', b'changed')
    f.call('fs-unlink', '/a')
    f.read('/b', b'changed')
    if stat(f, '/b')['links'] != 1:
        raise AssertionError('hardlink decrement mismatch')


def symlinks(f):
    f.initialize()
    f.write('/a', b'target')
    f.call('fs-symlink', 'a', '/link')
    if f.call('fs-readlink', '/link') != b'a' or not json.loads(f.call('fs-lstat', '/link'))['symlink']:
        raise AssertionError('symlink identity/target mismatch')
    f.read('/link', b'target')
    f.call('fs-unlink', '/a')
    f.call('fs-cat', '/link', error='path not found')
    f.call('fs-symlink', 'loop', '/loop')
    f.call('fs-cat', '/loop', error='too many symbolic links')


def removals(f):
    f.initialize()
    f.call('fs-mkdir', '/dir')
    f.write('/dir/a', b'keep')
    f.call('fs-rmdir', '/dir', error='directory is not empty')
    f.call('fs-unlink', '/dir', error='wrong removal operation for inode type')
    f.call('fs-rmdir', '/dir/a', error='wrong removal operation for inode type')
    f.read('/dir/a', b'keep')
    f.call('fs-unlink', '/dir/a')
    f.call('fs-rmdir', '/dir')
    if f.call('fs-ls', '/') != b'' or stat(f, '/')['links'] != 2:
        raise AssertionError('removal retained entries or directory links')


def error_boundaries(f):
    f.initialize()
    f.write('/a', b'unchanged')
    f.call('fs-stat', '/missing', error='path not found')
    f.call('fs-mkdir', 'relative', error='path must be absolute')
    f.call('fs-mkdir', '/a/../x', error='invalid path component')
    f.call('fs-truncate', '/a', 18446744073709551615, error='file size exceeds signed 64-bit range')
    f.read('/a', b'unchanged')
    if f.call('fs-ls', '/') != b'a\n':
        raise AssertionError('rejected operation mutated directory')


def handles(f):
    f.initialize()
    f.start_service()
    f.write('/a', b'original')
    session = json.loads(f.call('session-open', 60000))['session']
    def call(op, *args, **kwargs):
        return f.call(op, *args, '--session', session, **kwargs)
    handle = json.loads(call('fs-open', '/a', 'rw'))['handle']
    duplicate = json.loads(call('fs-dup', handle))['handle']
    if call('fs-hread-next', handle, 2) != b'or' or call('fs-hread-next', duplicate, 2) != b'ig':
        raise AssertionError('dup did not share the file offset')
    identity = stat(f, '/a')['inode_id']
    f.call('fs-rename', '/a', '/renamed')
    f.call('fs-unlink', '/renamed')
    attrs = json.loads(call('fs-hstat', handle))
    if attrs['inode_id'] != identity or attrs['links'] != 0:
        raise AssertionError('unlink lost open inode identity')
    payload = f.root / 'handle.bin'
    payload.write_bytes(b'changed')
    call('fs-hwrite', handle, 0, payload)
    call('fs-close', handle)
    if call('fs-hread', duplicate, 0, 8) != b'changedl':
        raise AssertionError('duplicate handle did not retain unlinked inode')
    f.write('/a', b'new')
    if stat(f, '/a')['inode_id'] == identity:
        raise AssertionError('recreated path reused a live orphan inode')
    f.read('/a', b'new')
    call('fs-close', duplicate)
    call('fs-hstat', duplicate, error='handle belongs to another session')
    live = json.loads(call('fs-open', '/a', 'r'))['handle']
    f.call('session-close', session)
    call('fs-hstat', live, error='session')


def directory_pages(f):
    f.initialize()
    f.start_service()
    f.call('fs-mkdir', '/dir')
    names = ['a', 'b', 'c', 'quote"name', 'slash\\name']
    for name in names:
        f.write('/dir/' + name, b'x')
    session = json.loads(f.call('session-open', 60000))['session']
    def call(op, *args, **kwargs):
        return f.call(op, *args, '--session', session, **kwargs)
    empty = json.loads(call('fs-dir-open', '/'))['cursor']
    call('fs-dir-close', empty)
    f.call('fs-mkdir', '/empty')
    empty = json.loads(call('fs-dir-open', '/empty'))['cursor']
    if json.loads(call('fs-dir-page', empty, 2)) != {'names': [], 'eof': True}:
        raise AssertionError('empty directory page mismatch')
    call('fs-dir-close', empty)
    cursor = json.loads(call('fs-dir-open', '/dir'))['cursor']
    first = json.loads(call('fs-dir-page', cursor, 2))
    f.call('fs-unlink', '/dir/c')
    f.write('/dir/new', b'y')
    call('fs-dir-page', cursor, 0, error='directory page limit')
    observed = first['names'][:]
    for _ in range(10):
        page = json.loads(call('fs-dir-page', cursor, 2))
        if len(page['names']) > 2:
            raise AssertionError('page exceeded requested bound')
        observed.extend(page['names'])
        if page['eof']:
            break
    else:
        raise AssertionError('directory pagination did not terminate')
    if observed != sorted(names) or first['eof']:
        raise AssertionError('pinned directory lost, duplicated or reordered entries')
    if json.loads(call('fs-dir-page', cursor, 2)) != {'names': [], 'eof': True}:
        raise AssertionError('EOF was not stable')
    call('fs-dir-rewind', cursor)
    if json.loads(call('fs-dir-page', cursor, 2)) != first:
        raise AssertionError('rewind changed the captured directory')
    call('fs-dir-close', cursor)
    call('fs-dir-page', cursor, 1, error='invalid directory cursor/session')
    if f.call('fs-ls', '/dir').decode().splitlines() != sorted(set(names) - {'c'} | {'new'}):
        raise AssertionError('live directory did not reflect mutations')
    f.call('session-close', session)


def baseline(f):
    f.initialize()
    f.call('fs-mkdir', '/dir')
    f.write('/dir/a', b'base')
    f.call('fs-chmod', '/dir/a', '640')
    f.call('fs-link', '/dir/a', '/alias')
    f.call('fs-symlink', 'dir/a', '/link')
    f.call('fs-snapshot', 'base')
    f.call('fs-fork', 'base', 'work')


def tree_signature(f, view=None):
    result = {}
    def walk(path):
        attrs = json.loads(f.call('fs-lstat', path, view=view))
        result[path] = {'attributes': attrs}
        if attrs['directory']:
            for name in f.call('fs-ls', path, view=view).decode().splitlines():
                walk(path.rstrip('/') + '/' + name)
        elif attrs['symlink']:
            result[path]['target'] = f.call('fs-readlink', path, view=view).hex()
        else:
            result[path]['content'] = f.call('fs-cat', path, view=view).hex()
    walk('/')
    return result


def fork_baseline(f):
    baseline(f)
    original = tree_signature(f)
    for view in [('revision', 'base'), ('world', 'work')]:
        if tree_signature(f, view) != original:
            raise AssertionError('fork baseline changed tree, attributes or inode topology')


def fork_writes(f):
    baseline(f)
    original = tree_signature(f)
    view = ('world', 'work')
    f.write('/dir/a', b'child', view)
    payload = f.root / 'append.bin'
    payload.write_bytes(b'!')
    f.call('fs-append', '/alias', payload, view=view)
    f.read('/dir/a', b'child!', view)
    f.call('fs-truncate', '/dir/a', 3, view=view)
    f.read('/alias', b'chi', view)
    if tree_signature(f) != original or tree_signature(f, ('revision', 'base')) != original:
        raise AssertionError('child writes changed the source or revision')


def fork_directories(f):
    baseline(f)
    original = tree_signature(f)
    view = ('world', 'work')
    f.call('fs-mkdir', '/new', view=view)
    f.write('/new/file', b'new', view)
    f.call('fs-rename', '/dir', '/moved', view=view)
    f.call('fs-unlink', '/alias', view=view)
    f.read('/moved/a', b'base', view)
    f.read('/new/file', b'new', view)
    f.call('fs-stat', '/dir', view=view, error='path not found')
    f.call('fs-stat', '/alias', view=view, error='path not found')
    if tree_signature(f) != original or tree_signature(f, ('revision', 'base')) != original:
        raise AssertionError('child directory edits changed the baseline')


def fork_divergence(f):
    baseline(f)
    f.write('/dir/a', b'parent')
    f.write('/dir/a', b'child', ('world', 'work'))
    # Offline calls reopen the engine for every operation.
    for view, content in [(None, b'parent'), (('world', 'work'), b'child'), (('revision', 'base'), b'base')]:
        f.read('/alias', content, view)
        f.call('fs-check', view=view)


def fork_handles(f):
    baseline(f)
    f.call('fs-fork', 'base', 'second')
    f.start_service()
    session = json.loads(f.call('session-open', 60000))['session']
    handles = []
    for name in ['work', 'second']:
        view = ('world', name)
        handle = json.loads(f.call('fs-open', '/dir/a', 'rw', '--session', session, view=view))['handle']
        handles.append(handle)
        if not json.loads(f.call('fs-hlock', handle, 'exclusive', '--session', session, view=view))['granted']:
            raise AssertionError('lock in one World blocked another World')
    f.call('fs-hread', handles[0], 0, 4, '--session', session, view=('world', 'second'),
           error='file handle belongs to another view')
    payload = f.root / 'handle.bin'
    payload.write_bytes(b'only')
    f.call('fs-hwrite', handles[0], 0, payload, '--session', session, view=('world', 'work'))
    f.read('/alias', b'only', ('world', 'work'))
    f.read('/alias', b'base', ('world', 'second'))
    f.read('/alias', b'base')
    f.call('session-close', session)


def fork_failures(f):
    baseline(f)
    f.start_service()
    before = json.loads(f.call('status'))
    worlds = f.call('fs-worlds')
    original = tree_signature(f, ('world', 'work'))
    for revision, world, reason in [('base', 'work', 'World already exists'),
                                     ('missing', 'unpublished', 'object root not found'),
                                     ('base', '../bad', 'invalid World/revision name')]:
        f.call('fs-fork', revision, world, error=reason)
    after = json.loads(f.call('status'))
    for field in ['last_committed_seq', 'indexed_objects', 'object_roots']:
        if before[field] != after[field]:
            raise AssertionError(f'failed fork changed {field}')
    if f.call('fs-worlds') != worlds or tree_signature(f, ('world', 'work')) != original:
        raise AssertionError('failed fork published or overwrote a World')


def fork_compaction(f):
    baseline(f)
    f.write('/dir/a', b'parent')
    f.write('/dir/a', b'child', ('world', 'work'))
    expected = {view: tree_signature(f, view) for view in [None, ('world', 'work'), ('revision', 'base')]}
    f.call('fs-compact')
    f.call('fs-compact', view=('world', 'work'))
    for view, signature in expected.items():
        if tree_signature(f, view) != signature:
            raise AssertionError('compaction/reopen changed a retained tree')
        f.call('fs-check', view=view)


def revision_history(f):
    baseline(f)
    original = tree_signature(f, ('revision', 'base'))
    f.write('/dir/a', b'live')
    f.call('fs-chmod', '/dir/a', '600')
    f.call('fs-rename', '/dir', '/moved')
    f.call('fs-unlink', '/alias')
    f.write('/new', b'new')
    if tree_signature(f, ('revision', 'base')) != original:
        raise AssertionError('live edits changed historical tree or attributes')


def revision_forks(f):
    baseline(f)
    f.write('/dir/a', b'two')
    f.call('fs-snapshot', 'r2')
    for revision, world, content in [('base', 'old', b'base'), ('r2', 'new', b'two')]:
        f.call('fs-fork', revision, world)
        f.read('/alias', content, ('world', world))
        if tree_signature(f, ('world', world)) != tree_signature(f, ('revision', revision)):
            raise AssertionError('fork selected the wrong revision tree')


def revision_readonly(f):
    baseline(f)
    f.start_service()
    view = ('revision', 'base')
    old, live = tree_signature(f, view), tree_signature(f)
    payload = f.root / 'rejected.bin'
    payload.write_bytes(b'rejected')
    for op, args in [('fs-write', ('/dir/a', payload)), ('fs-unlink', ('/alias',)),
                     ('fs-rename', ('/dir', '/moved')), ('fs-mkdir', ('/new',))]:
        f.call(op, *args, view=view, error='revision view is read-only')
    session = json.loads(f.call('session-open', 60000))['session']
    f.call('fs-open', '/dir/a', 'rw', '--session', session, view=view, error='revision view is read-only')
    f.call('session-close', session)
    if tree_signature(f, view) != old or tree_signature(f) != live:
        raise AssertionError('rejected revision edits changed history or live World')


def revision_failures(f):
    baseline(f)
    old = tree_signature(f, ('revision', 'base'))
    revisions = f.call('fs-revisions')
    f.call('fs-snapshot', 'base', error='revision name already exists')
    f.call('fs-snapshot', '../bad', error='invalid World/revision name')
    f.call('fs-stat', '/', view=('revision', 'missing'), error='object root not found')
    if f.call('fs-revisions') != revisions or tree_signature(f, ('revision', 'base')) != old:
        raise AssertionError('failed revision operation changed history')


def revision_restore(f):
    baseline(f)
    original = tree_signature(f, ('revision', 'base'))
    f.write('/dir/a', b'changed')
    f.call('fs-rename', '/dir', '/moved')
    f.write('/extra', b'new')
    f.call('fs-snapshot', 'r2')
    later = tree_signature(f, ('revision', 'r2'))
    f.call('fs-restore', 'missing', error='object root not found')
    if tree_signature(f) != later:
        raise AssertionError('failed restore changed live tree')
    f.call('fs-restore', 'base')
    if tree_signature(f) != original or tree_signature(f, ('revision', 'r2')) != later:
        raise AssertionError('restore did not preserve complete historical trees')
    f.write('/dir/a', b'after')
    f.read('/alias', b'after')
    f.read('/alias', b'base', ('revision', 'base'))
    f.call('fs-restore', 'r2', view=('world', 'work'))
    if tree_signature(f, ('world', 'work')) != later:
        raise AssertionError('selected World restore chose the wrong tree')
    f.read('/alias', b'after')


def revision_revocation(f):
    baseline(f)
    f.start_service()
    session = json.loads(f.call('session-open', 60000))['session']
    other_session = json.loads(f.call('session-open', 60000))['session']
    def call(op, *args, view=None, error=False, owner=session):
        return f.call(op, *args, '--session', owner, view=view, error=error)
    live = json.loads(call('fs-open', '/dir/a', 'rw'))['handle']
    duplicate = json.loads(call('fs-dup', live))['handle']
    second = json.loads(call('fs-open', '/alias', 'r', owner=other_session))['handle']
    old_dir = json.loads(call('fs-dir-open', '/dir'))['cursor']
    historical = json.loads(call('fs-open', '/dir/a', 'r', view=('revision', 'base')))['handle']
    work = json.loads(call('fs-open', '/dir/a', 'rw', view=('world', 'work')))['handle']
    if not json.loads(call('fs-hlock', live, 'exclusive'))['granted']:
        raise AssertionError('initial lock denied')
    f.call('fs-unlink', '/alias')
    f.call('fs-unlink', '/dir/a')  # All live handles now retain an orphan.
    f.call('fs-restore', 'base')
    for token, owner in [(live, session), (duplicate, session), (second, other_session)]:
        call('fs-hread', token, 0, 4, owner=owner, error='handle belongs to another session')
    payload = f.root / 'stale-write.bin'
    payload.write_bytes(b'bad')
    call('fs-hwrite', live, 0, payload, error='handle belongs to another session')
    call('fs-dir-page', old_dir, 1, error='invalid directory cursor/session')
    call('fs-dir-rewind', old_dir, error='invalid directory cursor/session')
    for token, view in [(historical, ('revision', 'base')), (work, ('world', 'work'))]:
        if call('fs-hread', token, 0, 4, view=view) != b'base':
            raise AssertionError('restore revoked an unrelated view')
    fresh = json.loads(call('fs-open', '/dir/a', 'rw'))['handle']
    if not json.loads(call('fs-hlock', fresh, 'exclusive'))['granted']:
        raise AssertionError('revoked lock survived restore')
    f.read('/alias', b'base')
    f.call('session-close', session)
    f.call('session-close', other_session)


def revision_compaction(f):
    baseline(f)
    f.write('/dir/a', b'two')
    f.call('fs-snapshot', 'r2')
    expected = {name: tree_signature(f, ('revision', name)) for name in ['base', 'r2']}
    f.start_service()
    f.call('fs-compact')
    f.call('fs-restore', 'base')
    f.close()
    f.process = None
    f.endpoint = None
    f.service_logs = []
    f.start_service()
    if tree_signature(f) != expected['base']:
        raise AssertionError('restored tree did not survive service restart')
    f.write('/dir/a', b'after-restart')
    for name, signature in expected.items():
        if tree_signature(f, ('revision', name)) != signature:
            raise AssertionError('compaction/service restart changed history')
        f.call('fs-check', view=('revision', name))


CASES = [('basic-fs', 'FS001', initialization), ('basic-fs', 'FS002', directories),
         ('basic-fs', 'FS003', file_io), ('basic-fs', 'FS004', sparse_and_truncate),
         ('basic-fs', 'FS005', rename), ('basic-fs', 'FS006', hardlinks),
         ('basic-fs', 'FS007', symlinks), ('basic-fs', 'FS008', handles),
         ('basic-fs', 'FS009', removals), ('basic-fs', 'FS010', directory_pages),
         ('basic-fs', 'FS011', error_boundaries), ('fork', 'FK001', fork_baseline),
         ('fork', 'FK002', fork_writes), ('fork', 'FK003', fork_directories),
         ('fork', 'FK004', fork_divergence), ('fork', 'FK005', fork_handles),
         ('fork', 'FK006', fork_failures), ('fork', 'FK007', fork_compaction),
         ('revision', 'RV001', revision_history), ('revision', 'RV002', revision_forks),
         ('revision', 'RV003', revision_readonly), ('revision', 'RV004', revision_failures),
         ('revision', 'RV005', revision_restore), ('revision', 'RV006', revision_revocation),
         ('revision', 'RV007', revision_compaction)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--suite', choices=['all', 'basic-fs', 'fork', 'revision'], default='all')
    parser.add_argument('--case', choices=[case for _, case, _ in CASES])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--profile', choices=['smoke', 'acceptance'], default='smoke')
    args = parser.parse_args()
    selected = [(suite, case, scenario) for suite, case, scenario in CASES
                if args.suite in ['all', suite] and (not args.case or args.case == case)]
    if not selected:
        parser.error('suite/case selection matched no cases')
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error('binary does not exist')
    if args.output:
        output = args.output.resolve()
        output.mkdir()  # Never reuse or overwrite an existing run.
    else:
        output = Path(tempfile.mkdtemp(prefix='forkfs-regression-'))
    manifest = {'schema': 1, 'adapter': 'offline-cli+resident-rpc', 'profile': args.profile,
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'suite': args.suite, 'case': args.case, 'output': str(output)}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    results = []
    for suite, case, scenario in selected:
        root = output / case
        root.mkdir()
        result = {'suite': suite, 'case': case, 'status': 'pass'}
        try:
            fixture = Fixture(binary, root)
            try:
                scenario(fixture)
                fixture.call('fs-check')
            finally:
                fixture.close()
            if fixture.endpoint is not None:
                fixture.endpoint = None
                fixture.call('fs-check')  # Reopen persisted state after resident shutdown.
        except Exception as error:
            result.update(status='fail', error=str(error))
            (root / 'failure.txt').write_text(traceback.format_exc())
        results.append(result)
    report = {'schema': 1, 'results': results, 'unimplemented': ['mounted', 'mount-generation-revocation', 'physical-cow-sharing', 'complete-posix-matrix'], 'artifacts': str(output)}
    if any(r['status'] == 'fail' for r in results):
        report['status'] = 'fail'
    else:
        report['status'] = 'unavailable' if args.profile == 'acceptance' else 'pass'
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    # Retain generated fixtures and logs for inspection; never unmount/delete host resources.
    return 0 if report['status'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
