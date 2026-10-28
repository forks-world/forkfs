"""Negative oracles for maintenance state and tail-latency reporting."""
import json
import unittest
from types import SimpleNamespace
from pathlib import Path
import threading
from unittest.mock import patch
from unittest.mock import Mock
from developer_soak import Soak, rpc_distribution, gc_job_rates


def status(state='blocked', pending=False, skipped=True):
    return {'state': state, 'active': False, 'last_step': {
        'logical_sweep_skipped': skipped, 'physical_pending': pending,
        'mark_pending': pending, 'pending_segments': int(pending)}}


class SoakOracleTests(unittest.TestCase):
    def trial(self, states):
        trial = Soak.__new__(Soak)
        trial.gc_status = Mock(side_effect=states)
        trial.start_gc = Mock()
        trial.logical_sweep_skipped = False
        return trial

    def test_retention_is_not_physical_convergence(self):
        pending, done = status(pending=True), status()
        trial = self.trial([pending, done])
        self.assertIs(trial.drain_gc('test', allow_logical_skip=True), done)
        trial.start_gc.assert_called_once_with('test')
        self.assertTrue(trial.logical_sweep_skipped)

    def test_failed_gc_never_passes_even_with_old_complete_report(self):
        for state in ('failed', 'stopped', 'idle'):
            with self.subTest(state=state), self.assertRaises(AssertionError):
                self.trial([status(state=state)]).drain_gc('test', allow_logical_skip=True)

    def test_complete_must_have_no_pending_work(self):
        with self.assertRaises(AssertionError):
            self.trial([status(state='complete', pending=True, skipped=False)]).drain_gc('test')

    def test_gc_report_objects_and_strings_validate(self):
        trial = self.trial([])
        report = status()['last_step']
        self.assertEqual(trial.gc_report({'last_step': report}), report)
        self.assertEqual(trial.gc_report({'last_step': json.dumps(report)}), report)
        for wrong in ('{', '[]', [], 17):
            with self.subTest(wrong=wrong), self.assertRaises(AssertionError):
                trial.gc_report({'last_step': wrong})

    def test_background_storage_failure_stops_next_request(self):
        trial = self.trial([])
        trial.shutdown_phase = False
        trial.check_background = Mock(side_effect=AssertionError('storage budget exceeded'))
        with patch('developer_soak._workload.Workload.rpc') as transport:
            with self.assertRaisesRegex(AssertionError, 'storage budget'):
                trial.rpc('fs-write', '/file', b'bytes')
            transport.assert_not_called()

    def test_replayed_repository_has_its_own_storage_limit(self):
        trial = self.trial([])
        trial.args = SimpleNamespace(max_store_mib=1)
        trial.replayed_storage = {}
        trial.cli = Mock()
        trial.start = Mock()
        with patch('developer_soak._workload.disk_usage', return_value={'allocated_bytes': 2 * 1024 * 1024}):
            with self.assertRaisesRegex(AssertionError, 'replayed repository storage budget'):
                trial.replay_verify(Path('/owned/archive'), Path('/owned/restored'))
        trial.start.assert_not_called()

    def test_request_recording_budget_is_enforced(self):
        trial = self.trial([])
        trial.shutdown_phase = False;trial.check_background = Mock()
        trial.lock = threading.Lock();trial.args = SimpleNamespace(max_rpcs=1)
        trial.rpc_requests = 1
        with patch('developer_soak._workload.Workload.rpc') as transport:
            with self.assertRaisesRegex(AssertionError, 'RPC recording budget'):
                trial.rpc('status')
            transport.assert_not_called()

    def test_final_artifact_guard_does_not_depend_on_sampler_tick(self):
        trial = self.trial([])
        trial.args = SimpleNamespace(max_artifact_mib=1);trial.root = Path('/owned/artifacts')
        with patch('developer_soak._workload.disk_usage', return_value={'allocated_bytes': 2 * 1024 * 1024}):
            with self.assertRaisesRegex(AssertionError, 'artifact storage budget'):
                trial.check_artifact_budget()

    def test_job_rate_uses_first_terminal_observation(self):
        events = [
            {'kind':'gc-start', 'gc_attempt':1, 'requested_perf_ns':0, 'perf_ns':1, 'status':{'active':True}},
            {'kind':'gc-status', 'gc_attempt':1, 'perf_ns':1000000000,
             'status':{'active':False, 'state':'complete', 'steps':10}},
            {'kind':'gc-status', 'gc_attempt':1, 'perf_ns':9000000000,
             'status':{'active':False, 'state':'complete', 'steps':10}},
        ]
        self.assertEqual(gc_job_rates(events)[0]['completed_steps_per_second'], 10)

    def test_insufficient_samples_never_fabricate_p99(self):
        self.assertIsNone(rpc_distribution([10, 20])['p99_ms'])
        self.assertEqual(rpc_distribution(list(range(1, 1001)))['p99_ms'], .00099)
