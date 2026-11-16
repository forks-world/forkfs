#!/usr/bin/env python3
"""Exercise POSIX FIFO creation and transient stream I/O on an already mounted filesystem."""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import stat
from filesystem_compat import check,filesystem

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('mountpoint',type=Path)
parser.add_argument('--reference-apfs',action='store_true')
args=parser.parse_args()
mountpoint=args.mountpoint.resolve(strict=True)
mounted,kind=filesystem(mountpoint)
check(kind==('apfs' if args.reference_apfs else 'forkfs'),'unexpected containing filesystem: '+kind)

root=Path(tempfile.mkdtemp(prefix='forkfs-fifo-',dir=mountpoint))
try:
    fifo=root/'pipe';previous_umask=os.umask(0)
    try:os.mkfifo(fifo,0o620)
    finally:os.umask(previous_umask)
    info=fifo.stat();assert stat.S_ISFIFO(info.st_mode) and info.st_mode&0o777==0o620 and info.st_size==0
    masked=root/'masked';previous_umask=os.umask(0o027)
    try:os.mkfifo(masked,0o666)
    finally:os.umask(previous_umask)
    assert masked.stat().st_mode&0o777==0o640;masked.unlink()
    try:
        unexpected=os.open(fifo,os.O_WRONLY|os.O_NONBLOCK);os.close(unexpected)
    except OSError as error:assert error.errno==errno.ENXIO,error
    else:raise AssertionError('nonblocking writer opened without reader')

    reader_code=r'''import errno,json,os,sys,time
p=sys.argv[1];fd=os.open(p,os.O_RDONLY|os.O_NONBLOCK);result={"no_writer_read":None,"active_empty_read":None,"payload":None,"eof":None,"seek":None}
try:
 result["no_writer_read"]="eof" if os.read(fd,1)==b"" else "unexpected_data"
 print("READY",flush=True)
 if sys.stdin.readline().strip()!="GO":raise RuntimeError("missing writer handshake")
 try:os.read(fd,1)
 except OSError as e:result["active_empty_read"]=e.errno
 print("EMPTY",flush=True)
 if sys.stdin.readline().strip()!="DATA":raise RuntimeError("missing data handshake")
 deadline=time.monotonic()+8;payload=bytearray()
 while time.monotonic()<deadline:
  try:
   data=os.read(fd,64)
   if data:payload.extend(data)
   if len(payload)>=20:break
  except BlockingIOError:pass
  time.sleep(.01)
 result["payload"]=bytes(payload).hex()
 deadline=time.monotonic()+8
 while time.monotonic()<deadline:
  try:
   if os.read(fd,1)==b"":result["eof"]=True;break
  except BlockingIOError:pass
  time.sleep(.01)
 try:os.lseek(fd,0,os.SEEK_SET)
 except OSError as e:result["seek"]=e.errno
 print(json.dumps(result),flush=True)
finally:os.close(fd)
'''
    child=subprocess.Popen([sys.executable,'-c',reader_code,str(fifo)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        import select
        deadline=time.monotonic()+8
        line=''
        while time.monotonic()<deadline:
            if select.select([child.stdout],[],[],.1)[0]:line=child.stdout.readline();break
            if child.poll() is not None:raise AssertionError(child.communicate())
        assert line.strip()=='READY','reader did not open FIFO'
        writer=os.open(fifo,os.O_WRONLY|os.O_NONBLOCK)
        child.stdin.write('GO\n');child.stdin.flush()
        assert select.select([child.stdout],[],[],8)[0] and child.stdout.readline().strip()=='EMPTY','reader did not test active empty stream'
        linked=root/'alias';os.link(fifo,linked);assert linked.stat().st_ino==fifo.stat().st_ino and linked.stat().st_nlink==2
        moved=root/'renamed';fifo.rename(moved);assert moved.stat().st_ino==linked.stat().st_ino
        linked.unlink();moved.unlink();assert not linked.exists() and not moved.exists()
        assert stat.S_ISFIFO(os.fstat(writer).st_mode),'open FIFO descriptor changed identity after unlink'
        child.stdin.write('DATA\n');child.stdin.flush()
        try:
            try:os.lseek(writer,0,os.SEEK_SET)
            except OSError as error:assert error.errno==errno.ESPIPE,error
            else:raise AssertionError('FIFO writer unexpectedly seekable')
            payload=b'kernel fifo stream\x00\xff';assert os.write(writer,payload)==len(payload)
        finally:os.close(writer)
        rest,errors=child.communicate(timeout=8);assert child.returncode==0,errors
        result=json.loads(rest.splitlines()[-1]);assert result=={'no_writer_read':'eof','active_empty_read':errno.EAGAIN,'payload':payload.hex(),'eof':True,'seek':errno.ESPIPE},result
    finally:
        if child.poll() is None:child.kill();child.wait()

    print(json.dumps({'filesystem':'apfs' if args.reference_apfs else 'forkfs','checks':['mkfifo type/mode/size','ENXIO without reader','EOF without writer','EAGAIN with active writer','binary pipe data','EOF after writer close','ESPIPE','open descriptor after hardlink/rename/unlink']}))
finally:
    shutil.rmtree(root)
