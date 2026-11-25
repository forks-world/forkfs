#!/usr/bin/env python3
"""Qualify native shared/blocking locks with explicit child-process handshakes.

Owned scratch directory only. No daemon RPC locks, fairness/deadlock guarantee,
network/distributed lock claim. OFD locks are a separate capability probe.
"""
import argparse
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import platform
import select
import shutil
import signal
import subprocess
import sys
import tempfile
from filesystem_compat import check, filesystem


class Flock(ctypes.Structure):
    _fields_=[('start',ctypes.c_int64),('length',ctypes.c_int64),('pid',ctypes.c_int32),('type',ctypes.c_int16),('whence',ctypes.c_int16)]


def raw_fcntl(fd, command, request):
    lib=ctypes.CDLL('/usr/lib/libSystem.B.dylib',use_errno=True)
    lib.fcntl.argtypes=[ctypes.c_int,ctypes.c_int]
    lib.fcntl.restype=ctypes.c_int
    result=lib.fcntl(fd,command,ctypes.byref(request))
    return result,ctypes.get_errno() if result==-1 else 0


def lock(fd, kind, mode, nonblocking=False, start=0):
    op={'shared':fcntl.LOCK_SH,'exclusive':fcntl.LOCK_EX,'unlock':fcntl.LOCK_UN}[mode]
    if nonblocking:op|=fcntl.LOCK_NB
    if kind=='flock':fcntl.flock(fd,op)
    else:fcntl.lockf(fd,op,8,start,os.SEEK_SET)


def child(args):
    fd=os.open(args.mountpoint,os.O_RDWR)
    try:
        if args.child=='interrupt':
            signal.signal(signal.SIGUSR1,lambda *_:None);signal.siginterrupt(signal.SIGUSR1,True)
            # Darwin sys/fcntl.h: F_WRLCK=3. ctypes makes one raw call, without
            # Python's automatic EINTR restart around fcntl/lockf wrappers.
            request=Flock(0,8,0,3,os.SEEK_SET)
            print('ready',flush=True)
            result,code=raw_fcntl(fd,fcntl.F_SETLKW,request)
            check(result==-1 and code==errno.EINTR,'blocking fcntl did not return EINTR')
            print('interrupted',flush=True);return
        print('ready',flush=True)
        try:lock(fd,args.kind,args.mode,args.child=='try',args.start)
        except OSError as error:
            check(args.child=='try' and error.errno in (errno.EAGAIN,errno.EACCES),'unexpected lock error')
            print('conflict',flush=True);return
        print('acquired',flush=True)
        if args.child=='hold':check(sys.stdin.readline().strip()=='release','missing release command')
    finally:os.close(fd)


class Child:
    def __init__(self,path,kind,mode='exclusive',action='try',start=0):
        self.proc=subprocess.Popen([sys.executable,str(Path(__file__).resolve()),str(path),'--child',action,
                                    '--kind',kind,'--mode',mode,'--start',str(start)],stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
        try:check(self.line()=='ready','missing child readiness')
        except BaseException:self.stop();raise

    def line(self,timeout=5):
        check(select.select([self.proc.stdout],[],[],timeout)[0],'child deadline exceeded')
        data=b''
        while not data.endswith(b'\n'):
            part=os.read(self.proc.stdout.fileno(),1);check(part,'child exited before reply');data+=part
        return data.decode().strip()

    def blocked(self):
        check(not select.select([self.proc.stdout],[],[],0.2)[0] and self.proc.poll() is None,'lock did not block')

    def finish(self,release=False):
        if release:self.proc.stdin.write(b'release\n');self.proc.stdin.flush()
        try:
            self.proc.wait(timeout=5);check(self.proc.returncode==0,self.proc.stderr.read().decode() or 'child failed')
        finally:
            if self.proc.poll() is None:self.proc.kill();self.proc.wait(timeout=5)
            self.proc.stdin.close();self.proc.stdout.close();self.proc.stderr.close()

    def stop(self):
        if self.proc.poll() is None:self.proc.kill()
        self.proc.wait(timeout=5)
        for pipe in [self.proc.stdin,self.proc.stdout,self.proc.stderr]:pipe.close()


def qualify(root,kind):
    path=root/kind;path.write_bytes(b'unchanged');fd=os.open(path,os.O_RDWR);children=[];dup=-1;report={}
    def probe(mode,expected,start=0):
        c=Child(path,kind,mode,start=start);children.append(c);check(c.line()==expected,'wrong conflict result');c.finish();children.remove(c)
    try:
        lock(fd,kind,'shared');probe('shared','acquired');probe('exclusive','conflict')
        if kind=='lockf':probe('exclusive','acquired',16)
        report['shared_coexistence_and_exclusive_conflict']=True
        lock(fd,kind,'unlock');lock(fd,kind,'exclusive')
        waiter=Child(path,kind,action='hold');children.append(waiter);waiter.blocked()
        lock(fd,kind,'unlock');check(waiter.line()=='acquired','waiter not awakened on unlock');waiter.finish(True);children.remove(waiter)
        report['blocking_waiter_awakened_by_unlock']=True
        lock(fd,kind,'exclusive');dup=os.dup(fd)
        if kind=='flock':
            os.close(fd);fd=-1;probe('exclusive','conflict')
            waiter=Child(path,kind,action='hold');children.append(waiter);waiter.blocked();os.close(dup);dup=-1
            check(waiter.line()=='acquired','last dup close did not release flock');waiter.finish(True);children.remove(waiter)
            report['last_description_close_releases']=True
        else:
            os.close(dup);dup=-1;probe('exclusive','acquired')
            report['any_process_fd_close_releases']=True
            lock(fd,kind,'exclusive');waiter=Child(path,kind,action='interrupt');children.append(waiter);waiter.blocked()
            waiter.proc.send_signal(signal.SIGUSR1);check(waiter.line()=='interrupted','missing EINTR');waiter.finish();children.remove(waiter)
            probe('exclusive','conflict');lock(fd,kind,'unlock');probe('exclusive','acquired')
            report['F_SETLKW_signal_EINTR_owner_lock_preserved']=True
        check(path.read_bytes()==b'unchanged','advisory locking changed file bytes')
        report['file_content_preserved']=True;return report
    finally:
        for c in children:c.stop()
        if dup>=0:os.close(dup)
        if fd>=0:os.close(fd)


def qualify_ofd(root):
    path = root / 'ofd'
    path.write_bytes(b'unchanged')
    fd = os.open(path, os.O_RDWR)
    duplicate = -1
    other = -1
    # Darwin SDK sys/fcntl.h ABI values, for Python builds without these names.
    setlk = getattr(fcntl, 'F_OFD_SETLK', 90)
    getlk = getattr(fcntl, 'F_OFD_GETLK', 92)

    def request(start=0):
        return Flock(start, 8, 0, fcntl.F_WRLCK, os.SEEK_SET)

    def call(descriptor, command, record):
        result, code = raw_fcntl(descriptor, command, record)
        if result == -1:
            raise OSError(code, os.strerror(code))

    def check_conflict():
        try:
            call(other, setlk, request())
        except OSError as error:
            check(error.errno in (errno.EAGAIN, errno.EACCES),
                  'unexpected OFD conflict errno %s' % error.errno)
        else:
            raise RuntimeError('independent OFD lock unexpectedly acquired')

    try:
        try:
            call(fd, setlk, request())
        except OSError as error:
            if error.errno in (errno.EINVAL, errno.ENOSYS, errno.ENOTSUP, errno.EOPNOTSUPP):
                return {'status': 'unsupported', 'errno': error.errno, 'error': str(error)}
            raise
        duplicate = os.dup(fd)
        call(duplicate, setlk, request())
        other = os.open(path, os.O_RDWR)
        check_conflict()
        found = request()
        call(other, getlk, found)
        actual = (found.type, found.start, found.length, found.pid)
        check(actual == (fcntl.F_WRLCK, 0, 8, -1), 'unexpected OFD GETLK result: %s' % (actual,))
        call(other, setlk, request(16))
        os.close(fd)
        fd = -1
        check_conflict()
        os.close(duplicate)
        duplicate = -1
        call(other, setlk, request())
        check(path.read_bytes() == b'unchanged', 'OFD locking changed file bytes')
        return {'status': 'passed', 'duplicate_reapply': True, 'independent_conflict': True,
                'getlk_write_range_and_pid': True, 'disjoint_range': True,
                'last_duplicate_close_releases': True, 'file_content_preserved': True}
    finally:
        for descriptor in (fd, duplicate, other):
            if descriptor >= 0:
                os.close(descriptor)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mountpoint',type=Path)
    parser.add_argument('--reference-apfs',action='store_true');parser.add_argument('--child',choices=['try','hold','interrupt'],help=argparse.SUPPRESS)
    parser.add_argument('--kind',choices=['flock','lockf'],help=argparse.SUPPRESS);parser.add_argument('--mode',choices=['shared','exclusive'],default='exclusive',help=argparse.SUPPRESS)
    parser.add_argument('--start',type=int,default=0,help=argparse.SUPPRESS);parser.add_argument('--worker',choices=['flock','lockf','ofd'],help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.child:child(args);return 0
    if args.worker:print(json.dumps(qualify_ofd(args.mountpoint) if args.worker=='ofd' else qualify(args.mountpoint,args.worker)));return 0
    check(sys.platform=='darwin','requires Darwin native locks');target=args.mountpoint.resolve(strict=True);mounted,kind=filesystem(target)
    check(kind==('apfs' if args.reference_apfs else 'forkfs'),'unexpected filesystem')
    if not args.reference_apfs:check(target==mounted and target.stat().st_mode&0o777==0o700,'requires private forkfs root')
    root=Path(tempfile.mkdtemp(prefix='forkfs-locks-',dir=target));report={'schema':1,'platform':platform.platform(),'filesystem':kind,'cases':{},'capability_probes':{},
        'limits':['native kernel locks, not daemon RPC locks','no fairness/deadlock or distributed guarantee','OFD is a scoped capability probe','single-user private mount, small files']}
    try:
        for case in ['flock','lockf','ofd']:
            group=report['capability_probes'] if case=='ofd' else report['cases']
            try:
                result=subprocess.run([sys.executable,str(Path(__file__).resolve()),str(root),'--worker',case],capture_output=True,text=True,timeout=45)
                check(result.returncode==0,result.stderr.strip() or 'lock worker failed');details=json.loads(result.stdout)
                status=details.pop('status','passed');group[case]={'status':status,'details':details}
            except (RuntimeError,subprocess.TimeoutExpired) as error:group[case]={'status':'failed','error':str(error)}
    finally:shutil.rmtree(root)
    print(json.dumps(report,indent=2));return int(any(v['status']!='passed' for v in report['cases'].values()) or
                                                any(v['status']=='failed' for v in report['capability_probes'].values()))


if __name__=='__main__':sys.exit(main())
