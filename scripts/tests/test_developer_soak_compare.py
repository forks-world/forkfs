"""Reject unmatched or failed performance traces before comparing tails."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
from compare_developer_soak import MATCH, compare


class ComparisonTests(unittest.TestCase):
    def make_pair(self, root):
        paths = []
        for mode in ('disabled', 'concurrent'):
            parent = root / mode;parent.mkdir()
            settings = {key: 'matched' for key in MATCH}
            settings.update(seed=173, files=8, rounds=2, workers=2, steps=7, seconds=0)
            (parent/'settings.json').write_text(json.dumps(settings))
            model = {'stage':'development-after' if mode == 'disabled' else 'pre-backup-gc-after',
                     'unique_reachable_payload_bytes': 40, 'retained_revisions': 5, 'modeled_worlds': 3}
            summary = {'state':'passed', 'gc_mode':mode, 'retained_revision_counts':[5],
                       'latency_by_phase':{f'development/{mode}/fs-save':{
                           'count':1000, 'p50_ms':1 if mode == 'disabled' else 2,
                           'p95_ms':None, 'p99_ms':None}}, 'storage':[model],
                       'sampled_storage_peaks':{}, 'pre_backup_gc_storage':None,
                       'base_archive_bytes':100, 'final_archive_bytes':120}
            path = parent/'summary.json';path.write_text(json.dumps(summary));paths.append(path)
        return paths

    def change(self, path, field, value):
        data = json.loads(path.read_text());data[field] = value;path.write_text(json.dumps(data))

    def test_matching_pair_preserves_missing_tails(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_pair(Path(temp));result = compare(*paths)
            self.assertEqual(result['latency']['fs-save']['p50_ms_ratio'], 2)
            self.assertIsNone(result['latency']['fs-save']['p99_ms_ratio'])

    def test_failed_trace_is_not_a_performance_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_pair(Path(temp));self.change(paths[1], 'state', 'failed')
            with self.assertRaisesRegex(ValueError, 'successful concurrent'):
                compare(*paths)

    def test_mismatched_seed_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_pair(Path(temp));self.change(paths[1].parent/'settings.json', 'seed', 42)
            with self.assertRaisesRegex(ValueError, 'seed'):
                compare(*paths)

    def test_different_generation_counts_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_pair(Path(temp));self.change(paths[1], 'retained_revision_counts', [5, 7])
            with self.assertRaisesRegex(ValueError, 'generation counts'):
                compare(*paths)
