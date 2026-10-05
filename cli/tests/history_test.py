"""Continuous work history (docs/CONTINUOUS_WORK_HISTORY.md), first delivery slice.

Drives `world fs history` against a disposable real store: incremental recording, immutable
content objects addressed by SHA-256, the no-change case, coverage degradation over the content
budget, and the JSON contract.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
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

    def test_recorded_addition_deleted_is_reconciled(self):
        # A recorded addition that is later deleted is no longer a baseline-diff candidate (the
        # live tree matches the snapshot), so reconciliation against the recorded baseline state
        # is what notices it. Without it the revision chain keeps claiming the file is present.
        self.populate()
        (self.world / 'added').write_text('temp\n')
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R1')
        (self.world / 'added').unlink()
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R2')
        show = self.query('history', 'show', 'R2')
        self.assertEqual(len(show['changes']), 1)
        change = show['changes'][0]
        self.assertEqual((change['path'], change['change']), ('added', 'deleted'))
        self.assertEqual(change['before_hash'], self.sha(b'temp\n'))
        self.assertEqual(change['after_hash'], '')
        # Nothing further to reconcile.
        self.assertEqual(self.query('history', 'record', 'W1')['result'], 'no-change')

    def test_deletion_recreated_at_baseline_is_reconciled(self):
        # A deletion that is undone with the baseline's exact content and mtime produces no
        # baseline-diff candidate either; reconciliation must report the addition.
        self.populate()
        (self.world / 'gone').unlink()
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R1')
        st = os.stat(self.source / 'gone')
        (self.world / 'gone').write_text('gone\n')
        os.utime(self.world / 'gone', ns=(st.st_atime_ns, st.st_mtime_ns))
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R2')
        show = self.query('history', 'show', 'R2')
        self.assertEqual(len(show['changes']), 1)
        change = show['changes'][0]
        self.assertEqual((change['path'], change['change']), ('gone', 'added'))
        self.assertEqual(change['after_hash'], self.sha(b'gone\n'))

    def test_modified_reverted_at_baseline_is_reconciled(self):
        # A modification undone with the baseline's exact content and mtime is invisible to the
        # baseline diff; reconciliation must still publish the revert.
        self.populate()
        (self.world / 'mod').write_text('after\n')
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R1')
        st = os.stat(self.source / 'mod')
        (self.world / 'mod').write_text('before\n')
        os.utime(self.world / 'mod', ns=(st.st_atime_ns, st.st_mtime_ns))
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R2')
        show = self.query('history', 'show', 'R2')
        self.assertEqual(len(show['changes']), 1)
        change = show['changes'][0]
        self.assertEqual((change['path'], change['change']), ('mod', 'modified'))
        self.assertEqual(change['before_hash'], self.sha(b'after\n'))
        self.assertEqual(change['after_hash'], self.sha(b'before\n'))

    def test_deleted_file_records_its_recorded_size(self):
        self.populate()
        (self.world / 'sized').write_text('12345\n')
        self.run_world('fs', 'history', 'record', 'W1')
        (self.world / 'sized').unlink()
        self.run_world('fs', 'history', 'record', 'W1')
        change = self.query('history', 'show', 'R2')['changes'][0]
        self.assertEqual((change['path'], change['change']), ('sized', 'deleted'))
        self.assertEqual(change['size'], 6)   # the deleted version's size, not 0

    def test_size_only_change_over_budget_is_not_no_change(self):
        # Both captures are over budget, so both hashes are empty; only the size distinguishes
        # them, and a size change must not be reported as no-change.
        self.populate()
        env = dict(self.env, WFS_HISTORY_MAX_CONTENT='16')
        (self.world / 'big').write_bytes(b'x' * 64)
        self.run_world('fs', 'history', 'record', 'W1', env=env)
        (self.world / 'big').write_bytes(b'x' * 128)
        rec = self.run_world('fs', 'history', 'record', 'W1', env=env)
        self.assertIn(b'R2', rec.stdout)
        show = self.query('history', 'show', 'R2')
        self.assertEqual(show['revision']['coverage'], 'incomplete')
        change = show['changes'][0]
        self.assertEqual((change['path'], change['change']), ('big', 'modified'))
        self.assertEqual(change['size'], 128)

    def test_reconciled_revert_stays_incomplete_when_baseline_content_unknown(self):
        # The baseline file is over budget, so base_hash is empty. Even when the file is later
        # restored to its baseline byte-for-byte and metadata, the revision cannot be lossless.
        self.populate()
        env = dict(self.env, WFS_HISTORY_MAX_CONTENT='4')
        (self.world / 'mod').write_text('after\n')
        self.run_world('fs', 'history', 'record', 'W1', env=env)
        st = os.stat(self.source / 'mod')
        (self.world / 'mod').write_text('before\n')
        os.utime(self.world / 'mod', ns=(st.st_atime_ns, st.st_mtime_ns))
        self.run_world('fs', 'history', 'record', 'W1', env=env)
        show = self.query('history', 'show', 'R2')
        self.assertEqual(show['revision']['coverage'], 'incomplete')
        self.assertEqual(show['changes'][0]['change'], 'modified')
        self.assertEqual(show['changes'][0]['after_hash'], '')

    def test_equal_length_change_over_budget_is_recorded(self):
        # Both captures are over budget, so both hashes are empty AND the sizes are equal; the
        # mtime is what proves the file was rewritten, and the change must not be silently lost.
        self.populate()
        env = dict(self.env, WFS_HISTORY_MAX_CONTENT='16')
        (self.world / 'big').write_bytes(b'a' * 64)
        self.run_world('fs', 'history', 'record', 'W1', env=env)
        st = os.stat(self.world / 'big')
        (self.world / 'big').write_bytes(b'b' * 64)
        os.utime(self.world / 'big', ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
        rec = self.run_world('fs', 'history', 'record', 'W1', env=env)
        self.assertIn(b'R2', rec.stdout)
        show = self.query('history', 'show', 'R2')
        self.assertEqual(show['revision']['coverage'], 'incomplete')
        self.assertEqual(show['changes'][0]['change'], 'modified')

    def test_symlinked_ancestor_is_not_followed(self):
        # A writer that replaces a directory on the way with a symlink must not make capture read
        # and store bytes from outside the World.
        self.populate()
        outside = self.root / 'outside'
        outside.mkdir()
        secret = b'SECRET-OUTSIDE-CONTENT\n'
        (outside / 'f').write_bytes(secret)
        (self.world / 'sub').mkdir()
        (self.world / 'sub' / 'keep').write_text('keep\n')
        self.run_world('fs', 'history', 'record', 'W1')
        shutil.rmtree(self.world / 'sub')
        os.symlink(str(outside), str(self.world / 'sub'))
        self.run_world('fs', 'history', 'record', 'W1')
        self.assertFalse(self.content_exists(self.sha(secret)))

    def export(self, rev, path):
        result = self.run_world('fs', 'history', 'export', rev, '--out', str(path))
        # "world: manifest <hash>"
        for line in result.stderr.decode().splitlines():
            if line.startswith('world: manifest '):
                return line.split(' ', 2)[2]
        self.fail('no manifest hash on stderr')

    def test_manifest_roundtrip_and_checks(self):
        self.populate()
        (self.world / 'a').write_text('one\n')
        self.run_world('fs', 'history', 'record', 'W1', '--actor', 'alice')
        manifest = self.root / 'r1.manifest'
        h = self.export('R1', manifest)
        self.assertEqual(len(h), 64)
        body = manifest.read_bytes()
        self.assertTrue(body.startswith(b'worldfs-revision 1\n'))
        self.assertIn(self.sha(b'one\n').encode(), body)   # hashes are plain hex, not re-encoded

        # A valid manifest checks out against its own world.
        self.run_world('fs', 'history', 'import', str(manifest), '--into', 'W1', '--check')
        # ... and importing it again is deduplication, not a second revision.
        out = self.query('history', 'import', str(manifest), '--into', 'W1')
        self.assertEqual(out['result'], 'imported')
        self.assertEqual(out['revision'], 'R1')
        self.assertEqual(out['manifest_hash'], h)

        # A reformatted manifest is refused: the id would otherwise change.
        tampered = self.root / 'tampered.manifest'
        tampered.write_bytes(body.replace(b'world_name ', b'world_name  '))
        result = self.run_world('fs', 'history', 'import', str(tampered), '--into', 'W1', '--check',
                                code=3)
        self.assertIn(b'canonical', result.stderr.lower())

        # A manifest that names a content object this store does not have is refused. It goes to
        # a fresh world so the parent check does not answer first.
        self.run_world('fs', 'fork', '--from', 'S1', '--to', str(self.root / 'w2'), '--no-pool')
        missing = self.root / 'missing.manifest'
        missing.write_bytes(body.replace(self.sha(b'one\n').encode(), b'a' * 64, 1))
        result = self.run_world('fs', 'history', 'import', str(missing), '--into', 'W2', '--check',
                                code=3)
        self.assertIn(b'content object', result.stderr.lower())

    def test_manifest_parent_chain_is_required(self):
        self.populate()
        (self.world / 'a').write_text('one\n')
        self.run_world('fs', 'history', 'record', 'W1')
        (self.world / 'a').write_text('two\n')
        self.run_world('fs', 'history', 'record', 'W1')
        r1 = self.root / 'r1.manifest'
        r2 = self.root / 'r2.manifest'
        h1 = self.export('R1', r1)
        h2 = self.export('R2', r2)
        self.run_world('fs', 'fork', '--from', 'S1', '--to', str(self.root / 'w2'), '--no-pool')
        self.w2 = self.root / 'w2'
        # Forget the local records so the import is not just a deduplication.
        db = sqlite3.connect(str(self.store / 'metadata3.db'))
        for table in ('changes', 'revisions', 'history_files', 'history_pins'):
            db.execute(f'DELETE FROM {table}')
        db.commit()
        db.close()
        # The child cannot arrive before its parent.
        result = self.run_world('fs', 'history', 'import', str(r2), '--into', 'W2', '--check', code=3)
        self.assertIn(b'parent', result.stderr.lower())
        # In order, the chain lands and W2 now has both.
        self.run_world('fs', 'history', 'import', str(r1), '--into', 'W2')
        self.run_world('fs', 'history', 'import', str(r2), '--into', 'W2')
        revs = self.query('history', 'W2')['revisions']
        self.assertEqual([r['id'] for r in revs], ['R4', 'R3'])
        self.assertEqual(revs[0]['parent_revision'], 'R3')
        self.assertEqual(revs[0]['manifest_hash'], h2)
        self.assertEqual(revs[1]['manifest_hash'], h1)

    def test_baseline_pin_blocks_snapshot_discard(self):
        self.populate()
        (self.world / 'a').write_text('one\n')
        self.run_world('fs', 'history', 'record', 'W1')
        self.run_world('fs', 'inspect', 'S1')  # does not disturb the pin
        self.run_world('fs', 'discard', 'W1', '--now')
        result = self.run_world('fs', 'discard', 'S1', code=3)
        self.assertIn(b'baseline', result.stderr.lower())
        # The pin is visible on the snapshot.
        snap = self.query('inspect', 'S1')
        self.assertEqual(snap['pins'], 1)

    def test_record_is_serialized_against_the_world_lock(self):
        self.populate()
        (self.world / 'x').write_text('x')
        with open(self.world / '.world', 'r') as marker:
            fcntl.flock(marker, fcntl.LOCK_EX)
            try:
                result = self.run_world('fs', 'history', 'record', 'W1', code=3)
                self.assertIn(b'lock', result.stderr.lower())
            finally:
                fcntl.flock(marker, fcntl.LOCK_UN)
        # Once the lock is free, the same record succeeds.
        self.assertEqual(self.query('history', 'record', 'W1')['revision'], 'R1')


if __name__ == '__main__':
    unittest.main()
