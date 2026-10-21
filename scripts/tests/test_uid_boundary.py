"""Nonprivileged admission tests for the opt-in UID experiment."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uid_boundary as boundary
from restricted_workload import profile


class UIDBoundary(unittest.TestCase):
    def test_cli_reports_blocked_without_falling_back_to_unrestricted_execution(self):
        output = io.StringIO()
        with patch.object(sys, 'argv', ['uid_boundary.py', '--host-user', 'host', '--worker-user', 'worker']), \
             patch.object(boundary, 'experiment', side_effect=RuntimeError('root unavailable')) as run, \
             contextlib.redirect_stdout(output):
            self.assertEqual(boundary.main(), 2)
        self.assertEqual(run.call_count, 1)
        report = json.loads(output.getvalue())
        self.assertEqual(report['status'], 'blocked')
        self.assertFalse(report['mounted_acceptance'])

    def test_experiment_refuses_non_root_before_account_or_tool_access(self):
        for system, uid, euid in (('Linux', 0, 0), ('Darwin', 501, 501), ('Darwin', 501, 0)):
            with self.subTest(system=system, uid=uid, euid=euid), \
                 patch.object(boundary.platform, 'system', return_value=system), \
                 patch.object(boundary.os, 'getuid', return_value=uid), \
                 patch.object(boundary.os, 'geteuid', return_value=euid), \
                 patch.object(boundary, 'identities') as accounts, \
                 patch.object(boundary.subprocess, 'check_output') as tools:
                with self.assertRaisesRegex(RuntimeError, 'root'):
                    boundary.experiment('host', 'worker')
                accounts.assert_not_called(); tools.assert_not_called()

    def test_distinct_ids_and_groups_required(self):
        host = SimpleNamespace(pw_uid=501, pw_gid=20)
        for uid, gid in ((0, 70), (-2, 70), (501, 70), (70, 0), (70, 20)):
            worker = SimpleNamespace(pw_uid=uid, pw_gid=gid)
            with self.subTest(uid=uid, gid=gid), \
                 patch.object(boundary.pwd, 'getpwnam', side_effect=[host, worker]):
                with self.assertRaises(ValueError):boundary.identities('host', 'worker')
        worker = SimpleNamespace(pw_uid=70, pw_gid=70)
        with patch.object(boundary.pwd, 'getpwnam', side_effect=[host, worker]):
            self.assertEqual(boundary.identities('host', 'worker'), (host, worker))
        launch = boundary.credentials(worker)
        self.assertEqual((launch['user'], launch['group'], launch['extra_groups']), (70, 70, ()))
        self.assertTrue(launch['close_fds']); self.assertTrue(launch['start_new_session'])
        self.assertEqual(launch['umask'], 0o077)

    def test_experimental_policy_cannot_change_default_launcher(self):
        with tempfile.TemporaryDirectory() as name:
            paths = [Path(name) / part for part in ('work', 'backing', 'scratch')]
            for path in paths:path.mkdir()
            before = profile(*paths)
            experiment = boundary.experimental_profile(*paths)
            self.assertIn('(deny process-info*)', before)
            self.assertIn('(allow process-info*)', experiment)
            self.assertNotIn('(deny process-info*)', experiment)
            self.assertIn('(deny file* (subpath ', experiment)
            self.assertNotIn('(allow network', experiment)
            self.assertEqual(profile(*paths), before)
            with patch.object(boundary, 'profile', return_value='changed policy'):
                with self.assertRaisesRegex(ValueError, 'review experiment'):
                    boundary.experimental_profile(*paths)


if __name__ == '__main__':unittest.main()
