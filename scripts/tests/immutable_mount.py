#!/usr/bin/env python3
"""Qualify UF_IMMUTABLE admission and mutation rules on an owned private scratch tree.

Retains an immutable revision on forkfs. Existing writable descriptions keep
Darwin data/size rights; this suite does not claim mmap revocation or multi-user policy.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from filesystem_compat import check, filesystem
from file_flags_mount import change, reject, libc

IMMUTABLE = 2
libc.setxattr.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int]
libc.setxattr.restype=ctypes.c_int


def set_attribute(path):
    if libc.setxattr(os.fsencode(path),b'user.denied',b'x',1,0,0):
        code=ctypes.get_errno();raise OSError(code,os.strerror(code))



def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint',type=Path)
    parser.add_argument('--reference-apfs',action='store_true')
    parser.add_argument('--expect-unsupported',action='store_true')
    parser.add_argument('--daemon',type=Path)
    parser.add_argument('--socket',type=Path)
    parser.add_argument('--verify-report',type=Path)
    args=parser.parse_args();target=args.mountpoint.resolve(strict=True);mounted,kind=filesystem(target)
    check(kind==('apfs' if args.reference_apfs else 'forkfs'),'unexpected filesystem')
    if not args.reference_apfs:check(target==mounted and target.stat().st_mode&0o777==0o700,'requires private forkfs root')
    if args.verify_report:
        report=json.loads(args.verify_report.read_text());ns=report['namespace']
        check(ns.startswith('/forkfs-immutable-') and len(Path(ns).parts)==2,'invalid namespace')
        path=target/ns.lstrip('/')/'file'
        check(path.stat().st_flags&IMMUTABLE and path.read_bytes()==b'data','immutable revision changed')
        check((target/ns.lstrip('/')/'dir').stat().st_flags&IMMUTABLE,'directory flag lost')
        reject(lambda:change(path,0),errno.EROFS)
        print(json.dumps({'immutable_after_gc_readonly_remount':True,'readonly_flag_clear_rejected':True}));return
    if not args.reference_apfs and not args.expect_unsupported:check(args.daemon and args.socket,'requires daemon/socket')
    def rpc(*fields,revision=None):
        view=['--revision',revision] if revision else ['--world','main']
        result=subprocess.run([str(args.daemon.resolve(strict=True)),'request',str(args.socket.resolve(strict=True)),*map(str,fields),*view],check=True,capture_output=True,timeout=30)
        return json.loads(result.stdout)
    root=Path(tempfile.mkdtemp(prefix='forkfs-immutable-',dir=target));fd=-1;report={'filesystem':kind,'namespace':'/'+root.name,'denied':{}}
    try:
        path=root/'file';path.write_bytes(b'data');os.link(path,root/'alias');(root/'source').write_bytes(b'source')
        fd=os.open(path,os.O_RDWR)
        if args.expect_unsupported:
            report={'prior_immutable_rejected':True,'errno':reject(lambda:change(path,IMMUTABLE),errno.ENOTSUP)}
            check(path.read_bytes()==b'data' and path.stat().st_flags==0,'failed flags changed file')
            print(json.dumps(report));return
        change(path,IMMUTABLE);check((root/'alias').stat().st_flags&IMMUTABLE,'hardlink flags differ')
        def open_writer():
            fresh=os.open(path,os.O_WRONLY);os.close(fresh)
        operations={'new_write_open':open_writer,'chmod':lambda:os.chmod(path,0o400),'utime':lambda:os.utime(path,ns=(1,2)),
                    'link':lambda:os.link(path,root/'extra'),'unlink':lambda:path.unlink(),
                    'rename_source':lambda:path.rename(root/'moved'),'rename_over_target':lambda:os.replace(root/'source',path),
                    'setxattr':lambda:set_attribute(path)}
        for name,fn in operations.items():report['denied'][name]=reject(fn,errno.EPERM)
        check(path.read_bytes()==b'data' and (root/'source').read_bytes()==b'source','denied mutation changed data')
        directory=root/'dir';directory.mkdir();child=directory/'child';child.write_bytes(b'child');change(directory,IMMUTABLE)
        for name,fn in {'directory_create':lambda:(directory/'new').touch(),'directory_unlink':lambda:child.unlink(),
                        'directory_rename_out':lambda:child.rename(root/'out'),'directory_rename_in':lambda:(root/'source').rename(directory/'in')}.items():
            report['denied'][name]=reject(fn,errno.EPERM)
        child.write_bytes(b'updated');check(child.read_bytes()==b'updated','immutable directory revoked child data rights')
        if not args.reference_apfs:
            report['revision']='immutable_accept_'+root.name.removeprefix('forkfs-immutable-').replace('-','_')
            rpc('fs-snapshot',report['revision'])
        # Darwin retains rights granted before the flag was set.
        check(os.pwrite(fd,b'OLD',0)==3,'old writer lost admission');os.ftruncate(fd,6);os.fsync(fd)
        check(os.pread(fd,6,0)==b'OLDa\0\0','admitted writer data/size changed')
        change(path,0);fresh=os.open(path,os.O_WRONLY);os.write(fresh,b'new');os.close(fresh)
        if not args.reference_apfs:
            check(rpc('fs-stat','/'+root.name+'/file',revision=report['revision'])['flags']&IMMUTABLE,'live clear changed revision')
            rpc('fs-check');report['revision_isolation']=True
        report['admitted_writer_retains_data_and_size_rights']=True;report['clear_allows_new_writer']=True
    finally:
        if fd>=0:os.close(fd)
        for name in ['file','alias','dir']:
            try:change(root/name,0)
            except OSError:pass
        shutil.rmtree(root)
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
