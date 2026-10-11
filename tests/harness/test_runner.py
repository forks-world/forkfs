import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import regression


class RunnerTests(unittest.TestCase):
    def fixture(self, root):
        return regression.Fixture(Path(sys.executable), Path(root))

    def test_crash_is_not_expected_refusal(self):
        with tempfile.TemporaryDirectory() as root:
            result = subprocess.CompletedProcess([], -9, b'', b'')
            with patch.object(regression.subprocess, 'run', return_value=result):
                with self.assertRaises(AssertionError):
                    self.fixture(root).call('fs-unlink', '/a', error=True)
            event = json.loads((Path(root) / 'events.jsonl').read_text())
            self.assertEqual(event['returncode'], -9)

    def test_wrong_error_is_not_expected_refusal(self):
        with tempfile.TemporaryDirectory() as root:
            result = subprocess.CompletedProcess([], 1, b'', b'cannot open repository')
            with patch.object(regression.subprocess, 'run', return_value=result):
                with self.assertRaises(AssertionError):
                    self.fixture(root).call('fs-unlink', '/a', error='revision view is read-only')

    def test_timeout_is_logged(self):
        with tempfile.TemporaryDirectory() as root:
            with patch.object(regression.subprocess, 'run', side_effect=subprocess.TimeoutExpired([], 15)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    self.fixture(root).call('fs-cat', '/a')
            self.assertEqual(json.loads((Path(root) / 'events.jsonl').read_text())['failure'], 'timeout')

    def test_acceptance_cannot_pass_missing_capabilities(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / 'run'
            args = ['runner', '--binary', sys.executable, '--output', str(output), '--profile', 'acceptance']
            with patch.object(sys, 'argv', args), patch.object(regression, 'CASES', [('revision', 'test', lambda f: None)]), patch.object(regression.Fixture, 'call', return_value=b''), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(regression.main(), 1)
            self.assertEqual(json.loads((output / 'result.json').read_text())['status'], 'unavailable')

    def test_conflicting_case_selection_is_rejected_before_setup(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / 'run'
            args = ['runner', '--binary', sys.executable, '--output', str(output), '--suite', 'fork', '--case', 'FS004']
            with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    regression.main()
            self.assertEqual(error.exception.code, 2)
            self.assertFalse(output.exists())

    def test_service_shutdown_timeout_kills_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as root:
            fixture = self.fixture(root)
            fixture.process = Mock()
            fixture.process.poll.return_value = None
            fixture.process.wait.side_effect = [subprocess.TimeoutExpired([], 5), None]
            fixture.socket_root = Mock()
            stream = Mock()
            fixture.service_logs = [stream]
            with self.assertRaisesRegex(AssertionError, 'shutdown timed out'):
                fixture.close()
            fixture.process.terminate.assert_called_once()
            fixture.process.kill.assert_called_once()
            stream.close.assert_called_once()
            fixture.socket_root.cleanup.assert_called_once()

    def test_service_crash_fails_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as root:
            fixture = self.fixture(root)
            fixture.process = Mock()
            fixture.process.poll.return_value = -9
            fixture.process.returncode = -9
            fixture.socket_root = Mock()
            with self.assertRaisesRegex(AssertionError, 'exited unexpectedly'):
                fixture.close()
            fixture.socket_root.cleanup.assert_called_once()
            fixture.process.terminate.assert_not_called()

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as root:
            marker = Path(root) / 'keep'
            marker.write_text('owned elsewhere')
            with patch.object(sys, 'argv', ['runner', '--binary', sys.executable, '--output', root]):
                with self.assertRaises(FileExistsError):
                    regression.main()
            self.assertEqual(marker.read_text(), 'owned elsewhere')


if __name__ == '__main__':
    unittest.main()
