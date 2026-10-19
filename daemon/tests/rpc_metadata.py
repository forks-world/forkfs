"""Black-box metadata RPC: atomic patches, binary xattrs and retained views."""
import errno
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]
MISSING = getattr(errno, 'ENOATTR', errno.ENODATA)


def exact(sock, size):
    result = b''
    while len(result) < size:
        part = sock.recv(size - len(result))
        assert part, 'truncated response'
        result += part
    return result


with tempfile.TemporaryDirectory(prefix='ffmeta-', dir='/tmp') as tmp:
    root = Path(tmp)
    endpoint, db, data = root/'rpc.sock', root/'db', root/'input'
    subprocess.run([BIN, 'init', str(db)], check=True, capture_output=True, timeout=10)

    def request(*fields, error=0, session=None, world=None, revision=None):
        if world is not None:
            fields += ('--world', world)
        if revision is not None:
            fields += ('--revision', revision)
        if session is not None:
            fields += ('--session', session)
        fields = [f.encode() if isinstance(f, str) else f for f in fields]
        payload = struct.pack('<I', len(fields)) + b''.join(
            struct.pack('<I', len(f)) + f for f in fields)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10)
            sock.connect(str(endpoint))
            sock.sendall(b'FFRPC001' + struct.pack('<I', len(payload)) + payload)
            header = exact(sock, 12)
            assert header[:8] == b'FFRSP001'
            size, = struct.unpack('<I', header[8:])
            assert 4 <= size <= 1024*1024
            reply = exact(sock, size)
            status, = struct.unpack('<I', reply[:4])
            value = reply[4:]
            assert sock.recv(1) == b''
        if error is None:
            assert status != 0, (fields[:2], value)
        else:
            expected = 0 if error == 0 else 0x80000000 | error
            assert status == expected, (fields[:2], status, expected, value)
        return value

    def obj(*args, **kwargs):
        return json.loads(request(*args, **kwargs))

    def cli(*args, ok=True):
        p = subprocess.run([BIN, 'request', str(endpoint), *args],
                           capture_output=True, timeout=10)
        assert (p.returncode == 0) == ok, (args, p.stderr)
        return p.stdout

    def start():
        proc = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while True:
                assert proc.poll() is None, proc.communicate()
                try:
                    request('status')
                    return proc
                except (FileNotFoundError, ConnectionRefusedError):
                    assert time.monotonic() < deadline, 'daemon startup timeout'
                    time.sleep(.01)
        except BaseException:
            proc.kill()
            proc.wait(timeout=5)
            raise

    proc = start()
    try:
        request('fs-init')
        request('fs-write', '/large', b'head')
        far = (1 << 40) + 65535
        request('fs-write-at', '/large', str(far), b'XYZ')
        assert request('fs-read-at', '/large', str(far-1), '5') == b'\0XYZ'
        assert obj('fs-stat', '/large')['size'] == far+3
        request('fs-truncate', '/large', '65537')
        request('fs-truncate', '/large', str(far+3))
        assert request('fs-read-at', '/large', str(far), '3') == bytes(3)
        request('fs-unlink', '/large')
        request('fs-write', '/stream', b'A' * 600000)
        request('fs-append', '/stream', b'B' * 600000)
        stream_session = obj('session-open', '60000')['session']
        stream = obj('fs-open', '/stream', 'r', session=stream_session)['handle']
        first = request('fs-hread-next', stream, str(2**64-1), session=stream_session)
        assert len(first) == 1024*1024-4
        assert obj('fs-hseek', stream, '0', 'cur', session=stream_session)['offset'] == len(first)
        second = request('fs-hread-next', stream, str(2**64-1), session=stream_session)
        assert first + second == b'A' * 600000 + b'B' * 600000
        assert request('fs-hread-next', stream, '1', session=stream_session) == b''
        assert request('fs-read-at', '/stream', '0', str(2**64-1)) == first
        request('session-close', stream_session)
        request('fs-unlink', '/stream')
        request('fs-write', '/file', b'abc')
        request('fs-link', '/file', '/alias')
        inode = obj('fs-stat', '/file')['inode_id']
        session = obj('session-open', '60000')['session']
        foreign = obj('session-open', '60000')['session']
        handle = obj('fs-open', '/file', 'rw', session=session)['handle']
        reader = obj('fs-open', '/file', 'r', session=session)['handle']
        patch = 'mode=600,size=6,uid=501,gid=20,modified_ns=33,accessed_ns=22,created_ns=11'
        request('fs-isetattr', inode, patch)
        attrs = dict(mode=0o600, size=6, uid=501, gid=20,
                     modified_ns=33, accessed_ns=22, created_ns=11)
        for op, address, selectors in [('fs-istat', inode, {}), ('fs-stat', '/alias', {}),
                                      ('fs-hstat', reader, {'session': session})]:
            result = obj(op, address, **selectors)
            assert all(result[k] == v for k, v in attrs.items()), result
        assert request('fs-cat', '/file') == b'abc\0\0\0'
        binary = bytes(range(256)) * 256  # Exactly 64 KiB, including NUL.
        data.write_bytes(binary)
        cli('fs-isetxattr', inode, 'user.binary', 'create', str(data))
        request('fs-isetxattr', inode, 'user.empty', 'upsert', b'')
        assert cli('fs-igetxattr', inode, 'user.binary') == binary
        assert request('fs-igetxattr', inode, 'user.empty') == b''
        assert request('fs-ilistxattr', inode) == b'user.binary\nuser.empty\n'
        request('fs-snapshot', 'base')
        request('fs-fork', 'base', 'work')
        before = obj('status')
        before_inode = obj('fs-istat', inode)
        for bad in ['', 'size=2,', 'size=2,size=3', 'uid=4294967296', 'gid=-1',
                    'uid=+1', 'mode=1000', 'mode=888', 'size=2,unknown=3', 'size=2,uid=',
                    'size=18446744073709551616', 'accessed_ns=9223372036854775808',
                    'created_ns=9223372036854775808', 'modified_ns=-1', 'uid=1=2']:
            request('fs-isetattr', inode, bad, error=errno.EINVAL)
        request('fs-isetattr', inode, 'uid=7,size=18446744073709551615', error=errno.EFBIG)
        request('fs-hsetattr', reader, 'uid=7,size=2', session=session, error=errno.EBADF)
        request('fs-isetattr', inode, 'size=2', revision='base', error=errno.EROFS)
        request('fs-istat', 'bad-id', error=errno.EINVAL)
        for policy, name, value, error in [
            ('create', 'user.binary', b'x', errno.EEXIST),
            ('replace', 'user.absent', b'x', MISSING),
            ('invalid', 'user.binary', b'x', errno.EINVAL),
            ('upsert', '', b'x', errno.EINVAL),
            ('upsert', 'user.large', binary+b'x', errno.E2BIG),
            ('upsert', 'user.large', binary, errno.E2BIG),  # Total inode size budget.
        ]:
            request('fs-isetxattr', inode, name, policy, value, error=error)
        request('fs-igetxattr', inode, 'user.absent', error=MISSING)
        request('fs-iremovexattr', inode, 'user.absent', error=MISSING)
        request('fs-isetxattr', inode, 'user.empty', 'replace', b'x', revision='base', error=errno.EROFS)
        request('fs-iremovexattr', inode, 'user.empty', revision='base', error=errno.EROFS)
        for op, tail in [('fs-hsetattr', ('uid=7',)), ('fs-hlistxattr', ()),
                         ('fs-hgetxattr', ('user.binary',)),
                         ('fs-hsetxattr', ('user.binary', 'replace', b'x')),
                         ('fs-hremovexattr', ('user.binary',))]:
            request(op, handle, *tail, session=foreign, error=None)
            request(op, handle, *tail, session=session, world='work', error=None)
            request(op, handle, *tail, error=None)
        assert obj('status') == before
        assert obj('fs-istat', inode) == before_inode
        assert request('fs-igetxattr', inode, 'user.binary') == binary
        # Nine fields: binary value plus both view and session selectors.
        wh = obj('fs-open', '/file', 'rw', world='work', session=session)['handle']
        data.write_bytes(b'work\0xattr')
        cli('fs-hsetxattr', wh, 'user.binary', 'replace', str(data),
            '--world', 'work', '--session', session)
        request('fs-hsetattr', wh, 'uid=9,size=2', world='work', session=session)
        assert request('fs-hgetxattr', wh, 'user.binary', world='work', session=session) == b'work\0xattr'
        assert request('fs-igetxattr', inode, 'user.binary', revision='base') == binary
        rh = obj('fs-open', '/file', 'r', revision='base', session=session)['handle']
        request('fs-hsetattr', rh, 'uid=7', revision='base', session=session, error=errno.EROFS)
        request('fs-hsetxattr', rh, 'user.binary', 'replace', b'x', revision='base', session=session, error=errno.EROFS)
        request('fs-hremovexattr', rh, 'user.binary', revision='base', session=session, error=errno.EROFS)
        assert request('fs-hgetxattr', rh, 'user.binary', revision='base', session=session) == binary
        request('fs-setattr', '/alias', 'uid=0,gid=0,size=4')
        assert obj('fs-istat', inode)['uid'] == 0
        # Direct inode addressing never follows the symlink target.
        request('fs-symlink', '/file', '/link')
        link = obj('fs-lstat', '/link')['inode_id']
        request('fs-isetxattr', link, 'user.link', 'create', b'link')
        assert request('fs-ilistxattr', inode) == b'user.binary\nuser.empty\n'
        request('fs-unlink', '/file')
        request('fs-unlink', '/alias')
        request('fs-istat', inode, error=errno.ENOENT)
        request('fs-igetxattr', inode, 'user.binary', error=errno.ENOENT)
        assert request('fs-hgetxattr', handle, 'user.binary', session=session) == binary
        request('fs-hsetattr', handle, 'uid=42,size=7', session=session)
        request('fs-hsetxattr', handle, 'user.binary', 'replace', b'orphan\0', session=session)
        assert obj('fs-hstat', reader, session=session)['uid'] == 42
        assert request('fs-hgetxattr', reader, 'user.binary', session=session) == b'orphan\0'
        request('fs-hremovexattr', handle, 'user.empty', session=session)
        assert request('fs-hlistxattr', reader, session=session) == b'user.binary\n'
        request('session-close', session)
        request('fs-hlistxattr', handle, session=session, error=None)
        request('fs-check')
        proc.kill()
        proc.wait(timeout=5)
        endpoint.unlink()
        proc = start()
        assert request('fs-igetxattr', inode, 'user.binary', world='work') == b'work\0xattr'
        assert obj('fs-istat', inode, world='work')['size'] == 2
        assert request('fs-igetxattr', inode, 'user.binary', revision='base') == binary
        persisted = obj('fs-istat', inode, revision='base')
        assert all(persisted[k] == v for k, v in attrs.items())
        assert request('fs-igetxattr', link, 'user.link') == b'link'
        for selectors in [{}, {'world': 'work'}, {'revision': 'base'}]:
            request('fs-check', **selectors)
        proc.terminate()
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.communicate()
        print('metadata RPC: atomic patches, errno, binary xattrs, isolation and restart passed')
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
