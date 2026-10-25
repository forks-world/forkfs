"""Lifecycle teardown ordering and failure quarantine contracts."""
import json
import contextlib
import os
from pathlib import Path
import stat
import shutil
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fskit'))
import mount_lifecycle
import agent_supervisor
import mount_session


def isolated_test_flags():
    flags = SimpleNamespace(**{name: getattr(sys.flags, name) for name in dir(sys.flags)
                               if not name.startswith('_')})
    flags.isolated = True
    flags.no_site = True
    return flags


class Probe:
    def __init__(self, events, mounted=()): self.events, self.mounted = events, set(mounted)
    def funmount_fd(self, fd): self.events.append('funmount'); self.mounted.clear()
    def mounted_fsids(self): self.events.append('enumerate'); return self.mounted


class MountLifecycle(unittest.TestCase):
    def test_session_volume_uuid_golden_vector(self):
        self.assertEqual(mount_lifecycle.expected_session_uuid(
            '0123456789abcdef0123456789abcdef',
            'fedcba9876543210fedcba9876543210'),
            'f5037f19-5ea9-8305-9558-863f32b2a19b')
        self.assertNotEqual(mount_lifecycle.expected_session_uuid('a'*32, 'b'*32),
                            mount_lifecycle.expected_session_uuid('a'*31+'c', 'b'*32))

    def bind_context(self, directory):
        root = Path(directory)
        receipt = dict(version=1, socket='/run/service/rpc.sock', service_uid=70, frontend_uid=71,
                       generation='a'*32, session='b'*32, name='main', read_only=False, lease_ms=300000)
        policy = dict(version=1, host_uid=50, service_uid=70, frontend_uid=71, worker_uid=72,
                      worker_gid=73, mount=str(root/'mount'), mount_fsid=[12, 34],
                      backing=str(root/'backing'), read_only=[])
        (root/'mount').mkdir()
        runtime = SimpleNamespace(admit_entry_environment=lambda: None, read_policy=lambda path: policy,
                                  trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
        probe = SimpleNamespace(require_mount=lambda path, expected_read_only: dict(fsid=[12, 34]),
                                descriptor_mount_info=lambda fd: dict(fstype='forkrevision', fskit=True,
                                    fsid=[12, 34], read_only=False, mountpoint=str(root/'mount')),
                                descriptor_volume_uuid=lambda fd: mount_lifecycle.expected_session_uuid(
                                    'a'*32, 'b'*32))
        original_lstat = os.lstat
        def service_lstat(path, *args, **kwargs):
            if str(path) == '/run/service':
                return SimpleNamespace(st_mode=stat.S_IFDIR | 0o711, st_uid=70)
            if str(path) == '/run/service/rpc.sock':
                return SimpleNamespace(st_mode=stat.S_IFSOCK | 0o600, st_uid=70)
            return original_lstat(path, *args, **kwargs)
        stack = contextlib.ExitStack()
        stack.enter_context(patch.object(mount_lifecycle.platform, 'system', return_value='Darwin'))
        stack.enter_context(patch.object(mount_lifecycle.os, 'getuid', return_value=0))
        stack.enter_context(patch.object(mount_lifecycle.os, 'geteuid', return_value=0))
        stack.enter_context(patch.object(mount_lifecycle, 'read_root_file', return_value=mount_lifecycle.canonical(receipt)))
        stack.enter_context(patch.object(mount_lifecycle.os, 'lstat', side_effect=service_lstat))
        stack.enter_context(patch.object(mount_session, 'request', return_value=dict(
            generation='a'*32, name='main', read_only=False)))
        return stack, runtime, policy, probe

    def guard(self, events, probe, rpc):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / 'record.json'
        record = dict(version=1, phase='stopping', policy_sha256='0'*64, mount='/mnt/world',
                      fsid=[12, 34], volume_uuid=mount_lifecycle.expected_session_uuid('a'*32,'b'*32),
                      service_uid=70, socket='/run/service/rpc.sock',
                      generation='a'*32, session='b'*32, name='worker', read_only=False)
        guard = mount_lifecycle.Guard(path, {}, type('Runtime', (), {'no_acl': lambda *args: None})(),
                                      probe=probe, rpc=rpc, sleep=lambda _: None)
        guard.record = record
        path.write_bytes(mount_lifecycle.canonical(record)); path.chmod(0o600)
        guard.record_fd = os.open(path, os.O_RDWR)
        guard.fd = os.open('/dev/null', os.O_RDONLY)
        self.addCleanup(guard.close)
        return guard, path

    def test_unmount_then_kernel_disappearance_then_current_generation_retirement(self):
        events = []
        def rpc(endpoint, uid, fields, timeout):
            events.append(tuple(fields))
            if fields == ['service-hello']: return dict(protocol=1, generation='c'*32)
            return {}
        guard, record_file = self.guard(events, Probe(events, [(12, 34)]), rpc)
        guard.finish()
        self.assertEqual(events[:3], ['enumerate', 'funmount', 'enumerate'])
        self.assertEqual(events[3], ('service-hello',))
        self.assertEqual(events[4], ('mount-retire', 'a'*32, 'b'*32, '--generation', 'c'*32))
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'retired')

    def test_unmount_error_keeps_stopping_record_and_never_retires_session(self):
        events = []
        class BrokenProbe(Probe):
            def funmount_fd(self, fd): events.append('funmount'); raise OSError('busy filesystem')
        guard, record_file = self.guard(events, BrokenProbe(events, [(12, 34)]), lambda *a, **k: events.append('rpc'))
        with self.assertRaisesRegex(OSError, 'busy filesystem'): guard.finish()
        self.assertEqual(events, ['enumerate', 'funmount'])
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'stopping')

    def test_already_absent_mount_skips_funmount_and_retires_after_verification(self):
        events = []
        def rpc(endpoint, uid, fields, timeout):
            events.append(tuple(fields))
            if fields == ['service-hello']: return dict(protocol=1, generation='c'*32)
            return {}
        guard, record_file = self.guard(events, Probe(events), rpc)
        guard.finish()
        self.assertNotIn('funmount', events)
        self.assertIn(('mount-retire', 'a'*32, 'b'*32, '--generation', 'c'*32), events)
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'retired')

    def test_filesystem_id_must_disappear_before_retirement(self):
        events = []
        class UncertainProbe(Probe):
            calls = 0
            def mounted_fsids(self):
                events.append('enumerate'); self.calls += 1
                if self.calls > 1: raise RuntimeError('mount table unavailable')
                return {(12, 34)}
        guard, record_file = self.guard(events, UncertainProbe(events, [(12, 34)]), lambda *a, **k: events.append('rpc'))
        with self.assertRaisesRegex(RuntimeError, 'mount table'): guard.finish()
        self.assertEqual(events, ['enumerate', 'funmount', 'enumerate'])
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'stopping')

    def test_retirement_failure_keeps_worker_session_quarantined(self):
        events = []
        def rpc(endpoint, uid, fields, timeout):
            events.append(tuple(fields))
            if fields == ['service-hello']: return dict(protocol=1, generation='c'*32)
            raise RuntimeError('daemon unavailable')
        guard, record_file = self.guard(events, Probe(events, [(12, 34)]), rpc)
        with self.assertRaisesRegex(RuntimeError, 'daemon unavailable'): guard.finish()
        self.assertEqual(events[1], 'funmount')
        self.assertIn(('mount-retire', 'a'*32, 'b'*32, '--generation', 'c'*32), events)
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'stopping')

    def test_atomic_phase_record_survives_short_writes_and_replace_fault(self):
        events = []
        guard, record_file = self.guard(events, Probe(events), lambda *a, **k: {})
        original_write = os.write
        def short_write(fd, data): return original_write(fd, data[:max(1, min(5, len(data)))])
        guard.record['phase'] = 'active'
        with patch.object(mount_lifecycle.os, 'write', side_effect=short_write): guard._persist()
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'active')
        guard.record['phase'] = 'stopping'
        with patch.object(mount_lifecycle.os, 'replace', side_effect=OSError('injected rename fault')):
            with self.assertRaisesRegex(OSError, 'rename fault'): guard._persist()
        self.assertEqual(json.loads(record_file.read_bytes())['phase'], 'active')

    def test_start_pins_and_attests_record_before_marking_active(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); record_path = root / 'record.json'; active = root / '70.active'
            mountpoint = root / 'mount'; mountpoint.mkdir()
            policy = dict(mount=str(mountpoint), mount_fsid=[12, 34], service_uid=70)
            record = dict(version=1, phase='bound', policy_sha256=mount_lifecycle.hashlib.sha256(
                mount_lifecycle.canonical(policy)).hexdigest(), mount=str(mountpoint), fsid=[12, 34],
                volume_uuid=mount_lifecycle.expected_session_uuid('a'*32,'b'*32),
                service_uid=70, socket='/run/service/rpc.sock', generation='a'*32, session='b'*32,
                name='worker', read_only=False)
            record_path.write_bytes(mount_lifecycle.canonical(record)); record_path.chmod(0o600)
            active.write_text('pending'); active.chmod(0o600)
            events = []
            class TestProbe(Probe):
                def descriptor_mount_info(self, fd):
                    events.append('attest')
                    return dict(fstype='forkrevision', fskit=True, fsid=[12, 34], read_only=False,
                                mountpoint=str(mountpoint))
                def descriptor_volume_uuid(self, fd):
                    return record['volume_uuid']
            probe = TestProbe(events, [(12, 34)])
            runtime = SimpleNamespace(trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
            original_fstat = os.fstat
            def root_fstat(fd):
                value = original_fstat(fd)
                return SimpleNamespace(**{name: (0 if name == 'st_uid' else getattr(value, name))
                                          for name in dir(value) if name.startswith('st_')})
            def rpc(endpoint, uid, fields, timeout):
                events.append(tuple(fields))
                return dict(generation='a'*32, name='worker', read_only=False)
            guard = mount_lifecycle.Guard(record_path, policy, runtime, probe=probe, rpc=rpc)
            with patch.object(mount_lifecycle.os, 'fstat', side_effect=root_fstat), \
                 patch.object(mount_lifecycle, 'load_record', return_value=record):
                health = guard.start(active, root / 'session')
            self.addCleanup(guard.close)
            self.assertEqual(guard.record['phase'], 'active')
            self.assertEqual(json.loads(active.read_bytes())['status'], 'active')
            self.assertEqual(events[:2], ['attest', ('mount-check', 'b'*32, '--generation', 'a'*32)])
            with patch.object(mount_lifecycle.os, 'fstat', side_effect=root_fstat): health()

    def test_parent_pipe_eof_triggers_explicit_recovery(self):
        death_read, death_write = os.pipe()
        ready_read, ready_write = os.pipe()
        result = []
        flags = isolated_test_flags()
        runtime = SimpleNamespace(ENTRY_ENV={}, admit_entry_environment=lambda: None,
                                  read_policy=lambda path: dict(worker_uid=70))
        lifecycle_module = SimpleNamespace(load_record=lambda path, runtime: dict(phase='active'))
        with patch.object(agent_supervisor.platform, 'system', return_value='Darwin'), \
             patch.object(agent_supervisor.os, 'getuid', return_value=0), \
             patch.object(agent_supervisor.os, 'geteuid', return_value=0), \
             patch.object(agent_supervisor.sys, 'flags', flags), \
             patch.object(agent_supervisor, 'installed_launcher', return_value=runtime), \
             patch.object(agent_supervisor, 'trusted_interpreter', return_value=Path('/python')), \
             patch.object(agent_supervisor.importlib, 'import_module', return_value=lifecycle_module), \
             patch.object(agent_supervisor, 'recover_mount', return_value=dict(status='recovered')) as recover:
            thread = threading.Thread(target=lambda: result.append(agent_supervisor.await_parent_and_recover(
                '/policy', '/state', '/record', death_read, ready_write)))
            thread.start()
            self.assertEqual(os.read(ready_read, 1), b'R')
            os.close(ready_read)
            os.close(death_write)
            thread.join(timeout=2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result, [dict(status='recovered')])
        recover.assert_called_once_with('/policy', '/state', '/record')

    def test_recovery_rejects_policy_mismatch_without_touching_mount(self):
        events = []
        guard, _ = self.guard(events, Probe(events), lambda *a, **k: events.append('rpc'))
        guard.runtime.trusted_path = lambda *a, **k: None
        original_fstat = os.fstat
        def root_fstat(fd):
            value = original_fstat(fd)
            return SimpleNamespace(**{name: (0 if name == 'st_uid' else getattr(value, name))
                                      for name in dir(value) if name.startswith('st_')})
        with patch.object(mount_lifecycle.os, 'fstat', side_effect=root_fstat), \
             patch.object(mount_lifecycle, 'load_record', return_value=guard.record):
            with self.assertRaisesRegex(ValueError, 'policy'):
                guard.recover()
        self.assertEqual(events, [])

    def test_recovery_rejects_same_fsid_replacement_uuid(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); mountpoint = root/'mount'; mountpoint.mkdir()
            path = root/'record.json'
            policy = dict(mount=str(mountpoint), mount_fsid=[12, 34], service_uid=70)
            record = dict(version=1, phase='active', policy_sha256=mount_lifecycle.hashlib.sha256(
                mount_lifecycle.canonical(policy)).hexdigest(), mount=str(mountpoint), fsid=[12, 34],
                volume_uuid=mount_lifecycle.expected_session_uuid('a'*32,'b'*32), service_uid=70,
                socket='/run/service/rpc.sock', generation='a'*32, session='b'*32, name='worker', read_only=False)
            path.write_bytes(mount_lifecycle.canonical(record)); path.chmod(0o600)
            events = []
            class ReplacementProbe:
                def mounted_fsids(self): events.append('fsids'); return {(12, 34)}
                def descriptor_mount_info(self, fd):
                    return dict(fstype='forkrevision', fskit=True, fsid=[12, 34], read_only=False,
                                mountpoint=str(mountpoint))
                def descriptor_volume_uuid(self, fd): return mount_lifecycle.expected_session_uuid('c'*32,'d'*32)
                def funmount_fd(self, fd): events.append('unsafe-funmount')
            runtime = SimpleNamespace(trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
            guard = mount_lifecycle.Guard(path, policy, runtime, probe=ReplacementProbe())
            self.addCleanup(guard.close)
            original_fstat = os.fstat
            def root_fstat(fd):
                value = original_fstat(fd)
                return SimpleNamespace(**{name: (0 if name == 'st_uid' else getattr(value, name))
                                          for name in dir(value) if name.startswith('st_')})
            with patch.object(mount_lifecycle.os, 'fstat', side_effect=root_fstat), \
                 patch.object(mount_lifecycle, 'load_record', return_value=record):
                with self.assertRaisesRegex(RuntimeError, 'pinned FSKit mount'):
                    guard.recover()
            self.assertEqual(events, ['fsids'])
            self.assertNotIn('unsafe-funmount', events)

    def test_record_schema_rejects_bool_version_and_nonboolean_read_only(self):
        runtime = SimpleNamespace(trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
        base = dict(version=1, phase='bound', policy_sha256='0'*64, mount='/mnt/world', fsid=[12, 34],
                    volume_uuid=mount_lifecycle.expected_session_uuid('a'*32,'b'*32),
                    service_uid=70, socket='/run/service/rpc.sock', generation='a'*32, session='b'*32,
                    name='worker', read_only=False)
        with patch.object(mount_lifecycle, 'read_root_file', return_value=b'{}'):
            for altered in (dict(base, version=True), dict(base, read_only=1)):
                with self.subTest(altered=altered), \
                     patch.object(mount_lifecycle, 'read_root_file', return_value=mount_lifecycle.canonical(altered)):
                    with self.assertRaises(ValueError): mount_lifecycle.load_record('/record', runtime)

    def test_bind_short_writes_publish_complete_record(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'record.json'
            stack, runtime, _, probe = self.bind_context(directory)
            original_write = os.write
            def short_write(fd, data): return original_write(fd, data[:max(1, min(7, len(data)))])
            with stack, patch.object(mount_lifecycle.os, 'write', side_effect=short_write):
                result = mount_lifecycle.bind('/receipt', '/policy', output, runtime, probe=probe)
            self.assertEqual(json.loads(output.read_bytes()), result)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)

    def test_bind_does_not_replace_foreign_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'record.json'; output.write_bytes(b'keep existing')
            stack, runtime, _, probe = self.bind_context(directory)
            with stack:
                with self.assertRaises(FileExistsError):
                    mount_lifecycle.bind('/receipt', '/policy', output, runtime, probe=probe)
            self.assertEqual(output.read_bytes(), b'keep existing')

    def test_recovery_finishes_partial_scratch_cleanup_after_retirement(self):
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory); state.chmod(0o711)
            policy = dict(version=1, worker_uid=70, worker_gid=71, mount='/mnt/world',
                          mount_fsid=[12, 34], service_uid=72)
            fingerprint = mount_lifecycle.hashlib.sha256(
                json.dumps(policy, sort_keys=True, separators=(',', ':')).encode()).hexdigest().encode()
            (state/'70.binding').write_bytes(fingerprint)
            record_path = '/record'
            marker = dict(status='active', mount_record=record_path, session_dir=str(state/'70-session'))
            active = state/'70.active'; active.write_text(json.dumps(marker)); active.chmod(0o600)
            events = []
            class Guard:
                record = dict(phase='retired')
                def __init__(self, *args): pass
                def recover(self): events.append('guard-recover')
                def finish(self): events.append('guard-finish')
                def close(self): events.append('guard-close')
            lifecycle_module = SimpleNamespace(load_record=lambda *a: dict(phase='retired'), Guard=Guard)
            runtime = SimpleNamespace(ENTRY_ENV={}, admit_entry_environment=lambda: None,
                read_policy=lambda path: policy, trusted_path=lambda *a, **k: None, no_acl=lambda *a: None)
            flags = isolated_test_flags()
            with patch.object(agent_supervisor.platform, 'system', return_value='Darwin'), \
                 patch.object(agent_supervisor.os, 'getuid', return_value=0), \
                 patch.object(agent_supervisor.os, 'geteuid', return_value=0), \
                 patch.object(agent_supervisor.sys, 'flags', flags), \
                 patch.object(agent_supervisor, 'installed_launcher', return_value=runtime), \
                 patch.object(agent_supervisor, 'trusted_interpreter', return_value=Path('/python')), \
                 patch.object(agent_supervisor.pwd, 'getpwuid', return_value=SimpleNamespace(
                     pw_gid=71, pw_shell='/usr/bin/false')), \
                 patch.object(agent_supervisor.importlib, 'import_module', return_value=lifecycle_module), \
                 patch.object(agent_supervisor, 'reap_identity', side_effect=lambda *a: events.append('reap')) as reap, \
                 patch.object(agent_supervisor, 'uid_processes', return_value=set()), \
                 patch.object(agent_supervisor.Path, 'resolve', lambda self, strict=False: self), \
                 patch.object(shutil.rmtree, 'avoids_symlink_attacks', True):
                original_fstat = os.fstat
                def root_fstat(fd):
                    value = original_fstat(fd)
                    return SimpleNamespace(**{name: (0 if name == 'st_uid' else getattr(value, name))
                                              for name in dir(value) if name.startswith('st_')})
                with patch.object(agent_supervisor.os, 'fstat', side_effect=root_fstat):
                    result = agent_supervisor.recover_mount('/policy', state, record_path)
            self.assertEqual(result['status'], 'recovered')
            self.assertEqual(events, ['reap', 'guard-recover', 'guard-finish', 'guard-close'])
            self.assertFalse(active.exists())
            reap.assert_called_once()


if __name__ == '__main__': unittest.main()
