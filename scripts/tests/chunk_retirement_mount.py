#!/usr/bin/env python3
"""Exercise dense retirement beyond 4096 chunks on a disposable forkfs mount.

Retains one named revision and cleans up live scratch files. --verify-report
checks the immutable dense bytes after offline GC and read-only remount.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from filesystem_compat import check, filesystem

SIZE = 320 * 1024 * 1024
BLOCK = 1024 * 1024


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            data = stream.read(BLOCK)
            if not data:
                return value.hexdigest()
            value.update(data)


def write_dense(path):
    value = hashlib.sha256()
    with path.open('wb', buffering=0) as stream:
        for offset in range(0, SIZE, BLOCK):
            data = (offset.to_bytes(8, 'little') + bytes(range(256)) * 4096)[:BLOCK]
            check(stream.write(data) == BLOCK, 'short dense write')
            value.update(data)
            if (offset + BLOCK) % (64 * BLOCK) == 0:
                print('%s: %s MiB' % (path.name, (offset + BLOCK) // BLOCK), file=sys.stderr, flush=True)
        os.fsync(stream.fileno())
    check(digest(path) == value.hexdigest(), 'dense initial readback')
    return value.hexdigest()


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
        check(namespace.startswith('/forkfs-retire-') and len(Path(namespace).parts) == 2, 'invalid namespace')
        root = target / namespace.lstrip('/')
        check(digest(root / 'open') == report['dense_sha256'], 'open-file revision hash after GC')
        check(digest(root / 'closed') == report['dense_sha256'], 'closed-file revision hash after GC')
        print(json.dumps({'read_only_revision_dense_hashes_match': True, 'bytes_per_file': SIZE}))
        return
    check(args.daemon is not None and args.socket is not None, 'requires --daemon and --socket')

    def rpc(*fields, revision=None):
        view = [] if fields[0] == 'status' else (['--revision', revision] if revision else ['--world', args.world])
        result = subprocess.run([str(args.daemon.resolve(strict=True)), 'request',
                                 str(args.socket.resolve(strict=True)), *map(str, fields), *view],
                                capture_output=True, timeout=90)
        check(result.returncode == 0, result.stderr.decode(errors='replace'))
        return result.stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-retire-', dir=target))
    report = {'namespace': '/' + root.name, 'bytes_per_file': SIZE,
              'allocated_chunks_per_file': SIZE // 65536}
    descriptor = duplicate = -1
    try:
        opened, closed = root / 'open', root / 'closed'
        expected = write_dense(opened)
        check(write_dense(closed) == expected, 'different dense payloads')
        report['dense_sha256'] = expected
        revision = 'retire_accept_' + root.name.removeprefix('forkfs-retire-').replace('-', '_')
        rpc('fs-snapshot', revision)
        report['revision'] = revision
        original_tail = rpc('fs-read-at', report['namespace'] + '/open', SIZE - 8, 8, revision=revision)
        descriptor = os.open(opened, os.O_RDWR)
        duplicate = os.dup(descriptor)
        identity = os.fstat(descriptor).st_ino
        os.link(opened, root / 'alias')
        before = json.loads(rpc('status'))
        opened.unlink()
        (root / 'alias').unlink()
        after = json.loads(rpc('status'))
        check(after['object_roots'] < before['object_roots'] + 256, 'retirement published per-chunk roots')
        report['unlink_object_root_delta'] = after['object_roots'] - before['object_roots']
        check(os.fstat(descriptor).st_nlink == 0 and os.fstat(descriptor).st_ino == identity, 'orphan identity')
        check(os.pwrite(descriptor, b'ORPHAN!!', SIZE - 8) == 8, 'orphan write')
        os.fsync(descriptor)
        check(os.pread(duplicate, 8, SIZE - 8) == b'ORPHAN!!', 'dup orphan bytes')
        os.ftruncate(descriptor, BLOCK + 17)
        os.ftruncate(descriptor, SIZE)
        os.fsync(descriptor)
        check(os.pread(duplicate, 8, SIZE - 8) == bytes(8), 'orphan truncate leaked tail')
        check(os.pread(duplicate, 4, BLOCK + 15) == b'\x07\x08\0\0', 'partial chunk zero fill')
        rpc('fs-check')
        opened.write_bytes(b'REPLACEMENT')
        check(opened.stat().st_ino != identity, 'path reuse retargeted orphan')
        os.close(descriptor)
        descriptor = -1
        check(os.pread(duplicate, 8, SIZE - 8) == bytes(8), 'first close released dup')
        os.close(duplicate)
        duplicate = -1
        report['open_unlink_dup_write_truncate_and_path_reuse'] = True
        with closed.open('rb') as stream:
            original_head = stream.read(17)
        before = json.loads(rpc('status'))
        os.truncate(closed, 17)
        after = json.loads(rpc('status'))
        check(after['object_roots'] < before['object_roots'] + 256, 'truncate published per-chunk roots')
        with closed.open('rb') as stream:
            os.fsync(stream.fileno())
        check(closed.read_bytes() == original_head, 'closed truncate bytes')
        report['closed_truncate'] = True
        report['truncate_object_root_delta'] = after['object_roots'] - before['object_roots']
        check(rpc('fs-read-at', report['namespace'] + '/open', SIZE - 8, 8,
                  revision=revision) == original_tail, 'orphan edit changed immutable revision')
        rpc('fs-check')
        rpc('fs-check', revision=revision)
        report['revision_isolation'] = True
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if duplicate >= 0:
            os.close(duplicate)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
