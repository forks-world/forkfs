#!/usr/bin/env python3
"""Qualify application-facing APIs on a mounted forkfs, or an explicit APFS reference.

Uses only a unique private scratch directory. Each case runs in a bounded child
process. This is native mount acceptance, not a daemon-only or power-loss test.
On forkfs, retains one explicitly named revision; use a disposable repository.
"""
import argparse
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
         'sqlite-wal': lambda root: database(root, 'WAL'), 'git': git_workflow}


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
    parser.add_argument('--worker-case', choices=CASES, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker_case:
        print(json.dumps(CASES[args.worker_case](args.mountpoint)))
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
              'python': platform.python_version(), 'cases': {},
              'limits': ['single mount', 'no power-loss or kernel-crash guarantee',
                         'small-file workloads only']}
    if not args.reference_apfs:
        report['forkfs_maximum_file_bytes'] = 64 * 1024 * 1024
    root = Path(tempfile.mkdtemp(prefix='forkfs-api-', dir=target))
    try:
        for name in CASES:
            case_root = root / name
            case_root.mkdir(mode=0o700)
            try:
                result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                         str(case_root), '--worker-case', name],
                                        capture_output=True, text=True, timeout=120)
                check(result.returncode == 0, result.stderr.strip() or 'worker failed')
                report['cases'][name] = {'status': 'passed', 'details': json.loads(result.stdout)}
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                report['cases'][name] = {'status': 'failed', 'error': str(error)}
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
    return int(any(case['status'] != 'passed' for case in report['cases'].values()))


if __name__ == '__main__':
    sys.exit(main())
