#!/usr/bin/env python3
"""Native xattr count/byte-budget acceptance on a disposable forkfs mount.

Retains one revision. Uses only an owned scratch directory; verify the retained
metadata after unmount, offline GC and read-only revision remount.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from filesystem_compat import check, filesystem
from large_xattr_mount import call, get, put, reject, libc

COUNT, VALUE = 128, 262143
for prefix, first in [('', ctypes.c_char_p), ('f', ctypes.c_int)]:
    fn = getattr(libc, prefix + 'listxattr')
    fn.argtypes, fn.restype = [first, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int], ctypes.c_ssize_t
    fn = getattr(libc, prefix + 'removexattr')
    fn.argtypes, fn.restype = [first, ctypes.c_char_p, ctypes.c_int], ctypes.c_int


def names(target):
    length = call('listxattr', target, None, 0, 0)
    data = ctypes.create_string_buffer(length)
    actual = call('listxattr', target, data, length, 0)
    return {name.decode() for name in data.raw[:actual].split(b'\0') if name}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--expect-old-limit', action='store_true')
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(target == mounted and kind == 'forkfs' and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    large = (bytes(range(256))*1024)[:VALUE]
    if args.verify_report:
        report = json.loads(args.verify_report.read_text())
        ns = report['namespace']
        check(ns.startswith('/forkfs-xattr-capacity-') and len(Path(ns).parts) == 2, 'invalid report path')
        root = target / ns.lstrip('/')
        check(names(root/'count') == set(report['count_names']), 'revision names changed')
        check(get(root/'count', 'user.k0') == b'0', 'revision count value changed')
        check(get(root/'budget', 'pad') == bytes(report['padding_bytes']), 'revision byte-budget state changed')
        check(get(root/'budget', 'large0') == large, 'revision payload changed')
        reject(lambda: put(root/'count', 'user.k0', b'changed'), errno.EROFS)
        print(json.dumps({'revision_count_and_byte_budget_after_gc': True, 'readonly_write_rejected': True}))
        return
    if not args.expect_old_limit:
        check(args.daemon is not None and args.socket is not None, 'requires daemon/socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                               *map(str, fields), *view], check=True, capture_output=True, timeout=60).stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-xattr-capacity-', dir=target))
    descriptor = budget = -1
    report = {'namespace': '/'+root.name, 'maximum_count': COUNT, 'logical_value_budget_bytes': 8*1024*1024}
    try:
        path = root/'count'
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        initial = names(path)
        if args.expect_old_limit:
            for i in range(32-len(initial)):
                put(path, 'user.k'+str(i), str(i).encode())
            reject(lambda: put(path, 'overflow', b'32'), errno.ENOSPC)
            print(json.dumps({'old_32_attribute_limit_reproduced': True}))
            return
        for i in range(COUNT-len(initial)):
            put(descriptor, 'user.k'+str(i), str(i).encode())
        check(len(names(path)) == COUNT and names(path) == names(descriptor), 'count/fd list mismatch')
        report['count_names'] = sorted(names(path))
        reject(lambda: put(descriptor, 'overflow', b''), errno.ENOSPC)
        put(path, 'user.k0', b'0', 4)
        budget_path = root/'budget'
        budget = os.open(budget_path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        inherited_bytes = sum(len(get(budget, name)) for name in names(budget))
        number, padding = divmod(8*1024*1024-inherited_bytes, VALUE)
        check(number > 0, 'unexpected inherited metadata budget')
        for i in range(number):
            put(budget_path, 'large'+str(i), large)
        put(budget, 'pad', bytes(padding))
        report['padding_bytes'] = padding
        report['inherited_value_bytes'] = inherited_bytes
        reject(lambda: put(budget, 'overflow', b'1'), errno.ENOSPC)
        check(get(budget, 'pad') == bytes(padding), 'byte-budget failure changed value')
        os.fsync(descriptor)
        os.fsync(budget)
        revision = 'xattr_capacity_' + root.name.removeprefix('forkfs-xattr-capacity-').replace('-', '_')
        rpc('fs-snapshot', revision)
        report['revision'] = revision
        path.unlink()
        check(len(names(descriptor)) == COUNT, 'orphan lost attribute names')
        reject(lambda: put(descriptor, 'overflow', b''), errno.ENOSPC)
        call('removexattr', descriptor, b'user.k0', 0)
        put(descriptor, 'reused', b'new')
        check(len(names(descriptor)) == COUNT and get(descriptor, 'reused') == b'new', 'removed name slot not reusable')
        budget_path.unlink()
        reject(lambda: put(budget, 'overflow', b'1'), errno.ENOSPC)
        put(budget, 'large0', large[:-1], 4)
        put(budget, 'reused', b'1')
        check(get(budget, 'reused') == b'1', 'freed byte budget not reusable')
        check(rpc('fs-xattr-get', report['namespace']+'/count', 'user.k0', revision=revision) == b'0', 'orphan changed revision')
        rpc('fs-check')
        rpc('fs-check', revision=revision)
        report.update(count_boundary_and_reuse=True, byte_boundary_and_reuse=True, orphan_and_revision_isolation=True)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if budget >= 0:
            os.close(budget)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
