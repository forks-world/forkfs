"""Check the workload oracle rejects plausible filesystem regressions."""
import json
import unittest
from developer_workload import Node, Workload, distribution, check_recovered_save


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


if __name__ == '__main__':
    unittest.main()
