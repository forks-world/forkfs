#!/usr/bin/env python3
"""Read an owned large directory fixture through native readdir/telldir/seekdir.

forkfs requires a disposable repository made by forkfsd_directory_fixture.
APFS creates and removes its own scratch directory. No fixture files are edited.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import shutil
import tempfile
from filesystem_compat import check, filesystem


class Dirent(ctypes.Structure):
    _fields_ = [('ino', ctypes.c_uint64), ('seekoff', ctypes.c_uint64),
                ('reclen', ctypes.c_uint16), ('namlen', ctypes.c_uint16),
                ('type', ctypes.c_uint8), ('name', ctypes.c_char * 1024)]


libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
libc.opendir.argtypes, libc.opendir.restype = [ctypes.c_char_p], ctypes.c_void_p
libc.readdir.argtypes, libc.readdir.restype = [ctypes.c_void_p], ctypes.POINTER(Dirent)
libc.telldir.argtypes, libc.telldir.restype = [ctypes.c_void_p], ctypes.c_long
libc.seekdir.argtypes, libc.seekdir.restype = [ctypes.c_void_p, ctypes.c_long], None
libc.closedir.argtypes, libc.closedir.restype = [ctypes.c_void_p], ctypes.c_int


def read_one(directory):
    ctypes.set_errno(0)
    entry = libc.readdir(directory)
    if not entry:
        code = ctypes.get_errno()
        if code: raise OSError(code, os.strerror(code))
        return None
    return bytes(entry.contents.name).decode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--count', type=int, default=66000)
    parser.add_argument('--reference-apfs', action='store_true')
    parser.add_argument('--expect-overflow', action='store_true')
    args = parser.parse_args()
    check(65537 <= args.count <= 100000, 'count must cross the old 65536 limit')
    target = args.mountpoint.resolve(strict=True); mounted, kind = filesystem(target)
    check(kind == ('apfs' if args.reference_apfs else 'forkfs'), 'unexpected filesystem')
    if not args.reference_apfs:
        check(target == mounted and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    root = Path(tempfile.mkdtemp(prefix='forkfs-dir-', dir=target)) if args.reference_apfs else target/'scale'
    try:
        if args.reference_apfs:
            for i in range(args.count): (root/('n%06d' % i)).touch()
        directory = libc.opendir(os.fsencode(root)); check(directory, 'opendir failed')
        names = []; saved = None; resume_name = None
        try:
            try:
                while True:
                    name = read_one(directory)
                    if name is None: break
                    if name in ('.', '..'): continue
                    names.append(name)
                    if len(names) == 257: saved = libc.telldir(directory)
                    if len(names) == 258: resume_name = name
            except OSError as error:
                check(args.expect_overflow and error.errno == errno.EOVERFLOW, 'unexpected readdir failure: %s' % error)
                print(json.dumps({'prior_overflow_reproduced': True, 'errno': error.errno, 'entries_before_failure': len(names)})); return
            check(not args.expect_overflow, 'old enumeration limit did not reproduce')
            expected = {'n%06d' % i for i in range(args.count)}
            check(len(names) == args.count and len(set(names)) == args.count and set(names) == expected, 'entries missing or duplicated')
            check(saved is not None and saved >= 0 and resume_name, 'missing directory cookie')
            libc.seekdir(directory, saved)
            check(read_one(directory) == resume_name, 'backward seekdir changed captured entry')
        finally:
            libc.closedir(directory)
        # Repeat with a fresh stream, including kernel-provided d_type and stat.
        with os.scandir(root) as entries:
            scanned = {entry.name for entry in entries if entry.is_file(follow_symlinks=False)}
        check(scanned == expected, 'scandir/type classification lost entries')
        for i in [0, 257, 512, 65535, args.count-1]:
            check((root/('n%06d' % i)).stat().st_size == 0, 'fixture file changed')
        print(json.dumps({'filesystem': kind, 'entries': args.count, 'native_readdir_and_scandir': True,
                          'unique_and_complete': True, 'backward_telldir_seekdir': True}, indent=2))
    finally:
        if args.reference_apfs: shutil.rmtree(root)


if __name__ == '__main__':
    main()
