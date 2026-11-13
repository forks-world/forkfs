#!/usr/bin/env python3
"""Rename an owned forkfs volume or an explicitly identified mounted APFS image.

Forkfs retains a named revision. Never use an existing user volume as a reference.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
from filesystem_compat import check, filesystem
from file_flags_mount import reject

class AttrList(ctypes.Structure):
    _fields_ = [('count', ctypes.c_uint16), ('reserved', ctypes.c_uint16),
                ('common', ctypes.c_uint32), ('volume', ctypes.c_uint32),
                ('directory', ctypes.c_uint32), ('file', ctypes.c_uint32), ('fork', ctypes.c_uint32)]

libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
for name in ['getattrlist', 'setattrlist']:
    fn = getattr(libc, name)
    fn.argtypes = [ctypes.c_char_p, ctypes.POINTER(AttrList), ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
    fn.restype = ctypes.c_int


def call(name, path, buffer):
    attrs = AttrList(5, 0, 0, 0x80002000, 0, 0, 0)
    if getattr(libc, name)(os.fsencode(path), ctypes.byref(attrs), buffer, len(buffer), 0):
        code = ctypes.get_errno(); raise OSError(code, os.strerror(code))


def get_name(path):
    buffer = ctypes.create_string_buffer(1024); call('getattrlist', path, buffer)
    total, offset, length = struct.unpack_from('=IiI', buffer.raw)
    start = 4+offset
    check(12 <= total <= len(buffer) and start >= 12 and 1 <= length <= 256 and start+length <= total, 'invalid volume name attribute')
    value = buffer.raw[start:start+length]; check(value[-1] == 0, 'unterminated volume name')
    return value[:-1].decode('utf-8')


def set_name(path, name):
    data = name.encode('utf-8')+b'\0'
    buffer = ctypes.create_string_buffer(struct.pack('=iI', 8, len(data))+data)
    call('setattrlist', path, buffer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs-image', type=Path)
    parser.add_argument('--daemon', type=Path)
    parser.add_argument('--socket', type=Path)
    parser.add_argument('--verify-report', type=Path)
    args = parser.parse_args(); target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target); check(target == mounted, 'requires exact mount root')
    check(target.stat().st_dev != target.parent.stat().st_dev, 'refusing host filesystem')
    if args.reference_apfs_image:
        image = args.reference_apfs_image.resolve(strict=True)
        check(kind == 'apfs' and image.is_file() and image.parent == target.parent, 'requires owned reference image next to its mount')
        info = plistlib.loads(subprocess.check_output(['/usr/bin/hdiutil', 'info', '-plist'], timeout=20))
        matches = [v for v in info['images'] if Path(v['image-path']).resolve() == image]
        check(len(matches) == 1 and any(e.get('mount-point') == str(target) for e in matches[0]['system-entities']), 'reference image does not own mount')
    else:
        check(kind == 'forkfs' and target.stat().st_mode & 0o777 == 0o700, 'requires private forkfs root')
    if args.verify_report:
        check(kind == 'forkfs', 'readonly revision verification requires forkfs')
        report = json.loads(args.verify_report.read_text())
        check(get_name(target) == report['retained_name'], 'volume label changed across remount')
        ns = report['namespace']; check(ns.startswith('/forkfs-volume-name-') and len(Path(ns).parts) == 2, 'invalid namespace')
        check((target/ns.lstrip('/')/'file').read_bytes() == b'revision label data', 'renaming changed revision data')
        reject(lambda: set_name(target, 'forbidden'), errno.EROFS)
        print(json.dumps({'volume_name_after_gc_readonly_remount': get_name(target), 'readonly_rename_errno': errno.EROFS, 'data_preserved': True})); return
    if kind == 'forkfs': check(args.daemon and args.socket, 'requires daemon/socket')
    def rpc(*fields, revision=None):
        view = ['--revision', revision] if revision else ['--world', 'main']
        p = subprocess.run([str(args.daemon.resolve(strict=True)), 'request', str(args.socket.resolve(strict=True)), *fields, *view], check=True, capture_output=True, timeout=30)
        return json.loads(p.stdout)
    initial = get_name(target); root = Path(tempfile.mkdtemp(prefix='forkfs-volume-name-', dir=target)); fd = -1
    report = {'filesystem': kind, 'initial_name': initial, 'namespace': '/'+root.name}
    try:
        path = root/'file'; path.write_bytes(b'revision label data'); fd = os.open(path, os.O_RDWR)
        before = os.fstat(fd); name = 'Research 卷📦'
        set_name(target, name); check(get_name(target) == name, 'native rename not visible')
        check(os.fstat(fd).st_ino == before.st_ino and os.fstat(fd).st_dev == before.st_dev and os.pread(fd, 99, 0) == b'revision label data', 'rename changed open inode')
        report['empty_name_errno'] = reject(lambda: set_name(target, ''), errno.EINVAL)
        check(get_name(target) == name, 'failed rename changed label')
        set_name(target, name)
        report['retained_name'] = name
        if kind == 'forkfs':
            check(rpc('fs-volume-name')['name'] == name, 'label absent from daemon')
            report['revision'] = 'volume_name_'+root.name[len('forkfs-volume-name-'):].replace('-', '_')
            rpc('fs-snapshot', report['revision'])
        set_name(target, 'Later name'); check(get_name(target) == 'Later name', 'second rename failed')
        if kind == 'forkfs':
            check(rpc('fs-volume-name', revision=report['revision'])['name'] == name, 'live rename changed revision')
            rpc('fs-check'); report['revision_isolation'] = True
        report['open_inode_preserved'] = True; report['native_rename_readback'] = True
    finally:
        active_error = sys.exc_info()[0] is not None
        cleanup_errors = []
        if fd >= 0:
            try: os.close(fd)
            except OSError as error: cleanup_errors.append(error)
        try: set_name(target, initial)
        except OSError as error: cleanup_errors.append(error)
        try: shutil.rmtree(root)
        except OSError as error: cleanup_errors.append(error)
        if cleanup_errors:
            if not active_error: raise cleanup_errors[0]
            print('volume-name cleanup failed: '+str(cleanup_errors), file=sys.stderr)
    check(get_name(target) == initial, 'initial volume name not restored')
    report['initial_name_restored'] = True
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__': main()
