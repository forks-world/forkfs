#!/usr/bin/env python3
"""Exercise actual POSIX syscalls on an already mounted forkfs World.

Refuses an ordinary directory. Creates and removes only a unique test directory.
An optional checkpoint is retained in the repository, since revision deletion is
not implemented. Use a disposable repository for acceptance testing.
"""
import argparse
import ctypes
import errno
import json
import os
import plistlib
from pathlib import Path
import shutil
import stat
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

# Apple's bundled Python may omit os.*xattr. Call the native Darwin APIs so
# acceptance still covers both pathname and open-file-descriptor syscalls.
libc=ctypes.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True)
for operation,argument_types,return_type in [
    ('setxattr',[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int],ctypes.c_int),
    ('getxattr',[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_int],ctypes.c_ssize_t),
    ('listxattr',[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int],ctypes.c_ssize_t),
    ('removexattr',[ctypes.c_char_p,ctypes.c_int],ctypes.c_int),
]:
    for prefix,first in [('',ctypes.c_char_p),('f',ctypes.c_int)]:
        function=getattr(libc,prefix+operation);function.argtypes=[first,*argument_types];function.restype=return_type

def xattr_call(operation,target,*args):
    fd=isinstance(target,int);function=getattr(libc,('f' if fd else '')+operation)
    result=function(target if fd else os.fsencode(target),*args)
    if result<0:
        code=ctypes.get_errno();raise OSError(code,os.strerror(code))
    return result

def setxattr(target,name,value,flags=0):
    data=ctypes.create_string_buffer(value);xattr_call('setxattr',target,os.fsencode(name),data,len(value),0,flags)

def getxattr(target,name):
    name=os.fsencode(name);size=xattr_call('getxattr',target,name,None,0,0,0)
    if not size:return b''
    data=ctypes.create_string_buffer(size);length=xattr_call('getxattr',target,name,data,size,0,0);return data.raw[:length]

def listxattr(target):
    size=xattr_call('listxattr',target,None,0,0)
    if not size:return []
    data=ctypes.create_string_buffer(size);length=xattr_call('listxattr',target,data,size,0)
    return [os.fsdecode(name) for name in data.raw[:length].split(b'\0') if name]

def removexattr(target,name):xattr_call('removexattr',target,os.fsencode(name),0)
XATTR_CREATE=2
XATTR_REPLACE=4

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
    attr='user.forkfs';attr_value=b'xattr\0binary\xff';tags=plistlib.dumps(['Revision\n6'],fmt=plistlib.FMT_BINARY)
    setxattr(file,attr,attr_value,flags=XATTR_CREATE)
    setxattr(file,'com.apple.metadata:_kMDItemUserTags',tags)
    assert getxattr(file,attr)==attr_value and getxattr(file,'com.apple.metadata:_kMDItemUserTags')==tags
    assert attr in listxattr(file) and rpc('fs-xattr-get',namespace+'/file',attr)==attr_value
    rejects(lambda:setxattr(file,attr,b'duplicate',flags=XATTR_CREATE),errno.EEXIST)
    rejects(lambda:setxattr(file,'user.too-big',bytes(65537)),errno.E2BIG)
    rejects(lambda:setxattr(file,b'bad\xff',b'x'),errno.EINVAL)
    rejects(lambda:getxattr(file,b'bad\xff'),errno.EINVAL)
    setxattr(file,'user.empty',b'');assert getxattr(file,'user.empty')==b'';removexattr(file,'user.empty')
    report['checks'].append('native binary/empty xattrs, Finder tags, create policy and size limit')
    old_inode=os.fstat(fd).st_ino
    os.link(file,root/'alias')
    assert (root/'alias').stat().st_ino==old_inode and file.stat().st_nlink==2
    assert getxattr(root/'alias',attr)==attr_value
    os.symlink('file',root/'symlink')
    assert os.readlink(root/'symlink')=='file' and (root/'symlink').read_bytes()==payload
    report['checks'].append('hardlink identity and symlink traversal')
    atime,mtime,birth=1234567890123456789,1345678901234567890,1123456789000000000
    os.utime(file,ns=(atime,mtime));assert file.stat().st_atime_ns==atime and file.stat().st_mtime_ns==mtime
    os.utime(fd,ns=(atime+1,mtime+1));atime+=1;mtime+=1
    class AttrList(ctypes.Structure):
        _fields_=[('count',ctypes.c_uint16),('reserved',ctypes.c_uint16),('common',ctypes.c_uint32),('volume',ctypes.c_uint32),('directory',ctypes.c_uint32),('file',ctypes.c_uint32),('fork',ctypes.c_uint32)]
    class Timespec(ctypes.Structure):
        _fields_=[('seconds',ctypes.c_long),('nanoseconds',ctypes.c_long)]
    attributes=AttrList(5,0,0x00000200,0,0,0,0) # ATTR_CMN_CRTIME from Darwin sys/attr.h
    stamp=Timespec(birth//1000000000,birth%1000000000)
    libc.setattrlist.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32];libc.setattrlist.restype=ctypes.c_int
    assert libc.setattrlist(os.fsencode(file),ctypes.byref(attributes),ctypes.byref(stamp),ctypes.sizeof(stamp),0)==0,ctypes.get_errno()
    metadata=json.loads(rpc('fs-stat',namespace+'/file'))
    assert [metadata[k] for k in ['accessed_ns','modified_ns','birth_ns']]==[atime,mtime,birth]
    assert file.stat().st_atime_ns==atime and file.stat().st_mtime_ns==mtime and int(file.stat().st_birthtime)==birth//1000000000
    report['timestamp_ns']={'accessed_ns':atime,'modified_ns':mtime,'birth_ns':birth}
    report['checks'].append('pathname/fd utime nanoseconds and native creation time reach revision inode')
    target_before=json.loads(rpc('fs-stat',namespace+'/file'))
    link=root/'symlink';link_atime,link_mtime,link_birth=111111111111111111,222222222222222222,100000000000000000
    os.utime(link,ns=(link_atime,link_mtime),follow_symlinks=False)
    libc.lchmod.argtypes=[ctypes.c_char_p,ctypes.c_uint32];libc.lchmod.restype=ctypes.c_int
    assert libc.lchmod(os.fsencode(link),0o701)==0,ctypes.get_errno()
    link_stamp=Timespec(link_birth//1000000000,link_birth%1000000000)
    assert libc.setattrlist(os.fsencode(link),ctypes.byref(attributes),ctypes.byref(link_stamp),ctypes.sizeof(link_stamp),1)==0,ctypes.get_errno() # FSOPT_NOFOLLOW
    link_metadata=json.loads(rpc('fs-lstat',namespace+'/symlink'))
    assert link_metadata['mode']==0o701 and [link_metadata[k] for k in ['accessed_ns','modified_ns','birth_ns']]==[link_atime,link_mtime,link_birth]
    assert json.loads(rpc('fs-stat',namespace+'/file'))==target_before
    retained_link_fd=os.open(link,os.O_RDONLY|0x00200000)
    try:
        setxattr(retained_link_fd,'user.link-retained',b'retained\0link\xff')
        assert getxattr(retained_link_fd,'user.link-retained')==b'retained\0link\xff'
    finally:os.close(retained_link_fd)
    dangling=root/'dangling-attrs';copied=root/'copied-link';os.symlink('missing-target',dangling)
    subprocess.run(['/usr/bin/touch','-h',str(dangling)],check=True)
    os.utime(dangling,ns=(link_atime,link_mtime),follow_symlinks=False)
    metadata_fd=os.open(dangling,os.O_RDWR|0x00200000) # O_SYMLINK from Darwin sys/fcntl.h
    duplicate_fd=-1
    try:
        assert stat.S_ISLNK(os.fstat(metadata_fd).st_mode)
        setxattr(metadata_fd,'user.link-fd',b'link\0metadata\xff')
        os.fchmod(metadata_fd,0o701);os.utime(metadata_fd,ns=(link_atime,link_mtime))
        rejects(lambda:os.read(metadata_fd,1),errno.EPERM)
        rejects(lambda:os.write(metadata_fd,b'x'),errno.EPERM)
        rejects(lambda:os.ftruncate(metadata_fd,0),errno.EINVAL)
        subprocess.run(['/bin/cp','-Pp',str(dangling),str(copied)],check=True)
        assert os.readlink(copied)=='missing-target' and copied.lstat().st_mtime_ns==dangling.lstat().st_mtime_ns
        copied_fd=os.open(copied,os.O_RDONLY|0x00200000)
        try:
            assert getxattr(copied_fd,'user.link-fd')==b'link\0metadata\xff'
        finally:os.close(copied_fd)
        os.unlink(dangling);os.symlink('replacement-target',dangling)
        assert os.fstat(metadata_fd).st_nlink==0 and getxattr(metadata_fd,'user.link-fd')==b'link\0metadata\xff'
        setxattr(metadata_fd,'user.link-fd',b'orphan-link');os.utime(metadata_fd,ns=(1,2))
        duplicate_fd=os.dup(metadata_fd);os.close(metadata_fd);metadata_fd=-1
        assert getxattr(duplicate_fd,'user.link-fd')==b'orphan-link' and os.fstat(duplicate_fd).st_mtime_ns==2
        assert os.readlink(dangling)=='replacement-target';rpc('fs-check')
    finally:
        if metadata_fd>=0:os.close(metadata_fd)
        if duplicate_fd>=0:os.close(duplicate_fd)
    assert not (root/'missing-target').exists();os.unlink(copied);os.unlink(dangling)
    report['symlink_attributes']={'mode':0o701,'accessed_ns':link_atime,'modified_ns':link_mtime,'birth_ns':link_birth,'target_unchanged':True,'dangling_touch_h':True,'dangling_cp_Pp':True,'copied_binary_xattr':True,'fd_open_unlink_dup':True,'data_read_write_errno':'EPERM','truncate_errno':'EINVAL'}
    report['checks'].append('nofollow metadata, O_SYMLINK fds, cp -Pp binary xattrs and open-unlink/dup identity')
    error_dir=root/'errno-cases';error_dir.mkdir()
    try:
        (error_dir/'file-src').write_bytes(b'S');(error_dir/'file-dst').write_bytes(b'D')
        (error_dir/'dir-src').mkdir();(error_dir/'dir-dst').mkdir()
        status_before=json.loads(subprocess.check_output([daemon,'request',socket,'status']))
        rejects(lambda:os.rename(error_dir/'file-src',error_dir/'dir-dst'),errno.EISDIR)
        rejects(lambda:os.rename(error_dir/'dir-src',error_dir/'file-dst'),errno.ENOTDIR)
        rejects(lambda:os.rmdir(error_dir/'file-src'),errno.ENOTDIR)
        rejects(lambda:os.unlink(error_dir/'dir-src'),errno.EPERM)
        rejects(lambda:os.link(error_dir/'dir-src',error_dir/'hard-dir'),errno.EPERM)
        status_after=json.loads(subprocess.check_output([daemon,'request',socket,'status']))
        assert all(status_before[k]==status_after[k] for k in ['last_committed_seq','indexed_objects','object_roots'])
        assert (error_dir/'file-src').read_bytes()==b'S' and (error_dir/'file-dst').read_bytes()==b'D'
        assert {p.name for p in error_dir.iterdir()}=={'file-src','file-dst','dir-src','dir-dst'}
    finally:shutil.rmtree(error_dir)
    report['checks'].append('native rename/removal/directory-hardlink errno and failed-operation atomicity')
    if args.checkpoint:
        revision='mount_accept_'+uuid.uuid4().hex[:12]
        rpc('fs-snapshot',revision)
        os.pwrite(fd,b'CHANGED!',0);os.fsync(fd)
        assert rpc('fs-cat',namespace+'/file',revision=revision)==payload
        retained=json.loads(rpc('fs-stat',namespace+'/file',revision=revision))
        assert all(retained[k]==v for k,v in report['timestamp_ns'].items())
        assert rpc('fs-cat',namespace+'/file')!=payload
        setxattr(file,attr,b'changed',flags=XATTR_REPLACE)
        assert rpc('fs-xattr-get',namespace+'/file',attr,revision=revision)==attr_value
        assert rpc('fs-xattr-get',namespace+'/file',attr)==b'changed'
        report['retained_revision']=revision
        report['checks'].append('fsync then revision checkpoint isolates subsequent writes')
    rejects(lambda:os.ftruncate(fd,(1<<40)+1),errno.EFBIG)
    assert os.fstat(fd).st_size==len(payload)
    os.rename(file,root/'renamed')
    assert (root/'renamed').stat().st_ino==old_inode
    os.unlink(root/'renamed');os.unlink(root/'alias')
    assert os.fstat(fd).st_nlink==0
    assert getxattr(fd,attr)==(b'changed' if args.checkpoint else attr_value)
    setxattr(fd,attr,b'orphan-xattr',flags=XATTR_REPLACE);assert getxattr(fd,attr)==b'orphan-xattr'
    removexattr(fd,attr);rejects(lambda:getxattr(fd,attr),getattr(errno,'ENOATTR',errno.ENODATA))
    report['checks'].append('xattrs follow hardlink identity, revision isolation and open-unlink handles')
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
    large=root/'large.bin';large_namespace=namespace+'/large.bin'
    large_payload=bytes(range(256))*(4*1024*1024//256)
    large.write_bytes(large_payload)
    assert large.read_bytes()==large_payload
    with large.open('r+b',buffering=0) as stream:
        os.fsync(stream.fileno())
        if args.checkpoint:
            large_revision='large_mount_'+uuid.uuid4().hex[:12]
            rpc('fs-snapshot',large_revision)
            assert rpc('fs-read-at',large_namespace,str(3*1024*1024),str(256*1024),revision=large_revision)==large_payload[3*1024*1024:3*1024*1024+256*1024]
            report['large_retained_revision']=large_revision
            report['large_namespace']=large_namespace
        os.pwrite(stream.fileno(),b'TAIL',64*1024*1024-4);os.fsync(stream.fileno())
        assert os.fstat(stream.fileno()).st_size==64*1024*1024
        assert os.pread(stream.fileno(),8,64*1024*1024-8)==bytes(4)+b'TAIL'
        rejects(lambda:os.ftruncate(stream.fileno(),(1<<40)+1),errno.EFBIG)
        os.ftruncate(stream.fileno(),65537);os.ftruncate(stream.fileno(),1024*1024);os.fsync(stream.fileno())
        assert os.pread(stream.fileno(),8,65536)==bytes([0])+bytes(7)
        os.unlink(large)
        assert os.pwrite(stream.fileno(),b'ORPHAN',900000)==6;os.fsync(stream.fileno())
        assert os.pread(stream.fileno(),6,900000)==b'ORPHAN'
    report['checks'].append('4 MiB file transfer, 64 MiB sparse growth, truncate zero-fill and chunked open-unlink')
    rpc('fs-check')
finally:
    if fd is not None:os.close(fd)
    shutil.rmtree(root)
rpc('fs-check')
print(json.dumps(report,indent=2))
