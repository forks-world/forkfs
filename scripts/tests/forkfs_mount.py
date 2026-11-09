#!/usr/bin/env python3
"""Exercise actual POSIX syscalls on an already mounted forkfs World.

Refuses an ordinary directory. Creates and removes only a unique test directory.
An optional checkpoint is retained in the repository, since revision deletion is
not implemented. Use a disposable repository for acceptance testing.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('mountpoint',type=Path)
parser.add_argument('--daemon',type=Path,required=True)
parser.add_argument('--socket',type=Path,required=True)
parser.add_argument('--world',default='main')
parser.add_argument('--checkpoint',action='store_true')
args=parser.parse_args()
mountpoint=args.mountpoint.resolve(strict=True)
listing=subprocess.run(['/sbin/mount'],check=True,capture_output=True,text=True).stdout.splitlines()
marker=' on '+str(mountpoint)+' ('
rows=[row for row in listing if marker in row]
if not rows or not any('forkfs' in row.split(marker,1)[1].split(')',1)[0].split(', ') for row in rows):
    parser.error('target is not an actual forkfs mount; refusing to write test data')
if mountpoint.stat().st_mode&0o777!=0o700:
    parser.error('mounted root must expose private mode 0700')
daemon=str(args.daemon.resolve(strict=True))
socket=str(args.socket.resolve(strict=True))
def rpc(*fields,revision=None):
    view=['--revision',revision] if revision is not None else ['--world',args.world]
    result=subprocess.run([daemon,'request',socket,*fields,*view],capture_output=True,timeout=30)
    assert result.returncode==0,(fields,result.stderr)
    return result.stdout

def rejects(operation,*codes):
    try:operation()
    except OSError as error:
        assert error.errno in codes,error
    else:raise AssertionError('operation unexpectedly succeeded')

root=Path(tempfile.mkdtemp(prefix='forkfs-accept-',dir=mountpoint))
namespace='/'+root.name
report={'mountpoint':str(mountpoint),'namespace':namespace,'checks':[]}
fd=None
try:
    file=root/'file';payload=b'revision\0binary\xffpayload'
    fd=os.open(file,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    assert os.write(fd,payload)==len(payload);os.fsync(fd)
    assert os.pread(fd,100,0)==payload
    assert rpc('fs-cat',namespace+'/file')==payload
    report['checks'].append('binary write/read/fsync reaches daemon content')
    old_inode=os.fstat(fd).st_ino
    os.link(file,root/'alias')
    assert (root/'alias').stat().st_ino==old_inode and file.stat().st_nlink==2
    os.symlink('file',root/'symlink')
    assert os.readlink(root/'symlink')=='file' and (root/'symlink').read_bytes()==payload
    report['checks'].append('hardlink identity and symlink traversal')
    if args.checkpoint:
        revision='mount_accept_'+uuid.uuid4().hex[:12]
        rpc('fs-snapshot',revision)
        os.pwrite(fd,b'CHANGED!',0);os.fsync(fd)
        assert rpc('fs-cat',namespace+'/file',revision=revision)==payload
        assert rpc('fs-cat',namespace+'/file')!=payload
        report['retained_revision']=revision
        report['checks'].append('fsync then revision checkpoint isolates subsequent writes')
    rejects(lambda:os.ftruncate(fd,256*1024+1),errno.EFBIG)
    assert os.fstat(fd).st_size==len(payload)
    os.rename(file,root/'renamed')
    assert (root/'renamed').stat().st_ino==old_inode
    os.unlink(root/'renamed');os.unlink(root/'alias')
    assert os.fstat(fd).st_nlink==0
    assert os.pwrite(fd,b'ORPHAN',0)==6;os.fsync(fd)
    assert os.pread(fd,6,0)==b'ORPHAN'
    os.ftruncate(fd,3);os.fsync(fd);assert os.fstat(fd).st_size==3
    file.write_bytes(b'new identity')
    assert file.stat().st_ino!=old_inode and os.pread(fd,3,0)==b'ORP'
    os.close(fd);fd=None
    report['checks'].append('rename/open-unlink/truncate/path reuse preserve file identity')
    # Reuse of /file intentionally retargets the symlink while the unlinked fd
    # above continued targeting the old inode.
    assert (root/'symlink').read_bytes()==b'new identity'
    (root/'subdir').mkdir(mode=0o700)
    for index in range(32):(root/f'entry{index:02d}').write_bytes(str(index).encode())
    expected={'file','symlink','subdir',*(f'entry{i:02d}' for i in range(32))}
    actual=set(os.listdir(root))
    # With no native xattrs, macOS stores metadata in AppleDouble sidecars.
    sidecars={name for name in actual-expected if name.startswith('._')}
    for name in sidecars:
        assert (root/name).read_bytes()[:4]==b'\x00\x05\x16\x07',name
    assert actual-sidecars==expected,{'missing':sorted(expected-actual),'unexpected':sorted(actual-expected-sidecars)}
    report['appledouble_sidecars']=len(sidecars)
    dots=subprocess.run(['/bin/ls','-a',str(root)],capture_output=True,text=True,check=True).stdout.splitlines()
    assert dots.count('.')==1 and dots.count('..')==1,dots
    report['checks'].append('directory enumeration and dot entries')
    replacement=root/'temporary';replacement.write_bytes(b'atomic replacement')
    with replacement.open('rb') as stream:os.fsync(stream.fileno())
    os.replace(replacement,file)
    assert file.read_bytes()==b'atomic replacement'
    assert rpc('fs-cat',namespace+'/file')==b'atomic replacement'
    report['checks'].append('atomic-save replacement reaches revision-backed namespace')
    unicode_dir=root/'中文目录📁';unicode_dir.mkdir(mode=0o700)
    unicode_file=unicode_dir/'文件📄.txt';unicode_file.write_bytes(payload)
    with unicode_file.open('rb') as stream:os.fsync(stream.fileno())
    unicode_inode=unicode_file.stat().st_ino
    unicode_alias=unicode_dir/'硬链接🔗';os.link(unicode_file,unicode_alias)
    unicode_renamed=unicode_dir/'重命名🚀.txt';os.rename(unicode_file,unicode_renamed)
    assert unicode_alias.stat().st_ino==unicode_renamed.stat().st_ino==unicode_inode
    os.symlink(unicode_renamed.name,unicode_dir/'符号链接')
    assert os.readlink(unicode_dir/'符号链接')==unicode_renamed.name
    assert (unicode_dir/'符号链接').read_bytes()==payload
    for spelling,data in [('é',b'NFC'),('e\u0301',b'NFD')]:
        path=unicode_dir/spelling;path.write_bytes(data)
        with path.open('rb') as stream:os.fsync(stream.fileno())
    assert (unicode_dir/'é').read_bytes()==b'NFC' and (unicode_dir/'e\u0301').read_bytes()==b'NFD'
    assert (unicode_dir/'é').stat().st_ino!=(unicode_dir/'e\u0301').stat().st_ino
    boundary=unicode_dir/('界'*85);boundary.write_bytes(b'255 UTF-8 bytes')
    assert boundary.read_bytes()==b'255 UTF-8 bytes'
    rejects(lambda:(unicode_dir/('界'*85+'a')).write_bytes(b'too long'),errno.ENAMETOOLONG,errno.EINVAL)
    assert {'文件📄.txt','重命名🚀.txt'}&set(os.listdir(unicode_dir))=={'重命名🚀.txt'}
    unicode_namespace=namespace+'/'+unicode_dir.name+'/'+unicode_renamed.name
    assert rpc('fs-cat',unicode_namespace)==payload
    if args.checkpoint:
        revision='unicode_mount_'+uuid.uuid4().hex[:12];rpc('fs-snapshot',revision)
        unicode_alias.write_bytes(b'changed after Unicode checkpoint')
        with unicode_alias.open('rb') as stream:os.fsync(stream.fileno())
        assert rpc('fs-cat',unicode_namespace,revision=revision)==payload
        report['unicode_retained_revision']=revision
    report['checks'].append('UTF-8 names, links, rename, NFC/NFD identity and byte limits')
    rpc('fs-check')
finally:
    if fd is not None:os.close(fd)
    shutil.rmtree(root)
rpc('fs-check')
print(json.dumps(report,indent=2))
