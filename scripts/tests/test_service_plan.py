"""Deployment plan contracts; these tests do not install accounts or services."""
from pathlib import Path
import plistlib
import os
import stat
import tempfile
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
import service_plan as plan

class ServicePlan(unittest.TestCase):
    def config(self,**changes):
        args=dict(service_user='_forkfs',admin_uid=501,binary='/Library/PrivilegedHelperTools/forkfsd',
                  backing='/Library/Forkfs/store',runtime='/Library/Forkfs/runtime');args.update(changes)
        return plan.configuration(**args)
    def test_roundtrip_no_shell_no_credentials(self):
        config=plistlib.loads(plistlib.dumps(self.config()))
        self.assertEqual(config['UserName'],'_forkfs')
        self.assertEqual(config['ProgramArguments'][-3:],['--rpc','--control-uid','501'])
        self.assertEqual(config['Umask'],0o077)
        self.assertEqual(config['EnvironmentVariables'],{'PATH':'/usr/bin:/bin:/usr/sbin:/sbin'})
        self.assertNotIn('GroupName',config)
    def test_invalid_configuration(self):
        for change in (dict(service_user='root'),dict(service_user='_x;sh'),dict(admin_uid=-1),
                       dict(admin_uid=2**32-1),dict(binary='relative'),dict(backing='/a/../b'),
                       dict(runtime='/Library/Forkfs/store/runtime')):
            with self.subTest(change=change),self.assertRaises(ValueError):self.config(**change)
    def test_account_admission_precedes_filesystem_checks(self):
        for uid,shell in ((0,'/usr/bin/false'),(501,'/usr/bin/false'),(502,'/bin/sh')):
            with patch.object(plan.pwd,'getpwnam',return_value=SimpleNamespace(pw_uid=uid,pw_shell=shell)),patch.object(plan,'check_path') as check:
                self.assertEqual(plan.preflight(self.config())['status'],'blocked');check.assert_not_called()
    def test_real_unsafe_binary_modes_and_hardlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/'tool';binary.write_bytes(b'fixture')
            for mode in (0o644,0o777,0o775,0o4755):
                binary.chmod(mode)
                with self.subTest(mode=mode),self.assertRaises(ValueError):
                    plan.check_path(binary,os.geteuid(),executable=True)
            binary.chmod(0o755);os.link(binary,Path(directory)/'alias')
            with self.assertRaisesRegex(ValueError,'single-link'):
                plan.check_path(binary,os.geteuid(),executable=True)
            link=Path(directory)/'link';link.symlink_to(binary)
            with self.assertRaisesRegex(ValueError,'symlinks'):
                plan.check_path(link,os.geteuid(),executable=True)
    def test_ancestor_ownership_and_write_access(self):
        path=Path('/secure/runtime')
        good={path:SimpleNamespace(st_uid=502,st_mode=stat.S_IFDIR|0o711),
              Path('/secure'):SimpleNamespace(st_uid=0,st_mode=stat.S_IFDIR|0o755),
              Path('/'):SimpleNamespace(st_uid=0,st_mode=stat.S_IFDIR|0o755)}
        with patch.object(plan.os,'lstat',side_effect=lambda p:good[Path(p)]):
            plan.check_path(path,502,0o711)
            for uid,mode in ((501,stat.S_IFDIR|0o755),(0,stat.S_IFDIR|0o775),
                             (0,stat.S_IFLNK|0o755)):
                good[Path('/secure')]=SimpleNamespace(st_uid=uid,st_mode=mode)
                with self.subTest(uid=uid,mode=mode),self.assertRaisesRegex(ValueError,'ancestor'):
                    plan.check_path(path,502,0o711)
    def test_frontend_identity_must_be_separate(self):
        with self.assertRaises(ValueError):self.config(frontend_uid=501)
        config=self.config(frontend_uid=503)
        self.assertEqual(config['ProgramArguments'][-2:],['--frontend-uid','503'])
        with patch.object(plan.pwd,'getpwnam',return_value=SimpleNamespace(pw_uid=503,pw_shell='/usr/bin/false')),patch.object(plan,'check_path') as check:
            self.assertEqual(plan.preflight(config)['status'],'blocked');check.assert_not_called()
    def test_missing_account_is_blocked(self):
        with patch.object(plan.pwd,'getpwnam',side_effect=KeyError('_forkfs')):
            self.assertEqual(plan.preflight(self.config())['status'],'blocked')
    def test_preflight_does_not_claim_installation(self):
        with patch.object(plan.pwd,'getpwnam',return_value=SimpleNamespace(pw_uid=502,pw_shell='/usr/bin/false')),patch.object(plan,'check_path') as check:
            result=plan.preflight(self.config());self.assertEqual(result['status'],'posix_checks_passed')
            self.assertFalse(result['installed']);self.assertEqual(check.call_count,3)
            self.assertEqual(check.call_args_list[0].args[1],0)
            self.assertEqual(check.call_args_list[1].args[2],0o700)
            self.assertEqual(check.call_args_list[2].args[2],0o711)
if __name__=='__main__':unittest.main()
