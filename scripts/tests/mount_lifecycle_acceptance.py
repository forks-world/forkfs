"""Disposable-root lifecycle acceptance; kernel mount/RPC observations are mocked.

UID drops, detached workers, root deathwatch, EOF, durable records and cleanup
are real. This fixture must never be reported as signed FSKit mounted acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import subprocess
import sys
import tempfile
import time

from agent_supervisor_acceptance import CLEAN, PYTHON, WORKER

DRIVER = r'''
import errno, json, os, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import agent_supervisor as supervisor
import mount_lifecycle, mount_probe, mount_session
stage = Path(__file__).parent
policy = json.loads((stage/'policy.json').read_bytes())
flag, fault = stage/'mounted', stage/'unmount-fails'
mount_probe.descriptor_mount_info = lambda fd: dict(fstype='forkrevision', fskit=True,
    fsid=policy['mount_fsid'], mountpoint=policy['mount'], read_only=False)
mount_probe.descriptor_volume_uuid = lambda fd: mount_lifecycle.expected_session_uuid('a'*32,'b'*32)
mount_probe.mounted_fsids = lambda: {tuple(policy['mount_fsid'])} if flag.exists() else set()
def unmount(fd):
    if fault.exists(): raise OSError(errno.EBUSY, 'fixture busy mount')
    flag.unlink()
mount_probe.funmount_fd = unmount
def rpc(endpoint, service_uid, fields, timeout=10):
    if fields[0] == 'mount-check':
        if (stage/'session-lost').exists(): raise RuntimeError('fixture session revoked')
        return dict(generation='a'*32, name='main', read_only=False)
    if fields == ['service-hello']: return dict(protocol=1, generation='c'*32)
    if fields == ['mount-retire','a'*32,'b'*32,'--generation','c'*32]:
        (stage/'retirement-observed').write_text('retired')
        return dict(generation='c'*32)
    raise AssertionError(fields)
mount_session.request = rpc
# The independent root child executes this same trusted fixture driver, so its
# mocked kernel/RPC boundary agrees with the supervisor's. No production option
# permits these substitutions.
supervisor.__file__ = str(Path(__file__).absolute())
supervisor.launcher_command = lambda python, policy, command: [str(python),'-I','-S',
    str(stage/'worker.py'),str(policy),*command]
raise SystemExit(supervisor.main())
'''


def acceptance(host_user, worker_user):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('requires root on a disposable macOS runner')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'fskit'))
    import agent_supervisor as supervisor
    import mount_lifecycle
    host, worker = pwd.getpwnam(host_user), pwd.getpwnam(worker_user)
    service, frontend = pwd.getpwnam('daemon'), pwd.getpwnam('_spotlight')
    if len({0, host.pw_uid, worker.pw_uid, service.pw_uid, frontend.pw_uid}) != 5:
        raise RuntimeError('fixture requires distinct identities')
    if supervisor.uid_processes(worker.pw_uid): raise RuntimeError('worker identity is occupied')
    checks, children = [], []
    with tempfile.TemporaryDirectory(prefix='ForkfsLifecycleTest-', dir='/Library') as temp:
        stage = Path(temp); stage.chmod(0o755)
        for filename in ('agent_supervisor.py','dedicated_workload.py','restricted_workload.py',
                         'mount_probe.py','mount_session.py','mount_lifecycle.py'):
            shutil.copyfile(Path(__file__).resolve().parents[1]/'fskit'/filename, stage/filename)
            (stage/filename).chmod(0o644)
        for filename, content in (('driver.py',DRIVER),('worker.py',WORKER)):
            (stage/filename).write_text(content); (stage/filename).chmod(0o644)
        state=stage/'state'; state.mkdir(); state.chmod(0o711)
        mount=stage/'work'; mount.mkdir(mode=0o700); os.chown(mount,worker.pw_uid,worker.pw_gid)
        backing=stage/'store'; backing.mkdir(mode=0o700); os.chown(backing,service.pw_uid,service.pw_gid)
        policy=dict(version=1,host_uid=host.pw_uid,service_uid=service.pw_uid,
                    frontend_uid=frontend.pw_uid,worker_uid=worker.pw_uid,worker_gid=worker.pw_gid,
                    mount=str(mount),mount_fsid=[7,9],backing=str(backing),read_only=[])
        policy_file=stage/'policy.json'; policy_file.write_text(json.dumps(policy)); policy_file.chmod(0o644)
        record=stage/'lifecycle.json'; active=state/f'{worker.pw_uid}.active'
        def reset():
            if active.exists() or supervisor.uid_processes(worker.pw_uid): raise AssertionError('fixture did not quiesce')
            value=dict(version=1,phase='bound',policy_sha256=hashlib.sha256(
                json.dumps(policy,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
                mount=str(mount),fsid=[7,9],volume_uuid=mount_lifecycle.expected_session_uuid('a'*32,'b'*32),service_uid=service.pw_uid,socket=str(stage/'rpc.sock'),
                generation='a'*32,session='b'*32,name='main',read_only=False)
            record.write_text(json.dumps(value)); record.chmod(0o600)
            (stage/'mounted').touch()
            for name in ('session-lost','unmount-fails','retirement-observed'):
                (stage/name).unlink(missing_ok=True)
        def start(command=None, recover=False):
            args=[PYTHON,'-I','-S',str(stage/'driver.py'),'--policy',str(policy_file),
                  '--state',str(state),'--mount-lifecycle',str(record)]
            args += ['--recover'] if recover else ['--timeout','20','--',*(command or ['/usr/bin/true'])]
            process=subprocess.Popen(args,cwd='/',env=CLEAN,stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            children.append(process); return process
        def finish(process):
            out,err=process.communicate(timeout=30)
            if not out: raise AssertionError((process.returncode,err))
            return json.loads(out)
        def wait_for(predicate, seconds=25):
            deadline=time.monotonic()+seconds
            while not predicate():
                if time.monotonic()>=deadline: raise AssertionError('lifecycle did not reach expected state')
                time.sleep(.05)
        def retired():
            return (json.loads(record.read_bytes())['phase']=='retired' and not active.exists()
                    and not supervisor.uid_processes(worker.pw_uid))
        try:
            reset(); report=finish(start(['/usr/bin/printf','managed']))
            if report['status']!='completed' or report['stdout']!='managed' or not retired(): raise AssertionError(report)
            if (stage/'mounted').exists() or not (stage/'retirement-observed').exists(): raise AssertionError('unmount/retire missing')
            checks.append('managed-normal-unmount-before-retirement')

            reset(); process=start(['/bin/sleep','60'])
            wait_for(lambda: bool(supervisor.uid_processes(worker.pw_uid)))
            (stage/'session-lost').touch(); report=finish(process)
            if report.get('reason')!='mount_lost' or not retired(): raise AssertionError(report)
            checks.append('session-loss-stops-worker-and-retires')

            reset(); ready=mount/'detached-ready'
            process=start([PYTHON,'-I','-S','-c',
                'import os,time\nfrom pathlib import Path\nif os.fork()==0:\n os.setsid();Path('+repr(str(ready))+').write_text(str(os.getpid()))\ntime.sleep(60)'])
            wait_for(lambda: ready.exists() and len(supervisor.uid_processes(worker.pw_uid))>=2)
            process.kill(); process.communicate(timeout=10)
            wait_for(retired)
            if (stage/'mounted').exists(): raise AssertionError('deathwatch left the mount live')
            checks.append('parent-SIGKILL-independent-deathwatch-and-detached-UID-reap')

            reset(); (stage/'unmount-fails').touch(); report=finish(start())
            if report.get('status')!='blocked' or not active.exists(): raise AssertionError(report)
            if supervisor.uid_processes(worker.pw_uid): raise AssertionError('quarantined worker still alive')
            if (stage/'retirement-observed').exists(): raise AssertionError('busy mount was retired')
            (stage/'unmount-fails').unlink()
            # The EOF child may already recover once the injected failure is
            # removed. Otherwise the administrator uses the identical protocol.
            if active.exists():
                recovery=finish(start(recover=True))
                if recovery.get('status') not in ('recovered','blocked'): raise AssertionError(recovery)
            wait_for(retired)
            checks.append('unmount-failure-quarantine-and-retry')
        finally:
            for process in children:
                if process.poll() is None: process.kill()
                process.communicate(timeout=10)
            supervisor.reap_identity(policy,None,PYTHON,CLEAN)
    return dict(checks=checks,mounted_acceptance=False,kernel_mount_and_rpc_mocked=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-user',required=True);parser.add_argument('--worker-user',default='_www')
    args=parser.parse_args()
    print(json.dumps(acceptance(args.host_user,args.worker_user),sort_keys=True))
