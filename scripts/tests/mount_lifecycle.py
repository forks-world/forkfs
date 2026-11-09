"""Exercise real daemon/bridge lifecycle with injected OS mount operations."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock

BINARY = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location('launcher', Path(__file__).resolve().parents[1] / 'mount-forkfs.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class MountLifecycle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ff-launch-', dir='/tmp')
        self.root = Path(self.tmp.name)
        self.sessions = []

    def tearDown(self):
        for session in reversed(self.sessions):
            session.mounted_on_host = lambda: False
            session.stop()
        self.tmp.cleanup()

    def session(self, **kwargs):
        session = launcher.MountSession(BINARY, self.root/'repo', self.root/'run', self.root/'mount', **kwargs)
        session.fake_mounted = False
        session.mounted_on_host = lambda: session.fake_mounted
        session.mount_volume = Mock(side_effect=lambda: setattr(session, 'fake_mounted', True))
        session.unmount_volume = Mock(side_effect=lambda: setattr(session, 'fake_mounted', False))
        self.sessions.append(session)
        return session

    def test_start_and_stop_preserve_order_and_private_files(self):
        session = self.session(initialize=True)
        session.start()
        self.assertIsNone(session.daemon.poll())
        self.assertIsNone(session.bridge.poll())
        self.assertTrue(session.mounted)
        for name in ['forkfs.json', 'daemon.log', 'bridge.log', '.forkfs-launcher.lock']:
            info = (session.runtime/name).stat()
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
        config = json.loads((session.runtime/'forkfs.json').read_text())
        self.assertEqual(config['rpc']['transport'], 'tcp-loopback')
        def unmount():
            self.assertIsNone(session.daemon.poll(), 'daemon stopped before unmount')
            self.assertIsNone(session.bridge.poll(), 'bridge stopped before unmount')
            session.fake_mounted = False
        session.unmount_volume.side_effect = unmount
        session.stop()
        self.assertIsNotNone(session.daemon.poll())
        self.assertIsNotNone(session.bridge.poll())
        self.assertFalse((session.runtime/'control.sock').exists())

    def test_mount_failure_cleans_up_children(self):
        session = self.session(initialize=True)
        session.mount_volume.side_effect = subprocess.CalledProcessError(69, 'mount')
        with self.assertRaises(subprocess.CalledProcessError):
            session.start()
        session.stop()
        session.unmount_volume.assert_not_called()
        self.assertIsNotNone(session.daemon.poll())
        self.assertIsNotNone(session.bridge.poll())
        self.assertFalse((session.runtime/'control.sock').exists())

    def test_busy_unmount_keeps_services_and_lock_until_retry(self):
        session = self.session(initialize=True)
        session.start()
        session.unmount_volume.side_effect = subprocess.CalledProcessError(1, 'umount')
        with self.assertRaises(launcher.UnmountBusy):
            session.stop()
        self.assertIsNone(session.daemon.poll())
        self.assertIsNone(session.bridge.poll())
        self.assertIsNotNone(session.lock)
        session.unmount_volume.side_effect = lambda: setattr(session, 'fake_mounted', False)
        session.stop()
        self.assertIsNone(session.lock)

    def test_duplicate_runtime_does_not_touch_running_session(self):
        first = self.session(initialize=True)
        first.start()
        second = self.session()
        with self.assertRaisesRegex(RuntimeError, 'another launcher'):
            second.start()
        second.stop()
        self.assertIsNone(first.daemon.poll())
        self.assertIsNone(first.bridge.poll())
        second.mount_volume.assert_not_called()

    def test_duplicate_mountpoint_does_not_touch_running_session(self):
        first = self.session(initialize=True)
        first.start()
        second = self.session()
        second.runtime = self.root/'other-run'
        with self.assertRaisesRegex(RuntimeError, 'mount point'):
            second.start()
        second.stop()
        self.assertIsNone(first.daemon.poll())
        second.mount_volume.assert_not_called()

    def test_repository_creation_is_explicit(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'does not exist'):
            session.start()
        self.assertFalse(session.repository.exists())
        self.assertIsNone(session.daemon)
        self.assertIsNone(session.bridge)

    def test_initialize_refuses_existing_repository(self):
        session = self.session(initialize=True)
        session.repository.mkdir()
        marker = session.repository/'existing'
        marker.write_text('keep')
        with self.assertRaisesRegex(RuntimeError, 'new repository'):
            session.start()
        self.assertEqual(marker.read_text(), 'keep')

    def test_invalid_world_stops_before_os_mount(self):
        session = self.session(initialize=True, world='missing')
        with self.assertRaisesRegex(RuntimeError, 'bridge exited'):
            session.start()
        session.stop()
        self.assertIsNotNone(session.daemon.poll())
        session.mount_volume.assert_not_called()

    def test_daemon_loss_is_observable(self):
        session = self.session(initialize=True)
        session.start()
        session.daemon.terminate()
        session.daemon.wait(timeout=10)
        self.assertEqual(session.failed_child(), 'daemon')

    def test_dead_backend_forces_detach_after_normal_unmount_fails(self):
        session = self.session(initialize=True)
        session.start()
        session.daemon.terminate()
        session.daemon.wait(timeout=10)
        def unmount(*, force=False):
            if not force:
                raise subprocess.CalledProcessError(1, 'umount')
            session.fake_mounted = False
        session.unmount_volume.side_effect = unmount
        session.stop()
        self.assertEqual(session.unmount_volume.call_count, 2)
        self.assertEqual(session.unmount_volume.call_args.kwargs, {'force': True})
        self.assertIsNotNone(session.bridge.poll())

    def test_healthy_busy_mount_is_never_force_unmounted(self):
        session = self.session(initialize=True)
        session.start()
        session.unmount_volume.side_effect = subprocess.CalledProcessError(1, 'umount')
        with self.assertRaises(launcher.UnmountBusy):
            session.stop()
        session.unmount_volume.assert_called_once_with()

    def test_crash_removes_the_owned_stale_socket(self):
        session = self.session(initialize=True)
        session.start()
        session.daemon.kill()
        session.daemon.wait(timeout=10)
        self.assertTrue((session.runtime/'control.sock').exists())
        session.stop()
        self.assertFalse((session.runtime/'control.sock').exists())

    def test_replaced_socket_path_is_preserved_after_crash(self):
        session = self.session(initialize=True)
        session.start()
        session.daemon.kill()
        session.daemon.wait(timeout=10)
        endpoint = session.runtime/'control.sock'
        endpoint.unlink()
        endpoint.write_text('replacement owned by another operation')
        session.stop()
        self.assertEqual(endpoint.read_text(), 'replacement owned by another operation')

    def test_nonempty_mountpoint_is_preserved(self):
        session = self.session(initialize=True)
        session.mountpoint.mkdir(mode=0o700)
        marker = session.mountpoint/'existing'
        marker.write_text('keep')
        with self.assertRaisesRegex(RuntimeError, 'must be empty'):
            session.start()
        self.assertEqual(marker.read_text(), 'keep')
        self.assertIsNone(session.daemon)

    def test_busy_cleanup_waits_for_signal_without_automatic_retry(self):
        self._check_cleanup_retry(None, None, [False, True], busy_calls=1, expected_waits=2)

    def test_child_failure_during_busy_wait_triggers_cleanup_retry(self):
        self._check_cleanup_retry(None, 'bridge', [False], busy_calls=1, expected_waits=1)

    def test_preexisting_child_failure_does_not_spin_busy_cleanup(self):
        self._check_cleanup_retry('daemon', None, [True, True], busy_calls=2, expected_waits=2)

    def test_initial_signal_is_consumed_once_before_cleanup(self):
        session, event = self._check_cleanup_retry(
            None, None, [False, True], busy_calls=1, expected_waits=2, initially_set=True)
        self.assertEqual(event.clear_count, 2)
        self.assertEqual(session.stop_count, 2)

    def test_signal_during_stop_is_not_discarded(self):
        session, event = self._check_cleanup_retry(
            None, None, [], busy_calls=1, expected_waits=1, signal_during_stop=True)
        self.assertEqual(session.stop_count, 2)
        self.assertEqual(event.wait_count, 1)

    def test_new_child_failure_retries_once_then_waits_if_detach_stays_busy(self):
        for child in ('daemon', 'bridge'):
            with self.subTest(child=child):
                session, event = self._check_cleanup_retry(
                    None, None, [False, True], busy_calls=2, expected_waits=2,
                    child_after_first_stop=child)
                self.assertEqual(session.stop_count, 3)
                self.assertEqual(event.wait_count, 2)

    def _check_cleanup_retry(self, initial_child, child_on_wait, waits, busy_calls, expected_waits,
                             initially_set=False, signal_during_stop=False,
                             child_after_first_stop=None):
        class Event:
            def __init__(self, session):
                self.outcomes = iter(waits)
                self.set = initially_set
                self.wait_count = 0
                self.clear_count = 0
                self.session = session

            def clear(self):
                self.clear_count += 1
                self.set = False

            def is_set(self):
                return self.set

            def wait(self, _timeout):
                self.wait_count += 1
                if not self.set:
                    self.set = next(self.outcomes)
                if child_on_wait:
                    self.session.child = child_on_wait
                return self.set

        class Session:
            def __init__(self):
                self.child = initial_child
                self.stop_count = 0
                self.busy_calls = busy_calls
                self.event = None

            def failed_child(self):
                return self.child

            def stop(self):
                self.stop_count += 1
                if self.stop_count == 1:
                    if signal_during_stop:
                        self.event.set = True
                    if child_after_first_stop:
                        self.child = child_after_first_stop
                if self.stop_count <= self.busy_calls:
                    raise launcher.UnmountBusy('busy')

        session = Session()
        event = Event(session)
        session.event = event
        result = launcher._stop_until_detached(session, event, 0, poll_interval=0)
        self.assertEqual(result, 1)
        self.assertEqual(event.wait_count, expected_waits)
        self.assertEqual(session.stop_count, busy_calls + 1)
        return session, event


if __name__ == '__main__':
    unittest.main()
