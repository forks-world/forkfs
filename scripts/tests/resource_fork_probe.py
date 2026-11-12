#!/usr/bin/env python3
"""Compare native ResourceFork range and EOF behavior on forkfs/APFS.

Read-only syscall cases after seeding one owned file; cp writes only private
scratch destinations. Failures are reported as capability results, not passes.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from filesystem_compat import check, filesystem
from large_xattr_mount import call, get, put, RESOURCE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--bytes', type=int, default=32768)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if not args.reference_apfs:
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    check(1 <= args.bytes <= 262143, 'invalid bounded payload size')
    root = Path(tempfile.mkdtemp(prefix='forkfs-resource-probe-', dir=target))
    fd = -1
    value = (bytes(range(251)) * ((args.bytes + 250) // 251))[:args.bytes]
    report = {'filesystem': kind, 'bytes': args.bytes, 'cases': []}
    try:
        path = root / 'file'
        path.write_bytes(b'data')
        put(path, RESOURCE, value)
        fd = os.open(path, os.O_RDWR)
        for target_kind, item in [('path', path), ('fd', fd)]:
            for position, capacity in [(0, len(value)), (0, 17), (1, 17), (len(value)-1, 17),
                                       (len(value), 17), (len(value)+1, 17)]:
                row = {'target': target_kind, 'position': position, 'capacity': capacity}
                data = ctypes.create_string_buffer(capacity)
                try:
                    length = call('getxattr', item, RESOURCE.encode(), data, capacity, position, 0)
                    actual = data.raw[:length]
                    row.update(returned_bytes=length, matches=actual == value[position:position+capacity])
                except OSError as error:
                    row['errno'] = error.errno
                report['cases'].append(row)
        copied = subprocess.run(['/bin/cp', '-p', str(path), str(root/'copy')], capture_output=True, timeout=60)
        report['copy'] = {'returncode': copied.returncode, 'stderr': copied.stderr.decode(errors='replace')}
        try:
            report['copy']['destination_value_matches'] = get(root/'copy', RESOURCE) == value
        except OSError as error:
            report['copy']['destination_read_errno'] = error.errno
        if args.reference_apfs:
            check(all(row.get('matches') for row in report['cases']) and copied.returncode == 0, 'APFS reference failed')
        check(get(fd, RESOURCE) == value and path.read_bytes() == b'data', 'probe changed source')
        report['source_unchanged'] = True
    finally:
        if fd >= 0:
            os.close(fd)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
