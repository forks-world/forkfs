#!/usr/bin/env python3
"""Accept large files on a disposable, actual forkfs mount.

Creates one scratch directory and retains one named revision. --verify-report
reads that revision after GC/restart/read-only remount without creating files.
"""
import argparse
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from filesystem_compat import check, filesystem

MAXIMUM = 1 << 40
BLOCK = 256 * 1024


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            data = stream.read(BLOCK)
            if not data:
                return result.hexdigest()
            result.update(data)


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
        check(namespace.startswith('/forkfs-large-') and len(Path(namespace).parts) == 2,
              'invalid acceptance namespace')
        root = target / namespace.lstrip('/')
        check(digest(root / 'dense') == report['dense_sha256'], 'dense revision bytes after GC/restart')
        with (root / 'sparse').open('rb') as stream:
            check(os.fstat(stream.fileno()).st_size == MAXIMUM, 'sparse revision size')
            for offset, value in report['sparse_markers'].items():
                check(os.pread(stream.fileno(), 4, int(offset)) == bytes.fromhex(value), 'sparse revision bytes')
        check((root / 'mapped').stat().st_size == report['mapped_size'], 'mapped revision size')
        with (root / 'mapped').open('rb') as stream:
            check(os.pread(stream.fileno(), 8, 64 * 1024 * 1024 - 4) == b'MAPPED!!', 'mapped revision bytes')
        print(json.dumps({'readback_after_gc_restart': True, 'dense_sha256': report['dense_sha256'],
                          'maximum_sparse_size': MAXIMUM}))
        return
    check(args.daemon is not None and args.socket is not None, 'requires --daemon and --socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', args.world]
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request',
                               str(args.socket.resolve(strict=True)), *map(str, fields), *view],
                              check=True, capture_output=True, timeout=90).stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-large-', dir=target))
    report = {'namespace': '/' + root.name, 'maximum_file_bytes': MAXIMUM}
    try:
        path = root / 'sparse'
        markers = {64 * 1024 * 1024 - 2: b'EDGE', 10000 * 65536 + 17: b'KEYS',
                   (4 << 30) + 123: b'4GiB', (5 << 30) - 4: b'5GiB', MAXIMUM - 4: b'TAIL'}
        with os.fdopen(os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_SYNC, 0o600), 'r+b') as stream:
            os.ftruncate(stream.fileno(), MAXIMUM)
            for offset, value in markers.items():
                check(os.pwrite(stream.fileno(), value, offset) == 4, 'sparse write length')
            os.fsync(stream.fileno())
            for offset, value in markers.items():
                check(os.pread(stream.fileno(), 4, offset) == value, 'sparse read content')
                check(rpc('fs-read-at', report['namespace'] + '/sparse', offset, 4) == value,
                      'sparse bytes did not reach daemon')
            check(os.pread(stream.fileno(), 16, 3 << 30) == bytes(16), 'hole is not zero')
            stable = os.fstat(stream.fileno())
            # Bypass buffered writeback for the failing write: a buffered write
            # may report success before FSKit sees it and fail only on fsync.
            fcntl.fcntl(stream.fileno(), 48, 1)  # F_NOCACHE, Darwin fcntl.h
            try:
                for name, operation in [('truncate', lambda: os.ftruncate(stream.fileno(), MAXIMUM + 1)),
                                        ('write', lambda: os.pwrite(stream.fileno(), b'x', MAXIMUM))]:
                    try:
                        operation()
                    except OSError as error:
                        check(error.errno == errno.EFBIG, 'wrong size limit errno')
                    else:
                        raise RuntimeError('size limit %s succeeded' % name)
            finally:
                fcntl.fcntl(stream.fileno(), 48, 0)
            after = os.fstat(stream.fileno())
            check((stable.st_size, stable.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
                  'rejected size update changed metadata')
        report['sparse_markers'] = {str(offset): value.hex() for offset, value in markers.items()}
        report['size_limit_errno'] = {'truncate': 'EFBIG', 'O_SYNC_F_NOCACHE_write': 'EFBIG'}
        dense = root / 'dense'
        expected = hashlib.sha256()
        dense_size = 65 * 1024 * 1024 + 17
        with dense.open('wb', buffering=0) as stream:
            for offset in range(0, dense_size, BLOCK):
                block = (offset.to_bytes(8, 'little') + bytes(range(256)) * 1024)[:min(BLOCK, dense_size - offset)]
                check(stream.write(block) == len(block), 'dense short write')
                expected.update(block)
            os.fsync(stream.fileno())
        check(digest(dense) == expected.hexdigest(), 'dense readback mismatch')
        report.update(dense_bytes=dense_size, dense_sha256=expected.hexdigest())
        mapped = root / 'mapped'
        mapped_size = 64 * 1024 * 1024 + mmap.PAGESIZE
        with mapped.open('w+b') as stream:
            stream.truncate(mapped_size)
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_WRITE) as mapping:
                mapping[64 * 1024 * 1024 - 4:64 * 1024 * 1024 + 4] = b'MAPPED!!'
                mapping.flush()
            os.fsync(stream.fileno())
        report['mapped_size'] = mapped_size
        revision = 'large_accept_' + root.name.removeprefix('forkfs-large-').replace('-', '_')
        rpc('fs-snapshot', revision)
        report['revision'] = revision
        # Mutating a linked alias and then an orphan must not change the revision.
        os.link(path, root / 'alias')
        descriptor = os.open(root / 'alias', os.O_RDWR)
        try:
            path.unlink()
            (root / 'alias').unlink()
            check(os.fstat(descriptor).st_nlink == 0, 'unlinked sparse inode link count')
            check(os.pwrite(descriptor, b'OPEN', (4 << 30) + 123) == 4, 'orphan write')
            os.ftruncate(descriptor, (4 << 30) + 125)
            os.ftruncate(descriptor, 5 << 30)
            os.fsync(descriptor)
            check(os.pread(descriptor, 4, (4 << 30) + 123) == b'OP\0\0', 'shrink/extend leaked tail')
            path.write_bytes(b'REPLACEMENT')
            check(path.read_bytes() == b'REPLACEMENT', 'path reuse')
        finally:
            os.close(descriptor)
        check(rpc('fs-read-at', report['namespace'] + '/sparse', (4 << 30) + 123, 4,
                  revision=revision) == b'4GiB', 'revision mutated through orphan')
        rpc('fs-check')
        rpc('fs-check', revision=revision)
        report.update(orphan_identity_and_zero_fill=True, revision_isolation=True)
    finally:
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
