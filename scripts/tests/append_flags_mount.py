#!/usr/bin/env python3
"""Qualify user append-only rules on an owned scratch tree; retain a forkfs revision."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from filesystem_compat import check, filesystem
from file_flags_mount import change, reject
from immutable_mount import set_attribute

APPEND = 4
PAYLOAD = b'data' + bytes(range(256)) * 1300


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
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
        check(ns.startswith('/forkfs-append-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        path = target / ns.lstrip('/') / 'file'
        check(path.stat().st_flags & APPEND and path.read_bytes() == PAYLOAD, 'append revision changed')
        check((path.parent/'dir').stat().st_flags & APPEND, 'directory flag lost')
        reject(lambda: change(path, 0), errno.EROFS)
        print(json.dumps({'append_after_gc_readonly_remount': True, 'sha256': hashlib.sha256(PAYLOAD).hexdigest()})); return
    if not args.reference_apfs:
        check(args.daemon and args.socket, 'requires daemon/socket')
    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        result = subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)), *map(str, fields), *view], check=True, capture_output=True, timeout=30)
        return json.loads(result.stdout)
    root = Path(tempfile.mkdtemp(prefix='forkfs-append-', dir=target)); old = appender = -1
    report = {'filesystem': kind, 'namespace': '/'+root.name, 'denied': {}}
    try:
        path = root/'file'; path.write_bytes(b'data'); os.link(path, root/'alias'); (root/'source').write_bytes(b'source')
        old = os.open(path, os.O_RDWR); change(path, APPEND)
        check((root/'alias').stat().st_flags & APPEND, 'hardlink flags differ')
        def open_plain():
            fd = os.open(path, os.O_WRONLY); os.close(fd)
        operations = {'new_write_open': open_plain, 'old_pwrite': lambda: os.pwrite(old, b'X', 0),
                      'old_ftruncate': lambda: os.ftruncate(old, 0), 'truncate_same': lambda: os.truncate(path, 4),
                      'chmod': lambda: os.chmod(path, 0o400), 'utime': lambda: os.utime(path, ns=(1, 2)),
                      'link': lambda: os.link(path, root/'extra'),
                      'unlink': lambda: path.unlink(), 'rename_source': lambda: path.rename(root/'moved'),
                      'rename_over_target': lambda: os.replace(root/'source', path)}
        for name, fn in operations.items():
            try: report['denied'][name] = reject(fn, errno.EPERM)
            except Exception as error: raise RuntimeError(name+': '+str(error)) from error
        check(path.read_bytes() == b'data', 'failed mutation changed data')
        set_attribute(path); report['xattr_update_allowed'] = True
        appender = os.open(path, os.O_WRONLY|os.O_APPEND)
        # Crosses the frontend's bounded RPC and a partial data page; flush must
        # actually publish the same bytes before accepting cached native reads.
        os.lseek(appender, 0, os.SEEK_SET)
        check(os.write(appender, PAYLOAD[4:]) == len(PAYLOAD)-4, 'short append')
        os.fsync(appender); os.close(appender); appender = -1
        check(path.read_bytes() == PAYLOAD, 'append overwrote data')
        directory = root/'dir'; directory.mkdir(); child = directory/'child'; child.write_bytes(b'child'); change(directory, APPEND)
        (directory/'new').write_bytes(b'new'); (directory/'sub').mkdir(); os.link(child, directory/'hard'); os.symlink('child', directory/'sym')
        (root/'source').rename(directory/'in')
        for name, fn in {'directory_unlink': lambda: child.unlink(), 'directory_move_out': lambda: child.rename(root/'out'),
                         'directory_replace': lambda: os.replace(directory/'new', child)}.items():
            report['denied'][name] = reject(fn, errno.EPERM)
        child.write_bytes(b'updated'); check(child.read_bytes() == b'updated', 'directory flag constrained child data')
        if not args.reference_apfs:
            report['revision'] = 'append_accept_'+root.name[len('forkfs-append-'):].replace('-', '_')
            rpc('fs-snapshot', report['revision'])
            # A daemon read avoids trusting only native page cache.
            # Read in bounded requests instead of a single oversized I/O.
            data = b''
            for offset in range(0, len(PAYLOAD), 256*1024):
                r = subprocess.run([str(args.daemon.resolve()), 'request', str(args.socket.resolve()), 'fs-read-at', '/'+root.name+'/file', str(offset), str(min(256*1024, len(PAYLOAD)-offset)), '--revision', report['revision']], check=True, capture_output=True, timeout=30)
                data += r.stdout
            check(data == PAYLOAD, 'append not persisted in revision')
            rpc('fs-check'); report['revision_isolation'] = True
        change(path, 0); os.pwrite(old, b'new', 0)
        report['clear_allows_overwrite'] = True; report['append_bytes'] = len(PAYLOAD); report['sha256'] = hashlib.sha256(PAYLOAD).hexdigest()
    finally:
        active_error = sys.exc_info()[0] is not None
        close_errors = []
        for fd in [appender, old]:
            if fd >= 0:
                try: os.close(fd)
                except OSError as error: close_errors.append(error)
        for name in ['file', 'alias', 'dir']:
            try: change(root/name, 0)
            except OSError: pass
        shutil.rmtree(root)
        if close_errors and not active_error: raise close_errors[0]
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
