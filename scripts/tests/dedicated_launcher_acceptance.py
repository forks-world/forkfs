"""Disposable-root CI acceptance of installed launcher admission and child exec.

The success-path driver mocks only mount attestation for a native fixture. The
real CLI must reject that fixture. Neither result claims mounted acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import secrets
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uid_boundary import credentials, require_success, PROBE


DRIVER = '''
import importlib.util, json, sys
from pathlib import Path
directory = Path(sys.argv[1])
sys.path.insert(0, str(directory))
import dedicated_workload as launcher
import restricted_workload as runtime
config = launcher.read_policy(directory / 'policy.json')
if sys.argv[2] == 'substitute':
    observations = iter([config['mount_fsid'], [999,999]])
    runtime.require_mount = lambda path: dict(fsid=next(observations))
else:
    runtime.require_mount = lambda path: dict(fsid=config['mount_fsid'])
raise SystemExit(launcher.run(directory / 'policy.json', sys.argv[3:]))
'''


RAW_EXEC = r'''
import ctypes, os, sys
libc = ctypes.CDLL(None, use_errno=True)
libc.execve.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p),
                       ctypes.POINTER(ctypes.c_char_p)]
libc.execve.restype = ctypes.c_int
python, launcher, policy, key, value, sentinel = sys.argv[1:]
arguments = [python, '-I', '-S', launcher, '--policy', policy, '--',
             '/usr/bin/touch', sentinel]
# Pass a native array: a Python dict would erase exactly the bug being tested.
environment = [b'PATH=/usr/bin:/bin:/usr/sbin:/sbin', b'HOME=/var/empty',
               b'LANG=C', b'LC_ALL=C']
if key == '__CF_USER_TEXT_ENCODING':
    environment.append(('__CF_USER_TEXT_ENCODING=0x%X:0:0' % os.getuid()).encode())
environment.append((key + '=' + value).encode())
argv = (ctypes.c_char_p * (len(arguments) + 1))(*map(os.fsencode, arguments), None)
envp = (ctypes.c_char_p * (len(environment) + 1))(*environment, None)
libc.execve(os.fsencode(python), argv, envp)
raise OSError(ctypes.get_errno(), 'native execve failed')
'''


def acceptance(host_user, worker_user):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('requires root on a disposable macOS runner')
    host, worker = pwd.getpwnam(host_user), pwd.getpwnam(worker_user)
    service, frontend = pwd.getpwnam('daemon'), pwd.getpwnam('_spotlight')
    if len({0,host.pw_uid,worker.pw_uid,service.pw_uid,frontend.pw_uid}) != 5:
        raise RuntimeError('CI needs four distinct non-root fixture identities')
    clean=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin',HOME='/var/empty',LANG='C',LC_ALL='C')
    developer=Path(subprocess.check_output(['/usr/bin/xcode-select','-p'],env=clean,text=True).strip())
    python=developer/'usr/bin/python3'
    sdk=subprocess.check_output(['/usr/bin/xcrun','--sdk','macosx','--show-sdk-path'],env=clean,text=True).strip()
    compiler=subprocess.check_output(['/usr/bin/xcrun','--find','clang++'],env=clean,text=True).strip()
    checks=[]
    with tempfile.TemporaryDirectory(prefix='ForkfsLauncherTest-',dir='/Library') as name:
        stage=Path(name);stage.chmod(0o755)
        for filename in ('dedicated_workload.py','restricted_workload.py','mount_probe.py'):
            shutil.copyfile(Path(__file__).resolve().parents[1]/'fskit'/filename,stage/filename)
            (stage/filename).chmod(0o644)
        (stage/'driver.py').write_text(DRIVER);(stage/'driver.py').chmod(0o644)
        mount=stage/'work';mount.mkdir(mode=0o700);os.chown(mount,worker.pw_uid,worker.pw_gid)
        backing=stage/'store';backing.mkdir(mode=0o700);os.chown(backing,service.pw_uid,service.pw_gid)
        (backing/'secret').write_text('private backing fixture')
        policy=dict(version=1,host_uid=host.pw_uid,service_uid=service.pw_uid,
                    frontend_uid=frontend.pw_uid,worker_uid=worker.pw_uid,worker_gid=worker.pw_gid,
                    mount=str(mount),mount_fsid=[7,9],backing=str(backing),read_only=[str(developer),sdk])
        policy_file=stage/'policy.json';policy_file.write_text(json.dumps(policy));policy_file.chmod(0o644)
        def invoke(argv,account=worker,environment=None,**options):
            return subprocess.run([str(python),'-I','-S',*map(str,argv)],cwd='/',env=environment or clean,
                    stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=60,
                    **dict(credentials(account),**options))
        cli=[stage/'dedicated_workload.py','--policy',policy_file,'--','/usr/bin/true']
        native=invoke(cli)
        if native.returncode!=2 or 'not an attested forkfs' not in native.stderr:
            diagnostic=invoke(['-c', 'import ctypes,json,os,platform,subprocess,tempfile; '
                'print(json.dumps(dict(keys=sorted(os.environ),'
                'encoding=os.environ.get("__CF_USER_TEXT_ENCODING"),uid=os.getuid())))'])
            # This child was execed with the fixed four-variable clean fixture;
            # print only key names and generated encoding, never ambient values.
            raise RuntimeError('real CLI failed to reject ordinary directory: '+native.stderr+
                               '; clean-exec runtime metadata: '+diagnostic.stdout)
        checks.append('real-cli-native-directory-denial')
        # Each identity/config failure must precede the mount gate.
        wrong=invoke(cli,account=host)
        if wrong.returncode!=2 or 'supervisor must drop' not in wrong.stderr:
            raise RuntimeError('wrong caller UID admitted: '+wrong.stderr)
        checks.append('wrong-uid-denial')
        # Darwin setgid replaces the first group slot. Keep the worker primary
        # group first so the host group survives as an actual extra permission.
        inherited_groups=(worker.pw_gid,host.pw_gid)
        group_probe=require_success(invoke(['-c',
            'import json,sys; sys.path.insert(0,sys.argv[1]); import dedicated_workload; '
            'print(json.dumps(sorted(dedicated_workload.kernel_groups())))',stage],
            extra_groups=inherited_groups), 'extra-group fixture')
        if host.pw_gid not in json.loads(group_probe.stdout):
            raise RuntimeError('extra-group fixture did not retain the host group')
        extra=invoke(cli,extra_groups=inherited_groups)
        if extra.returncode!=2 or 'supervisor must drop' not in extra.stderr:
            raise RuntimeError('inherited host group admitted: '+extra.stderr)
        checks.append('supplementary-group-denial')
        policy_file.chmod(0o666)
        writable=invoke(cli);policy_file.chmod(0o644)
        if writable.returncode!=2 or 'root-owned' not in writable.stderr:
            raise RuntimeError('writable policy admitted: '+writable.stderr)
        checks.append('writable-policy-denial')
        alias=stage/'alias.json';alias.symlink_to(policy_file)
        linked=invoke([stage/'dedicated_workload.py','--policy',alias,'--','/usr/bin/true'])
        if linked.returncode!=2 or 'root-owned' not in linked.stderr:
            raise RuntimeError('symlink policy admitted: '+linked.stderr)
        checks.append('symlink-policy-denial')
        for key in ('PATH','HOME','LANG','LC_ALL','__CF_USER_TEXT_ENCODING'):
            sentinel=mount/('duplicate-'+key)
            duplicated=invoke(['-c',RAW_EXEC,python,stage/'dedicated_workload.py',
                               policy_file,key,'disposable-hidden-marker',sentinel])
            if (duplicated.returncode!=2 or 'duplicate' not in duplicated.stderr
                    or sentinel.exists()):
                raise RuntimeError('native duplicate environment was not rejected for '+key+': '+duplicated.stderr)
        checks.append('native-execve-duplicate-environment-denial')
        # The success path below uses the actual installed modules/profile/spawn.
        # Mount attestation alone is mocked in a separate, visibly test-only driver.
        prefix=[stage/'driver.py',stage,'stable']
        result=require_success(invoke([*prefix,'/usr/bin/id','-u']), 'installed child UID')
        if result.stdout.strip()!=str(worker.pw_uid):raise RuntimeError('child has wrong UID')
        checks.append('production-spawn-worker-uid-with-mocked-mount')
        changed=invoke([stage/'driver.py',stage,'substitute','/usr/bin/touch',mount/'unexpected'])
        if changed.returncode==0 or 'mount changed' not in changed.stderr or (mount/'unexpected').exists():
            raise RuntimeError('substituted mount reached workload: '+changed.stderr)
        checks.append('mount-substitution-denial')
        source=mount/'hello.cc';source.write_text('#include <iostream>\nint main(){std::cout<<"dedicated-build-ok";}\n')
        os.chown(source,worker.pw_uid,worker.pw_gid)
        require_success(invoke([*prefix,compiler,'-std=c++20','-isysroot',sdk,source,'-o',mount/'hello']), 'production compile/link')
        result=require_success(invoke([*prefix,mount/'hello']), 'production execute')
        if result.stdout!='dedicated-build-ok':raise RuntimeError('incorrect linked output')
        checks.append('production-cxx-build-with-mocked-mount')
        hostile=dict(clean,OPENAI_API_KEY='disposable-test-token',SSH_AUTH_SOCK='/host/ssh',
                     FORKFS_SOCKET='/host/control',NODE_OPTIONS='--require=/host/hook',
                     PYTHONPATH='/host/injection',GIT_CONFIG_COUNT='1')
        rejected=invoke([*prefix,'/usr/bin/touch',mount/'unsafe-environment'],environment=hostile)
        if rejected.returncode==0 or 'fresh entry environment' not in rejected.stderr or (mount/'unsafe-environment').exists():
            raise RuntimeError('credential-bearing supervisor environment admitted: '+rejected.stderr)
        checks.append('credential-bearing-supervisor-denial')
        result=require_success(invoke([*prefix,str(python),'-I','-S','-c',
            'import json,os; print(json.dumps(dict(os.environ)))']), 'fresh child environment')
        inherited=json.loads(result.stdout)
        if any(key in inherited for key in set(hostile)-set(clean)):
            raise RuntimeError('host credential/control/injection environment inherited')
        checks.append('production-fresh-environment')
        token=secrets.token_hex(32)
        marker=subprocess.Popen([str(python),'-I','-S','-c',
            'import time; print("ready",flush=True); time.sleep(120)',token],cwd='/',env=clean,
            stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
            **credentials(host))
        try:
            import select
            ready,_,_=select.select([marker.stdout],[],[],15)
            if not ready or marker.stdout.readline().strip()!='ready' or marker.poll() is not None:
                raise RuntimeError('host marker did not start')
            result=require_success(invoke([*prefix,str(python),'-I','-S','-c',PROBE,
                'deny',str(marker.pid),token]), 'production host-argv denial')
            if marker.poll() is not None or json.loads(result.stdout)['uid']!=worker.pw_uid:
                raise RuntimeError('invalid production argv-denial probe')
            checks.append('production-host-argv-denial-with-mocked-mount')
        finally:
            if marker.poll() is None:marker.terminate()
            try:marker.communicate(timeout=10)
            except subprocess.TimeoutExpired:marker.kill();marker.communicate(timeout=10)
        denied=invoke([*prefix,'/bin/cat',backing/'secret'])
        if denied.returncode!=1 or denied.stdout:raise RuntimeError('backing contents exposed')
        checks.append('production-backing-denial-with-mocked-mount')
    return dict(status='passed',mounted_acceptance=False,mount_attestation_mocked_on_success=True,
                checks=checks)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-user',required=True);parser.add_argument('--worker-user',required=True)
    args=parser.parse_args()
    try:result=acceptance(args.host_user,args.worker_user)
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='blocked',mounted_acceptance=False,error=str(error))));return 2
    print(json.dumps(result,indent=2));return 0


if __name__=='__main__':raise SystemExit(main())
