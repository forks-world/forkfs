#!/usr/bin/env python3
"""Qualify backed data allocation on an actual disposable forkfs mount.

Allocation excludes holes and counts stored payloads rounded to 512 bytes per
position. Shared payload references count per file position; this is not an
exclusive physical-byte bill for the LevelDB repository. Retains one revision.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from filesystem_compat import check, filesystem

MAXIMUM = 1 << 40


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--world', default='main')
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(target == mounted and kind == 'forkfs', 'requires actual forkfs mount root')
    check(target.stat().st_mode & 0o777 == 0o700, 'root must remain private')
    if args.verify_report:
        report = json.loads(args.verify_report.read_text())
        namespace = report['namespace']
        check(namespace.startswith('/forkfs-allocation-') and len(Path(namespace).parts) == 2, 'invalid namespace')
        root = target / namespace.lstrip('/')
        for name in ['sparse', 'alias']:
            with (root / name).open('rb') as stream:
                info = os.fstat(stream.fileno())
                check(info.st_size == MAXIMUM and info.st_blocks * 512 == 66048, 'revision allocation changed')
                check(os.pread(stream.fileno(), 1, 0) == b'A' and os.pread(stream.fileno(), 1, MAXIMUM - 1) == b'Z', 'revision bytes changed')
        print(json.dumps({'revision_size_and_allocation_after_gc': True, 'allocated_size': 66048}))
        return
    check(args.daemon is not None and args.socket is not None, 'requires --daemon and --socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', args.world]
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request',
                               str(args.socket.resolve(strict=True)), *map(str, fields), *view],
                              check=True, capture_output=True, timeout=60).stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-allocation-', dir=target))
    namespace = '/' + root.name
    report = {'namespace': namespace, 'allocation_unit': 512, 'logical_size': MAXIMUM, 'checks': []}
    descriptor = -1
    try:
        path = root / 'sparse'
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)

        def allocated(expected):
            os.fsync(descriptor)
            actual = os.fstat(descriptor).st_blocks * 512
            check(actual == expected, 'allocation mismatch: %s != %s' % (actual, expected))
            return actual

        os.ftruncate(descriptor, MAXIMUM)
        allocated(0)
        report['checks'].append('1 TiB hole-only file allocates zero data blocks')
        check(os.pwrite(descriptor, b'A', 0) == 1, 'head write')
        allocated(512)
        check(os.pwrite(descriptor, b'Z', MAXIMUM - 1) == 1, 'tail write')
        allocated(66048)
        check(json.loads(rpc('fs-stat', namespace + '/sparse'))['allocated_size'] == 66048, 'daemon/native allocation differs')
        os.link(path, root / 'alias')
        check((root / 'alias').stat().st_blocks * 512 == 66048, 'hardlink allocation identity')
        report['du_kib'] = int(subprocess.run(['/usr/bin/du', '-k', str(path)], check=True, capture_output=True, text=True).stdout.split()[0])
        check(report['du_kib'] == 65, 'du still reports logical sparse length')
        revision = 'allocation_accept_' + root.name.removeprefix('forkfs-allocation-').replace('-', '_')
        rpc('fs-snapshot', revision)
        report['revision'] = revision
        check(os.pwrite(descriptor, b'\0', MAXIMUM - 1) == 1, 'zero tail')
        allocated(512)
        path.unlink()
        (root / 'alias').unlink()
        check(os.fstat(descriptor).st_nlink == 0, 'orphan identity')
        check(os.pwrite(descriptor, b'A', 65536) == 1, 'shared payload write')
        allocated(1024)
        check(os.pwrite(descriptor, b'\0', 0) == 1, 'zero head')
        allocated(512)
        os.ftruncate(descriptor, 0)
        allocated(0)
        os.ftruncate(descriptor, MAXIMUM)
        allocated(0)
        os.close(descriptor)
        descriptor = -1
        report['checks'].append('zero writes, hardlinks, orphan edits and shrink/extend update allocation')
        inline = root / 'inline'
        inline.write_bytes(b'A')
        check(inline.stat().st_blocks * 512 == 512, 'inline allocation')
        os.truncate(inline, 4097)
        check(inline.stat().st_blocks * 512 == 4608, 'inline stored zero padding allocation')
        report['checks'].append('inline payloads round to 512-byte units')
        rpc('fs-check')
        rpc('fs-check', revision=revision)
        report['revision_allocated_size'] = json.loads(rpc('fs-stat', namespace + '/sparse', revision=revision))['allocated_size']
        check(report['revision_allocated_size'] == 66048, 'mutable edits changed revision allocation')
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
