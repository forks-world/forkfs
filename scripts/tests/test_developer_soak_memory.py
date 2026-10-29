"""Memory sampling must fail visibly, tolerate exited PIDs, and retain evidence."""
import io
import json
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch
from developer_soak import Soak, process_rss


class ProcessMemoryTests(unittest.TestCase):
    def process(self, polls=(None, None)):
        return SimpleNamespace(pid=123, poll=Mock(side_effect=polls))

    def test_ps_units_and_fixed_argv(self):
        result = SimpleNamespace(returncode=0, stdout='  12345\n', stderr='')
        with patch('developer_soak.subprocess.run', return_value=result) as run:
            self.assertEqual(process_rss(self.process()), {'pid': 123, 'rss_bytes': 12345 * 1024})
        self.assertEqual(run.call_args.args[0], ['/bin/ps', '-o', 'rss=', '-p', '123'])
        self.assertNotIn('shell', run.call_args.kwargs)

    def test_missing_or_exited_process_does_not_launch_ps(self):
        with patch('developer_soak.subprocess.run') as run:
            self.assertIsNone(process_rss(None))
            self.assertIsNone(process_rss(self.process((0,))))
        run.assert_not_called()

    def test_exit_during_sample_rejects_even_valid_reused_pid_result(self):
        with patch('developer_soak.subprocess.run', return_value=SimpleNamespace(returncode=0, stdout='100', stderr='')):
            self.assertIsNone(process_rss(self.process((None, 0))))

    def test_live_process_sample_errors_never_become_zero_rss(self):
        for code, value in ((1, ''), (0, ''), (0, '0'), (0, '-1'), (0, 'nan'),
                            (0, '12 34'), (0, '1\n2'), (0, '１２')):
            with self.subTest(code=code, value=value):
                with patch('developer_soak.subprocess.run', return_value=SimpleNamespace(returncode=code, stdout=value, stderr='ps error')):
                    with self.assertRaisesRegex(RuntimeError, 'cannot sample live daemon RSS'):
                        process_rss(self.process())
        with patch('developer_soak.subprocess.run', side_effect=subprocess.TimeoutExpired('ps', 2)):
            with self.assertRaises(subprocess.TimeoutExpired):
                process_rss(self.process())

    def trial(self, limit):
        trial = Soak.__new__(Soak)
        trial.proc = self.process()
        trial.args = SimpleNamespace(max_rss_mib=limit)
        trial.phase = 'test'
        trial.memory_lock = threading.Lock()
        trial.memory_log = io.StringIO()
        trial.memory_sample_count = trial.memory_peak_bytes = 0
        return trial

    def test_limit_boundary_peak_and_failure_evidence(self):
        trial = self.trial(1)
        for size in (1024 * 1024, 42, 1024 * 1024 + 1):
            with patch('developer_soak.process_rss', return_value={'pid': 123, 'rss_bytes': size}):
                if size > 1024 * 1024:
                    with self.assertRaisesRegex(AssertionError, 'daemon RSS budget exceeded'):
                        trial.sample_memory()
                else:
                    trial.sample_memory()
        rows = [json.loads(line) for line in trial.memory_log.getvalue().splitlines()]
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[-1]['rss_bytes'], 1024 * 1024 + 1)
        self.assertEqual(trial.memory_peak_bytes, 1024 * 1024 + 1)
        self.assertEqual(trial.memory_sample_count, 3)

    def test_no_limit_and_exit_skip(self):
        trial = self.trial(None)
        with patch('developer_soak.process_rss', return_value={'pid': 123, 'rss_bytes': 2**40}):
            trial.sample_memory()
        with patch('developer_soak.process_rss', return_value=None):
            trial.sample_memory()
        self.assertEqual(trial.memory_sample_count, 1)
        self.assertEqual(trial.memory_peak_bytes, 2**40)

    def test_sampler_failure_stops_and_reaches_foreground_guard(self):
        trial = self.trial(None)
        trial.sample_stop = threading.Event()
        trial.background_errors = []
        trial.remaining = Mock()
        trial.sample_memory = Mock(side_effect=RuntimeError('RSS unavailable'))
        trial.start_sampler()
        trial.sample_thread.join(timeout=5)
        self.assertFalse(trial.sample_thread.is_alive())
        self.assertTrue(trial.sample_stop.is_set())
        with self.assertRaisesRegex(AssertionError, 'RSS unavailable'):
            trial.check_background()

    def test_missing_samples_cannot_publish_success_summary(self):
        trial = self.trial(None)
        trial.root = Path('/unused')
        trial.sample_stop = threading.Event()
        trial.sample_thread = None
        trial.check_background = Mock()
        trial.check_artifact_budget = Mock()
        with patch('developer_soak._workload.Workload.report') as report:
            with self.assertRaisesRegex(AssertionError, 'no daemon RSS samples'):
                trial.report('passed')
        report.assert_not_called()

    def test_real_child_process_and_exit(self):
        proc = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.buffer.read()'], stdin=subprocess.PIPE)
        try:
            result = process_rss(proc)
            self.assertEqual(result['pid'], proc.pid)
            self.assertGreater(result['rss_bytes'], 0)
        finally:
            proc.terminate()
            proc.wait(timeout=5)
            proc.stdin.close()
        self.assertIsNone(process_rss(proc))
