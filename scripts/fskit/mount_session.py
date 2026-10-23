"""Administrator delivery of a daemon mount session into a frontend-only resource.

Run trusted installed code as the configured control UID (root for this CLI).
The resource directory is the daemon's service-owned 0711 socket directory;
backing data must be elsewhere. Tokens are never printed or put in argv.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import socket
import stat
import struct
import sys
import time

MARKER = '.forkfs-fskit-session'
LIMIT = 1024 * 1024


def request(endpoint, service_uid, fields):
    payload = struct.pack('<I', len(fields))
    for field in fields:
        value = field.encode('utf-8')
        payload += struct.pack('<I', len(value)) + value
    if len(payload) > LIMIT:raise ValueError('RPC request exceeds frame')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        deadline = time.monotonic() + 10
        peer.settimeout(10)
        peer.connect(str(endpoint))
        uid, gid = ctypes.c_uint(), ctypes.c_uint()
        libc = ctypes.CDLL(None, use_errno=True)
        libc.getpeereid.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        if libc.getpeereid(peer.fileno(), ctypes.byref(uid), ctypes.byref(gid)) or uid.value != service_uid:
            raise ValueError('service peer UID mismatch')
        # Authentication precedes any token or management request transmission.
        peer.settimeout(max(0.001, deadline-time.monotonic()))
        peer.sendall(b'FFRPC001' + struct.pack('<I', len(payload)) + payload)
        def receive(count):
            out = bytearray()
            while len(out) < count:
                remaining = deadline-time.monotonic()
                if remaining <= 0:raise TimeoutError('mount RPC deadline exceeded')
                peer.settimeout(remaining)
                chunk = peer.recv(count-len(out))
                if not chunk:raise ValueError('truncated RPC response')
                out.extend(chunk)
            return bytes(out)
        header = receive(12)
        if header[:8] != b'FFRSP001':raise ValueError('invalid RPC response')
        size, = struct.unpack('<I', header[8:])
        if not 4 <= size <= LIMIT:raise ValueError('invalid RPC response size')
        body = receive(size)
        code, = struct.unpack('<I', body[:4])
        if code:raise RuntimeError('daemon rejected mount lifecycle request')
        return json.loads(body[4:])


def token(value):
    return isinstance(value, str) and len(value) == 32 and all(c in '0123456789abcdef' for c in value)


def provision(directory, service_uid, frontend_uid, frontend_gid, name, revision=False, lease_ms=300000):
    if sys.platform != 'darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('mount provisioning requires root on macOS')
    if (type(service_uid) is not int or type(frontend_uid) is not int or
        service_uid <= 0 or frontend_uid <= 0 or service_uid == frontend_uid or frontend_gid <= 0):
        raise ValueError('separate nonroot service/frontend identities required')
    if not 3000 <= lease_ms <= 300000:raise ValueError('lease must be 3000..300000 ms')
    if not name or len(name) > 64 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in name):
        raise ValueError('invalid view name')
    directory = Path(directory)
    if not directory.is_absolute() or directory.resolve() != directory:raise ValueError('resource must be canonical absolute path')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.acl_get_link_np.restype = ctypes.c_void_p
    libc.acl_free.argtypes = [ctypes.c_void_p]
    for path in [directory, *directory.parents]:
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid not in (0, service_uid) or info.st_mode & 0o7022:
            raise ValueError('unsafe resource ancestor')
        ctypes.set_errno(0)
        acl = libc.acl_get_link_np(os.fsencode(path), 0x100)
        if acl:
            libc.acl_free(acl);raise ValueError('extended resource ACL is not allowed')
        if ctypes.get_errno() != errno.ENOENT:raise RuntimeError('cannot inspect resource ACL')
    info = directory.lstat()
    if info.st_uid != service_uid or stat.S_IMODE(info.st_mode) != 0o711:
        raise ValueError('resource must be service-owned 0711 socket directory')
    endpoint = directory/'rpc.sock'
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    created = False
    session = generation = None
    try:
        # Reserve a name first; concurrent provisioners cannot overwrite a token.
        for marker in ('.forkfs-fskit-world', '.forkfs-fskit-revision'):
            try:os.stat(marker, dir_fd=descriptor, follow_symlinks=False)
            except FileNotFoundError:continue
            raise ValueError('conflicting local resource marker')
        fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                     0o600, dir_fd=descriptor)
        created = True
        try:
            hello = request(endpoint, service_uid, ['service-hello'])
            generation = hello.get('generation')
            if hello.get('protocol') != 1 or not token(generation):raise ValueError('unsupported service hello')
            result = request(endpoint, service_uid, ['mount-open', 'revision' if revision else 'world', name,
                str(lease_ms), '--generation', generation])
            session = result.get('session')
            if not token(session) or result.get('generation') != generation:raise ValueError('invalid mount session response')
            value = dict(version=1, socket=str(endpoint), service_uid=service_uid, frontend_uid=frontend_uid,
                         generation=generation, session=session, name=name, read_only=bool(revision), lease_ms=lease_ms)
            with os.fdopen(fd, 'wb', closefd=False) as stream:
                stream.write(json.dumps(value).encode());stream.flush();os.fsync(fd)
            os.fchown(fd, frontend_uid, frontend_gid)
            os.fchmod(fd, 0o400)
            os.fsync(fd);os.fsync(descriptor)
        finally:os.close(fd)
    except BaseException:
        if token(session) and token(generation):
            try:request(endpoint, service_uid, ['mount-revoke', session, '--generation', generation])
            except Exception:pass  # server lease expires; do not hide original error
        if created:os.unlink(MARKER, dir_fd=descriptor)
        raise
    finally:os.close(descriptor)
    return dict(resource=str(directory), frontend_uid=frontend_uid, lease_ms=lease_ms)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resource', type=Path, required=True)
    parser.add_argument('--service-uid', type=int, required=True)
    parser.add_argument('--frontend-uid', type=int, required=True)
    parser.add_argument('--frontend-gid', type=int, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--revision', action='store_true')
    parser.add_argument('--lease-ms', type=int, default=300000)
    args = parser.parse_args()
    try:
        print(json.dumps(provision(args.resource, args.service_uid, args.frontend_uid,
            args.frontend_gid, args.name, args.revision, args.lease_ms)))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps(dict(status='blocked', error=str(error))))
        return 2


if __name__ == '__main__':raise SystemExit(main())
