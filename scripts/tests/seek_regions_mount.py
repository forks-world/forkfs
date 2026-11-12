#!/usr/bin/env python3
"""Qualify sparse lseek on an owned scratch directory; retain a revision for readback."""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from filesystem_compat import check, filesystem

C, SIZE = 65536, 1 << 40


def no_region(fd, offset, origin):
    try:
        os.lseek(fd, offset, origin)
    except OSError as error:
        check(error.errno == errno.ENXIO, 'unexpected seek errno: %s' % error.errno)
        return
    raise RuntimeError('missing ENXIO')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--probe-only', action='store_true')
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
        report = json.loads(args.verify_report.read_text()); ns = report['namespace']
        check(ns.startswith('/forkfs-seek-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        fd = os.open(target/ns.lstrip('/')/'sparse', os.O_RDONLY)
        try:
            check(os.lseek(fd, C, os.SEEK_DATA) == 3*C, 'retained data region changed')
            check(os.lseek(fd, 3*C, os.SEEK_HOLE) == 4*C, 'retained hole changed')
            check(os.pread(fd, 1, 3*C+7) == b'A', 'retained payload changed')
            no_region(fd, SIZE, os.SEEK_DATA)
        finally:
            os.close(fd)
        print(json.dumps({'seek_after_gc_readonly_remount': True})); return
    if not args.reference_apfs and not args.probe_only:
        check(args.daemon is not None and args.socket is not None, 'requires daemon/socket')

    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        result = subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                                 *map(str, fields), *view], check=True, capture_output=True, timeout=60)
        return json.loads(result.stdout)

    root = Path(tempfile.mkdtemp(prefix='forkfs-seek-', dir=target)); fd = duplicate = -1
    report = {'filesystem': kind, 'namespace': '/'+root.name}
    try:
        path = root/'sparse'; fd = os.open(path, os.O_CREAT|os.O_EXCL|os.O_RDWR, 0o600)
        os.ftruncate(fd, SIZE)
        if not args.probe_only:
            no_region(fd, 0, os.SEEK_DATA)
            check(os.lseek(fd, C, os.SEEK_HOLE) == C, 'all-hole file lost hole')
        for offset, payload in [(3*C+7, b'A'), (1023*C+9, b'B'), (1024*C, b'C'), (SIZE-1, b'Z')]:
            os.pwrite(fd, payload, offset)
        os.fsync(fd)
        if args.probe_only:
            for name, origin in [('data', os.SEEK_DATA), ('hole', os.SEEK_HOLE)]:
                try:
                    report[name] = os.lseek(fd, C, origin)
                except OSError as error:
                    report[name] = {'errno': error.errno}
            print(json.dumps(report, indent=2)); return
        stable = os.fstat(fd)
        if not args.reference_apfs:
            check(os.lseek(fd, C, os.SEEK_HOLE) == C, 'hole query does not return hole offset')
        first = os.lseek(fd, C, os.SEEK_DATA)
        check(first <= 3*C+7 and first >= C, 'data seek skipped stored bytes')
        hole = os.lseek(fd, first, os.SEEK_HOLE)
        check(hole > 3*C+7 and hole <= 1023*C+9, 'hole query skipped gap')
        if not args.reference_apfs:
            check(first == 3*C and hole == 4*C, 'unexpected forkfs extent granularity')
            check(os.lseek(fd, 1023*C+10, os.SEEK_HOLE) == 1025*C, 'old/new chunk key boundary gap')
            check(rpc('fs-seek-region', '/'+root.name+'/sparse', C, 'data')['offset'] == first, 'RPC/native mismatch')
            revision = 'seek_accept_' + root.name.removeprefix('forkfs-seek-').replace('-', '_')
            rpc('fs-snapshot', revision); report['revision'] = revision
        for origin in [os.SEEK_DATA, os.SEEK_HOLE]:
            no_region(fd, SIZE, origin); no_region(fd, SIZE+1, origin)
        check(os.lseek(fd, SIZE-1, os.SEEK_HOLE) == SIZE, 'missing EOF terminal hole')
        after = os.fstat(fd)
        check((after.st_mtime_ns, after.st_ctime_ns) == (stable.st_mtime_ns, stable.st_ctime_ns), 'seek mutated inode times')
        duplicate = os.dup(fd); path.unlink(); os.pwrite(duplicate, b'\0', 3*C+7)
        path.write_bytes(b'new')
        check(os.pread(fd, 1, SIZE-1) == b'Z', 'path reuse changed orphan data')
        if not args.reference_apfs:
            check(os.lseek(duplicate, C, os.SEEK_DATA) == 1023*C, 'orphan extent not updated')
            check(os.lseek(fd, 0, os.SEEK_CUR) == 1023*C, 'dup does not share seek position')
            check(rpc('fs-seek-region', '/'+root.name+'/sparse', C, 'data', revision=revision)['offset'] == 3*C, 'orphan changed revision')
            report['revision_isolation'] = True
        report.update({'first_data': first, 'first_hole': hole, 'eof_enxio': True,
                       'one_tib_sparse': True, 'orphan_dup_path_reuse': True})
        empty = os.open(root/'empty', os.O_CREAT|os.O_EXCL|os.O_RDWR, 0o600)
        try:
            no_region(empty, 0, os.SEEK_DATA); no_region(empty, 0, os.SEEK_HOLE)
        finally:
            os.close(empty)
    finally:
        if duplicate >= 0: os.close(duplicate)
        if fd >= 0: os.close(fd)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
