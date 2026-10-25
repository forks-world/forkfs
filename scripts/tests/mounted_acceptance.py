"""Actual mounted World acceptance. Ordinary directories cannot pass attestation."""
import argparse
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import unicodedata
import select
import stat
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
from mount_probe import require_mount,descriptor_mount_info,descriptor_volume_uuid

def workload_environment(home):
    # Workload children get only a system PATH and a private HOME. In
    # particular, credentials, proxy settings and inherited Git config cannot
    # silently affect either filesystem's result.
    return {'PATH':os.defpath,'HOME':str(home),'GIT_CONFIG_NOSYSTEM':'1',
            'GIT_CONFIG_GLOBAL':'/dev/null','GIT_TEMPLATE_DIR':'/dev/null',
            'LC_ALL':'C'}

def command(directory,*args):
    return subprocess.run(list(args),cwd=directory,env=workload_environment(directory),capture_output=True,check=True,timeout=60).stdout

def pinned_mount_identity(fd):
    info=descriptor_mount_info(fd)
    if info['fstype']!='forkrevision' or not info['fskit']:
        raise RuntimeError('pinned descriptor is not on the ForkFS FSKit volume')
    return dict(fsid=list(info['fsid']),volume_uuid=descriptor_volume_uuid(fd))

def attest_mount_identity(path,pinned_fd,expected):
    info=require_mount(path)
    if list(info['fsid'])!=expected['fsid']:
        raise RuntimeError('mounted path FSID changed during acceptance')
    current=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC)
    try:current_identity=pinned_mount_identity(current)
    finally:os.close(current)
    pinned_identity=pinned_mount_identity(pinned_fd)
    if current_identity!=expected or pinned_identity!=expected:
        raise RuntimeError('mounted filesystem identity changed during acceptance')
    return expected

def _remove_tree_contents(directory_fd,expected_device):
    for name in os.listdir(directory_fd):
        info=os.stat(name,dir_fd=directory_fd,follow_symlinks=False)
        if stat.S_ISDIR(info.st_mode):
            if info.st_dev!=expected_device:
                raise RuntimeError('refusing to traverse a workspace mount boundary')
            child=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC|os.O_NOFOLLOW,dir_fd=directory_fd)
            try:
                opened=os.fstat(child)
                if opened.st_dev!=expected_device or (opened.st_dev,opened.st_ino)!=(info.st_dev,info.st_ino):
                    raise RuntimeError('workspace child identity changed')
                _remove_tree_contents(child,expected_device)
            finally:os.close(child)
            current=os.stat(name,dir_fd=directory_fd,follow_symlinks=False)
            if not stat.S_ISDIR(current.st_mode) or (current.st_dev,current.st_ino)!=(info.st_dev,info.st_ino):
                raise RuntimeError('workspace child identity changed')
            os.rmdir(name,dir_fd=directory_fd)
        else:
            # Symlinks and other nondirectories are unlinked as entries; their
            # targets are never opened or traversed.
            current=os.stat(name,dir_fd=directory_fd,follow_symlinks=False)
            if (current.st_dev,current.st_ino)!=(info.st_dev,info.st_ino):
                raise RuntimeError('workspace child identity changed')
            os.unlink(name,dir_fd=directory_fd)

def safe_remove_owned(root_fd,name,device,inode,token):
    info=os.stat(name,dir_fd=root_fd,follow_symlinks=False)
    if not stat.S_ISDIR(info.st_mode) or (info.st_dev,info.st_ino)!=(device,inode):
        raise RuntimeError('workspace cleanup identity changed')
    workspace=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC|os.O_NOFOLLOW,dir_fd=root_fd)
    try:
        current=os.fstat(workspace)
        if (current.st_dev,current.st_ino)!=(device,inode):raise RuntimeError('workspace cleanup identity changed')
        marker=os.open('.owner',os.O_RDONLY|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK,dir_fd=workspace)
        try:
            marker_info=os.fstat(marker)
            if not stat.S_ISREG(marker_info.st_mode):raise RuntimeError('workspace ownership marker is not a regular file')
            actual=os.read(marker,128).decode('ascii')
        finally:os.close(marker)
        if actual!=token:raise RuntimeError('workspace ownership marker changed')
        _remove_tree_contents(workspace,device)
    finally:os.close(workspace)
    # Both the parent descriptor and the final inode check keep cleanup on the
    # original filesystem and refuse a replacement directory at this name.
    current=os.stat(name,dir_fd=root_fd,follow_symlinks=False)
    if not stat.S_ISDIR(current.st_mode) or (current.st_dev,current.st_ino)!=(device,inode):
        raise RuntimeError('workspace cleanup identity changed')
    os.rmdir(name,dir_fd=root_fd)

def correctness(root):
    result={};data=bytes(range(256))*65536;file=root/'large';file.write_bytes(data)
    with file.open('r+b') as f:
        with mmap.mmap(f.fileno(),0) as mapped:mapped[65535:65539]=b'fork';mapped.flush()
        os.fsync(f.fileno())
    expected=data[:65535]+b'fork'+data[65539:]
    if file.read_bytes()!=expected:raise AssertionError('mmap/fsync content mismatch')
    result['mmap_fsync_sha256']=hashlib.sha256(expected).hexdigest()
    os.link(file,root/'alias');fd=os.open(file,os.O_RDWR)
    try:
        file.unlink();os.pwrite(fd,b'old',0);replacement=root/'replacement';replacement.write_bytes(b'new');os.replace(replacement,root/'alias')
        if os.pread(fd,3,0)!=b'old' or (root/'alias').read_bytes()!=b'new':raise AssertionError('retained old inode changed after replace')
        os.ftruncate(fd,65537);os.ftruncate(fd,131072)
        if os.pread(fd,65535,65537)!=bytes(65535):raise AssertionError('truncated bytes returned after regrow')
    finally:os.close(fd)
    lock=root/'lock';lock.touch()
    with lock.open('r+b') as f:
        fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
        code='import errno,fcntl,sys\nf=open(sys.argv[1],"r+b")\ntry: fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\nexcept OSError as e: sys.exit(73 if e.errno in (errno.EAGAIN,errno.EWOULDBLOCK) else 74)'
        other=subprocess.run([sys.executable,'-I','-c',code,str(lock)],capture_output=True,timeout=10,env=workload_environment(root))
        if other.returncode!=73:raise AssertionError(('exclusive lock failed',other.returncode,other.stderr.decode()))
    result['retained_inode']=True;result['truncate_regrow']=True;result['cross_process_lock']=True
    return result

def metadata_contract(root,require_forkfs):
    directory=root/'metadata';directory.mkdir()
    original='漢字-🧪-control-\x01.txt';renamed='改名-🚀-control-\x02.txt';payload=b'filename-bytes\x00\xff'
    source=directory/original
    unicode_exact=False;unicode_issue=None
    try:
        with source.open('xb') as f:f.write(payload)
        if source.read_bytes()!=payload:raise AssertionError('Unicode/control filename readback changed bytes')
        target=directory/renamed;os.rename(source,target)
        if target.read_bytes()!=payload or os.fsencode(target.name)!=os.fsencode(renamed):
            raise AssertionError('Unicode/control filename rename or byte spelling changed')
        if os.fsencode(renamed) not in {os.fsencode(name) for name in os.listdir(directory)}:
            raise AssertionError('renamed Unicode/control filename missing from readdir')
        unicode_exact=True
    except (OSError,AssertionError) as error:
        if require_forkfs:raise
        unicode_issue=repr(error)

    def pair(first,second):
        outcomes=[]
        for index,name in enumerate((first,second)):
            path=directory/name
            try:
                with path.open('xb') as f:f.write(str(index).encode())
                outcomes.append(True)
            except FileExistsError:outcomes.append(False)
            except OSError:
                if require_forkfs:raise
                outcomes.append(False)
        names={os.fsencode(name) for name in os.listdir(directory)}
        distinct=all(outcomes) and os.fsencode(first) in names and os.fsencode(second) in names
        if require_forkfs and not distinct:raise AssertionError('ForkFS must preserve distinct filename byte spellings')
        return distinct

    nfc=unicodedata.normalize('NFC','café.txt');nfd=unicodedata.normalize('NFD','café.txt')
    normalization_distinct=pair(nfc,nfd)
    case_sensitive=pair('ForkFS-Case.txt','forkfs-case.txt')
    result=dict(unicode_control_exact=unicode_exact,nfc_nfd_distinct=normalization_distinct,
                case_sensitive=case_sensitive)
    if unicode_issue:result['unicode_control_issue']=unicode_issue
    return result

def crossprocess_atomic_replace(root):
    directory=root/'crossprocess';directory.mkdir();target=directory/'payload.bin'
    old=bytes(range(251))*17+b'old-tail';new=bytes(reversed(range(251)))*19+b'new-tail'
    target.write_bytes(old)
    reader_code=(
        'import hashlib,json,os,sys\n'
        'path=sys.argv[1];old_expected=bytes.fromhex(sys.argv[2]);new_expected=bytes.fromhex(sys.argv[3])\n'
        'fd=os.open(path,os.O_RDONLY);os.write(1,b"READY\\n")\n'
        'if sys.stdin.buffer.readline()!=b"go\\n":raise RuntimeError("missing writer handshake")\n'
        'parts=[];offset=0\n'
        'while offset<len(old_expected):\n part=os.pread(fd,len(old_expected)-offset,offset)\n'
        ' if not part:break\n parts.append(part);offset+=len(part)\n'
        'old=b"".join(parts);fresh=open(path,"rb").read();os.close(fd)\n'
        'result={"old_matches":old==old_expected,"fresh_matches":fresh==new_expected,\n'
        '"old_sha256":hashlib.sha256(old).hexdigest(),"fresh_sha256":hashlib.sha256(fresh).hexdigest()}\n'
        'print(json.dumps(result),flush=True)\n')
    writer_code=(
        'import os,sys,uuid\n'
        'path=sys.argv[1];data=bytes.fromhex(sys.argv[2]);directory=os.path.dirname(path)\n'
        'temp=os.path.join(directory,".replace-"+uuid.uuid4().hex)\n'
        'fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)\n'
        'try:\n view=memoryview(data)\n while view:\n  count=os.write(fd,view)\n  if count<=0:raise OSError("short write")\n  view=view[count:]\n os.fsync(fd)\nfinally:os.close(fd)\n'
        'os.replace(temp,path);dfd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY)\n'
        'try:os.fsync(dfd)\nfinally:os.close(dfd)\n')
    env=workload_environment(root)
    reader=subprocess.Popen([sys.executable,'-I','-c',reader_code,str(target),old.hex(),new.hex()],
                            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                            env=env,close_fds=True)
    try:
        ready,_,_=select.select([reader.stdout],[],[],10)
        if not ready or reader.stdout.readline()!=b'READY\n':raise AssertionError('cross-process reader did not pin old file')
        subprocess.run([sys.executable,'-I','-c',writer_code,str(target),new.hex()],
                       env=env,capture_output=True,check=True,timeout=10,close_fds=True)
        output,_=reader.communicate(input=b'go\n',timeout=10)
        if reader.returncode!=0:raise AssertionError(('cross-process reader failed',reader.returncode))
        result=json.loads(output)
        expected=dict(old_matches=True,fresh_matches=True,old_sha256=hashlib.sha256(old).hexdigest(),
                      fresh_sha256=hashlib.sha256(new).hexdigest())
        if result!=expected:raise AssertionError(('cross-process atomic replace mismatch',result,expected))
        return result
    finally:
        if reader.poll() is None:reader.kill();reader.communicate(timeout=5)

def development(root):
    command(root,'git','init','-q','--template=/dev/null')
    (root/'helper.c').write_text('int value(void){return 1;}\n');(root/'main.c').write_text('int value(void);int main(void){return value()!=1;}\n')
    command(root,'git','add','main.c','helper.c');command(root,'git','-c','user.name=forkfs-test','-c','user.email=test@example.invalid','commit','-qm','one')
    start=time.perf_counter_ns();command(root,'cc','main.c','helper.c','-o','app');command(root,str(root/'app'));full=time.perf_counter_ns()-start
    (root/'helper.c').write_text('int value(void){return 2;}\n');command(root,'git','add','helper.c');command(root,'git','-c','user.name=forkfs-test','-c','user.email=test@example.invalid','commit','-qm','two')
    command(root,'git','checkout','-q','HEAD~1');
    if 'return 1' not in (root/'helper.c').read_text():raise AssertionError('git checkout contents')
    start=time.perf_counter_ns();command(root,'cc','-c','helper.c','-o','helper.o');incremental=time.perf_counter_ns()-start
    return dict(full_build_ns=full,incremental_compile_ns=incremental,git_checkout=True,compiled_executable=True)

def timed(root,samples):
    raw=[];target=root/'hot';target.write_bytes(b'x')
    for kind in ('stat','save_fsync_rename'):
        for i in range(samples):
            start=time.perf_counter_ns()
            if kind=='stat':target.stat()
            else:
                temp=root/'save.tmp'
                with temp.open('wb') as f:f.write(bytes(4096));f.flush();os.fsync(f.fileno())
                os.replace(temp,target)
            raw.append(dict(workload=kind,sample=i,elapsed_ns=time.perf_counter_ns()-start))
    result={}
    for kind in ('stat','save_fsync_rename'):
        values=sorted(r['elapsed_ns'] for r in raw if r['workload']==kind)
        rank=lambda q:values[max(0,int(len(values)*q+.999999)-1)]
        result[kind]=dict(samples=len(values),p50_ns=rank(.5),p95_ns=rank(.95) if len(values)>=1000 else None,p99_ns=rank(.99) if len(values)>=1000 else None)
    return raw,result

def exercise(root,samples):return dict(correctness=correctness(root),development=development(root),timing=timed(root,samples))

def native_exercise(root,samples):
    return dict(exercise(root,samples),metadata=metadata_contract(root,False),
                crossprocess_atomic_replace=crossprocess_atomic_replace(root))

def mounted_exercise(root,samples):
    return dict(exercise(root,samples),metadata=metadata_contract(root,True),
                crossprocess_atomic_replace=crossprocess_atomic_replace(root))

def validate_paths(mount,native,output):
    mount=Path(mount).resolve(strict=True);native=Path(native).resolve(strict=True)
    if not mount.is_dir() or not native.is_dir():raise ValueError('mount and native roots must be directories')
    output=Path(output).resolve(strict=False)
    roots=(native,mount)
    if any(output==root or root in output.parents for root in roots):
        raise ValueError('output must be outside both workspaces')
    return mount,native,output

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mount',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--samples',type=int,default=1000)
    args=p.parse_args()
    if not 1<=args.samples<=1000:p.error('samples must be 1..1000')
    mount,native,args.output=validate_paths(args.mount,args.native,args.output)
    result=dict(status='blocked',mounted_acceptance=False)
    try:result['mount']=require_mount(mount)
    except Exception as error:
        result['blocker']=str(error)
        args.output.mkdir(parents=True,exist_ok=True)
        with (args.output/('blocked-'+uuid.uuid4().hex+'.json')).open('x') as f:json.dump(result,f,indent=2)
        print(json.dumps(result));return 2
    roots=[native,mount];token=uuid.uuid4().hex;owned=[];root_fds=[];identity_fd=None;baseline=None;failure=None
    args.output.mkdir(parents=True,exist_ok=True)
    try:
        native_fd=os.open(native,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC|os.O_NOFOLLOW);root_fds.append(native_fd)
        identity_fd=os.open(mount,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC|os.O_NOFOLLOW);root_fds.append(identity_fd)
        baseline=pinned_mount_identity(identity_fd)
        result['world_identity']={'fsid':list(baseline['fsid']),'volume_uuid':baseline['volume_uuid']}
        attest_mount_identity(mount,identity_fd,baseline)
        if os.fstat(native_fd).st_dev==os.fstat(identity_fd).st_dev:raise ValueError('native reference must be a different filesystem device')
        for index,root in enumerate(roots):
            root_fd=root_fds[index];name='.forkfs-accept-'+token
            os.mkdir(name,mode=0o700,dir_fd=root_fd)
            work_fd=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC|os.O_NOFOLLOW,dir_fd=root_fd)
            try:
                info=os.fstat(work_fd);owned.append((root_fd,name,info.st_dev,info.st_ino))
                marker=os.open('.owner',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC|os.O_NOFOLLOW,0o600,dir_fd=work_fd)
                try:os.write(marker,token.encode('ascii'));os.fsync(marker)
                finally:os.close(marker)
            finally:os.close(work_fd)
            path=root/name
            if index==0:result['native']=native_exercise(path,args.samples)
            else:
                result['world_identity']['before_workload']=attest_mount_identity(mount,identity_fd,baseline)
                try:result['world']=mounted_exercise(path,args.samples)
                finally:result['world_identity']['after_workload']=attest_mount_identity(mount,identity_fd,baseline)
        result.update(status='passed',mounted_acceptance=True,os_caches_dropped=False)
    except Exception as error:
        failure=error;result.update(status='failed',mounted_acceptance=False,error=repr(error))
    finally:
        cleanup_errors=[]
        for root_fd,name,device,inode in reversed(owned):
            try:safe_remove_owned(root_fd,name,device,inode,token)
            except Exception as error:cleanup_errors.append(repr(error))
        if identity_fd is not None and baseline is not None:
            try:result['world_identity']['after_cleanup']=attest_mount_identity(mount,identity_fd,baseline)
            except Exception as error:cleanup_errors.append(repr(error))
        for fd in reversed(root_fds):
            try:os.close(fd)
            except OSError as error:cleanup_errors.append(repr(error))
        if cleanup_errors:
            result.update(status='failed',mounted_acceptance=False,cleanup_errors=cleanup_errors)
            if failure is None:failure=RuntimeError('; '.join(cleanup_errors))
        result.setdefault('status','failed');result.setdefault('mounted_acceptance',False)
        try:
            with (args.output/('acceptance-'+token+'.json')).open('x') as f:json.dump(result,f,indent=2)
        except Exception as error:
            if failure is None:failure=error
    print(json.dumps(dict(status=result['status'],mounted_acceptance=result['mounted_acceptance'])))
    return 1 if failure is not None or result['status']!='passed' else 0
if __name__=='__main__':raise SystemExit(main())
