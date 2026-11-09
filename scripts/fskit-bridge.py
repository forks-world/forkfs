#!/usr/bin/env python3
"""Run an authenticated loopback bridge for a sandboxed FSKit module.

The daemon continues using its same-UID Unix socket. Keep this process alive
until after unmounting. Each start publishes a fresh endpoint and secret in the
private mount descriptor; an old mount never silently reconnects to a new bridge.
"""
import argparse
import concurrent.futures
import ctypes
import fcntl
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

LIMIT = 1024 * 1024


def receive(peer, size, deadline):
    result = bytearray()
    while len(result) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('bridge request deadline exceeded')
        peer.settimeout(remaining)
        data = peer.recv(size - len(result))
        if not data:
            raise ValueError('truncated bridge request')
        result.extend(data)
    return bytes(result)


def send(peer, data, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('bridge request deadline exceeded')
    peer.settimeout(remaining)
    peer.sendall(data)


def same_user(peer):
    if sys.platform == 'darwin':
        uid, gid = ctypes.c_uint(), ctypes.c_uint()
        libc = ctypes.CDLL(None, use_errno=True)
        return libc.getpeereid(peer.fileno(), ctypes.byref(uid), ctypes.byref(gid)) == 0 and uid.value == os.geteuid()
    pid, uid, gid = struct.unpack('3i', peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    return uid == os.geteuid()


def frame(peer, magic, deadline):
    header = receive(peer, 12, deadline)
    size, = struct.unpack('<I', header[8:])
    if header[:8] != magic or not 4 <= size <= LIMIT:
        raise ValueError('invalid bridge frame')
    return header + receive(peer, size, deadline)


def handle(client, backend_path, key):
    deadline = time.monotonic() + 10
    with client:
        try:
            hello = receive(client, 40, deadline)
            if hello[:8] != b'FFBR001!':
                return
            nonces = hello[8:] + secrets.token_bytes(32)
            proof = hmac.digest(key, b'FFBR001S' + nonces, hashlib.sha256)
            send(client, nonces[32:] + proof, deadline)
            expected = hmac.digest(key, b'FFBR001C' + nonces, hashlib.sha256)
            if not hmac.compare_digest(receive(client, 32, deadline), expected):
                return
            request = frame(client, b'FFRPC001', deadline)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as backend:
                backend.settimeout(max(0.001, deadline - time.monotonic()))
                backend.connect(str(backend_path))
                if not same_user(backend):
                    raise ValueError('daemon identity mismatch')
                send(backend, request, deadline)
                send(client, frame(backend, b'FFRSP001', deadline), deadline)
        except (OSError, ValueError):
            # Authentication failures and abandoned requests never reach dispatch.
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('daemon', type=Path)
    parser.add_argument('runtime', type=Path)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument('--world')
    selector.add_argument('--revision')
    args = parser.parse_args()
    runtime = args.runtime.absolute()
    info = runtime.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        parser.error('runtime must be a private owned directory')
    lock = os.open(runtime / '.forkfs-bridge.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    lock_info = os.fstat(lock)
    if not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != os.geteuid() or lock_info.st_mode & 0o077:
        parser.error('invalid bridge lock')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error('a bridge already owns this runtime')
    view = ['--revision', args.revision] if args.revision is not None else ['--world', args.world]
    subprocess.run([sys.executable, str(Path(__file__).with_name('fskit-config.py')), str(args.daemon), str(runtime), *view], check=True, stdout=subprocess.DEVNULL)
    descriptor = runtime / 'forkfs.json'
    with descriptor.open() as stream:
        config = json.load(stream)
    key = secrets.token_bytes(32)
    stopping = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: stopping.set())
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(16)
        listener.settimeout(0.2)
        config['rpc'] = {'transport': 'tcp-loopback', 'port': listener.getsockname()[1], 'key': key.hex()}
        fd, temporary = tempfile.mkstemp(prefix='.forkfs-bridge-', dir=runtime)
        try:
            with os.fdopen(fd, 'w') as stream:
                json.dump(config, stream)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, descriptor)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        print('FSKit bridge ready: ' + str(descriptor), flush=True)
        available = threading.BoundedSemaphore(16)
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            def worker(client):
                try:
                    handle(client, runtime / 'control.sock', key)
                finally:
                    available.release()
            while not stopping.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                if available.acquire(blocking=False):
                    pool.submit(worker, client)
                else:
                    client.close()
    os.close(lock)


if __name__ == '__main__':
    main()
