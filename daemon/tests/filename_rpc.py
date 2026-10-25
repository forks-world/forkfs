"""Byte filename behavior through the real Unix RPC server."""
import json
import errno
import random
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]

def packet(fields):
    fields = [x if isinstance(x, bytes) else str(x).encode() for x in fields]
    body = struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(x)) + x for x in fields)
    return b'FFRPC001' + struct.pack('<I', len(body)) + body

def exact(sock, size):
    data = b''
    while len(data) < size:
        part = sock.recv(size - len(data))
        assert part
        data += part
    return data

with tempfile.TemporaryDirectory(prefix='ff-filename-', dir='/tmp') as tmp:
    root = Path(tmp); db = root / 'store'; endpoint = root / 'control.sock'
    p = subprocess.run([BIN, 'init', str(db)], capture_output=True, timeout=10)
    assert p.returncode == 0, p.stderr
    proc = subprocess.Popen([BIN, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def rpc(*args, ok=True, code=None):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(20); sock.connect(str(endpoint)); sock.sendall(packet(args))
            header = exact(sock, 12); assert header[:8] == b'FFRSP001'
            body = exact(sock, struct.unpack('<I', header[8:])[0])
        got = struct.unpack('<I', body[:4])[0]
        assert (got == 0) == ok, (args[:2], got, body[4:])
        if code is not None: assert got == code, (got, body[4:])
        return body[4:]

    def open_dir(path, session):
        return json.loads(rpc('fs-dir-open', path, '--session', session))['cursor']

    try:
        deadline = time.monotonic() + 5
        while True:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                raise AssertionError(f'RPC daemon exited before readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                try:
                    hello = json.loads(rpc('service-hello'))
                except OSError as error:
                    if error.errno not in (errno.ENOENT, errno.ECONNREFUSED):
                        raise
                else:
                    assert 'protocol' in hello and 'generation' in hello, hello
                    if proc.poll() is not None:
                        stdout, stderr = proc.communicate()
                        raise AssertionError(f'RPC daemon exited after binding: {stdout!r} {stderr!r}')
                    break
            assert time.monotonic() < deadline, 'RPC daemon readiness timed out'
            time.sleep(.01)
        rpc('fs-init')
        session = json.loads(rpc('session-open', 300000))['session']
        rpc('fs-mkdir', '/names')

        # Valid UTF-8 spellings are preserved as byte-distinct POSIX names.
        good = ['中文', 'emoji-😀', 'é', 'e\u0301', 'Case', 'case', 'ctrl-\x01\x1f',
                'quote"and\\slash']
        for name in good:
            rpc('fs-write', ('/names/' + name).encode(), b'')
        cursor = open_dir('/names', session)
        got = json.loads(rpc('fs-dir-page', cursor, 1024, '--session', session))['names']
        assert got == sorted(good), (got, sorted(good))
        assert '中文' in got and 'emoji-😀' in got and 'é' in got and 'e\u0301' in got
        assert 'Case' in got and 'case' in got
        assert json.loads(rpc('fs-dir-page', cursor, 1024, '--session', session))['names'] == []

        # Exercise every name independently: valid UTF-8 must round-trip as
        # JSON text; invalid sequences must fail without moving the cursor,
        # after which raw RPC must return the exact bytes.
        rpc('fs-mkdir', '/utf8-cases')
        invalid_names = [b'bad-\xff-name', b'\xc0\xaf', b'\xe0\x80\xaf',
                         b'\xed\xa0\x80', b'\xf4\x90\x80\x80',
                         b'\xe2\x82', b'\x80']
        boundary_names = [chr(cp).encode('utf-8') for cp in
                          (0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0x10ffff)]
        bad_forms = [b'\xc0\xaf', b'\xe0\x80\xaf', b'\xed\xa0\x80',
                     b'\xf4\x90\x80\x80', b'\xe2\x82', b'\x80']
        rng = random.Random(0xF04B)
        random_names = set()
        while len(random_names) < 128:
            candidate = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
            if b'\x00' not in candidate and b'/' not in candidate and candidate not in (b'.', b'..'):
                random_names.add(candidate)
        byte_names = set(invalid_names + boundary_names + bad_forms) | random_names
        for name in byte_names:
            rpc('fs-write', b'/utf8-cases/' + name, b'')
        utf8_cursor = open_dir('/utf8-cases', session)
        for expected in sorted(byte_names):
            try:
                decoded = expected.decode('utf-8', errors='strict')
            except UnicodeDecodeError:
                rpc('fs-dir-page', utf8_cursor, 1, '--session', session, ok=False,
                    code=0x80000000 | errno.EILSEQ)
                raw = json.loads(rpc('fs-dir-entries-raw', utf8_cursor, 1, '--session', session))
                assert [bytes.fromhex(e['name_hex']) for e in raw['entries']] == [expected]
                assert not raw['eof'] or expected == sorted(byte_names)[-1]
            else:
                page = json.loads(rpc('fs-dir-page', utf8_cursor, 1, '--session', session))
                assert page['names'] == [decoded]
        assert json.loads(rpc('fs-dir-page', utf8_cursor, 1, '--session', session)) == {'names': [], 'eof': True}

        # Rejected inode name components must not publish a filesystem
        # sequence. This exercises component validation rather than path
        # lookup failures for slash-containing paths.
        root_inode = json.loads(rpc('fs-iroot'))['inode_id']
        names_inode = json.loads(rpc('fs-ilookup', root_inode, 'names'))['inode_id']
        before = rpc('status')
        for component in (b'has\x00nul', b'has/slash', b'.', b'..', b'x' * 256):
            rpc('fs-icreate', names_inode, component, 'file', 'mode=644',
                '--session', session, ok=False)
        assert rpc('status') == before

        # 700 maximum-length names with control bytes expand beyond 1 MiB in
        # normal JSON. E2BIG leaves the cursor intact for a raw retry.
        rpc('fs-mkdir', '/large')
        large_names = []
        for i in range(700):
            name = ('%04d' % i).encode() + b'\x01' * 251
            large_names.append(name)
            rpc('fs-write', b'/large/' + name, b'')
        large_cursor = open_dir('/large', session)
        rpc('fs-dir-page', large_cursor, 1024, '--session', session,
            ok=False, code=0x80000000 | errno.E2BIG)
        raw_large = json.loads(rpc('fs-dir-entries-raw', large_cursor, 1024, '--session', session))
        assert [bytes.fromhex(e['name_hex']) for e in raw_large['entries']] == sorted(large_names)
        assert raw_large['eof'] is True
        rpc('session-close', session)
        rpc('fs-check')
        proc.terminate(); proc.wait(timeout=5)
        assert proc.returncode == 0, proc.communicate()
        print('POSIX byte filenames and bounded strict JSON directory RPC passed')
    finally:
        if proc.poll() is None:
            proc.kill(); proc.wait(timeout=5)
