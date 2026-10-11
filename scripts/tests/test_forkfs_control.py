import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('forkfs_control', Path(__file__).resolve().parents[1] / 'forkfs.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)

class ControlTests(unittest.TestCase):
    def test_rpc_branch_arguments_are_literal(self):
        result = subprocess.CompletedProcess([], 0)
        with patch.object(control.shutil, 'which', return_value=sys.executable), patch.object(control.subprocess, 'run', return_value=result) as run:
            self.assertEqual(control.main(['--socket', '/tmp/control.sock', '--branch', 'child', 'rename', '/a;literal', '/b']), 0)
            self.assertEqual(run.call_args.args[0], [str(Path(sys.executable).resolve()), 'request', str(Path('/tmp/control.sock').resolve()), 'fs-rename', '/a;literal', '/b', '--world', 'child'])
            self.assertNotIn('shell', run.call_args.kwargs)

    def test_timeout_is_nonzero(self):
        with patch.object(control.shutil, 'which', return_value=sys.executable), patch.object(control.subprocess, 'run', side_effect=subprocess.TimeoutExpired([], 30)), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(control.main(['--repo', '/tmp/store', 'check']), 124)

    def test_rpc_timeout_is_distinct_from_ordinary_errors(self):
        for code, message, expected in [(1, b'forkfsd: RPC timeout\n', 124),
                                        (1, b'forkfsd: RPC connect failed\n', 1),
                                        (1, b'other: RPC timeout\n', 1),
                                        (2, b'forkfsd: RPC timeout\n', 2)]:
            def run(*args, **kwargs):
                kwargs['stderr'].write(message)
                return subprocess.CompletedProcess([], code)
            output = io.StringIO()
            with self.subTest(code=code, message=message), patch.object(control.shutil, 'which', return_value=sys.executable), patch.object(control.subprocess, 'run', side_effect=run), contextlib.redirect_stderr(output):
                self.assertEqual(control.main(['--socket', '/tmp/control.sock', 'check']), expected)
            self.assertTrue(output.getvalue().startswith(message.decode()))
            self.assertEqual('commit outcome' in output.getvalue(), expected == 124)

    def test_rpc_outer_timeout_preserves_diagnostics(self):
        def run(*args, **kwargs):
            kwargs['stderr'].write(b'partial diagnostic\n')
            raise subprocess.TimeoutExpired([], 30)
        output = io.StringIO()
        with patch.object(control.shutil, 'which', return_value=sys.executable), patch.object(control.subprocess, 'run', side_effect=run), contextlib.redirect_stderr(output):
            self.assertEqual(control.main(['--socket', '/tmp/control.sock', 'check']), 124)
        self.assertTrue(output.getvalue().startswith('partial diagnostic\n'))
        self.assertIn('commit outcome', output.getvalue())

    def test_crash_is_nonzero(self):
        with patch.object(control.shutil, 'which', return_value=sys.executable), patch.object(control.subprocess, 'run', return_value=subprocess.CompletedProcess([], -9)):
            self.assertEqual(control.main(['--repo', '/tmp/store', 'check']), 137)

    def test_mount_never_invokes_binary(self):
        with patch.object(control.shutil, 'which') as find, patch.object(control.subprocess, 'run') as run, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(control.main(['--repo', '/tmp/store', 'mount', '/tmp/unused']), 3)
            find.assert_not_called()
            run.assert_not_called()

    def test_empty_selectors_rejected_before_execution(self):
        for selector in ['--branch', '--revision']:
            with self.subTest(selector=selector), patch.object(control.shutil, 'which') as find, patch.object(control.subprocess, 'run') as run, patch.object(control.os, 'execv') as execute, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    control.main(['--repo', '/tmp/store', selector, '', 'unlink', '/a'])
                self.assertEqual(error.exception.code, 2)
                find.assert_not_called()
                run.assert_not_called()
                execute.assert_not_called()

    def test_invalid_selector_rejected_before_execution(self):
        with patch.object(control.shutil, 'which') as find, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                control.main(['--repo', '/tmp/store', '--revision', 'base', 'serve', '--listen', '/tmp/control.sock'])
            self.assertEqual(error.exception.code, 2)
            find.assert_not_called()

if __name__ == '__main__':
    unittest.main()
