"""Regression checks for benchmark reporting; no mounts or fixtures required."""

import json
import unittest

from mounted_fs import summarize


class SummaryTests(unittest.TestCase):
    def test_small_runs_do_not_publish_p95(self):
        for count in (1, 100, 999):
            with self.subTest(count=count):
                result = json.loads(json.dumps(summarize("stat_hot", "native", list(range(1, count + 1)))))
                self.assertIsNone(result["p95_ns"])
                self.assertEqual(result["p95_status"], "insufficient_samples")
                self.assertEqual(result["p95_min_samples"], 1000)
                self.assertEqual(result["samples"], count)
                self.assertEqual(result["p50_ns"], (count + 1) // 2)

    def test_threshold_reports_nearest_rank_on_unsorted_samples(self):
        result = summarize("edit_save", "world", list(range(1000, 0, -1)))
        self.assertEqual(result["p95_ns"], 950)
        self.assertEqual(result["p50_ns"], 500)
        self.assertEqual(result["p95_status"], "reported")
        self.assertEqual(result["samples"], 1000)


if __name__ == "__main__":
    unittest.main()
