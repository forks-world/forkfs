#!/usr/bin/env python3
"""Qualify BSD metadata flags on a private forkfs mount or explicit APFS.

Uses one owned scratch directory; forkfs retains one revision for GC/remount
readback. Only hidden/nodump are supported; protection flags must be rejected.
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

HIDDEN, NODUMP = 0x8000, 1
libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
for name, first in [('chflags', ctypes.c_char_p), ('lchflags', ctypes.c_char_p), ('fchflags', ctypes.c_int)]:
    fn = getattr(libc, name)
    fn.argtypes, fn.restype = [first, ctypes.c_uint32], ctypes.c_int


def change(target, value, nofollow=False):
    fd = isinstance(target, int)
    fn = libc.fchflags if fd else libc.lchflags if nofollow else libc.chflags
    if fn(target if fd else os.fsencode(target), value) < 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code))


def reject(operation, *codes):
    try:
        operation()
    except OSError as error:
        check(error.errno in codes, 'unexpected errno %s' % error.errno)
        return error.errno
    raise RuntimeError('operation unexpectedly succeeded')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--expect-unsupported', action='store_true')
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if not args.reference_apfs:
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    if args.verify_report:
        report = json.loads(args.verify_report.read_text())
        ns = report['namespace']
        check(ns.startswith('/forkfs-flags-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        root = target / ns.lstrip('/')
        for name in ['file', 'alias', 'copy']:
            check((root/name).stat().st_flags == HIDDEN|NODUMP, 'revision file flags changed')
        check((root/'dir').stat().st_flags == HIDDEN and (root/'link').lstat().st_flags == NODUMP, 'revision directory/link flags changed')
        reject(lambda: change(root/'file', 0), errno.EROFS)
        print(json.dumps({'flags_after_gc_readonly_remount': True, 'readonly_change_rejected': True}))
        return
    if not args.reference_apfs and not args.expect_unsupported:
        check(args.daemon is not None and args.socket is not None, 'requires daemon/socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                               *map(str, fields), *view], check=True, capture_output=True, timeout=60).stdout

    root = Path(tempfile.mkdtemp(prefix='forkfs-flags-', dir=target))
    fd = duplicate = -1
    report = {'filesystem': kind, 'namespace': '/'+root.name}
    try:
        path = root/'file'
        fd = os.open(path, os.O_CREAT|os.O_EXCL|os.O_RDWR, 0o600)
        os.write(fd, b'data')
        os.fsync(fd)
        original = os.fstat(fd)
        if args.expect_unsupported:
            code = reject(lambda: change(path, HIDDEN), errno.ENOTSUP)
            check(os.fstat(fd).st_flags == original.st_flags and path.read_bytes() == b'data', 'rejection changed file')
            print(json.dumps({'prior_flags_rejected': True, 'errno': code}))
            return
        change(path, HIDDEN|NODUMP)
        check(os.fstat(fd).st_flags == HIDDEN|NODUMP and os.fstat(fd).st_mtime_ns == original.st_mtime_ns, 'flag set changed data time')
        os.link(path, root/'alias')
        check((root/'alias').stat().st_flags == HIDDEN|NODUMP, 'hardlink flags differ')
        (root/'dir').mkdir()
        change(root/'dir', HIDDEN)
        os.symlink('file', root/'link')
        change(root/'link', NODUMP, nofollow=True)
        check((root/'link').lstat().st_flags == NODUMP and path.stat().st_flags == HIDDEN|NODUMP, 'nofollow flags changed target')
        subprocess.run(['/bin/cp', '-p', str(path), str(root/'copy')], check=True, capture_output=True, timeout=60)
        check((root/'copy').stat().st_flags == HIDDEN|NODUMP and (root/'copy').read_bytes() == b'data', 'cp lost metadata flags')
        report['path_fd_directory_symlink_hardlink_copy'] = True
        if not args.reference_apfs:
            report['unsupported_errno'] = {}
            for flags in [2, 4, 0x20000]:
                report['unsupported_errno'][str(flags)] = reject(lambda: change(path, flags), errno.ENOTSUP, errno.EPERM)
                check(os.fstat(fd).st_flags == HIDDEN|NODUMP and path.read_bytes() == b'data', 'unsupported flag changed state')
            revision = 'flags_accept_' + root.name.removeprefix('forkfs-flags-').replace('-', '_')
            rpc('fs-snapshot', revision)
            report['revision'] = revision
            check(json.loads(rpc('fs-stat', report['namespace']+'/file'))['flags'] == HIDDEN|NODUMP, 'native/RPC flags mismatch')
        duplicate = os.dup(fd)
        path.unlink()
        (root/'alias').unlink()
        change(duplicate, NODUMP)
        check(os.fstat(fd).st_flags == NODUMP, 'orphan/dup flags differ')
        path.write_bytes(b'new')
        check(path.stat().st_flags == 0 and os.pread(fd, 4, 0) == b'data', 'path reuse changed orphan identity')
        if not args.reference_apfs:
            check(json.loads(rpc('fs-stat', report['namespace']+'/file', revision=revision))['flags'] == HIDDEN|NODUMP, 'orphan flags changed revision')
            rpc('fs-check')
            rpc('fs-check', revision=revision)
        if not args.reference_apfs:
            report['revision_isolation'] = True
        report['orphan_dup_path_reuse'] = True
    finally:
        if duplicate >= 0:
            os.close(duplicate)
        if fd >= 0:
            os.close(fd)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
