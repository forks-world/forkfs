"""Admission and failure-order regressions for the dedicated launcher."""
import errno
import ctypes
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fskit'))
import dedicated_workload as launch
import restricted_workload


def policy():
    return dict(version=1, host_uid=501, service_uid=502, frontend_uid=503,
                worker_uid=504, worker_gid=504, mount='/world', mount_fsid=[7, 9],
                backing='/Library/Forkfs/store', read_only=[])


class DedicatedWorkload(unittest.TestCase):
    def test_only_uid_derived_corefoundation_environment_is_admitted(self):
        with patch.object(launch.os,'getuid',return_value=70):
            for extra in ({}, {'__CF_USER_TEXT_ENCODING':'0x46:0x0:0x0'}):
                with patch.dict(os.environ,dict(launch.ENTRY_ENV,**extra),clear=True):
                    launch.admit_entry_environment()
            for value in ('0x1F5:0x0:0x0','token','0x46:0x0:0x0\nhost-secret'):
                with patch.dict(os.environ,dict(launch.ENTRY_ENV,__CF_USER_TEXT_ENCODING=value),clear=True):
                    with self.assertRaisesRegex(ValueError,'fresh entry environment'):
                        launch.admit_entry_environment()

    @unittest.skipUnless(sys.platform=='darwin','Darwin ACL API')
    def test_real_extended_acl_is_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'policy';path.write_text('{}')
            launch.no_acl(path)
            # Construct a real ACL without directory-service/name-resolution
            # tools, which cannot run inside the outer execution sandbox.
            libc=ctypes.CDLL(None,use_errno=True)
            pointer=ctypes.c_void_p
            signatures={
                'acl_init':([ctypes.c_int],pointer),
                'acl_create_entry':([ctypes.POINTER(pointer),ctypes.POINTER(pointer)],ctypes.c_int),
                'acl_set_tag_type':([pointer,ctypes.c_int],ctypes.c_int),
                'acl_set_qualifier':([pointer,pointer],ctypes.c_int),
                'acl_set_permset_mask_np':([pointer,ctypes.c_uint64],ctypes.c_int),
                'acl_set_file':([ctypes.c_char_p,ctypes.c_int,pointer],ctypes.c_int),
                'acl_free':([pointer],ctypes.c_int)}
            for symbol,(arguments,result) in signatures.items():
                function=getattr(libc,symbol);function.argtypes=arguments;function.restype=result
            acl=pointer(libc.acl_init(1));self.assertTrue(acl)
            try:
                entry=pointer();self.assertEqual(libc.acl_create_entry(ctypes.byref(acl),ctypes.byref(entry)),0)
                self.assertEqual(libc.acl_set_tag_type(entry,1),0)  # ACL_EXTENDED_ALLOW
                qualifier=ctypes.create_string_buffer(bytes.fromhex('123456789abcdef0123456789abcdef0'))
                self.assertEqual(libc.acl_set_qualifier(entry,ctypes.cast(qualifier,pointer)),0)
                self.assertEqual(libc.acl_set_permset_mask_np(entry,1<<2),0)  # ACL_WRITE_DATA
                self.assertEqual(libc.acl_set_file(os.fsencode(path),0x100,acl),0)
                with self.assertRaisesRegex(ValueError,'extended ACL'):launch.no_acl(path)
            finally:libc.acl_free(acl)

    def test_real_policy_reads_are_bounded_and_do_not_follow_symlinks(self):
        with tempfile.TemporaryDirectory() as name:
            path=Path(name)/'policy';path.write_text(json.dumps(policy()))
            with patch.object(launch,'trusted_path',side_effect=lambda p: Path(p)):
                self.assertEqual(launch.read_policy(path),policy())
                alias=Path(name)/'alias';alias.symlink_to(path)
                with self.assertRaises(OSError):launch.read_policy(alias)
                path.write_bytes(b' '*65537)
                with self.assertRaisesRegex(ValueError,'too large'):launch.read_policy(path)

    def test_schema_rejects_aliases_ambiguity_and_traversal(self):
        self.assertEqual(launch.parse_policy(json.dumps(policy())), policy())
        changes = [dict(host_uid=504), dict(frontend_uid=502), dict(worker_uid=0),
                   dict(worker_uid=True), dict(worker_uid=-1), dict(service_uid=2**32-1),
                   dict(worker_gid=0), dict(version=True), dict(version=2), dict(extra=1),
                   dict(mount='/x/../world'), dict(backing='relative'), dict(mount_fsid=[1]),
                   dict(mount_fsid=[True, 0]), dict(mount_fsid=[2**31, 1]),
                   dict(read_only=['/x/../secret']), dict(read_only='/usr')]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                launch.parse_policy(json.dumps(dict(policy(), **change)))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            launch.parse_policy(json.dumps(policy())[:-1] + ',"host_uid":504}')

    def test_trust_rejects_any_writable_symlink_or_acl_ancestor(self):
        paths = {Path('/'): SimpleNamespace(st_uid=0, st_mode=stat.S_IFDIR|0o755, st_nlink=1),
                 Path('/secure'): SimpleNamespace(st_uid=0, st_mode=stat.S_IFDIR|0o755, st_nlink=1),
                 Path('/secure/policy'): SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG|0o644, st_nlink=1)}
        with patch.object(launch.os, 'lstat', side_effect=lambda p: paths[Path(p)]), \
             patch.object(launch, 'no_acl') as acl:
            launch.trusted_path('/secure/policy')
            self.assertEqual(acl.call_count, 3)
            for uid, mode in ((501, stat.S_IFDIR|0o755), (0, stat.S_IFDIR|0o775),
                              (0, stat.S_IFDIR|0o1777), (0, stat.S_IFLNK|0o755)):
                paths[Path('/secure')] = SimpleNamespace(st_uid=uid, st_mode=mode, st_nlink=1)
                with self.subTest(uid=uid, mode=mode), self.assertRaises(ValueError):
                    launch.trusted_path('/secure/policy')
        with patch.object(launch.os, 'lstat', return_value=SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG|0o644, st_nlink=2)):
            with self.assertRaises(ValueError):launch.trusted_path('/policy')

    def test_wrong_credentials_fail_before_root_recovery_probes(self):
        for uid, euid, gid, egid, groups in ((501,504,504,504,{504}), (504,0,504,504,{504}),
                (504,504,20,504,{504}), (504,504,504,20,{504}), (504,504,504,504,{504,20})):
            with self.subTest(uid=uid,euid=euid,gid=gid,egid=egid,groups=groups), \
                 patch.object(launch.os,'getuid',return_value=uid), \
                 patch.object(launch.os,'geteuid',return_value=euid), \
                 patch.object(launch.os,'getgid',return_value=gid), \
                 patch.object(launch.os,'getegid',return_value=egid), \
                 patch.object(launch,'kernel_groups',return_value=groups), \
                 patch.object(launch.os,'setuid') as recovery:
                with self.assertRaisesRegex(ValueError,'supervisor'):launch.admit_identity(policy())
                recovery.assert_not_called()

    def test_saved_root_and_unexpected_probe_failure_are_not_admitted(self):
        with patch.object(launch.os,'getuid',return_value=504), \
             patch.object(launch.os,'geteuid',return_value=504), \
             patch.object(launch.os,'getgid',return_value=504), \
             patch.object(launch.os,'getegid',return_value=504), \
             patch.object(launch,'kernel_groups',return_value={504}), \
             patch.object(launch.os,'setuid') as setuid, \
             patch.object(launch.os,'setgid',side_effect=PermissionError(errno.EPERM,'denied')), \
             patch.object(launch.os,'setgroups',side_effect=PermissionError(errno.EPERM,'denied')):
            with self.assertRaisesRegex(RuntimeError,'regain root'):launch.admit_identity(policy())
            setuid.side_effect=OSError(errno.EIO,'unexpected')
            with self.assertRaises(OSError):launch.admit_identity(policy())
            setuid.side_effect=PermissionError(errno.EPERM,'denied')
            launch.admit_identity(policy())

    def test_native_mount_or_wrong_fsid_never_starts_command(self):
        runtime=SimpleNamespace(require_mount=lambda p: {'fsid':[99,99]})
        for attestation in (RuntimeError('not a real mount'), None):
            with self.subTest(attestation=attestation), \
                 patch.object(launch.platform,'system',return_value='Darwin'), \
                 patch.object(launch.os,'getuid',return_value=504), \
                 patch.object(launch.os,'geteuid',return_value=504), \
                 patch.dict(os.environ,launch.ENTRY_ENV,clear=True), \
                 patch.object(launch,'read_policy',return_value=policy()), \
                 patch.object(launch,'admit_identity'), \
                 patch.object(launch,'installed_runtime',return_value=runtime), \
                 patch.object(launch.subprocess,'run') as spawn:
                if attestation:runtime.require_mount=lambda p: (_ for _ in ()).throw(attestation)
                else:runtime.require_mount=lambda p: {'fsid':[99,99]}
                with self.assertRaises((ValueError,RuntimeError)):launch.run('/policy',['/bin/true'])
                spawn.assert_not_called()

    def test_ambient_supervisor_credentials_refused_before_policy_or_command(self):
        with patch.object(launch.platform,'system',return_value='Darwin'), \
             patch.object(launch.os,'getuid',return_value=504), \
             patch.object(launch.os,'geteuid',return_value=504), \
             patch.dict(os.environ,dict(launch.ENTRY_ENV,OPENAI_API_KEY='test-token'),clear=True), \
             patch.object(launch,'read_policy') as read, patch.object(launch.subprocess,'run') as spawn:
            with self.assertRaisesRegex(ValueError,'fresh entry environment'):launch.run('/policy',['true'])
            read.assert_not_called();spawn.assert_not_called()

    def test_root_is_refused_before_reading_policy(self):
        with patch.object(launch.platform,'system',return_value='Darwin'), \
             patch.object(launch.os,'getuid',return_value=0), patch.object(launch,'read_policy') as read:
            with self.assertRaisesRegex(RuntimeError,'non-root'):launch.run('/policy',['true'])
            read.assert_not_called()

    def test_profile_preserves_default_and_denies_backing_network(self):
        with tempfile.TemporaryDirectory() as name:
            paths=[Path(name)/p for p in ('work','backing','scratch')]
            for p in paths:p.mkdir()
            baseline=restricted_workload.profile(*paths)
            dedicated=launch.dedicated_profile(restricted_workload,*paths,[])
            self.assertIn('(allow process-info*)',dedicated)
            self.assertNotIn('(allow network',dedicated)
            self.assertIn('(deny file* (subpath ',dedicated)
            self.assertEqual(restricted_workload.profile(*paths),baseline)
            self.assertIn('(deny process-info*)',baseline)

    def test_real_stdio_rejects_regular_files_and_accepts_pipes(self):
        with tempfile.TemporaryFile() as regular:
            read_fd,write_fd=os.pipe()
            original=launch.os.fstat
            try:
                with patch.object(launch.os,'fstat',side_effect=lambda fd: original(regular.fileno())):
                    with self.assertRaisesRegex(ValueError,'output must be a pipe'):launch.safe_output()
                with patch.object(launch.os,'fstat',side_effect=lambda fd: original(write_fd)):
                    launch.safe_output()
            finally:os.close(read_fd);os.close(write_fd)


if __name__=='__main__':unittest.main()
