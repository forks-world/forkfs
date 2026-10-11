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
        self.service_logs = [(self.root / name).open('wb') for name in ('service.stdout', 'service.stderr')]
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
    f.call('fs-truncate', '/a', 262145, error='prototype files are limited to 256 KiB')
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


def fork(f):
    f.initialize()
    f.write('/a', b'base')
    f.call('fs-snapshot', 'base')
    f.call('fs-fork', 'base', 'work')
    f.write('/a', b'child', ('world', 'work'))
    f.read('/a', b'base')
    f.read('/a', b'base', ('revision', 'base'))
    f.read('/a', b'child', ('world', 'work'))
    f.write('/a', b'parent')
    f.read('/a', b'child', ('world', 'work'))
    f.call('fs-fork', 'base', 'work', error='World already exists')
    f.read('/a', b'child', ('world', 'work'))
    f.call('fs-compact', view=('world', 'work'))
    f.read('/a', b'base', ('revision', 'base'))
    f.call('fs-check', view=('world', 'work'))


def revision(f):
    f.initialize()
    f.write('/a', b'one')
    f.call('fs-snapshot', 'r1')
    f.write('/a', b'two')
    f.call('fs-snapshot', 'r2')
    f.call('fs-snapshot', 'r1', error='revision name already exists')
    f.call('fs-unlink', '/a', view=('revision', 'r1'), error='revision view is read-only')
    f.read('/a', b'one', ('revision', 'r1'))
    f.read('/a', b'two', ('revision', 'r2'))
    f.read('/a', b'two')
    f.call('fs-compact')
    f.read('/a', b'one', ('revision', 'r1'))
    f.call('fs-check', view=('revision', 'r1'))


CASES = [('basic-fs', 'FS001', initialization), ('basic-fs', 'FS002', directories),
         ('basic-fs', 'FS003', file_io), ('basic-fs', 'FS004', sparse_and_truncate),
         ('basic-fs', 'FS005', rename), ('basic-fs', 'FS006', hardlinks),
         ('basic-fs', 'FS007', symlinks), ('basic-fs', 'FS008', handles),
         ('basic-fs', 'FS009', removals), ('basic-fs', 'FS010', directory_pages),
         ('basic-fs', 'FS011', error_boundaries), ('fork', 'FK-isolation-smoke', fork),
         ('revision', 'RV-history-smoke', revision)]


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
    report = {'schema': 1, 'results': results, 'unimplemented': ['full-fork-revision-matrix',
              'mounted', 'revision-restore', 'mount-generation-revocation'], 'artifacts': str(output)}
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
