#!/usr/bin/env python3
"""Qualify application-facing APIs on a mounted forkfs, or an explicit APFS reference.

Uses only a unique private scratch directory. Each case runs in a bounded child
process. This is native mount acceptance, not a daemon-only or power-loss test.
On forkfs, retains one explicitly named revision; use a disposable repository.
"""
import argparse
import ctypes
import errno
import fcntl
import json
import mmap
import os
from pathlib import Path
import platform
import shutil
import sqlite3
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def child(code, *arguments):
    return subprocess.run([sys.executable, '-c', code, *map(str, arguments)],
                          check=True, capture_output=True, text=True, timeout=20)


def mapped_io(root):
    path = root / 'mapped'
    page = mmap.PAGESIZE
    with path.open('w+b') as stream:
        stream.truncate(2 * page)
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_WRITE) as shared:
            shared[page:page + 8] = b'SHARED\x00\xff'
            shared.flush()
            os.fsync(stream.fileno())
            check(os.pread(stream.fileno(), 8, page) == b'SHARED\x00\xff', 'mmap -> pread coherence')
            child('import pathlib,sys; data=pathlib.Path(sys.argv[1]).read_bytes(); '
                  'assert data[int(sys.argv[2]):int(sys.argv[2])+8]==b"SHARED\\x00\\xff"', path, page)
            os.pwrite(stream.fileno(), b'PWRITE', 0)
            check(shared[:6] == b'PWRITE', 'pwrite -> mmap coherence')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_COPY) as private:
            private[:7] = b'PRIVATE'
            private.flush()
        check(os.pread(stream.fileno(), 6, 0) == b'PWRITE', 'MAP_PRIVATE changed file')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as readonly:
            check(readonly[page:page + 8] == b'SHARED\x00\xff', 'read-only mapping content')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_WRITE) as orphan:
            path.unlink()
            path.write_bytes(b'REPLACEMENT')
            orphan[:6] = b'ORPHAN'
            orphan.flush()
            os.fsync(stream.fileno())
            check(os.pread(stream.fileno(), 6, 0) == b'ORPHAN', 'unlinked mapping identity')
            check(path.read_bytes() == b'REPLACEMENT', 'mapping changed replacement inode')
    # Retain a separate mapped artifact for direct backend and revision checks.
    artifact = root / 'mapped-durable'
    with artifact.open('w+b') as stream:
        stream.truncate(page)
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_WRITE) as mapping:
            mapping[:8] = b'REVISION'
            mapping.flush()
        os.fsync(stream.fileno())
    return {'page_size': page, 'shared_private_readonly': True, 'open_unlink': True}


def rejects(operation, expected):
    try:
        operation()
    except OSError as error:
        check(error.errno == expected, 'expected errno %s, got %s' % (expected, error.errno))
    else:
        raise RuntimeError('operation unexpectedly succeeded (expected errno %s)' % expected)


def open_flags(root):
    path = root / 'file'
    path.write_bytes(b'original')
    directory = root / 'directory'
    directory.mkdir()
    os.symlink('file', root / 'link')
    os.symlink('missing', root / 'dangling')
    os.symlink('loop', root / 'loop')

    def opened(target, flags):
        descriptor = os.open(target, flags, 0o600)
        os.close(descriptor)

    failures = [('file', os.O_WRONLY | os.O_CREAT | os.O_EXCL, errno.EEXIST),
                ('link', os.O_WRONLY | os.O_CREAT | os.O_EXCL, errno.EEXIST),
                ('dangling', os.O_WRONLY | os.O_CREAT | os.O_EXCL, errno.EEXIST),
                ('link', os.O_RDONLY | os.O_NOFOLLOW, errno.ELOOP),
                ('dangling', os.O_RDONLY | os.O_NOFOLLOW, errno.ELOOP),
                ('link', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, errno.ELOOP),
                ('loop', os.O_RDONLY, errno.ELOOP),
                ('absent', os.O_RDONLY, errno.ENOENT),
                ('file', os.O_RDONLY | os.O_DIRECTORY, errno.ENOTDIR),
                ('file/child', os.O_RDONLY | os.O_CREAT, errno.ENOTDIR),
                ('directory', os.O_WRONLY, errno.EISDIR),
                ('directory', os.O_RDWR, errno.EISDIR)]
    names = sorted(os.listdir(root))
    info = path.stat()
    for name, flags, code in failures:
        rejects(lambda: opened(root / name, flags), code)
        check(sorted(os.listdir(root)) == names, 'failed open created an entry')
        after = path.stat()
        check((after.st_ino, after.st_size, after.st_mtime_ns) ==
              (info.st_ino, info.st_size, info.st_mtime_ns), 'failed open changed file metadata')
        check(path.read_bytes() == b'original', 'failed open changed bytes')
    opened(directory, os.O_RDONLY | os.O_DIRECTORY)
    opened(root / 'link', os.O_RDONLY)
    opened(root / 'dangling', os.O_WRONLY | os.O_CREAT)
    check((root / 'missing').is_file() and (root / 'dangling').is_symlink(), 'create through dangling link')
    with path.open('rb') as original:
        identity = os.fstat(original.fileno()).st_ino
        opened(path, os.O_WRONLY | os.O_TRUNC)
        check(os.fstat(original.fileno()).st_size == 0 and path.stat().st_ino == identity,
              'truncate replaced inode or left stale size')
    # Darwin accepts this combination and truncates even with a read-only fd.
    path.write_bytes(b'truncate-readonly')
    opened(path, os.O_RDONLY | os.O_TRUNC)
    check(path.stat().st_size == 0, 'Darwin read-only O_TRUNC behavior')
    old_mask = os.umask(0o027)
    try:
        descriptor = os.open(root / 'umask', os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o666)
        os.close(descriptor)
    finally:
        os.umask(old_mask)
    check((root / 'umask').stat().st_mode & 0o777 == 0o640, 'creation umask')
    return {'rejected_combinations': len(failures), 'failure_preserves_file': True,
            'dangling_create': True, 'truncate_preserves_inode': True,
            'darwin_readonly_truncate': True, 'umask': '0027 -> 0640'}


def append_io(root):
    path = root / 'append'
    path.write_bytes(b'HEAD')
    # Records exceed the daemon's 256 KiB I/O bound: the native append must not
    # interleave records when one callback requires several daemon requests.
    workers, records, size = 4, 4, 300 * 1024
    code = '''import os,sys
fd=os.open(sys.argv[1],os.O_WRONLY|os.O_APPEND)
print('ready',flush=True);sys.stdin.buffer.read(1)
for sequence in range(int(sys.argv[3])):
    header=('%02d:%02d:'%(int(sys.argv[2]),sequence)).encode()
    record=header+bytes([65+int(sys.argv[2])])*(int(sys.argv[4])-len(header))
    if os.write(fd,record)!=len(record):raise RuntimeError('short append')
os.fsync(fd);os.close(fd)
'''
    processes = []
    try:
        for worker in range(workers):
            process = subprocess.Popen([sys.executable, '-c', code, str(path), str(worker),
                                        str(records), str(size)], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(process)
        for process in processes:
            check(process.stdout.readline() == b'ready\n', 'append worker did not open')
        for process in processes:
            process.stdin.write(b'x')
            process.stdin.flush()
        for process in processes:
            check(process.wait(timeout=30) == 0, 'append worker failed: ' + process.stderr.read().decode())
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()
    data = path.read_bytes()
    check(data[:4] == b'HEAD' and len(data) == 4 + workers * records * size, 'append lost bytes')
    expected = set()
    for worker in range(workers):
        for sequence in range(records):
            header = ('%02d:%02d:' % (worker, sequence)).encode()
            expected.add(header + bytes([65 + worker]) * (size - len(header)))
    actual = [data[offset:offset + size] for offset in range(4, len(data), size)]
    check(len(set(actual)) == workers * records and set(actual) == expected,
          'append records interleaved, duplicated, or lost')
    descriptor = os.open(path, os.O_RDWR | os.O_APPEND)
    try:
        os.lseek(descriptor, 0, os.SEEK_SET)
        check(os.write(descriptor, b'TAIL') == 4, 'append write length')
        check(os.pwrite(descriptor, b'EDIT', 0) == 4, 'positional write length')
        os.fsync(descriptor)
        check(os.pread(descriptor, 4, 0) == b'EDIT', 'Darwin pwrite must ignore O_APPEND')
    finally:
        os.close(descriptor)
    check(path.read_bytes()[-4:] == b'TAIL', 'lseek defeated O_APPEND')
    return {'processes': workers, 'records_per_process': records, 'record_bytes': size,
            'no_lost_or_interleaved_records': True, 'darwin_pwrite_ignores_append': True}


def permissions(root):
    check(os.geteuid() != 0, 'permission acceptance must run as an ordinary user')
    path = root / 'file'
    path.write_bytes(b'original')
    directory = root / 'directory'
    directory.mkdir()
    (directory / 'child').write_bytes(b'child')
    descriptor = os.open(path, os.O_RDWR)
    try:
        path.chmod(0)
        rejects(lambda: os.open(path, os.O_RDONLY), errno.EACCES)
        rejects(lambda: os.open(path, os.O_WRONLY), errno.EACCES)
        check(not os.access(path, os.R_OK) and not os.access(path, os.W_OK), 'access ignored mode 000')
        # Access checks occur at open. chmod must not revoke an existing fd.
        check(os.pread(descriptor, 8, 0) == b'original', 'chmod revoked open read fd')
        check(os.pwrite(descriptor, b'UPDATED!', 0) == 8, 'chmod revoked open write fd')
        os.fsync(descriptor)
        path.chmod(0o400)
        rejects(lambda: os.open(path, os.O_WRONLY | os.O_TRUNC), errno.EACCES)
        check(path.read_bytes() == b'UPDATED!', 'denied truncate changed bytes')
        directory.chmod(0o600)
        rejects(lambda: (directory / 'child').stat(), errno.EACCES)
        directory.chmod(0o500)
        rejects(lambda: os.open(directory / 'new', os.O_CREAT | os.O_WRONLY, 0o600), errno.EACCES)
        rejects(lambda: (directory / 'child').unlink(), errno.EACCES)
        directory.chmod(0o700)
        check((directory / 'child').read_bytes() == b'child' and not (directory / 'new').exists(),
              'denied directory mutation changed namespace')
    finally:
        os.close(descriptor)
        path.chmod(0o600)
        directory.chmod(0o700)
    return {'mode_access_checks': True, 'directory_search_and_write': True,
            'open_fd_survives_chmod': True, 'denied_mutations_preserve_data': True}


def relative_paths(root):
    directory = root / 'directory'
    directory.mkdir()
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        file_descriptor = os.open('file', os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600, dir_fd=descriptor)
        try:
            check(os.write(file_descriptor, b'relative\x00\xff') == 10, 'openat write')
            os.fsync(file_descriptor)
        finally:
            os.close(file_descriptor)
        os.rename(directory, root / 'moved')
        check(os.stat('file', dir_fd=descriptor).st_size == 10, 'dirfd lost identity after rename')
        os.mkdir('child', mode=0o700, dir_fd=descriptor)
        os.symlink('file', 'link', dir_fd=descriptor)
        check(os.readlink('link', dir_fd=descriptor) == 'file', 'readlinkat')
        os.link('file', 'alias', src_dir_fd=descriptor, dst_dir_fd=descriptor)
        check(os.stat('alias', dir_fd=descriptor).st_ino == os.stat('file', dir_fd=descriptor).st_ino,
              'linkat inode identity')
        os.rename('file', 'renamed', src_dir_fd=descriptor, dst_dir_fd=descriptor)
        rejects(lambda: os.stat('file', dir_fd=descriptor), errno.ENOENT)
        os.unlink('alias', dir_fd=descriptor)
        os.rmdir('child', dir_fd=descriptor)
        check((root / 'moved/renamed').read_bytes() == b'relative\x00\xff', 'renameat bytes')
    finally:
        os.close(descriptor)
    return {'openat_statat_linkat_renameat_unlinkat_mkdirat_readlinkat': True,
            'directory_fd_survives_rename': True}


def rename_extensions(root):
    libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
    libc.renamex_np.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    libc.renamex_np.restype = ctypes.c_int
    kind = filesystem(root)[1]
    source, destination = root / 'source', root / 'destination'
    results = {}
    for name, flags, exists in [('swap', 2, True), ('exclusive-existing', 4, True),
                                ('exclusive-missing', 4, False)]:
        source.write_bytes(b'SOURCE')
        if exists:
            destination.write_bytes(b'DESTINATION')
        elif destination.exists():
            destination.unlink()
        before = {path.name: (path.stat().st_ino, path.read_bytes())
                  for path in (source, destination) if path.exists()}
        ctypes.set_errno(0)
        result = libc.renamex_np(os.fsencode(source), os.fsencode(destination), flags)
        code = ctypes.get_errno() if result != 0 else 0
        expected = errno.EEXIST if name == 'exclusive-existing' else (errno.ENOTSUP if kind == 'forkfs' else 0)
        check(code == expected, 'unexpected renamex_np %s errno %s' % (name, code))
        if code:
            after = {path.name: (path.stat().st_ino, path.read_bytes())
                     for path in (source, destination) if path.exists()}
            check(before == after, 'rejected rename extension changed namespace')
        elif name == 'swap':
            check(source.read_bytes() == b'DESTINATION' and destination.read_bytes() == b'SOURCE',
                  'rename swap contents')
        else:
            check(not source.exists() and destination.read_bytes() == b'SOURCE', 'exclusive rename contents')
        results[name] = {'errno': code, 'operation_completed': code == 0,
                         'unsupported': code == errno.ENOTSUP}
    return results


def locks(root, name):
    path = root / name
    path.write_bytes(bytes(32))
    operation = getattr(fcntl, name)
    # Distinguish expected contention from unrelated syscall failures.
    contender = '''import errno,fcntl,sys
with open(sys.argv[1], 'r+b') as stream:
    try:
        if sys.argv[2]=='lockf':
            fcntl.lockf(stream,fcntl.LOCK_EX|fcntl.LOCK_NB,8,int(sys.argv[3]))
        else:
            fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except OSError as error:
        if error.errno not in (errno.EACCES,errno.EAGAIN): raise
        sys.exit(23)
'''

    def attempt(offset, blocked):
        result = subprocess.run([sys.executable, '-c', contender, str(path), name, str(offset)],
                                capture_output=True, text=True, timeout=10)
        check(result.returncode == (23 if blocked else 0), 'lock contention/release: ' + result.stderr)

    with path.open('r+b') as stream:
        if name == 'lockf':
            operation(stream, fcntl.LOCK_EX | fcntl.LOCK_NB, 8, 0)
            attempt(16, False)
        else:
            operation(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        attempt(0, True)
    attempt(0, False)
    # An independently exiting holder must also release its lock.
    child('import fcntl,sys; f=open(sys.argv[1],"r+b"); '
          'getattr(fcntl,sys.argv[2])(f,fcntl.LOCK_EX|fcntl.LOCK_NB)', path, name)
    attempt(0, False)
    return {'cross_process_exclusion': True, 'close_and_exit_release': True,
            'scope': 'byte ranges (including disjoint ranges)' if name == 'lockf' else 'whole file'}


def database(root, journal):
    path = root / ('sqlite-' + journal.lower())
    connection = sqlite3.connect(str(path), timeout=0)
    try:
        actual = connection.execute('PRAGMA journal_mode=' + journal).fetchone()[0]
        check(actual.upper() == journal, 'SQLite changed requested journal mode')
        connection.execute('PRAGMA synchronous=FULL')
        connection.execute('CREATE TABLE records(id INTEGER PRIMARY KEY, data BLOB)')
        connection.execute('INSERT INTO records VALUES(1,?)', (b'committed\x00\xff',))
        connection.commit()
        connection.execute('BEGIN IMMEDIATE')
        connection.execute('INSERT INTO records VALUES(2,?)', (b'rollback',))
        child('''import sqlite3,sys
c=sqlite3.connect(sys.argv[1],timeout=0)
assert c.execute('SELECT count(*) FROM records').fetchone()==(1,)
try: c.execute('BEGIN IMMEDIATE')
except sqlite3.OperationalError as error:
    assert 'locked' in str(error), error
else: raise AssertionError('concurrent writer bypassed lock')
c.close()
''', path)
        connection.rollback()
    finally:
        connection.close()
    # Leave a committed transaction and an uncommitted large transaction behind
    # without close()/SQLite cleanup, exercising journal/WAL recovery on reopen.
    child('''import os,sqlite3,sys
c=sqlite3.connect(sys.argv[1],timeout=0)
c.execute('PRAGMA synchronous=FULL')
c.execute('INSERT INTO records VALUES(3,?)',(b'child-committed',));c.commit()
c.execute('PRAGMA cache_size=2')
c.execute('BEGIN IMMEDIATE')
c.executemany('INSERT INTO records VALUES(?,?)',[(i,bytes(4096)) for i in range(100,164)])
os._exit(0)
''', path)
    recovered = sqlite3.connect(str(path), timeout=0)
    try:
        check(recovered.execute('SELECT id,data FROM records ORDER BY id').fetchall() ==
              [(1, b'committed\x00\xff'), (3, b'child-committed')], 'SQLite recovered wrong rows')
        check(recovered.execute('PRAGMA integrity_check').fetchone() == ('ok',), 'SQLite integrity')
        if journal == 'WAL':
            check(recovered.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()[0] == 0, 'WAL checkpoint busy')
    finally:
        recovered.close()
    return {'journal': journal, 'sqlite_version': sqlite3.sqlite_version,
            'concurrent_reader_writer': True, 'abrupt_process_exit_recovery': True}


def git_workflow(root):
    directory = root / 'git'
    directory.mkdir()
    # No user/system config, signing, hooks, credentials, templates or network.
    environment = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL': 'C',
                   'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
                   'GIT_TERMINAL_PROMPT': '0', 'GIT_AUTHOR_NAME': 'ForkFS acceptance',
                   'GIT_AUTHOR_EMAIL': 'acceptance@example.invalid',
                   'GIT_COMMITTER_NAME': 'ForkFS acceptance',
                   'GIT_COMMITTER_EMAIL': 'acceptance@example.invalid'}

    def git(*arguments):
        return subprocess.run(['/usr/bin/git', '-c', 'core.hooksPath=/dev/null',
                               '-c', 'commit.gpgSign=false', *arguments], cwd=directory,
                              env=environment, check=True, capture_output=True, timeout=30).stdout

    git('init', '--template=', '-b', 'main')
    (directory / 'binary').write_bytes(b'first\x00\xff')
    executable = directory / 'executable'
    executable.write_text('#!/bin/sh\nexit 0\n')
    executable.chmod(0o755)
    os.symlink('binary', directory / 'link')
    git('add', '.')
    git('commit', '-m', 'first')
    first = git('rev-parse', 'HEAD').strip()
    git('checkout', '-b', 'work')
    (directory / 'binary').write_bytes(b'second\x00\xfe')
    git('add', 'binary')
    git('commit', '-m', 'second')
    git('checkout', 'main')
    check((directory / 'binary').read_bytes() == b'first\x00\xff', 'Git checkout bytes')
    check(os.readlink(directory / 'link') == 'binary', 'Git symlink checkout')
    check(executable.stat().st_mode & 0o111, 'Git executable mode')
    git('merge', '--ff-only', 'work')
    check((directory / 'binary').read_bytes() == b'second\x00\xfe', 'Git fast-forward bytes')
    git('repack', '-ad')
    git('fsck', '--full', '--strict')
    check(git('status', '--porcelain') == b'', 'Git dirty after checkout/repack')
    check(git('show', first.decode() + ':binary') == b'first\x00\xff', 'Git packed historical bytes')
    return {'version': git('--version').decode().strip(), 'checkout_merge_repack_fsck': True}


CASES = {'mmap': mapped_io, 'lockf': lambda root: locks(root, 'lockf'),
         'flock': lambda root: locks(root, 'flock'),
         'sqlite-delete': lambda root: database(root, 'DELETE'),
         'sqlite-wal': lambda root: database(root, 'WAL'), 'git': git_workflow,
         'open-flags': open_flags, 'append': append_io, 'permissions': permissions,
         'relative-paths': relative_paths}
PROBES = {'rename-extensions': rename_extensions}


def filesystem(path):
    listing = subprocess.run(['/sbin/mount'], check=True, capture_output=True, text=True).stdout
    matches = []
    for row in listing.splitlines():
        if ' on ' not in row or ' (' not in row:
            continue
        prefix, options = row.rsplit(' (', 1)
        mounted = Path(prefix.split(' on ', 1)[1])
        if path == mounted or mounted in path.parents:
            matches.append((len(mounted.parts), mounted, options.rstrip(')').split(', ')[0]))
    check(bool(matches), 'cannot identify containing filesystem')
    return max(matches, key=lambda value: value[0])[1:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--world', default='main')
    operations = dict(CASES, **PROBES)
    parser.add_argument('--worker-case', choices=operations, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker_case:
        print(json.dumps(operations[args.worker_case](args.mountpoint)))
        return 0
    check(sys.platform == 'darwin', 'requires native macOS mounts')
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem: ' + kind)
    if not args.reference_apfs:
        check(target == mounted, 'specify the actual forkfs mount root')
        check(target.stat().st_mode & 0o777 == 0o700, 'forkfs root must remain private')
        check(args.daemon is not None and args.socket is not None, 'forkfs requires --daemon and --socket')
    report = {'schema': 1, 'platform': platform.platform(), 'filesystem': kind,
              'python': platform.python_version(), 'cases': {}, 'capability_probes': {},
              'limits': ['single mount', 'no power-loss or kernel-crash guarantee',
                         'small-file workloads only']}
    if not args.reference_apfs:
        report['forkfs_maximum_file_bytes'] = 1 << 40
    root = Path(tempfile.mkdtemp(prefix='forkfs-api-', dir=target))
    try:
        for name in operations:
            group = report['cases'] if name in CASES else report['capability_probes']
            case_root = root / name
            case_root.mkdir(mode=0o700)
            try:
                result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                         str(case_root), '--worker-case', name],
                                        capture_output=True, text=True, timeout=120)
                check(result.returncode == 0, result.stderr.strip() or 'worker failed')
                group[name] = {'status': 'passed' if name in CASES else 'measured',
                               'details': json.loads(result.stdout)}
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                group[name] = {'status': 'failed', 'error': str(error)}
        if not args.reference_apfs and report['cases']['mmap']['status'] == 'passed':
            def rpc(*fields, revision=None):
                view = ['--revision', revision] if revision else ['--world', args.world]
                return subprocess.run([str(args.daemon.resolve(strict=True)), 'request',
                                       str(args.socket.resolve(strict=True)), *fields, *view],
                                      check=True, capture_output=True, timeout=30).stdout
            artifact = '/' + root.name + '/mmap/mapped-durable'
            revision = 'api_accept_' + root.name.removeprefix('forkfs-api-').replace('-', '_')
            data = rpc('fs-cat', artifact)
            check(data == b'REVISION' + bytes(mmap.PAGESIZE - 8), 'mmap bytes did not reach daemon')
            rpc('fs-snapshot', revision)
            with (root / 'mmap/mapped-durable').open('r+b') as stream:
                os.pwrite(stream.fileno(), b'CHANGED!', 0)
                os.fsync(stream.fileno())
            check(rpc('fs-cat', artifact, revision=revision) == data, 'mmap revision changed')
            check(rpc('fs-cat', artifact)[:8] == b'CHANGED!', 'subsequent write did not reach daemon')
            rpc('fs-check')
            rpc('fs-check', revision=revision)
            report['revision'] = revision
            report['revision_artifact'] = artifact
            report['mmap_backend_and_revision_isolation'] = True
    finally:
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))
    return int(any(case['status'] == 'failed' for group in ('cases', 'capability_probes')
                   for case in report[group].values()))


if __name__ == '__main__':
    sys.exit(main())
