"""Harness validation; native runs are deliberately not mounted acceptance."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
import mount_probe
from restricted_workload import profile
import mounted_acceptance

class Harness(unittest.TestCase):
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
    def test_overlapping_grants_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name)
            with self.assertRaises(ValueError):profile(root,root/'backing',root/'scratch')
            for child in ('mount','backing','scratch'):(root/child).mkdir()
            with self.assertRaises(ValueError):profile(root/'mount',root/'backing',root/'scratch',[root])
    @unittest.skipUnless(platform.system()=='Darwin','macOS sandbox')
    def test_actual_sandbox_read_write_denial(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name).resolve();dirs=[root/p for p in ('mount','backing','scratch')]
            for path in dirs:path.mkdir()
            secret=dirs[1]/'secret';secret.write_text('secret')
            allowed=dirs[0]/'file';allowed.write_text('allowed')
            def run(*args):return subprocess.run(['/usr/bin/sandbox-exec','-p',profile(*dirs),*map(str,args)],capture_output=True,timeout=10)
            self.assertEqual(run('/bin/cat',allowed).stdout,b'allowed')
            denied=run('/bin/cat',secret)
            self.assertEqual(denied.returncode,1);self.assertNotIn(b'secret',denied.stdout)
            self.assertEqual(run('/usr/bin/touch',dirs[1]/'created').returncode,1)
            self.assertFalse((dirs[1]/'created').exists())
            self.assertEqual(run('/usr/bin/touch',dirs[0]/'created').returncode,0)
            (dirs[0]/'escape').symlink_to(dirs[1],target_is_directory=True)
            self.assertEqual(run('/bin/cat',dirs[0]/'escape'/'secret').returncode,1)
            self.assertEqual(run('/usr/bin/touch',dirs[0]/'escape'/'through-link').returncode,1)
            outside=root/'outside';outside.write_text('outside')
            self.assertEqual(run('/bin/cat',outside).returncode,1)
    @unittest.skipUnless(platform.system()=='Darwin','macOS statfs')
    def test_native_path_cannot_pass_acceptance(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);mount=root/'mount';mount.mkdir();out=root/'result'
            p=subprocess.run([sys.executable,str(Path(mounted_acceptance.__file__)),'--mount',str(mount),'--native',name,'--output',str(out)],capture_output=True)
            self.assertEqual(p.returncode,2)
            self.assertEqual(list(mount.iterdir()),[])
            self.assertFalse(json.loads(next(out.glob('*.json')).read_text())['mounted_acceptance'])
if __name__=='__main__':unittest.main()
