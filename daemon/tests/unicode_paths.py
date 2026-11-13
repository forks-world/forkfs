"""UTF-8 namespace identity, byte limits, invalid-input atomicity and revisions."""
import errno
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile

BIN = sys.argv[1]


def cli(*args, ok=True):
    result = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=15)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='ff-unicode-', dir='/tmp') as temporary:
    root = Path(temporary)
    db, data = root/'repository', root/'input'
    cli('init', db)
    cli('fs-init', db)
    payload = b'UTF-8 name, binary contents\x00\xff'
    data.write_bytes(payload)
    cli('fs-mkdir', db, '/目录')
    cli('fs-write', db, '/目录/文件📄', data)
    identity = json.loads(cli('fs-stat', db, '/目录/文件📄'))['inode_id']
    cli('fs-link', db, '/目录/文件📄', '/硬链接🔗')
    cli('fs-symlink', db, '目录/文件📄', '/相对链接')
    cli('fs-symlink', db, '/目录/文件📄', '/绝对链接')
    cli('fs-mkdir', db, '/子目录')
    cli('fs-symlink', db, '../目录//./文件📄', '/子目录/链接')
    for name in ['/相对链接', '/绝对链接', '/子目录/链接', '/硬链接🔗']:
        assert cli('fs-cat', db, name) == payload
    assert cli('fs-readlink', db, '/子目录/链接') == '../目录//./文件📄'.encode()

    # Canonically equivalent spellings are separate byte identities, not aliases.
    spellings = ['é', 'e\u0301', '名字👩\u200d💻', '"反斜线\\📄', '界'*85, '😀'*63+'abc', '\U0010ffff']
    for index, name in enumerate(spellings):
        assert len(name.encode()) <= 255
        data.write_bytes(str(index).encode())
        cli('fs-write', db, '/'+name, data)
    assert json.loads(cli('fs-stat', db, '/é'))['inode_id'] != json.loads(cli('fs-stat', db, '/e\u0301'))['inode_id']
    names = {line.decode() for line in cli('fs-ls', db, '/').split(b'\n') if line}
    assert set(spellings) <= names
    cli('fs-snapshot', db, 'unicode_base')
    cli('fs-fork', db, 'unicode_base', 'unicode_work')
    cli('fs-rename', db, '/目录/文件📄', '/目录/已重命名🚀')
    assert json.loads(cli('fs-stat', db, '/目录/已重命名🚀'))['inode_id'] == identity
    assert cli('fs-cat', db, '/目录/文件📄', '--revision', 'unicode_base') == payload
    assert cli('fs-cat', db, '/目录/文件📄', '--world', 'unicode_work') == payload
    assert cli('fs-readlink', db, '/相对链接', '--revision', 'unicode_base') == '目录/文件📄'.encode()
    before = json.loads(cli('check', db))['last_committed_seq']
    cli('fs-snapshot', db, '非文件名称', ok=False)
    assert json.loads(cli('check', db))['last_committed_seq'] == before

    endpoint = root/'control.sock'
    server = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def exact(peer, size):
        value = b''
        while len(value) < size:
            part = peer.recv(size-len(value))
            assert part, 'truncated RPC reply'
            value += part
        return value

    def rpc(*fields, error=None):
        fields = [field if isinstance(field, bytes) else str(field).encode() for field in fields]
        body = struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(field))+field for field in fields)
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(15)
            peer.connect(str(endpoint))
            peer.sendall(b'FFRPC001'+struct.pack('<I', len(body))+body)
            header = exact(peer, 12)
            assert header[:8] == b'FFRSP001'
            value = exact(peer, struct.unpack('<I', header[8:])[0])
        code, = struct.unpack('<I', value[:4])
        assert code == (0 if error is None else error+1), (fields, code, value[4:], error)
        return value[4:]

    def obj(*fields, **kwargs):
        return json.loads(rpc(*fields, **kwargs))

    try:
        assert select.select([server.stdout], [], [], 10)[0], 'daemon not ready'
        assert json.loads(server.stdout.readline())['state'] == 'leveldb'
        session = obj('session-open', '60000')['session']
        selector = ['--session', session]
        inode = obj('inode-root', *selector)['inode']['inode_id']
        assert obj('inode-lookup', inode, 'é', *selector)['inode_id'] != obj('inode-lookup', inode, 'e\u0301', *selector)['inode_id']
        cursor = obj('inode-dir-open', inode, *selector)['cursor']
        entries = obj('fs-dir-entries', cursor, '100', *selector)['entries']
        assert set(spellings) <= {entry['name'] for entry in entries}
        rpc('fs-dir-close', cursor, *selector)
        handle = obj('inode-create', inode, 'RPC文件📄', str(0o600), *selector)['handle']
        rpc('fs-hwrite', handle, '0', b'\xff\x00binary', *selector)
        assert rpc('fs-hread', handle, '0', '100', *selector) == b'\xff\x00binary'
        rpc('fs-close', handle, *selector)
        file_inode = obj('inode-lookup', inode, 'RPC文件📄', *selector)['inode_id']

        # All are invalid UTF-8, excluded controls, or overlong byte components.
        invalid = [b'\x80', b'\xbf', b'\xc0\xaf', b'\xc0\xae', b'\xc1\xbf', b'\xe0\x80\xaf',
                   b'\xf0\x80\x80\xaf', b'\xed\xa0\x80', b'\xed\xbf\xbf', b'\xf4\x90\x80\x80',
                   b'\xf5\x80\x80\x80', b'\xff', b'\xc2', b'\xe2\x82', b'\xf0\x9f\x92',
                   b'\xe2\x28\xa1', b'\x00', b'x\ny', b'x\ty', b'\x7f', b'\xc2\x80', b'\xc2\x9f',
                   ('界'*85+'a').encode(), ('😀'*64).encode()]
        sequence = obj('status')['last_committed_seq']
        for name in invalid:
            error = errno.ENAMETOOLONG if len(name)>255 else errno.EINVAL
            rpc('inode-mkdir', inode, name, str(0o700), *selector, error=error)
            rpc('inode-symlink', inode, '拒绝创建', name, *selector, error=error)
            rpc('inode-link', file_inode, inode, name, *selector, error=error)
            rpc('inode-rename', inode, 'RPC文件📄', inode, name, file_inode, '-', *selector, error=error)
        for name in ['a'*256, 'é'*128]:
            rpc('inode-lookup', inode, name, *selector, error=errno.ENAMETOOLONG)
            rpc('inode-create', inode, name, str(0o600), *selector, error=errno.ENAMETOOLONG)
            rpc('fs-stat', '/'+name, error=errno.ENAMETOOLONG)
        # Components individually fit; the full absolute path exceeds 4096.
        long_path = '/'+'/'.join(['a'*255]*16)+'/x'
        assert len(long_path.encode()) > 4096
        rpc('fs-stat', long_path, error=errno.ENAMETOOLONG)
        rpc('fs-mkdir', long_path, *selector, error=errno.ENAMETOOLONG)
        rpc('inode-symlink', inode, '拒绝创建', ('界'*1366).encode(), *selector, error=errno.ENAMETOOLONG)
        assert obj('status')['last_committed_seq'] == sequence
        assert obj('inode-lookup', inode, 'RPC文件📄', *selector)['inode_id'] == file_inode
        rpc('fs-check')
        rpc('session-close', session)
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)
        assert server.returncode == 0, server.communicate()
    for arguments in [[], ['--revision', 'unicode_base'], ['--world', 'unicode_work']]:
        cli('fs-check', db, *arguments)
    cli('gc', db)
    assert cli('fs-cat', db, '/目录/已重命名🚀') == payload
    assert cli('fs-cat', db, '/目录/文件📄', '--revision', 'unicode_base') == payload
    for index, name in enumerate(spellings):
        assert cli('fs-cat', db, '/'+name) == str(index).encode()
    cli('fs-check', db)
print('UTF-8 byte identity, links, revisions, restart/GC and invalid-input atomicity passed')
