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


def basic(f):
    f.initialize()
    f.write('/a', b'abc')
    f.call('fs-init', error='filesystem already initialized')
    f.read('/a', b'abc')
    f.call('fs-mkdir', '/dir')
    f.call('fs-rename', '/a', '/dir/a')
    f.call('fs-link', '/dir/a', '/dir/b')
    left = json.loads(f.call('fs-stat', '/dir/a'))
    right = json.loads(f.call('fs-stat', '/dir/b'))
    if left['inode_id'] != right['inode_id'] or left['links'] != 2:
        raise AssertionError('hardlink identity/count mismatch')
    f.write('/dir/b', b'changed')
    f.read('/dir/a', b'changed')
    f.call('fs-rmdir', '/dir', error='directory is not empty')
    f.call('fs-unlink', '/dir/a')
    f.read('/dir/b', b'changed')
    f.call('fs-check')


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


CASES = [('basic-fs', 'FS-basic-smoke', basic), ('fork', 'FK-isolation-smoke', fork),
         ('revision', 'RV-history-smoke', revision)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--suite', choices=['all', 'basic-fs', 'fork', 'revision'], default='all')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--profile', choices=['smoke', 'acceptance'], default='smoke')
    args = parser.parse_args()
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
                'suite': args.suite, 'output': str(output)}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    results = []
    for suite, case, scenario in CASES:
        if args.suite not in ['all', suite]:
            continue
        root = output / case
        root.mkdir()
        result = {'suite': suite, 'case': case, 'status': 'pass'}
        try:
            scenario(Fixture(binary, root))
        except Exception as error:
            result.update(status='fail', error=str(error))
            (root / 'failure.txt').write_text(traceback.format_exc())
        results.append(result)
    report = {'schema': 1, 'results': results, 'unimplemented': ['full-fixed-case-matrix', 'resident-rpc',
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
