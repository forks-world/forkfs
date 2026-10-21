"""Harness validation; native runs are deliberately not mounted acceptance."""
import json
import fcntl
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import socket
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
import mount_probe
from restricted_workload import profile,validate_backing,validate_owner_lease,validate_stdio
from restricted_workload import workload_environment
import mounted_acceptance

class Harness(unittest.TestCase):
    def test_environment_does_not_inherit_credentials_or_control_endpoints(self):
        secrets=dict(OPENAI_API_KEY='host-token',AWS_SECRET_ACCESS_KEY='cloud-token',
                     SSH_AUTH_SOCK='/host/ssh',FORKFS_SOCKET='/host/control',
                     HTTPS_PROXY='http://credential@proxy',BASH_ENV='/host/startup',
                     ENV='/host/startup',HOME='/host/home',PATH='/host/bin',
                     GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='core.sshCommand',
                     GIT_CONFIG_VALUE_0='host-program',NODE_OPTIONS='--require=/host/script')
        with tempfile.TemporaryDirectory() as name,patch.dict(os.environ,secrets):
            environment=workload_environment(name)
            for key in secrets:
                if key not in ('HOME','PATH'):self.assertNotIn(key,environment)
            self.assertEqual(environment['PATH'],'/usr/bin:/bin:/usr/sbin:/sbin')
            home=Path(environment['HOME']);self.assertEqual(home.parent,Path(name).resolve())
            self.assertEqual(list(home.iterdir()),[])
            self.assertEqual(home.stat().st_mode&0o777,0o700)
            self.assertTrue(Path(environment['TMPDIR']).is_dir())
            self.assertEqual(environment['GIT_CONFIG_GLOBAL'],'/dev/null')

    def test_attestation_rejects_fakes(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name)
            valid=dict(fstype='forkrevision',fskit=True,mountpoint=name,read_only=False)
            for change in (dict(fstype='apfs'),dict(fskit=False),dict(mountpoint='/'),dict(read_only=True)):
                with patch.object(mount_probe,'mount_info',return_value=dict(valid,**change)):
                    with self.assertRaises(RuntimeError):mount_probe.require_mount(root)
            with patch.object(mount_probe,'mount_info',return_value=valid):mount_probe.require_mount(root)
    def test_native_workload_oracle(self):
        with tempfile.TemporaryDirectory() as name:
            result=mounted_acceptance.exercise(Path(name),3)
            self.assertTrue(result['correctness']['cross_process_lock'])
            self.assertTrue(result['development']['git_checkout'])
            self.assertIsNone(result['timing'][1]['stat']['p99_ns'])
    def test_invalid_output_is_rejected_before_creating_any_hierarchy(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mount=root/'mount';native=root/'native'
            mount.mkdir();native.mkdir()
            outputs=(native/'new-parent'/'result',mount/'new-parent'/'result')
            for output in outputs:
                argv=['mounted_acceptance.py','--mount',str(mount),'--native',str(native),'--output',str(output)]
                with patch.object(sys,'argv',argv),patch.object(mounted_acceptance,'require_mount') as attest:
                    with self.assertRaisesRegex(ValueError,'output must be outside'):
                        mounted_acceptance.main()
                    attest.assert_not_called()
                self.assertFalse(output.parent.exists())
            alias=root/'native-alias';alias.symlink_to(native,target_is_directory=True)
            output=alias/'via-link'/'result'
            argv=['mounted_acceptance.py','--mount',str(mount),'--native',str(native),'--output',str(output)]
            with patch.object(sys,'argv',argv),patch.object(mounted_acceptance,'require_mount') as attest:
                with self.assertRaisesRegex(ValueError,'output must be outside'):
                    mounted_acceptance.main()
                attest.assert_not_called()
            self.assertFalse((native/'via-link').exists())
    def test_canonical_external_output_does_not_create_lexical_prefix(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mount=root/'mount';native=root/'native';mount.mkdir();native.mkdir()
            output=native/'unmade'/'..'/'..'/'safe-output'
            argv=['mounted_acceptance.py','--mount',str(mount),'--native',str(native),'--output',str(output)]
            with patch.object(sys,'argv',argv),patch.object(mounted_acceptance,'require_mount',side_effect=RuntimeError('ordinary directory')):
                self.assertEqual(mounted_acceptance.main(),2)
            self.assertFalse((native/'unmade').exists())
            self.assertEqual(len(list((root/'safe-output').glob('blocked-*.json'))),1)
    def test_ordinary_mount_attestation_failure_writes_safe_blocked_receipt(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mount=root/'mount';native=root/'native';output=root/'results'
            mount.mkdir();native.mkdir()
            argv=['mounted_acceptance.py','--mount',str(mount),'--native',str(native),'--output',str(output)]
            with patch.object(sys,'argv',argv),patch.object(mounted_acceptance,'require_mount',side_effect=RuntimeError('ordinary directory')):
                self.assertEqual(mounted_acceptance.main(),2)
            receipt=json.loads(next(output.glob('blocked-*.json')).read_text())
            self.assertEqual(receipt['status'],'blocked')
            self.assertFalse(receipt['mounted_acceptance'])
    def test_overlapping_grants_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name)
            with self.assertRaises(ValueError):profile(root,root/'backing',root/'scratch')
            for child in ('mount','backing','scratch'):(root/child).mkdir()
            with self.assertRaises(ValueError):profile(root/'mount',root/'backing',root/'scratch',[root])
    def test_backing_hardlink_alias_rejects_startup_admission(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);backing=root/'backing';toolchain=root/'toolchain'
            backing.mkdir();(backing/'CURRENT').write_text('repository')
            (backing/'OWNERSHIP').touch();(backing/'object').write_text('private')
            toolchain.mkdir();os.link(backing/'object',toolchain/'alias')
            with self.assertRaisesRegex(ValueError,'hardlink alias'):
                validate_backing(backing,[toolchain])
    def test_stdio_socket_rejected_and_pipe_allowed(self):
        with tempfile.TemporaryDirectory() as name:
            backing=Path(name)
            sockets=[socket.socketpair() for _ in range(3)]
            saved=[]
            try:
                for descriptor in range(3):
                    saved.append(os.dup(descriptor))
                for descriptor in range(3):
                    os.dup2(sockets[descriptor][0].fileno(),descriptor)
                    with self.assertRaisesRegex(ValueError,'socket stdio'):
                        validate_stdio(backing,(descriptor,))
            finally:
                for descriptor,backup in enumerate(saved):
                    os.dup2(backup,descriptor);os.close(backup)
                for pair in sockets:
                    for endpoint in pair:endpoint.close()
            read_fd,write_fd=os.pipe()
            try:validate_stdio(backing,(read_fd,write_fd))
            finally:os.close(read_fd);os.close(write_fd)
    def test_owner_lease_requires_live_exclusive_flock(self):
        with tempfile.TemporaryDirectory() as name:
            lease=Path(name)/'OWNERSHIP';lease.touch()
            descriptor=os.open(lease,os.O_RDONLY)
            try:
                fcntl.flock(descriptor,fcntl.LOCK_EX|fcntl.LOCK_NB)
                validate_owner_lease(name)
                fcntl.flock(descriptor,fcntl.LOCK_UN)
                with self.assertRaisesRegex(ValueError,'no live exclusive owner'):
                    validate_owner_lease(name)
            finally:os.close(descriptor)
    @unittest.skipUnless(platform.system()=='Darwin','macOS sandbox')
    def test_actual_sandbox_read_write_denial(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name).resolve();dirs=[root/p for p in ('mount','backing','scratch')]
            for path in dirs:path.mkdir()
            environment=workload_environment(dirs[2])
            secret=dirs[1]/'secret';secret.write_text('secret')
            allowed=dirs[0]/'file';allowed.write_text('allowed')
            def run(*args):return subprocess.run(['/usr/bin/sandbox-exec','-p',profile(*dirs),*map(str,args)],capture_output=True,timeout=10,cwd=dirs[0],env=environment,close_fds=True)
            self.assertEqual(run('/bin/cat',allowed).stdout,b'allowed')
            denied=run('/bin/cat',secret)
            self.assertEqual(denied.returncode,1);self.assertNotIn(b'secret',denied.stdout)
            self.assertEqual(run('/usr/bin/touch',dirs[1]/'created').returncode,1)
            self.assertFalse((dirs[1]/'created').exists())
            self.assertEqual(run('/usr/bin/touch',dirs[0]/'created').returncode,0)
            (dirs[0]/'escape').symlink_to(dirs[1],target_is_directory=True)
            self.assertEqual(run('/bin/cat',dirs[0]/'escape'/'secret').returncode,1)
            self.assertEqual(run('/usr/bin/touch',dirs[0]/'escape'/'through-link').returncode,1)
            # The complete descendant tree remains constrained, including a shell.
            self.assertNotEqual(run('/bin/sh','-c','exec /bin/cat "$1"','probe',secret).returncode,0)
            # Positive interpreter probe prevents an exec failure from posing as denial.
            python='/Library/Developer/CommandLineTools/usr/bin/python3'
            self.assertTrue(Path(python).is_file(),'CLT Python is required for the runtime probes')
            positive=run(python,'-c','import errno,socket,sys; print(42)')
            self.assertEqual((positive.returncode,positive.stdout.strip()),(0,b'42'),positive.stderr)
            control=root/'control.sock'
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
                server.bind(str(control));server.listen()
                code='import errno,socket,sys\ntry:\n s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)\n s.connect(sys.argv[1])\nexcept OSError as e: sys.exit(73 if e.errno in (errno.EPERM,errno.EACCES) else 74)'
                denied=run(python,'-c',code,control)
                self.assertEqual(denied.returncode,73,denied.stderr)
            with socket.socket() as server:
                server.bind(('127.0.0.1',0));server.listen()
                code='import errno,socket,sys\ntry:\n s=socket.socket(socket.AF_INET,socket.SOCK_STREAM)\n s.settimeout(2)\n s.connect(("127.0.0.1",int(sys.argv[1])))\nexcept OSError as e: sys.exit(73 if e.errno in (errno.EPERM,errno.EACCES) else 74)'
                denied=run(python,'-c',code,server.getsockname()[1])
                self.assertEqual(denied.returncode,73,denied.stderr)
            outside=root/'outside';outside.write_text('outside')
            self.assertEqual(run('/bin/cat',outside).returncode,1)
    @unittest.skipUnless(platform.system()=='Darwin','macOS statfs')
    def test_native_path_cannot_pass_acceptance(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mount=root/'mount';native=root/'native';mount.mkdir();native.mkdir();out=root/'result'
            p=subprocess.run([sys.executable,str(Path(mounted_acceptance.__file__)),'--mount',str(mount),'--native',str(native),'--output',str(out)],capture_output=True)
            self.assertEqual(p.returncode,2)
            self.assertEqual(list(mount.iterdir()),[])
            self.assertFalse(json.loads(next(out.glob('*.json')).read_text())['mounted_acceptance'])
if __name__=='__main__':unittest.main()
