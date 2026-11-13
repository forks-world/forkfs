#!/usr/bin/env python3
"""Qualify Darwin copyfile/fcopyfile/sendfile on one owned scratch tree.

Clone attempts are separate capability observations. No ResourceFork, ACL,
recursive-copy, sparse allocation or zero-copy-performance claim is made.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from filesystem_compat import check, filesystem
from file_flags_mount import change, reject
from large_xattr_mount import put, get

DATA, STAT, XATTR, EXCL, CLONE, CLONE_FORCE = 8, 2, 4, 1 << 17, 1 << 24, 1 << 25
PAYLOAD = bytes(range(251)) * 2000
ATTRIBUTE = bytes(range(256)) * 80
ATTR_NAME = 'user.copy-transfer'
MTIME = 1234567890123456789
libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
libc.copyfile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32]
libc.fcopyfile.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
libc.copyfile.restype = libc.fcopyfile.restype = ctypes.c_int
libc.copyfile_state_alloc.restype = ctypes.c_void_p
libc.copyfile_state_free.argtypes = [ctypes.c_void_p]
libc.copyfile_state_get.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
libc.copyfile_state_get.restype = ctypes.c_int
libc.sendfile.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int64, ctypes.POINTER(ctypes.c_int64), ctypes.c_void_p, ctypes.c_int]
libc.sendfile.restype = ctypes.c_int


def invoke(name, *args):
    if getattr(libc, name)(*args) < 0:
        code = ctypes.get_errno(); raise OSError(code, os.strerror(code))


def copy(source, target, flags, state=None):
    invoke('copyfile', os.fsencode(source), os.fsencode(target), state, flags)


def send(fd, offset, count):
    """Use the Darwin syscall directly, a bounded loopback receiver and deadlines."""
    listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(1)
    received = bytearray(); errors = []
    expected = max(0, min(count or len(PAYLOAD), len(PAYLOAD)-offset))
    def receive():
        try:
            listener.settimeout(10)
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                while True:
                    data = connection.recv(65536)
                    if not data: break
                    received.extend(data); check(len(received) <= expected, 'sendfile overran request')
        except Exception as error: errors.append(error)
    worker = threading.Thread(target=receive, daemon=True); worker.start()
    stream = socket.socket(); stream.bind(('127.0.0.1', 0)); stream.settimeout(10)
    total = calls = 0
    try:
        stream.connect(listener.getsockname()); stream.setblocking(False)
        deadline = time.monotonic()+15
        while True:
            check(time.monotonic() < deadline, 'sendfile deadline expired')
            amount = ctypes.c_int64(count if calls == 0 else expected-total)
            result = libc.sendfile(fd, stream.fileno(), offset+total, ctypes.byref(amount), None, 0)
            code = ctypes.get_errno(); calls += 1
            check(0 <= amount.value <= expected-total, 'invalid sendfile byte count')
            total += amount.value
            if result == 0: break
            if code not in (errno.EAGAIN, errno.EINTR): raise OSError(code, os.strerror(code))
            if total == expected: break
            time.sleep(.005)
        stream.shutdown(socket.SHUT_WR)
        worker.join(timeout=11); check(not worker.is_alive(), 'sendfile receiver did not finish')
        if errors: raise errors[0]
        check(total == expected and bytes(received) == PAYLOAD[offset:offset+expected], 'sendfile bytes differ')
        return {'offset': offset, 'requested': count, 'sent': total, 'calls': calls}
    finally:
        stream.close(); listener.close(); worker.join(timeout=11)


def sync(path):
    fd = os.open(path, os.O_RDWR)
    try: os.fsync(fd)
    finally: os.close(fd)


def metadata(path):
    st = path.stat()
    return st.st_ino, st.st_mode & 0o777, st.st_mtime_ns, st.st_flags


def verify_copies(root):
    for name in ['copy', 'fdcopy', 'fallback']:
        check((root/name).read_bytes() == PAYLOAD, name+' data changed')
    check(metadata(root/'copy')[1:] == (0o740, MTIME, 0x8001), 'copied stat metadata differs')
    check(get(root/'copy', ATTR_NAME) == ATTRIBUTE, 'copied binary xattr differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args(); target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if kind == 'forkfs': check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    if args.verify_report:
        check(kind == 'forkfs', 'revision verification requires forkfs')
        report = json.loads(args.verify_report.read_text()); ns = report['namespace']
        check(ns.startswith('/forkfs-copy-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        verify_copies(target/ns.lstrip('/'))
        reject(lambda: (target/ns.lstrip('/')/'new').touch(), errno.EROFS)
        print(json.dumps({'copies_after_gc_readonly_remount': True, 'metadata_and_xattr_preserved': True})); return
    if kind == 'forkfs': check(args.daemon and args.socket, 'requires daemon/socket')
    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)), *map(str, fields), *view], check=True, capture_output=True, timeout=30).stdout
    root = Path(tempfile.mkdtemp(prefix='forkfs-copy-', dir=target)); source_fd = destination_fd = -1
    report = {'filesystem': kind, 'namespace': '/'+root.name, 'bytes': len(PAYLOAD), 'sha256': hashlib.sha256(PAYLOAD).hexdigest()}
    try:
        source = root/'source'; source.write_bytes(PAYLOAD); os.chmod(source, 0o740)
        put(source, ATTR_NAME, ATTRIBUTE); os.utime(source, ns=(MTIME, MTIME)); change(source, 0x8001)
        source_fd = os.open(source, os.O_RDONLY)
        copy(source, root/'copy', DATA|STAT|XATTR)
        check((root/'copy').read_bytes() == PAYLOAD and metadata(root/'copy')[1:] == metadata(source)[1:] and get(root/'copy', ATTR_NAME) == ATTRIBUTE, 'copyfile data/stat/xattr differs')
        exclusive = root/'exclusive'; exclusive.write_bytes(b'sentinel')
        stable = metadata(exclusive); before_ctime = exclusive.stat().st_ctime_ns
        report['exclusive_errno'] = reject(lambda: copy(source, exclusive, DATA|EXCL), errno.EEXIST)
        check(metadata(exclusive) == stable and exclusive.read_bytes() == b'sentinel', 'exclusive failure changed data/identity/mode/mtime/flags')
        # libcopyfile may restore flags even on EEXIST, updating ctime.
        # Record this separately; the compound library call is not a single
        # atomic namespace mutation.
        report['exclusive_ctime_changed'] = exclusive.stat().st_ctime_ns != before_ctime
        destination_fd = os.open(root/'fdcopy', os.O_CREAT|os.O_EXCL|os.O_RDWR, 0o600)
        invoke('fcopyfile', source_fd, destination_fd, None, DATA); os.fsync(destination_fd)
        check(os.pread(destination_fd, len(PAYLOAD), 0) == PAYLOAD, 'fcopyfile data differs')
        os.close(destination_fd); destination_fd = -1
        state = libc.copyfile_state_alloc(); check(state, 'copy state allocation failed')
        try:
            copy(source, root/'fallback', DATA|CLONE, state)
            cloned = ctypes.c_bool(); invoke('copyfile_state_get', state, 10, ctypes.byref(cloned))
            report['clone_requested_copy'] = {'was_cloned': cloned.value, 'data_matches': (root/'fallback').read_bytes() == PAYLOAD}
        finally: libc.copyfile_state_free(state)
        check(report['clone_requested_copy']['data_matches'], 'clone-requested copy lost data')
        with (root/'fallback').open('r+b') as out: out.write(b'Z')
        check(source.read_bytes() == PAYLOAD, 'copy mutation changed source')
        (root/'fallback').write_bytes(PAYLOAD)
        try:
            copy(source, root/'forced', DATA|CLONE_FORCE)
            check((root/'forced').read_bytes() == PAYLOAD, 'forced clone data differs')
            report['forced_clone'] = {'supported': True}
        except OSError as error:
            check(error.errno == errno.ENOTSUP, 'unexpected forced-clone failure')
            report['forced_clone'] = {'supported': False, 'errno': error.errno, 'destination_exists_after_failure': (root/'forced').exists()}
        if kind == 'apfs': check(report['forced_clone']['supported'] and cloned.value, 'APFS clone reference failed')
        # Both old descriptors survive unlink and same-path replacement.
        source.unlink(); source.write_bytes(b'new source')
        destination = root/'orphan-destination'; destination_fd = os.open(destination, os.O_CREAT|os.O_EXCL|os.O_RDWR, 0o600)
        destination.unlink(); destination.write_bytes(b'new target'); os.lseek(source_fd, 0, os.SEEK_SET)
        invoke('fcopyfile', source_fd, destination_fd, None, DATA); os.fsync(destination_fd)
        check(os.pread(destination_fd, len(PAYLOAD), 0) == PAYLOAD and destination.read_bytes() == b'new target' and source.read_bytes() == b'new source', 'unlinked fcopyfile lost inode identity')
        os.close(destination_fd); destination_fd = -1
        os.lseek(source_fd, 5, os.SEEK_SET)
        report['sendfile'] = [send(source_fd, 17, 300001), send(source_fd, 0, 0), send(source_fd, len(PAYLOAD)+9, 0)]
        check(os.lseek(source_fd, 0, os.SEEK_CUR) == 5, 'sendfile changed source fd position')
        report['unlinked_fd_copy_and_sendfile'] = True
        for name in ['copy', 'fdcopy', 'fallback']: sync(root/name)
        verify_copies(root)
        if kind == 'forkfs':
            report['revision'] = 'copy_accept_'+root.name[len('forkfs-copy-'):].replace('-', '_')
            rpc('fs-snapshot', report['revision'])
            for name in ['copy', 'fdcopy', 'fallback']:
                data = b''
                for offset in range(0, len(PAYLOAD), 256*1024):
                    data += rpc('fs-read-at', '/'+root.name+'/'+name, offset, min(256*1024, len(PAYLOAD)-offset), revision=report['revision'])
                check(data == PAYLOAD, 'copied data absent from daemon revision')
            rpc('fs-check'); report['daemon_revision_data_matches'] = True
        report['copyfile_data_stat_xattr'] = True; report['fcopyfile_data'] = True
    finally:
        active_error = sys.exc_info()[0] is not None; errors = []
        for fd in [destination_fd, source_fd]:
            if fd >= 0:
                try: os.close(fd)
                except OSError as error: errors.append(error)
        try: shutil.rmtree(root)
        except OSError as error: errors.append(error)
        if errors:
            if not active_error: raise errors[0]
            print('copy-transfer cleanup failed: '+str(errors), file=sys.stderr)
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
