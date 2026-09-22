"""CLI JSON contract against a disposable real store and APFS worlds."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

WORLD = str(Path(sys.argv.pop(1)).resolve())


class JsonTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='forkfs-json-')).resolve()
        self.addCleanup(self.cleanup)
        self.store = self.root / 'store'
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'file').write_text('original\n')
        self.env = dict(os.environ, WORLD_STORE=str(self.store), WORLD_POOL_TOPUP='0')

    def cleanup(self):
        # Open snapshot gates only inside this test's private directory before removal.
        for root, dirs, _ in os.walk(self.root):
            for name in dirs:
                path = Path(root) / name
                if not path.is_symlink():
                    path.chmod(0o700)
        shutil.rmtree(self.root)

    def run_world(self, *args, code=0):
        result = subprocess.run([WORLD, *args], env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, code, result.stderr.decode(errors='replace'))
        return result

    def query(self, *args):
        result = self.run_world('fs', *args, '--json')
        return json.loads(result.stdout.decode('utf-8'))

    def populate(self):
        name = '项目 "quoted" \\ tab\tline\n\x01'
        self.run_world('fs', 'init', str(self.source), '--name', name)
        self.world = self.root / 'world "中文"'
        self.run_world('fs', 'fork', '--from', 'S1', '--to', str(self.world), '--no-pool')
        return name

    def test_empty_store(self):
        self.assertEqual(self.query('list'), {'schema_version': 1, 'snapshots': [], 'worlds': []})
        status = self.query('status')
        for field in ('snapshots', 'worlds_active', 'trash_entries', 'pool_ready'):
            self.assertEqual(status[field], 0)

    def test_list_inspect_and_status(self):
        name = self.populate()
        listing = self.query('list')
        snap = self.query('inspect', 'S1')
        world = self.query('inspect', 'W1')
        self.assertEqual(listing['snapshots'], [snap])
        self.assertEqual(listing['worlds'], [world])
        self.assertEqual(snap['name'], name)
        self.assertEqual(world['name'], name)
        self.assertEqual(snap['protection'], 'gate')
        self.assertEqual(snap['state'], 'active')
        self.assertIsNone(snap['from_world'])
        self.assertEqual(world['snapshot_id'], 'S1')
        self.assertEqual(world['path'], str(self.world))
        self.assertIs(world['present'], True)
        self.assertIsInstance(world['created_at'], int)
        status = self.query('status')
        self.assertEqual(status['snapshots'], 1)
        self.assertEqual(status['worlds_active'], 1)
        self.assertEqual(status['dir'], str(self.store))
        self.assertGreater(status['volume_total_bytes'], 0)
        self.assertEqual(status['database'], 'metadata3.db')
        self.run_world('fs', 'checkpoint', 'W1')
        self.assertEqual(self.query('inspect', 'S2')['from_world'], 'W1')

    def test_diff_paths_and_stat(self):
        self.populate()
        clean = self.query('diff', 'W1', '--full')
        self.assertEqual(clean['changes'], [])
        filename = '添加 "x" \\ \t\n\x01'
        (self.world / filename).write_text('added')
        (self.world / 'file').write_text('modified content\n')
        result = self.query('diff', 'W1', '--full')
        self.assertEqual({x['path']: x['change'] for x in result['changes']},
                         {filename: 'A', 'file': 'M'})
        self.assertEqual(result['stats']['added'], 1)
        self.assertEqual(result['stats']['modified'], 1)
        self.assertIs(result['stats']['full_scan'], True)
        stat = self.query('diff', 'W1', '--stat', '--full')
        self.assertEqual(stat['changes'], [])
        self.assertIs(stat['stat_only'], True)
        self.assertEqual(stat['stats']['added'], 1)
        (self.world / 'file').unlink()
        result = self.query('diff', 'W1', '--full')
        self.assertIn({'path': 'file', 'change': 'D'}, result['changes'])

    def test_undecodable_name_bytes(self):
        name = b'invalid-\xff-\xe0\x80\x80-truncated-\xf0\x9f'
        self.run_world('fs', 'init', str(self.source), '--name', name)
        result = self.query('inspect', 'S1')
        self.assertEqual(os.fsencode(result['name']), name)

    def test_large_diff_document(self):
        self.populate()
        expected = set()
        for i in range(300):
            name = f'new-{i:04d}-' + 'x' * 40
            expected.add(name)
            (self.world / name).write_text('data')
        result = self.run_world('fs', 'diff', 'W1', '--full', '--json')
        self.assertGreater(len(result.stdout), 8192)
        data = json.loads(result.stdout)
        self.assertEqual({entry['path'] for entry in data['changes']}, expected)
        self.assertEqual(data['stats']['added'], len(expected))

    @unittest.skipIf(os.geteuid() == 0, 'directory permission test requires a normal user')
    def test_status_exposes_unreadable_directories(self):
        self.populate()
        scratch = self.store / 'tmp'
        scratch.mkdir(exist_ok=True)
        scratch.chmod(0)
        self.addCleanup(scratch.chmod, 0o700)
        status = self.query('status')
        self.assertGreater(status['dirs_unreadable'], 0)
        self.assertEqual(status['dirs_unreadable_path'], str(scratch))
        self.assertGreater(status['dirs_unreadable_errno'], 0)

    def test_errors_have_no_json_document(self):
        self.populate()
        for args, code in [(('inspect', 'W999'), 1), (('diff', 'W999'), 1),
                           (('inspect', 'invalid'), 2), (('status', 'unexpected'), 2)]:
            result = self.run_world('fs', *args, '--json', code=code)
            self.assertEqual(result.stdout, b'')
            self.assertTrue(result.stderr)
        moved = self.root / 'moved'
        self.world.rename(moved)
        self.assertIs(self.query('inspect', 'W1')['present'], False)
        result = self.run_world('fs', 'diff', 'W1', '--json', code=3)
        self.assertEqual(result.stdout, b'')

    def test_query_failure_does_not_publish_partial_list(self):
        import sqlite3
        self.populate()
        with sqlite3.connect(self.store / 'metadata3.db') as db:
            db.execute('ALTER TABLE worlds RENAME COLUMN name TO damaged_name')
        result = self.run_world('fs', 'list', '--json', code=1)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'list', result.stderr)

    def test_pool_status(self):
        self.assertEqual(self.query('pool', 'status')['pool'], [])
        name = self.populate()
        self.run_world('fs', 'pool', 'fill', 'S1', '--count', '2')
        rows = self.query('pool', 'status')['pool']
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['snapshot'], 'S1')
        self.assertEqual(rows[0]['snapshot_name'], name)
        self.assertEqual(rows[0]['ready'], 2)
        self.assertEqual(rows[0]['stale'], 0)
        self.assertGreater(rows[0]['newest_at'], 0)
        self.run_world('fs', 'pool', 'drain', 'S1')
        self.assertEqual(self.query('pool', 'status')['pool'], [])

    def test_gc_status(self):
        empty = self.query('gc', '--status')
        self.assertEqual(empty['entries'], 0)
        self.assertEqual(empty['worker_pid'], 0)
        self.populate()
        self.run_world('fs', 'discard', 'W1')
        report = self.query('gc', '--status', '--retention', '0.5')
        self.assertEqual(report['entries'], 1)
        self.assertEqual(report['worlds'], 1)
        self.assertEqual(report['due'], 0)
        self.assertEqual(report['dirs_unreadable'], 0)
        self.assertEqual(self.query('gc', '--status', '--retention', '0')['due'], 1)
        self.assertEqual(self.query('inspect', 'W1')['state'], 'trashed')
        self.run_world('fs', 'restore', 'W1')

    def test_reject_invalid_numbers_without_changing_worlds(self):
        self.populate()
        for value in ('', 'oops', '-1', 'nan', 'inf', '1junk', '1e9999', '999999999999999999999', '0x1', ' 1'):
            for args in [('gc', '--now'), ('discard', 'W1')]:
                with self.subTest(args=args, value=value):
                    result = self.run_world('fs', *args, '--retention', value, code=2)
                    self.assertEqual(result.stdout, b'')
        for value in ('', 'oops', '-1', '1.5', '4097', '999999999999999999999'):
            result = self.run_world('fs', 'pool', 'fill', 'S1', '--count', value, code=2)
            self.assertEqual(result.stdout, b'')
        self.assertEqual(self.query('pool', 'status')['pool'], [])
        self.assertEqual(self.query('inspect', 'W1')['state'], 'active')
        self.run_world('fs', 'pool', 'fill', 'S1', '--count', '0')
        self.assertEqual(self.query('pool', 'status')['pool'], [])

    def test_gc_rejects_conflicting_modes(self):
        self.populate()
        self.run_world('fs', 'discard', 'W1')
        for flag in ('--now', '--reconcile', '--worker'):
            result = self.run_world('fs', 'gc', '--status', '--json', flag, code=2)
            self.assertEqual(result.stdout, b'')
        self.run_world('fs', 'gc', '--json', code=2)
        self.assertEqual(self.query('gc', '--status')['entries'], 1)
        self.run_world('fs', 'restore', 'W1')

    def test_help_does_not_open_store(self):
        for args in [('--help',), ('-h',), ('help',), ('fs', '--help'),
                     ('fs', 'list', '--help'), ('fs', 'pool', 'status', '--help')]:
            result = self.run_world(*args)
            self.assertIn(b'usage:', result.stdout)
            self.assertEqual(result.stderr, b'')
            self.assertFalse(self.store.exists())

    def test_text_output_and_exec_passthrough(self):
        self.populate()
        self.assertIn(b'SNAP', self.run_world('fs', 'list').stdout)
        self.assertIn(b'snapshot:', self.run_world('fs', 'inspect', 'S1').stdout)
        self.assertIn(b'store:', self.run_world('fs', 'status').stdout)
        self.assertEqual(self.run_world('fs', 'diff', 'W1', '--full').stdout, b'')
        result = self.run_world('exec', 'W1', '--no-sandbox', '--', '/usr/bin/printf', '%s', '--json')
        self.assertEqual(result.stdout, b'--json')
        result = self.run_world('fs', 'list', '--json', '--store', str(self.store))
        self.assertEqual(json.loads(result.stdout)['worlds'][0]['id'], 'W1')


if __name__ == '__main__':
    unittest.main()
