"""Root-only disposable macOS supervisor acceptance; no real mounted claim.

Only the worker success driver's mount attestation is mocked. Credential drops,
process observation/reaping, sandboxing, locks and persistent markers are real.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import signal
import subprocess
import sys
import tempfile
import time

CLEAN = dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin', HOME='/var/empty', LANG='C', LC_ALL='C')
PYTHON = '/Library/Developer/CommandLineTools/usr/bin/python3'
DRIVER = '''
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent))
import agent_supervisor as supervisor
supervisor.launcher_command = lambda python, policy, command: [str(python),'-I','-S',
    str(Path(__file__).parent/'worker.py'),str(policy),*command]
raise SystemExit(supervisor.main())
'''
WORKER = '''
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent))
import dedicated_workload as launcher
import restricted_workload as runtime
config = launcher.read_policy(Path(sys.argv[1]))
runtime.require_mount = lambda path: dict(fsid=config['mount_fsid'])
raise SystemExit(launcher.run(Path(sys.argv[1]),sys.argv[2:]))
'''


def acceptance(host_user, worker_user):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('requires root on a disposable macOS runner')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'fskit'))
    import agent_supervisor as supervisor
    host, worker = pwd.getpwnam(host_user), pwd.getpwnam(worker_user)
    service, frontend = pwd.getpwnam('daemon'), pwd.getpwnam('_spotlight')
    if len({0,host.pw_uid,worker.pw_uid,service.pw_uid,frontend.pw_uid}) != 5:
        raise RuntimeError('requires four distinct nonroot fixture identities')
    if supervisor.uid_processes(worker.pw_uid):raise RuntimeError('fixture worker UID already occupied')
    checks = []
    with tempfile.TemporaryDirectory(prefix='ForkfsSupervisorTest-', dir='/Library') as name:
        stage = Path(name);stage.chmod(0o755)
        for filename in ('agent_supervisor.py','dedicated_workload.py','restricted_workload.py','mount_probe.py'):
            shutil.copyfile(Path(__file__).resolve().parents[1]/'fskit'/filename,stage/filename)
            (stage/filename).chmod(0o644)
        for filename, content in (('driver.py',DRIVER),('worker.py',WORKER)):
            (stage/filename).write_text(content);(stage/filename).chmod(0o644)
        state=stage/'state';state.mkdir(mode=0o711);state.chmod(0o711)
        mount=stage/'work';mount.mkdir(mode=0o700);os.chown(mount,worker.pw_uid,worker.pw_gid)
        backing=stage/'store';backing.mkdir(mode=0o700);os.chown(backing,service.pw_uid,service.pw_gid)
        policy=dict(version=1,host_uid=host.pw_uid,service_uid=service.pw_uid,
                    frontend_uid=frontend.pw_uid,worker_uid=worker.pw_uid,worker_gid=worker.pw_gid,
                    mount=str(mount),mount_fsid=[7,9],backing=str(backing),read_only=[])
        policy_file=stage/'policy.json';policy_file.write_text(json.dumps(policy));policy_file.chmod(0o644)
        active=state/f'{worker.pw_uid}.active'
        supervisors=[]
        def start(command, timeout=10, real=False):
            child=subprocess.Popen([PYTHON,'-I','-S',str(stage/('agent_supervisor.py' if real else 'driver.py')),
                '--policy',str(policy_file),'--state',str(state),'--timeout',str(timeout),'--',*command],
                cwd='/',env=CLEAN,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            supervisors.append(child)
            return child
        def finish(child):
            out, err=child.communicate(timeout=25)
            if not out:raise AssertionError(err.decode(errors='replace'))
            report=json.loads(out)
            if report.get('status')!='completed':raise AssertionError(report)
            if not report['identity_idle'] or supervisor.uid_processes(worker.pw_uid):raise AssertionError(report)
            if active.exists() or list(state.glob(f'{worker.pw_uid}-*')):raise AssertionError('session leaked')
            return report
        def idle_cleanup():
            supervisor.reap_identity(policy,None,PYTHON,CLEAN)
        try:
            report=finish(start(['/usr/bin/true'],real=True))
            if report['returncode']!=2 or 'not an attested forkfs' not in report['stderr']:
                raise AssertionError(report)
            checks.append('real-cli-refuses-native-directory')
            report=finish(start(['/usr/bin/printf','hello']))
            if report['stdout']!='hello' or report['returncode']!=0:raise AssertionError(report)
            checks.append('real-drop-launch-and-output')
            report=finish(start(['/bin/sh','-c','exit 7']))
            if report['returncode']!=7:raise AssertionError(report)
            checks.append('nonzero-status')
            # Detached descendant and inherited pipes must not outlive the lease.
            script='import os,time\nif os.fork()==0:\n os.setsid();time.sleep(60)\nelse:\n time.sleep(.2)'
            report=finish(start([PYTHON,'-I','-S','-c',script]))
            if report['returncode']!=0:raise AssertionError(report)
            checks.append('detached-descendant-cleanup')
            report=finish(start(['/bin/sleep','60'],timeout=.5))
            if report['reason']!='timeout':raise AssertionError(report)
            checks.append('timeout-cleanup')
            report=finish(start([PYTHON,'-I','-S','-c',"import os\nwhile True:os.write(1,b'x'*65536)"]))
            if report['reason']!='output_limit' or len(report['stdout'])>supervisor.MAX_OUTPUT:
                raise AssertionError('output limit failed')
            checks.append('output-flood-cleanup')
            first=start(['/bin/sleep','60'])
            deadline=time.monotonic()+10
            while not active.exists():
                if first.poll() is not None:raise AssertionError(first.communicate())
                if time.monotonic()>deadline:raise AssertionError('no active marker')
                time.sleep(.05)
            second=start(['/usr/bin/true'])
            out,err=second.communicate(timeout=10)
            if second.returncode!=2 or json.loads(out)['status']!='blocked':raise AssertionError((out,err))
            first.send_signal(signal.SIGTERM)
            if finish(first)['reason']!='interrupted':raise AssertionError('signal ignored')
            checks.append('exclusive-lease-and-signal-cleanup')
            first=start(['/bin/sleep','60'])
            deadline=time.monotonic()+10
            while not supervisor.uid_processes(worker.pw_uid):
                if first.poll() is not None:raise AssertionError(first.communicate())
                if time.monotonic()>deadline:raise AssertionError('worker did not start')
                time.sleep(.05)
            first.kill();first.communicate(timeout=10)
            second=start(['/usr/bin/true']);out,err=second.communicate(timeout=10)
            if second.returncode!=2 or 'active/crashed' not in json.loads(out).get('error',''):
                raise AssertionError((out,err))
            checks.append('crashed-supervisor-refuses-reuse')
        finally:
            # Fixture-only explicit recovery. Production intentionally has no
            # automatic active-marker deletion or UID retirement command.
            for child in supervisors:
                if child.poll() is None:child.terminate()
            for child in supervisors:
                try:child.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    child.kill();child.communicate(timeout=5)
            idle_cleanup()
    return dict(checks=checks,mounted_acceptance=False,mount_attestation_mocked_for_success=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--host-user',required=True);parser.add_argument('--worker-user',default='_www')
    args=parser.parse_args()
    print(json.dumps(acceptance(args.host_user,args.worker_user),sort_keys=True))
