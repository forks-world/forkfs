"""Supervisor contracts; privileged Darwin behavior has a separate CI harness."""
import json
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

    def test_reaper_refuses_root_before_signalling(self):
        result = subprocess.run([sys.executable, '-I', '-S', '-c', supervisor.REAPER, '0', '0'],
                                capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'refused', result.stderr)

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

    def test_scratch_symlink_cannot_delete_external_target(self):
        target = self.state/'protected';target.write_text('keep')
        with self.lease() as lease:
            (lease.session/'escape').symlink_to(self.state, target_is_directory=True)
            lease.release_after_quiescence()
        self.assertEqual(target.read_text(), 'keep')


if __name__ == '__main__':unittest.main()
