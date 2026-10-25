"""Supervisor contracts; privileged Darwin behavior has a separate CI harness."""
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fskit'))
import agent_supervisor as supervisor
import dedicated_workload


class Supervisor(unittest.TestCase):
    def test_credentials_remove_groups_and_inherited_descriptors(self):
        self.assertEqual(supervisor.credentials(dict(worker_uid=70, worker_gid=70)),
                         dict(user=70, group=70, extra_groups=(), umask=0o077,
                              close_fds=True, start_new_session=True))

    def test_nonroot_rejected_before_installation_access(self):
        with patch.object(supervisor.platform, 'system', return_value='Darwin'), \
             patch.object(supervisor.os, 'getuid', return_value=501), \
             patch.object(supervisor, 'installed_launcher') as installed:
            with self.assertRaisesRegex(RuntimeError, 'root'):
                supervisor.supervise('/absent', '/absent', ['true'])
            installed.assert_not_called()

    def test_missing_provisioned_account_blocks_admission_before_reservation(self):
        runtime = SimpleNamespace(
            ENTRY_ENV={},
            admit_entry_environment=lambda: None,
            read_policy=lambda path: dict(version=1, worker_uid=701, worker_gid=702),
            trusted_path=lambda *args, **kwargs: self.fail('interpreter admission must not run'),
        )
        output = io.StringIO()
        flags = {name: getattr(sys.flags, name) for name in dir(sys.flags)
                 if not name.startswith('_') and isinstance(getattr(sys.flags, name), int)}
        flags.update(isolated=True, no_site=True)
        with patch.object(supervisor.platform, 'system', return_value='Darwin'), \
             patch.object(supervisor.os, 'getuid', return_value=0), \
             patch.object(supervisor.os, 'geteuid', return_value=0), \
             patch.object(sys, 'flags', SimpleNamespace(**flags)), \
             patch.object(supervisor, 'installed_launcher', return_value=runtime), \
             patch.object(supervisor.pwd, 'getpwuid', side_effect=KeyError(701)), \
             patch.object(supervisor, 'Reservation') as reservation, \
             patch.object(supervisor.subprocess, 'Popen') as popen, \
             patch.object(supervisor.subprocess, 'run') as run, \
             patch.object(supervisor.os.path, 'lexists') as lexists, \
             patch.object(sys, 'argv', ['agent_supervisor.py', '--policy', '/policy',
                                        '--state', '/state', '--', 'true']), \
             patch('sys.stdout', output):
            self.assertEqual(supervisor.main(), 2)

        report = json.loads(output.getvalue())
        self.assertEqual(report['status'], 'blocked')
        self.assertIn('provisioned worker account', report['error'])
        self.assertIn('701', report['error'])
        reservation.assert_not_called()
        popen.assert_not_called()
        run.assert_not_called()
        lexists.assert_not_called()

    def test_process_enumeration_requires_positive_control(self):
        class Query:
            def __call__(self, kind, identity, buffer, capacity):return 0
        library=SimpleNamespace(proc_listpids=Query())
        with patch.object(supervisor.ctypes, 'CDLL', return_value=library):
            with self.assertRaisesRegex(RuntimeError, 'verify root'):
                supervisor.uid_processes(70)

    def test_process_enumeration_unions_real_and_effective_uid(self):
        class Query:
            def __call__(self, kind, identity, buffer, capacity):
                buffer[0]=os.getpid() if identity==0 else (123 if kind==4 else 456)
                return supervisor.ctypes.sizeof(supervisor.ctypes.c_int)
        library=SimpleNamespace(proc_listpids=Query())
        with patch.object(supervisor.ctypes, 'CDLL', return_value=library):
            self.assertEqual(supervisor.uid_processes(70), {123,456})

    def test_process_enumeration_overflow_fails_closed(self):
        class Query:
            def __call__(self, kind, identity, buffer, capacity):return capacity
        library=SimpleNamespace(proc_listpids=Query())
        with patch.object(supervisor.ctypes, 'CDLL', return_value=library):
            with self.assertRaisesRegex(RuntimeError, 'capacity'):
                supervisor.uid_processes(70)

    def test_reaper_refuses_root_before_signalling(self):
        result = subprocess.run([sys.executable, '-I', '-S', '-c', supervisor.REAPER, '0', '0'],
                                capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'refused', result.stderr)

    def test_reaper_validates_all_targets_before_any_signal(self):
        for targets in ([123, 0], [123, -1], [123, True], [123, '456']):
            with self.subTest(targets=targets), \
                 patch.object(supervisor.os, 'getuid', return_value=70), \
                 patch.object(supervisor.os, 'geteuid', return_value=70), \
                 patch.object(supervisor.os, 'getgid', return_value=70), \
                 patch.object(supervisor.os, 'getegid', return_value=70), \
                 patch.object(sys, 'argv', ['reaper','70','70']), \
                 patch.object(sys, 'stdin', SimpleNamespace(buffer=io.BytesIO(json.dumps(targets).encode()))), \
                 patch.object(supervisor.os, 'kill') as kill:
                with self.assertRaises(SystemExit):exec(supervisor.REAPER, {})
                kill.assert_not_called()

    def child(self, code):
        return subprocess.Popen([sys.executable, '-I', '-S', '-c', code],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def observed(self, code, timeout=3, interrupted=lambda: False):
        child = self.child(code)
        try:
            result = supervisor.monitor(child, timeout, interrupted)
            return result
        finally:
            if child.poll() is None:child.kill()
            child.wait(timeout=5)
            child.stdout.close();child.stderr.close()

    def test_fast_exit_preserves_both_streams(self):
        reason, output = self.observed("import os;os.write(1,b'out');os.write(2,b'err')")
        self.assertEqual(reason, 'exited')
        self.assertEqual(output, dict(stdout=b'out', stderr=b'err'))

    def test_large_pipe_output_drains_without_deadlock(self):
        reason, output = self.observed("import os;os.write(1,b'x'*200000);os.write(2,b'y'*200000)")
        self.assertEqual(reason, 'exited')
        self.assertEqual(output, dict(stdout=b'x'*200000, stderr=b'y'*200000))

    def test_timeout(self):
        reason, _ = self.observed('import time;time.sleep(30)', timeout=0.05)
        self.assertEqual(reason, 'timeout')

    def test_interruption(self):
        reason, _ = self.observed('import time;time.sleep(30)', interrupted=lambda: True)
        self.assertEqual(reason, 'interrupted')

    def test_output_flood_is_bounded(self):
        with patch.object(supervisor, 'MAX_OUTPUT', 10000):
            reason, output = self.observed("import os\nwhile True:os.write(1,b'x'*4096)")
        self.assertEqual(reason, 'output_limit')
        self.assertLessEqual(sum(map(len, output.values())), 10000)

    def test_version_two_requires_explicit_scratch(self):
        from test_dedicated_workload import policy
        original = policy()
        self.assertEqual(dedicated_workload.parse_policy(json.dumps(original)), original)
        with self.assertRaises(ValueError):
            dedicated_workload.parse_policy(json.dumps(dict(original, version=2)))
        value = dict(original, version=2, scratch_parent='/Library/Forkfs/session/scratch')
        self.assertEqual(dedicated_workload.parse_policy(json.dumps(value)), value)
        with self.assertRaises(ValueError):
            dedicated_workload.parse_policy(json.dumps(dict(value, scratch_parent='../scratch')))


class Reservations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.state = Path(self.temporary.name)
        self.state.chmod(0o711)
        self.runtime = SimpleNamespace(trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
        self.policy = dict(worker_uid=70, mount='/world')
        original = os.fstat
        def root_metadata(fd):
            value = original(fd)
            return SimpleNamespace(**{name: (0 if name == "st_uid" else getattr(value, name))
                                      for name in dir(value) if name.startswith("st_")})
        for name, value in (('uid_processes', lambda uid: set()),):
            context = patch.object(supervisor, name, value);context.start();self.addCleanup(context.stop)
        context = patch.object(supervisor.os, 'fstat', side_effect=root_metadata)
        context.start();self.addCleanup(context.stop)

    def lease(self):return supervisor.Reservation(self.state, self.policy, self.runtime)

    def test_success_removes_active_and_scratch_retains_binding(self):
        with self.lease() as lease:
            session = lease.session
            self.assertTrue(lease.active.exists())
            lease.release_after_quiescence()
        self.assertFalse(session.exists())
        self.assertFalse((self.state/'70.active').exists())
        self.assertTrue((self.state/'70.binding').exists())

    def test_crash_marker_blocks_next_reservation(self):
        with self.lease():pass
        with self.assertRaisesRegex(ValueError, 'active/crashed'):
            with self.lease():pass

    def test_live_uid_is_refused_without_marker(self):
        with patch.object(supervisor, 'uid_processes', return_value={123}):
            with self.assertRaisesRegex(ValueError, 'already in use'):
                with self.lease():pass
        self.assertFalse((self.state/'70.active').exists())

    def test_concurrent_reservation_is_refused(self):
        with self.lease() as lease:
            with self.assertRaises(BlockingIOError):
                with self.lease():pass
            lease.release_after_quiescence()

    def test_different_policy_cannot_rebind_idle_uid(self):
        with self.lease() as lease:lease.release_after_quiescence()
        self.policy['mount'] = '/other-world'
        with self.assertRaisesRegex(ValueError, 'another policy'):
            with self.lease():pass

    def test_live_uid_prevents_release(self):
        with self.lease() as lease:
            with patch.object(supervisor, 'uid_processes', return_value={123}):
                with self.assertRaisesRegex(RuntimeError, 'live UID'):lease.release_after_quiescence()
            self.assertTrue(lease.active.exists())
            lease.release_after_quiescence()

    def test_atomic_active_marker_handles_short_writes(self):
        original = os.write
        original_lstat = os.lstat
        def short_write(fd, data):return original(fd, data[:max(1, min(3, len(data)))])
        with self.lease() as lease:
            def root_active(path, *args, **kwargs):
                value = original_lstat(path, *args, **kwargs)
                if Path(path) == lease.active:
                    return SimpleNamespace(**{name: (0 if name == 'st_uid' else getattr(value, name))
                                              for name in dir(value) if name.startswith('st_')})
                return value
            with patch.object(supervisor.os, 'write', side_effect=short_write), \
                 patch.object(supervisor.os, 'lstat', side_effect=root_active):
                lease.write_active(dict(status='active', mount_record='/record', session_dir=str(lease.session)))
                value = json.loads(lease.active.read_text())
                self.assertEqual(value['status'], 'active')
                self.assertEqual(value['session_dir'], str(lease.session))
                self.assertEqual(os.stat(lease.active).st_mode & 0o777, 0o600)
            lease.release_after_quiescence()

    def test_scratch_symlink_cannot_delete_external_target(self):
        target = self.state/'protected';target.write_text('keep')
        with self.lease() as lease:
            (lease.session/'escape').symlink_to(self.state, target_is_directory=True)
            lease.release_after_quiescence()
        self.assertEqual(target.read_text(), 'keep')


if __name__ == '__main__':unittest.main()
