"""Disposable-root, distinct-UID acceptance of the FSKit RPC resource path.

Only the driver's security-scope grant is simulated. No actual mounted volume.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'fskit'))
import mount_session

CLEAN=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin',HOME='/var/empty',LANG='C',LC_ALL='C')
PYTHON='/Library/Developer/CommandLineTools/usr/bin/python3'


def credentials(account):
    return dict(user=account.pw_uid,group=account.pw_gid,extra_groups=(),close_fds=True,umask=0o077)


def acceptance(build, frontend_user):
    if platform.system()!='Darwin' or os.getuid()!=0:raise RuntimeError('requires disposable macOS root')
    service=pwd.getpwnam('daemon');worker=pwd.getpwnam('_www');frontend=pwd.getpwnam(frontend_user)
    if len({0,service.pw_uid,worker.pw_uid,frontend.pw_uid})!=4:raise RuntimeError('requires distinct fixture UIDs')
    binary=build/'daemon/forkfsd';driver=build/'forkfsd_fskit_transport_test'
    def run(args,account,**options):
        result=subprocess.run(list(map(str,args)),cwd='/',env=CLEAN,stdin=subprocess.DEVNULL,
            capture_output=True,text=True,timeout=30,**credentials(account),**options)
        if result.returncode:raise AssertionError((result.returncode,result.stdout,result.stderr))
        return result
    with tempfile.TemporaryDirectory(prefix='ForkfsTransportTest-',dir='/Library') as name:
        stage=Path(name);stage.chmod(0o755)
        # Stage executables outside runner home so fixture identities can execute.
        import shutil
        for source,target in ((binary,stage/'forkfsd'),(driver,stage/'driver')):
            shutil.copyfile(source,target);target.chmod(0o755)
        binary=stage/'forkfsd';driver=stage/'driver'
        backing=stage/'backing';backing.mkdir(mode=0o700);os.chown(backing,service.pw_uid,service.pw_gid)
        control=stage/'control';control.mkdir(mode=0o711);control.chmod(0o711);os.chown(control,service.pw_uid,service.pw_gid)
        store=backing/'store';run([binary,'init',store],service)
        daemon=subprocess.Popen([str(binary),'serve',str(store),'--socket',str(control/'rpc.sock'),'--rpc',
            '--control-uid','0','--frontend-uid',str(frontend.pw_uid)],cwd='/',env=CLEAN,
            stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,**credentials(service))
        try:
            deadline=time.monotonic()+10
            while not (control/'rpc.sock').exists():
                if daemon.poll() is not None:raise AssertionError(daemon.communicate())
                if time.monotonic()>deadline:raise AssertionError('service startup timeout')
                time.sleep(.02)
            mount_session.request(control/'rpc.sock',service.pw_uid,['fs-init'])
            result=mount_session.provision(control,service.pw_uid,frontend.pw_uid,frontend.pw_gid,'main',lease_ms=3000)
            assert 'session' not in json.dumps(result),result
            marker=control/mount_session.MARKER
            # Worker cannot read the credential, access backing data or use RPC.
            probe='''import os,socket,sys
for path in sys.argv[1:3]:
 try:
  fd=os.open(path,os.O_RDONLY);os.close(fd)
 except PermissionError:pass
 else:raise SystemExit('worker accessed protected data')
s=socket.socket(socket.AF_UNIX);s.settimeout(2);s.connect(sys.argv[3])
try:s.sendall(b'FFRPC001'+(4).to_bytes(4,'little')+(1).to_bytes(4,'little'));result=s.recv(16)
except (ConnectionResetError,BrokenPipeError):result=b''
if result:raise SystemExit('worker RPC was admitted')
'''
            run([PYTHON,'-I','-S','-c',probe,marker,store,control/'rpc.sock'],worker)
            # Frontend can use its session via real callbacks but cannot reach
            # backing files, even if it knows the exact path.
            run([PYTHON,'-I','-S','-c',"import os,sys\ntry:os.open(sys.argv[1],os.O_RDONLY)\nexcept PermissionError:pass\nelse:raise SystemExit('frontend accessed backing')",store],frontend)
            result=run([driver,'--resource',control],frontend)
            assert result.stdout.strip()=='OK',result.stdout
            mount_session.request(control/'rpc.sock',service.pw_uid,['fs-snapshot','base'])
            marker.unlink()  # fixture retired the first loader/session explicitly
            mount_session.provision(control,service.pw_uid,frontend.pw_uid,frontend.pw_gid,'base',revision=True,lease_ms=3000)
            result=run([driver,'--resource',control],frontend)
            assert result.stdout.strip()=='OK',result.stdout

        finally:
            daemon.terminate()
            try:daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:daemon.kill();daemon.wait(timeout=5)
    return dict(mounted_acceptance=False,security_scope_grant_mocked=True,
        checks=['frontend-only-token','worker-RPC-denied','frontend-backing-denied',
                'frontend-management-denied','resource-loader-RPC-callbacks','revision-readonly'])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('build',type=Path);parser.add_argument('--frontend-user',required=True)
    args=parser.parse_args();print(json.dumps(acceptance(args.build.resolve(),args.frontend_user)))
