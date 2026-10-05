"""Continuous work history (docs/CONTINUOUS_WORK_HISTORY.md), first delivery slice.

Drives `world fs history` against a disposable real store: incremental recording, immutable
content objects addressed by SHA-256, the no-change case, coverage degradation over the content
budget, and the JSON contract.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

WORLD = str(Path(sys.argv.pop(1)).resolve())


class HistoryTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='forkfs-history-')).resolve()
        self.addCleanup(self.cleanup)
        self.store = self.root / 'store'
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'keep').write_text('keep\n')
        (self.source / 'mod').write_text('before\n')
        (self.source / 'gone').write_text('gone\n')
        self.env = dict(os.environ, WORLD_STORE=str(self.store), WORLD_POOL_TOPUP='0')

    def cleanup(self):
        for root, dirs, _ in os.walk(self.root):
            for name in dirs:
                path = Path(root) / name
                if not path.is_symlink():
                    path.chmod(0o700)
        shutil.rmtree(self.root)

    def run_world(self, *args, code=0, env=None):
        result = subprocess.run([WORLD, *args], env=env or self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, code, result.stderr.decode(errors='replace'))
        return result

    def query(self, *args):
        result = self.run_world('fs', *args, '--json')
        return json.loads(result.stdout.decode('utf-8'))

    def populate(self):
        self.run_world('fs', 'init', str(self.source), '--name', 'proj')
        self.world = self.root / 'w1'
        self.run_world('fs', 'fork', '--from', 'S1', '--to', str(self.world), '--no-pool')

    def sha(self, data):
        return hashlib.sha256(data).hexdigest()

    def content_exists(self, hexhash):
        return (self.store / 'content' / hexhash[:2] / hexhash).is_file()

    def test_record_incremental_and_content(self):
        self.populate()
        (self.world / 'mod').write_text('after\n')
        (self.world / 'added').write_text('new\n')
        (self.world / 'gone').unlink()

        rec = self.query('history', 'record', 'W1', '--actor', 'alice', '--tool-call', 'c1')
        self.assertEqual(rec['result'], 'recorded')
        self.assertEqual(rec['world'], 'W1')
        rev = rec['revision']
        self.assertEqual(rev, 'R1')

        show = self.query('history', 'show', 'R1')
        self.assertEqual(show['revision']['id'], 'R1')
        self.assertIsNone(show['revision']['parent_revision'])
        self.assertEqual(show['revision']['baseline_snapshot'], 'S1')
        self.assertEqual(show['revision']['origin'], 'filesystem')
        self.assertEqual(show['revision']['coverage'], 'observed')
        self.assertEqual(show['revision']['actor_id'], 'alice')
        self.assertEqual(show['revision']['tool_call_id'], 'c1')
        self.assertEqual(show['revision']['changes'], 3)
        by_path = {c['path']: c for c in show['changes']}
        self.assertEqual(by_path['mod']['change'], 'modified')
        self.assertEqual(by_path['added']['change'], 'added')
        self.assertEqual(by_path['gone']['change'], 'deleted')

        # The changed bytes are immutable content objects, addressed by their SHA-256.
        self.assertEqual(by_path['mod']['before_hash'], self.sha(b'before\n'))
        self.assertEqual(by_path['mod']['after_hash'], self.sha(b'after\n'))
        self.assertEqual(by_path['added']['after_hash'], self.sha(b'new\n'))
        self.assertEqual(by_path['gone']['before_hash'], self.sha(b'gone\n'))
        for h in (by_path['mod']['before_hash'], by_path['mod']['after_hash'],
                  by_path['added']['after_hash'], by_path['gone']['before_hash']):
            self.assertTrue(self.content_exists(h), h)

        # A second record with no change publishes nothing.
        again = self.query('history', 'record', 'W1')
        self.assertEqual(again['result'], 'no-change')
        self.assertIsNone(again['revision'])
        self.assertEqual(len(self.query('history', 'W1')['revisions']), 1)

        # Only the file that changed again appears in the next revision, and it chains to R1.
        (self.world / 'added').write_text('newer\n')
        rec2 = self.query('history', 'record', 'W1')
        self.assertEqual(rec2['revision'], 'R2')
        show2 = self.query('history', 'show', 'R2')
        self.assertEqual(show2['revision']['parent_revision'], 'R1')
        self.assertEqual(len(show2['changes']), 1)
        self.assertEqual(show2['changes'][0]['path'], 'added')
        self.assertEqual(show2['changes'][0]['before_hash'], self.sha(b'new\n'))
        self.assertEqual(show2['changes'][0]['after_hash'], self.sha(b'newer\n'))

        # Identical bytes deduplicate: 'newer' is stored once, and 'keep' was never stored at all.
        self.assertTrue(self.content_exists(self.sha(b'newer\n')))
        self.assertFalse(self.content_exists(self.sha(b'keep\n')))

    def test_mode_change_is_metadata_only(self):
        self.populate()
        (self.world / 'exec').write_text('#!/bin/sh\n')
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R1')
        (self.world / 'exec').chmod(0o755)
        rec = self.query('history', 'record', 'W1')
        self.assertEqual(rec['revision'], 'R2')
        show = self.query('history', 'show', 'R2')
        self.assertEqual(show['changes'][0]['change'], 'metadata')
        self.assertEqual(show['revision']['meta'], 1)
        # The bytes did not change, so the two records name the same object.
        self.assertEqual(show['changes'][0]['before_hash'], show['changes'][0]['after_hash'])

    def test_size_budget_marks_incomplete(self):
        self.populate()
        (self.world / 'big').write_bytes(b'x' * 64)
        env = dict(self.env, WFS_HISTORY_MAX_CONTENT='16')
        result = self.run_world('fs', 'history', 'record', 'W1', env=env)
        self.assertIn(b'recorded', result.stdout)
        show = self.query('history', 'show', 'R1')
        self.assertEqual(show['revision']['coverage'], 'incomplete')
        big = next(c for c in show['changes'] if c['path'] == 'big')
        self.assertEqual(big['after_hash'], '')       # over budget: metadata only
        self.assertEqual(big['size'], 64)

    def test_list_orders_newest_first(self):
        self.populate()
        (self.world / 'one').write_text('1')
        self.run_world('fs', 'history', 'record', 'W1')
        (self.world / 'two').write_text('2')
        self.run_world('fs', 'history', 'record', 'W1')
        revs = self.query('history')['revisions']
        self.assertEqual([r['id'] for r in revs], ['R2', 'R1'])
        self.assertEqual([r['world_id'] for r in revs], ['W1', 'W1'])

    def test_missing_revision_is_an_error(self):
        self.populate()
        result = self.run_world('fs', 'history', 'show', 'R9', code=1)
        self.assertIn(b'no such', result.stderr.lower())


if __name__ == '__main__':
    unittest.main()
