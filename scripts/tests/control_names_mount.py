#!/usr/bin/env python3
"""Qualify control-character names on an APFS reference or mounted forkfs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from filesystem_compat import check, filesystem


def worker(root):
    root = os.fsencode(root)
    name = b'controls-' + bytes(range(1, 32)) + b'\x7f\xc2\x80\xc2\x9f'
    payload = b'control-name-bytes'
    path = root + b'/' + name
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        check(os.write(fd, payload) == len(payload), 'short write')
        os.fsync(fd)
        check(os.pread(fd, len(payload), 0) == payload, 'file readback mismatch')
        check(name in os.listdir(root), 'readdir changed filename bytes')
        target = b'target-' + bytes(range(1, 32)) + b'\x7f\xc2\x80\xc2\x9f'
        link = root + b'/control-link'
        os.symlink(target, link)
        check(os.readlink(link) == target, 'readlink changed target bytes')
        return {'filename_bytes_roundtrip': True, 'readlink_bytes_roundtrip': True,
                'control_scalars': 34}
    finally:
        os.close(fd)
        for entry in (root + b'/control-link', path):
            try:
                os.unlink(entry)
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path, nargs='?')
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--worker', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        print(json.dumps(worker(args.worker), sort_keys=True))
        return
    check(args.mountpoint is not None and sys.platform == 'darwin', 'requires a macOS mountpoint')
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'),
          'unexpected containing filesystem: ' + kind)
    if not args.reference_apfs:
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700,
              'candidate mode requires the exact private forkfs mount root')
    scratch = Path(tempfile.mkdtemp(prefix='forkfs-control-names-', dir=target))
    try:
        result = subprocess.run([sys.executable, __file__, '--worker', str(scratch)],
                                check=True, capture_output=True, text=True, timeout=30)
        report = json.loads(result.stdout)
        report.update({'filesystem': 'apfs' if args.reference_apfs else 'forkfs',
                       'mode': 'reference' if args.reference_apfs else 'candidate'})
        print(json.dumps(report, sort_keys=True))
    finally:
        shutil.rmtree(scratch)


if __name__ == '__main__':
    main()
