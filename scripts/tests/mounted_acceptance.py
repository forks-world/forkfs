"""Actual mounted World acceptance. Ordinary directories cannot pass attestation."""
import argparse
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
from mount_probe import require_mount

def command(directory,*args):
    env={k:v for k,v in os.environ.items() if not k.startswith(('GIT_','DYLD_','LD_'))}
    env.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null',GIT_TEMPLATE_DIR='/dev/null')
    return subprocess.run(list(args),cwd=directory,env=env,capture_output=True,check=True,timeout=60).stdout

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
        other=subprocess.run([sys.executable,'-c',code,str(lock)],capture_output=True,timeout=10)
        if other.returncode!=73:raise AssertionError(('exclusive lock failed',other.returncode,other.stderr.decode()))
    result['retained_inode']=True;result['truncate_regrow']=True;result['cross_process_lock']=True
    return result

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
    roots=[native,mount];token=uuid.uuid4().hex;owned=[]
    if roots[0].stat().st_dev==roots[1].stat().st_dev:raise ValueError('native reference must be a different filesystem device')
    args.output.mkdir(parents=True,exist_ok=True)
    try:
        for root in roots:
            path=root/('.forkfs-accept-'+token);path.mkdir(mode=0o700);(path/'.owner').write_text(token);owned.append((path,path.stat().st_dev,path.stat().st_ino));result['native' if root==roots[0] else 'world']=exercise(path,args.samples)
        result.update(status='passed',mounted_acceptance=True,os_caches_dropped=False)
    except Exception as error:
        result.update(status='failed',error=repr(error))
        raise
    finally:
        for path,device,inode in owned:
            if path.is_symlink() or path.stat().st_dev!=device or path.stat().st_ino!=inode or (path/'.owner').read_text()!=token:raise RuntimeError('workspace cleanup identity changed')
            shutil.rmtree(path)
        with (args.output/('acceptance-'+token+'.json')).open('x') as f:json.dump(result,f,indent=2)
    print(json.dumps(dict(status=result['status'],mounted_acceptance=result['mounted_acceptance'])));return 0
if __name__=='__main__':raise SystemExit(main())
