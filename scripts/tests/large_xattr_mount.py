#!/usr/bin/env python3
"""Whole-value large xattr/resource fork acceptance on forkfs or explicit APFS.

Uses only an owned scratch directory. Forkfs mode retains one revision; use a
disposable repository. Position-based resource-fork I/O is a separate probe.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from filesystem_compat import check, filesystem

LIMIT = 256 * 1024 - 1
RESOURCE = 'com.apple.ResourceFork'
libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
for operation, trailing, result in [
    ('setxattr', [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int], ctypes.c_int),
    ('getxattr', [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int], ctypes.c_ssize_t),
]:
    for prefix, first in [('', ctypes.c_char_p), ('f', ctypes.c_int)]:
        fn = getattr(libc, prefix + operation)
        fn.argtypes, fn.restype = [first, *trailing], result


def call(op, target, *args):
    fd = isinstance(target, int)
    value = getattr(libc, ('f' if fd else '') + op)(target if fd else os.fsencode(target), *args)
    if value < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))
    return value


def put(target, name, value, flags=0):
    data = ctypes.create_string_buffer(value)
    call('setxattr', target, name.encode(), data, len(value), 0, flags)


def get(target, name):
    length = call('getxattr', target, name.encode(), None, 0, 0, 0)
    data = ctypes.create_string_buffer(length)
    count = call('getxattr', target, name.encode(), data, length, 0, 0)
    return data.raw[:count]


def reject(operation, expected):
    try:
        operation()
    except OSError as error:
        check(error.errno in (expected if isinstance(expected, tuple) else (expected,)), 'unexpected errno: %s' % error.errno)
        return error.errno
    else:
        raise RuntimeError('operation unexpectedly succeeded')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--expect-size-limit', action='store_true')
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if not args.reference_apfs:
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs mount root')
    if args.verify_report:
        report = json.loads(args.verify_report.read_text())
        namespace = report['namespace']
        check(namespace.startswith('/forkfs-xattr-') and len(Path(namespace).parts) == 2, 'invalid report path')
        root = target / namespace.lstrip('/')
        for name in ['file', 'alias', 'copy']:
            for attr in ['user.large', RESOURCE]:
                value = get(root / name, attr)
                check(len(value) == LIMIT and hashlib.sha256(value).hexdigest() == report['sha256'], 'revision xattr changed')
        reject(lambda: put(root / 'file', 'user.large', b'changed'), errno.EROFS)
        print(json.dumps({'read_only_large_xattrs_after_gc': True, 'readonly_write_rejected': True, 'bytes_per_attribute': LIMIT}))
        return
    if not args.reference_apfs and not args.expect_size_limit:
        check(args.daemon is not None and args.socket is not None, 'requires daemon/socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                               *map(str, fields), *view], check=True, capture_output=True, timeout=60).stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-xattr-', dir=target))
    fd = -1
    report = {'filesystem': kind, 'namespace': '/' + root.name, 'bytes_per_attribute': LIMIT,
              'xattr_size_bits': os.pathconf(target, 26)}
    value = (bytes(range(256)) * 1024)[:LIMIT]
    report['sha256'] = hashlib.sha256(value).hexdigest()
    try:
        path = root / 'file'
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        os.write(fd, b'data')
        if args.expect_size_limit:
            code = reject(lambda: put(path, 'user.large', value), (errno.E2BIG, errno.EFBIG))
            print(json.dumps({'old_64k_limit_reproduced': True, 'errno': code, 'xattr_size_bits': report['xattr_size_bits']}))
            return
        put(path, 'user.large', value, 2)
        check(get(fd, 'user.large') == value, 'large pathname/fd xattr mismatch')
        reject(lambda: put(path, 'user.large', b'other', 2), errno.EEXIST)
        short = ctypes.create_string_buffer(16)
        reject(lambda: call('getxattr', fd, b'user.large', short, 16, 0, 0), errno.ERANGE)
        subprocess.run(['/bin/cp', '-p', str(path), str(root / 'copy')], check=True, capture_output=True, timeout=60)
        check(get(root / 'copy', 'user.large') == value, 'cp lost ordinary large xattr')
        report['copy_preserves_large_user_xattr'] = True
        put(path, RESOURCE, value, 2)
        check(get(fd, RESOURCE) == value, 'large resource fork mismatch')
        reject(lambda: put(path, RESOURCE, b'other', 2), errno.EEXIST)
        copied = subprocess.run(['/bin/cp', '-p', str(path), str(root / 'resource-copy')], capture_output=True, timeout=60)
        preserved = copied.returncode == 0 and get(root / 'resource-copy', RESOURCE) == value
        report['resource_fork_copy_probe'] = {'preserved': preserved, 'returncode': copied.returncode,
                                             'stderr': copied.stderr.decode(errors='replace')}
        # The revision also retains a separate inode with both large values.
        # Resource-fork cp support is measured above, not assumed here.
        put(root / 'copy', RESOURCE, value, 2)
        os.fsync(fd)
        os.link(path, root / 'alias')
        check(get(root / 'alias', RESOURCE) == value, 'hardlink xattr mismatch')
        if not args.reference_apfs:
            report['oversize_errno'] = {}
            for attr in ['user.large', RESOURCE]:
                report['oversize_errno'][attr] = reject(lambda: put(path, attr, bytes(LIMIT + 1), 4), (errno.E2BIG, errno.EFBIG))
                check(get(fd, attr) == value, 'failed replace changed xattr')
            revision = 'xattr_accept_' + root.name.removeprefix('forkfs-xattr-').replace('-', '_')
            rpc('fs-snapshot', revision)
            report['revision'] = revision
            check(rpc('fs-xattr-get', report['namespace'] + '/file', RESOURCE) == value, 'native/RPC xattr mismatch')
        # Measure the separate positional syscall behavior without mistaking
        # whole-value support for resource fork stream/offset compatibility.
        try:
            piece = ctypes.create_string_buffer(17)
            count = call('getxattr', fd, RESOURCE.encode(), piece, 17, 65536, 0)
            report['position_read_probe'] = {'matches': piece.raw[:count] == value[65536:65536 + 17], 'returned_bytes': count}
        except OSError as error:
            report['position_read_probe'] = {'errno': error.errno}
        path.unlink()
        (root / 'alias').unlink()
        check(get(fd, RESOURCE) == value, 'open-unlink lost xattr')
        changed = value[::-1]
        put(fd, RESOURCE, changed, 4)
        check(get(fd, RESOURCE) == changed, 'orphan xattr replacement failed')
        if not args.reference_apfs:
            check(rpc('fs-xattr-get', report['namespace'] + '/file', RESOURCE, revision=revision) == value, 'orphan edit changed revision')
            rpc('fs-check')
            rpc('fs-check', revision=revision)
        report['open_unlink'] = True
        if not args.reference_apfs:
            report['revision_isolation'] = True
    finally:
        if fd >= 0:
            os.close(fd)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
