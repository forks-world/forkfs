#!/usr/bin/env python3
"""Native fsync/atomic-save acceptance on owned scratch; no power-loss claim."""
import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from filesystem_compat import check, filesystem, rejects


PAYLOAD = bytes(range(251)) * 4097
MTIME = 1234567890123456789
F_FULLFSYNC, F_BARRIERFSYNC = 51, 85  # Darwin sys/fcntl.h


def flush(path):
    fd = os.open(path, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def barrier_probe(fd, command):
    try:
        fcntl.fcntl(fd, command)
        return {'accepted': True}
    except OSError as error:
        check(error.errno in (errno.EINVAL, errno.ENOTSUP), 'unexpected barrier failure: '+str(error))
        return {'accepted': False, 'errno': error.errno}


def verify(root):
    data = root/'renamed'/'data'; linked = root/'renamed'/'hardlink'
    check(data.read_bytes() == PAYLOAD and (root/'atomic').read_bytes() == PAYLOAD, 'saved bytes differ')
    check(data.stat().st_ino == linked.stat().st_ino and data.stat().st_nlink == 2, 'hardlink identity differs')
    check(data.stat().st_mode & 0o777 == 0o640 and data.stat().st_mtime_ns == MTIME, 'saved metadata differs')
    check(os.readlink(root/'renamed'/'symlink') == 'data', 'saved symlink differs')
    check(not (root/'first').exists() and not (root/'temporary').exists(), 'rename source reappeared')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args()
    target = args.mountpoint.resolve(strict=True); mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if kind == 'forkfs':
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs mount root')
    report = {'platform': platform.platform(), 'filesystem': kind, 'bytes': len(PAYLOAD),
              'sha256': hashlib.sha256(PAYLOAD).hexdigest(),
              'limits': ['graceful unmount/restart only; no power-loss or kernel-crash qualification',
                         'barrier syscall acceptance does not prove a hardware media flush',
                         'single private mount; no concurrent atomic-save qualification']}
    if args.verify_report:
        check(kind == 'forkfs', 'requires readonly forkfs revision')
        previous = json.loads(args.verify_report.read_text()); ns = previous['namespace']
        check(ns.startswith('/forkfs-sync-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        root = target/ns.lstrip('/'); verify(root)
        rejects(lambda: (root/'refused').touch(), errno.EROFS)
        report['readonly_revision_after_gc'] = True
        print(json.dumps(report, indent=2)); return
    if kind == 'forkfs': check(args.daemon and args.socket, 'requires daemon/socket')
    def rpc(*fields, revision=None):
        view = [] if fields[0] == 'status' else ['--revision', revision] if revision else ['--world', 'main']
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                               *map(str, fields), *view], check=True, capture_output=True, timeout=30).stdout
    def backend_data(path, revision=None):
        result = bytearray()
        for offset in range(0, len(PAYLOAD), 256*1024):
            result.extend(rpc('fs-read-at', path, offset, min(256*1024, len(PAYLOAD)-offset), revision=revision))
        check(result == PAYLOAD, 'fsync data absent from daemon')
    root = Path(tempfile.mkdtemp(prefix='forkfs-sync-', dir=target)); report['namespace'] = '/'+root.name
    directory_fd = old_fd = -1
    try:
        directory = root/'first'; directory.mkdir()
        directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        data = directory/'data'
        with data.open('w+b') as stream:
            stream.write(PAYLOAD); stream.flush(); os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o640); os.utime(data, ns=(MTIME, MTIME)); os.fsync(stream.fileno())
            check(os.pread(stream.fileno(), len(PAYLOAD), 0) == PAYLOAD, 'fsync changed file data')
            report['barrier_probes'] = {name: barrier_probe(stream.fileno(), command) for name, command in
                                        [('F_FULLFSYNC', F_FULLFSYNC), ('F_BARRIERFSYNC', F_BARRIERFSYNC)]}
        if kind == 'apfs':
            check(all(item['accepted'] for item in report['barrier_probes'].values()), 'APFS barrier reference rejected')
        os.link(data, directory/'hardlink'); os.symlink('data', directory/'symlink')
        os.fsync(directory_fd); flush(root)
        directory.rename(root/'renamed')
        os.fsync(directory_fd); flush(root)
        # Retaining the replaced fd must not retarget it to the new inode.
        atomic = root/'atomic'; atomic.write_bytes(b'previous inode'); flush(atomic)
        old_fd = os.open(atomic, os.O_RDWR)
        temporary = root/'temporary'; temporary.write_bytes(PAYLOAD); flush(temporary)
        os.replace(temporary, atomic); flush(root)
        check(os.pread(old_fd, 99, 0) == b'previous inode', 'atomic save retargeted old fd')
        check(os.pwrite(old_fd, b'OLD', 0) == 3, 'replaced inode write failed'); os.fsync(old_fd)
        check(atomic.read_bytes() == PAYLOAD and os.pread(old_fd, 99, 0) == b'OLDvious inode', 'orphan fsync changed replacement')
        os.close(old_fd); old_fd = -1
        verify(root)
        report.update(file_and_metadata_fsync=True, directory_fsync_create_link_rename=True,
                      renamed_directory_fd_fsync=True, atomic_save_and_replaced_fd_fsync=True)
        if kind == 'forkfs':
            for suffix in ['/renamed/data', '/atomic']:
                backend_data(report['namespace']+suffix)
            info = json.loads(rpc('fs-lstat', report['namespace']+'/renamed/data'))
            check(info['mode'] == 0o640 and info['modified_ns'] == MTIME, 'fsync metadata absent from daemon')
            before = json.loads(rpc('status'))['last_committed_seq']
            flush(root/'renamed'/'data'); os.fsync(directory_fd); flush(root)
            check(json.loads(rpc('status'))['last_committed_seq'] == before, 'clean fsync published a mutation')
            report['clean_sync_without_publication'] = True
            report['revision'] = 'sync_'+root.name[len('forkfs-sync-'):].replace('-', '_')
            rpc('fs-snapshot', report['revision'])
            for suffix in ['/renamed/data', '/atomic']:
                backend_data(report['namespace']+suffix, report['revision'])
            rpc('fs-check'); report['daemon_live_and_revision_data_verified'] = True
    finally:
        for fd in [old_fd, directory_fd]:
            if fd >= 0: os.close(fd)
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
