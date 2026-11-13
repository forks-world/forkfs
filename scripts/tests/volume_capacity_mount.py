#!/usr/bin/env python3
"""Qualify Darwin volume queries on owned scratch, or a readonly revision.

Capacity is shared host space, not a quota or a reservation. Free counts can
change between calls; only stable geometry and per-result bounds are compared.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from filesystem_compat import check, filesystem, rejects


# Darwin 64-bit struct statfs, from sys/mount.h. Use the explicit 64-bit
# symbols: the unsuffixed libc symbol can expose the legacy inode ABI.
class StatFS(ctypes.Structure):
    _fields_ = [('bsize', ctypes.c_uint32), ('iosize', ctypes.c_int32),
                *[(name, ctypes.c_uint64) for name in
                  ['blocks', 'bfree', 'bavail', 'files', 'ffree']],
                ('fsid', ctypes.c_int32 * 2), ('owner', ctypes.c_uint32),
                ('type', ctypes.c_uint32), ('flags', ctypes.c_uint32),
                ('subtype', ctypes.c_uint32), ('fstypename', ctypes.c_char * 16),
                ('mntonname', ctypes.c_char * 1024), ('mntfromname', ctypes.c_char * 1024),
                ('flags_ext', ctypes.c_uint32), ('reserved', ctypes.c_uint32 * 7)]


libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
libc.statfs64.argtypes = [ctypes.c_char_p, ctypes.POINTER(StatFS)]
libc.fstatfs64.argtypes = [ctypes.c_int, ctypes.POINTER(StatFS)]
libc.statfs64.restype = libc.fstatfs64.restype = ctypes.c_int
CONFIG = ['PC_NAME_MAX', 'PC_LINK_MAX', 'PC_CHOWN_RESTRICTED',
          'PC_NO_TRUNC', 'PC_FILESIZEBITS']


def query(target, kind, readonly=False):
    fd = isinstance(target, int)
    st = os.fstatvfs(target) if fd else os.statvfs(target)
    check(st.f_bsize > 0 and st.f_frsize > 0 and st.f_blocks > 0, 'invalid geometry')
    check(0 <= st.f_bavail <= st.f_bfree <= st.f_blocks, 'invalid block counts')
    check(0 <= st.f_ffree <= st.f_files, 'invalid file counts')
    raw = StatFS()
    result = libc.fstatfs64(target, ctypes.byref(raw)) if fd else libc.statfs64(os.fsencode(target), ctypes.byref(raw))
    if result:
        code = ctypes.get_errno(); raise OSError(code, os.strerror(code))
    check(raw.fstypename.decode() == kind, 'statfs filesystem mismatch')
    check(raw.bsize == st.f_frsize and raw.blocks == st.f_blocks, 'statfs/statvfs geometry differs')
    check(0 <= raw.bavail <= raw.bfree <= raw.blocks, 'invalid statfs block counts')
    check(0 <= raw.ffree <= raw.files and raw.iosize > 0, 'invalid statfs counts/io size')
    if readonly:
        check(st.f_flag & os.ST_RDONLY and raw.flags & 1, 'readonly mount flag missing')
    conf = {name: os.fpathconf(target, name) if fd else os.pathconf(target, name) for name in CONFIG}
    check(st.f_namemax == conf['PC_NAME_MAX'], 'statvfs/pathconf name limits differ')
    try:
        path_max = {'value': os.fpathconf(target, 'PC_PATH_MAX') if fd else os.pathconf(target, 'PC_PATH_MAX')}
    except OSError as error:
        check(kind == 'forkfs' and error.errno == errno.EINVAL, 'unexpected PC_PATH_MAX failure')
        path_max = {'errno': error.errno}
    if kind == 'forkfs':
        check(conf['PC_NAME_MAX'] == 255 and conf['PC_LINK_MAX'] == 32767, 'wrong forkfs limits')
        check(conf['PC_CHOWN_RESTRICTED'] > 0 and conf['PC_NO_TRUNC'] > 0, 'wrong forkfs policy')
        check(conf['PC_FILESIZEBITS'] == 42, 'wrong inclusive 1 TiB signed size bound')
    return {'statvfs': {name: getattr(st, name) for name in ['f_bsize', 'f_frsize', 'f_blocks', 'f_bfree', 'f_bavail',
                                                'f_files', 'f_ffree', 'f_flag', 'f_namemax']},
            'statfs': {name: getattr(raw, name) for name in ['bsize', 'iosize', 'blocks', 'bfree', 'bavail', 'files', 'ffree', 'flags']},
            'fsid': list(raw.fsid), 'pathconf': conf, 'path_max_probe': path_max}


def same_volume(a, b):
    check(a['fsid'] == b['fsid'] and a['pathconf'] == b['pathconf'], 'path/fd volume identity or limits differ')
    for key in ['f_bsize', 'f_frsize', 'f_blocks']:
        check(a['statvfs'][key] == b['statvfs'][key], 'path/fd geometry differs')


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
    report = {'platform': platform.platform(), 'filesystem': kind, 'queries': {},
              'limits': ['shared host capacity; no quota/reservation', 'no exact free-count equality between calls',
                         'no disk-full or hardware durability qualification']}
    if args.verify_report:
        check(kind == 'forkfs', 'requires readonly forkfs revision')
        previous = json.loads(args.verify_report.read_text()); ns = previous['namespace']
        check(ns.startswith('/forkfs-capacity-') and len(Path(ns).parts) == 2, 'invalid report namespace')
        root = target / ns.lstrip('/'); artifact = root / 'retained'
        check(artifact.read_bytes() == b'volume-capacity\x00\xff', 'revision artifact differs')
        with artifact.open('rb') as stream:
            a = query(root, kind, True); b = query(stream.fileno(), kind, True); same_volume(a, b)
        check(a['pathconf'] == previous['queries']['directory']['pathconf'], 'readonly limits changed')
        rejects(lambda: (root/'refused').touch(), errno.EROFS)
        report.update(readonly_after_gc=True, queries={'directory': a, 'fd': b})
        print(json.dumps(report, indent=2)); return
    if kind == 'forkfs': check(args.daemon and args.socket, 'requires daemon/socket')
    def rpc(*fields):
        return subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                               *map(str, fields), '--world', 'main'], check=True, capture_output=True, timeout=30).stdout
    root = Path(tempfile.mkdtemp(prefix='forkfs-capacity-', dir=target))
    report['namespace'] = '/' + root.name
    try:
        baseline = query(root, kind); report['queries']['directory'] = baseline
        path = root/'file'; path.write_bytes(b'volume-capacity\x00\xff')
        with path.open('rb') as stream:
            for phase in ['open', 'renamed', 'unlinked', 'reused']:
                if phase == 'renamed': path.rename(root/'moved')
                elif phase == 'unlinked': (root/'moved').unlink()
                elif phase == 'reused': path.write_bytes(b'replacement')
                current = query(stream.fileno(), kind); same_volume(baseline, current)
                report['queries'][phase] = current
        maximum = baseline['pathconf']['PC_NAME_MAX']; check(maximum == 255, 'unexpected name bound')
        for name in ['a'*maximum, '\u00e9'*127+'a']:
            item = root/name; item.touch(); check(item.name == name, 'name was truncated'); item.unlink()
        before = sorted(p.name for p in root.iterdir())
        rejects(lambda: (root/('a'*(maximum+1))).touch(), errno.ENAMETOOLONG)
        check(sorted(p.name for p in root.iterdir()) == before, 'overlong name mutated directory')
        # APFS permits this 256-byte name; forkfs defines its limit in UTF-8
        # bytes. Keep the difference explicit rather than claiming parity.
        multibyte = root/('\u00e9'*128)
        if kind == 'forkfs':
            rejects(multibyte.touch, errno.ENAMETOOLONG)
            check(sorted(p.name for p in root.iterdir()) == before, 'overlong UTF-8 name mutated directory')
        else:
            multibyte.touch(); multibyte.unlink()
        report['name_limits'] = {'ascii_255_accepted': True, 'ascii_256_errno': errno.ENAMETOOLONG,
                                 'utf8_255_accepted': True, 'utf8_256_accepted': kind == 'apfs'}
        if kind == 'forkfs':
            with (root/'size-boundary').open('w+b') as stream:
                limit = 1 << 40
                stream.truncate(limit)
                check(os.pwrite(stream.fileno(), b'Z', limit-1) == 1, '1 TiB last byte write failed')
                os.fsync(stream.fileno())
                rejects(lambda: os.ftruncate(stream.fileno(), limit+1), errno.EFBIG)
                check(os.fstat(stream.fileno()).st_size == limit and os.pread(stream.fileno(), 1, limit-1) == b'Z',
                      'failed oversize truncate changed file')
            (root/'size-boundary').unlink()
            report['inclusive_file_limit'] = {'bytes': limit, 'next_size_errno': errno.EFBIG, 'failure_preserves_data': True}
        artifact = root/'retained'; artifact.write_bytes(b'volume-capacity\x00\xff')
        with artifact.open('rb') as stream: os.fsync(stream.fileno())
        if kind == 'forkfs':
            report['revision'] = 'capacity_'+root.name[len('forkfs-capacity-'):].replace('-', '_')
            rpc('fs-snapshot', report['revision']); rpc('fs-check')
            data = subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)),
                                   'fs-cat', report['namespace']+'/retained', '--revision', report['revision']],
                                  check=True, capture_output=True, timeout=30).stdout
            check(data == artifact.read_bytes(), 'revision backend data differs')
            report['revision_data_verified'] = True
    finally:
        shutil.rmtree(root)
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
