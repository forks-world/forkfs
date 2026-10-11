"""Fixed CLI regressions; each case owns a new repository and JSONL event log."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import traceback


class Fixture:
    def __init__(self, binary, root):
        self.binary, self.root = binary, root
        self.repository = root / 'repository'
        self.step = 0

    def call(self, operation, *arguments, view=None, error=False):
        self.step += 1
        argv = [str(self.binary), operation, str(self.repository), *map(str, arguments)]
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
         ('basic-fs', 'FS007', symlinks), ('basic-fs', 'FS009', removals),
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
    manifest = {'schema': 1, 'adapter': 'offline-cli', 'profile': args.profile,
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
            scenario(fixture)
            fixture.call('fs-check')
        except Exception as error:
            result.update(status='fail', error=str(error))
            (root / 'failure.txt').write_text(traceback.format_exc())
        results.append(result)
    report = {'schema': 1, 'results': results, 'unimplemented': ['FS008-resident-handles', 'FS010-directory-pagination', 'full-fork-revision-matrix', 'resident-rpc',
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
