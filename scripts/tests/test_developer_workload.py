"""Check the workload oracle rejects plausible filesystem regressions."""
import json
import io
import struct
import tempfile
import threading
from pathlib import Path
from unittest.mock import patch
import unittest
from developer_workload import Node, Workload, distribution, check_recovered_save, disk_usage


class OracleTests(unittest.TestCase):
    def trial(self, *, payload=b'expected', names=('file',), mode=0o644, size=8):
        trial = Workload.__new__(Workload)
        def rpc(op, path, **selector):
            if op == 'fs-lstat':
                return json.dumps(dict(directory=path == '/', symlink=False,
                                       mode=0o755 if path == '/' else mode, size=size)).encode()
            if op == 'fs-ls':
                return ('\n'.join(names) + '\n').encode()
            if op == 'fs-cat':
                return payload
            raise AssertionError(op)
        trial.rpc = rpc
        return trial

    @property
    def model(self):
        return {'/': Node('dir', mode=0o755), '/file': Node('file', b'expected')}

    def test_correct_tree(self):
        self.trial().verify(self.model, world='main')

    def test_wrong_payload_same_size(self):
        with self.assertRaises(AssertionError):
            self.trial(payload=b'corrupt!').verify(self.model, world='main')

    def test_missing_and_unexpected_names(self):
        for names in ((), ('file', 'leaked-temp')):
            with self.subTest(names=names), self.assertRaises(AssertionError):
                self.trial(names=names).verify(self.model, world='main')

    def test_metadata_corruption(self):
        for args in ({'mode': 0o777}, {'size': 7}):
            with self.subTest(args=args), self.assertRaises(AssertionError):
                self.trial(**args).verify(self.model, world='main')

    def test_crash_oracle_rejects_lost_ack_and_torn_save(self):
        for acknowledged in (False, True):
            with self.assertRaises(AssertionError):
                check_recovered_save(b'old', b'new', b'torn', acknowledged)
        with self.assertRaises(AssertionError):
            check_recovered_save(b'old', b'new', b'old', True)
        check_recovered_save(b'old', b'new', b'old', False)
        check_recovered_save(b'old', b'new', b'new', False)
        check_recovered_save(b'old', b'new', b'new', True)

    def test_percentiles_require_enough_samples(self):
        self.assertIsNone(distribution([1, 2, 3])['p95_ms'])
        self.assertEqual(distribution(list(range(1, 1001)))['p95_ms'], 950 / 1e6)
        self.assertEqual(distribution(list(range(1, 1001)))['p99_ms'], 990 / 1e6)
        self.assertIsNone(distribution([1, 2, 3])['p99_ms'])
        self.assertEqual(distribution([])['count'], 0)
        self.assertIsNone(distribution([])['p50_ms'])

    def test_storage_components_and_unlink_race(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            files = {'DATA-1.seg': 100, '000001.ldb': 20, 'OWNERSHIP': 3,
                     '.gc-mark-own/db/LOG': 7, '.gc-physical-own/db/000002.ldb': 11,
                     '.sync/catalog': 5, 'vanishing': 13}
            for name, count in files.items():
                p = root / name;p.parent.mkdir(parents=True, exist_ok=True);p.write_bytes(b'x' * count)
            (root / 'link').symlink_to(root / 'DATA-1.seg')
            original = Path.lstat
            def racing_stat(path, *args, **kwargs):
                if path.name == 'vanishing':
                    raise FileNotFoundError(path)
                return original(path, *args, **kwargs)
            with patch.object(Path, 'lstat', racing_stat):
                usage = disk_usage(root)
            self.assertEqual(usage['segment_bytes'], 100)
            self.assertEqual(usage['index_bytes'], 20)
            self.assertEqual(usage['control_bytes'], 8)
            self.assertEqual(usage['scratch_bytes'], 18)
            self.assertEqual(usage['mark_scratch_bytes'], 7)
            self.assertEqual(usage['physical_scratch_bytes'], 11)
            self.assertEqual(usage['apparent_bytes'], 146)
            self.assertEqual(usage['files'], 6)

    def test_rpc_phase_is_captured_at_request_start(self):
        trial = Workload.__new__(Workload)
        trial.phase = 'before';trial.endpoint = '/private/socket';trial.lock = threading.Lock()
        trial.samples = {};trial.phase_samples = {};trial.outcomes = {};trial.coverage = {}
        trial.input_bytes = 0;trial.mutations = 0;trial.log = io.StringIO()
        class Response:
            data = b'FFRSP001' + struct.pack('<II', 6, 0) + b'ok'
            def __enter__(self):trial.phase = 'after';return self
            def __exit__(self, *args):pass
            def settimeout(self, value):pass
            def connect(self, value):pass
            def sendall(self, value):pass
            def recv(self, count):
                out, self.data = self.data[:count], self.data[count:];return out
        with patch('developer_workload.socket.socket', return_value=Response()):
            self.assertEqual(trial.rpc('fs-cat', Path('/virtual')), b'ok')
        self.assertIn('before/fs-cat', trial.phase_samples)
        self.assertNotIn('after/fs-cat', trial.phase_samples)
        self.assertEqual(json.loads(trial.log.getvalue())['args'], ['/virtual'])


if __name__ == '__main__':
    unittest.main()
