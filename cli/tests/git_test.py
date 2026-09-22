"""Git-aware worlds: real repositories, worktrees and complete workspace lifecycles."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

WORLD = str(Path(sys.argv.pop(1)).resolve())

class GitWorldTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='forkfs-git-')).resolve()
        self.addCleanup(self.cleanup)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.store = self.root / 'store'
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        self.env.update(WORLD_STORE=str(self.store), WORLD_POOL_TOPUP='0',
                        GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1',
                        GIT_CONFIG_COUNT='2', GIT_CONFIG_KEY_0='maintenance.auto',
                        GIT_CONFIG_VALUE_0='false', GIT_CONFIG_KEY_1='gc.auto',
                        GIT_CONFIG_VALUE_1='0')
        self.git(self.source, 'init', '-b', 'main')
        self.git(self.source, 'config', 'user.name', 'World Test')
        self.git(self.source, 'config', 'user.email', 'world@example.com')
        (self.source / 'file').write_text('original\n')
        (self.source / '.gitignore').write_text('build/\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-m', 'base')
        self.base = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()

    def cleanup(self):
        for root, dirs, files in os.walk(self.root):
            for name in dirs + files:
                path = Path(root) / name
                if not path.is_symlink():
                    if hasattr(os, 'chflags'):
                        os.chflags(path, 0)
                    path.chmod(0o700 if path.is_dir() else 0o600)
        shutil.rmtree(self.root)

    def run_cmd(self, *args, code=0):
        p = subprocess.run(args, env=self.env, capture_output=True, timeout=60)
        self.assertEqual(p.returncode, code, (args, p.stdout, p.stderr))
        return p

    def git(self, path, *args, code=0):
        return self.run_cmd('git', '-C', str(path), *args, code=code)

    def world(self, *args, code=0):
        return self.run_cmd(WORLD, 'fs', *args, code=code)

    def fork(self, name='one', source='S1', *args):
        path = self.root / name
        p = self.world('fork', '--from', source, '--to', str(path), *args)
        return path, p.stdout.decode().split()[0]

    def test_clean_import_fork_commit_isolation(self):
        before = self.git(self.source, 'worktree', 'list', '--porcelain').stdout
        self.world('init', str(self.source))
        one, wid = self.fork()
        two, _ = self.fork('two')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1')
        info = json.loads(self.world('inspect', wid, '--json').stdout)['git']
        self.assertEqual(info['baseline'].encode(), self.base)
        self.assertEqual(info['git_dir'], str(one / '.world-git/repo.git'))
        self.assertIn(str(one).encode(), self.git(one, 'worktree', 'list', '--porcelain').stdout)
        (one / 'file').write_text('child commit\n')
        self.git(one, 'add', 'file')
        self.git(one, 'commit', '-m', 'child')
        self.assertEqual(self.git(two, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.assertEqual(self.git(self.source, 'worktree', 'list', '--porcelain').stdout, before)
        self.world('verify', 'S1')

    def test_dirty_state_is_explicit_and_index_is_preserved(self):
        (self.source / 'file').write_text('staged\n')
        (self.source / 'new-staged').write_text('only in index\n')
        self.git(self.source, 'add', '.')
        (self.source / 'file').write_text('unstaged\n')
        (self.source / 'untracked').write_text('untracked\n')
        (self.source / 'build').mkdir()
        (self.source / 'build/model.bin').write_bytes(b'ignored artifact')
        status = self.git(self.source, 'status', '--porcelain').stdout
        self.world('init', str(self.source), code=3)
        self.world('init', str(self.source), '--include-changes')
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, status)
        self.assertEqual(self.git(one, 'show', ':file').stdout, b'staged\n')
        self.assertEqual(self.git(one, 'show', ':new-staged').stdout, b'only in index\n')
        self.assertEqual((one / 'file').read_text(), 'unstaged\n')
        self.assertEqual((one / 'build/model.bin').read_bytes(), b'ignored artifact')
        self.world('checkpoint', wid, code=3)
        self.world('checkpoint', wid, '--include-changes')
        self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
        self.assertFalse((self.root / 'refused').exists())
        two, _ = self.fork('two', wid, '--include-changes')
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, status)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, status)

    def test_linked_source_can_be_deleted_after_import(self):
        linked = self.root / 'linked'
        self.git(self.source, 'worktree', 'add', '-b', 'linked', str(linked))
        (linked / 'file').write_text('staged linked\n')
        self.git(linked, 'add', 'file')
        self.world('init', str(linked), '--include-changes')
        shutil.rmtree(linked)
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'show', ':file').stdout, b'staged linked\n')
        self.git(one, 'fsck', '--full')
        self.git(one, 'commit', '-m', 'source gone')

    def test_import_preserves_all_refs_after_source_is_deleted(self):
        base = self.base.decode()
        retained = self.git(self.source, 'commit-tree', base + '^{tree}', '-p', base,
                            '-m', 'ref-only object').stdout.decode().strip()
        refs = {
            'refs/stash': retained,
            'refs/remotes/origin/review': retained,
            'refs/remotes/origin/main': retained,
            'refs/notes/test': retained,
            'refs/custom/retained': retained,
            'refs/custom/target': retained,
        }
        symrefs = {
            'refs/remotes/origin/HEAD': 'refs/remotes/origin/main',
            'refs/custom/alias2': 'refs/custom/target',
            'refs/custom/alias1': 'refs/custom/alias2',
        }
        for ref, oid in refs.items():
            self.git(self.source, 'update-ref', ref, oid)
        for ref, target in symrefs.items():
            self.git(self.source, 'symbolic-ref', ref, target)
        self.world('init', str(self.source))
        for ref, oid in refs.items():
            self.assertEqual(self.git(self.source, 'rev-parse', '--verify', ref).stdout.strip().decode(), oid)
        for ref, target in symrefs.items():
            self.assertEqual(self.git(self.source, 'symbolic-ref', '--quiet', '--no-recurse', ref).stdout.strip().decode(), target)
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.git(one, 'gc', '--quiet')
        for ref, oid in refs.items():
            self.assertEqual(self.git(one, 'rev-parse', '--verify', ref).stdout.strip().decode(), oid)
        for ref, target in symrefs.items():
            self.assertEqual(self.git(one, 'symbolic-ref', '--quiet', '--no-recurse', ref).stdout.strip().decode(), target)
        moved = self.git(one, 'commit-tree', base + '^{tree}', '-p', retained,
                         '-m', 'moved ref target').stdout.decode().strip()
        self.git(one, 'update-ref', 'refs/remotes/origin/main', moved)
        self.assertEqual(self.git(one, 'rev-parse', '--verify', 'refs/remotes/origin/HEAD').stdout.strip().decode(), moved)
        self.git(one, 'update-ref', 'refs/custom/target', moved)
        self.assertEqual(self.git(one, 'rev-parse', '--verify', 'refs/custom/alias1').stdout.strip().decode(), moved)
        self.git(one, 'config', '--get', 'remote.origin.url', code=1)

    def test_import_preserves_local_excludes(self):
        exclude = self.source / '.git' / 'info' / 'exclude'
        exclude.write_bytes(b'local-only/\nignored.txt')
        (self.source / 'ignored.txt').write_text('ignored\n')
        (self.source / 'local-only').mkdir()
        (self.source / 'local-only' / 'artifact').write_text('ignored directory\n')
        source_exclude = exclude.read_bytes()
        self.world('init', str(self.source))
        self.assertEqual(exclude.read_bytes(), source_exclude)
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual((one / 'ignored.txt').read_text(), 'ignored\n')
        self.assertEqual((one / 'local-only' / 'artifact').read_text(), 'ignored directory\n')
        self.world('checkpoint', wid)

    def test_import_preserves_local_attributes(self):
        attributes = self.source / '.git' / 'info' / 'attributes'
        attributes.write_bytes(b'*.txt text')
        (self.source / 'line.txt').write_bytes(b'line\r\n')
        self.git(self.source, 'add', 'line.txt')
        self.git(self.source, 'commit', '-m', 'text file')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.assertIn(b'text: set', self.git(self.source, 'check-attr', 'text', '--', 'line.txt').stdout)
        source_attributes = attributes.read_bytes()
        self.world('init', str(self.source))
        self.assertEqual(attributes.read_bytes(), source_attributes)
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual((one / 'line.txt').read_bytes(), b'line\r\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertIn(b'text: set', self.git(one, 'check-attr', 'text', '--', 'line.txt').stdout)
        self.world('checkpoint', wid)

    def test_configured_external_git_policies_are_refused(self):
        # The source-local files are copied into the owned repository. An explicit config
        # override may point anywhere (and has different precedence), so importing it is
        # refused before a snapshot can be published, including empty and missing paths.
        cases = (
            ('core.excludesFile', self.root / 'external-exclude'),
            ('core.attributesFile', self.root / 'external-attributes'),
        )
        for key, path in cases:
            with self.subTest(key=key, value='existing'):
                path.write_text('external-only/\n' if key.endswith('excludesFile') else '*.txt text\n')
                if key.endswith('excludesFile'):
                    (self.source / 'external-only').mkdir()
                    (self.source / 'external-only' / 'artifact').write_text('kept\n')
                self.git(self.source, 'config', '--local', key, str(path))
                before = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')]
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(before, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')])
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
                if key.endswith('excludesFile'):
                    self.assertEqual((self.source / 'external-only' / 'artifact').read_text(), 'kept\n')
                self.git(self.source, 'config', '--local', '--unset-all', key)
                if key.endswith('excludesFile'):
                    shutil.rmtree(self.source / 'external-only')
            with self.subTest(key=key, value='empty'):
                self.git(self.source, 'config', '--local', key, '')
                before = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')]
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(before, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')])
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.git(self.source, 'config', '--local', '--unset-all', key)

            with self.subTest(key=key, value='missing'):
                self.git(self.source, 'config', '--local', key, str(path / 'missing'))
                before = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')]
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(before, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')])
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.git(self.source, 'config', '--local', '--unset-all', key)

    def test_import_preserves_autocrlf_and_filemode_settings(self):
        self.git(self.source, 'config', '--local', 'core.autocrlf', 'true')
        self.git(self.source, 'config', '--local', 'core.filemode', 'false')
        line = self.source / 'line.txt'
        line.write_bytes(b'line\r\n')
        self.git(self.source, 'add', 'line.txt')
        self.git(self.source, 'commit', '-m', 'crlf')
        os.chmod(self.source / 'file', 0o755)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual((one / 'line.txt').read_bytes(), b'line\r\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'config', '--get', 'core.autocrlf').stdout.strip(), b'true')
        self.assertEqual(self.git(one, 'config', '--get', 'core.filemode').stdout.strip(), b'false')
        self.world('checkpoint', wid)

    def test_status_policy_types_and_absent_defaults(self):
        self.git(self.source, 'config', '--local', 'core.ignorecase', 'true')
        self.git(self.source, 'config', '--local', '--unset-all', 'core.ignorecase')
        with (self.source / '.git' / 'config').open('a') as config:
            config.write('[core]\n    autocrlf\n    safecrlf = warn\n    checkstat = minimal\n    checkRoundtripEncoding = SHIFT-JIS,UTF-16LE\n    symlinks =\n')
        self.world('init', str(self.source))
        one, wid = self.fork()
        for key, expected in (('core.autocrlf', b'true'), ('core.safecrlf', b'warn'),
                              ('core.checkstat', b'minimal'),
                              ('core.checkRoundtripEncoding', b'SHIFT-JIS,UTF-16LE'),
                              ('core.symlinks', b'false')):
            self.assertEqual(self.git(one, 'config', '--get', key).stdout.strip(), expected)
        self.git(one, 'config', '--get', 'core.ignorecase', code=1)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.world('checkpoint', wid)

    def test_filter_configuration_is_refused_before_execution(self):
        (self.source / '.gitattributes').write_text('file filter=example\n')
        self.git(self.source, 'add', '.gitattributes')
        self.git(self.source, 'commit', '-m', 'filter attribute')
        included = self.root / 'filter.config'
        included.write_text('[filter "example"]\n    clean = touch filter-ran; cat\n')
        self.git(self.source, 'config', '--local', 'include.path', str(included))
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertFalse((self.source / 'filter-ran').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_external_hidden_refs_are_refused(self):
        self.git(self.source, 'update-ref', 'refs/custom/hidden', self.base.decode())
        for key in ('transfer.hideRefs', 'uploadpack.hideRefs'):
            with self.subTest(key=key):
                self.git(self.source, 'config', '--local', key, 'refs/custom')
                before = (self.source / '.git' / 'config').read_bytes()
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(self.git(self.source, 'rev-parse', 'refs/custom/hidden').stdout.strip(), self.base)
                self.assertEqual((self.source / '.git' / 'config').read_bytes(), before)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.git(self.source, 'config', '--local', '--unset-all', key)

    def test_external_stash_stack_is_refused_unchanged(self):
        for value in ('first', 'second'):
            (self.source / 'file').write_text(value + '\n')
            self.git(self.source, 'stash', 'push', '-m', value)
        before = self.git(self.source, 'stash', 'list', '--format=%H:%gs').stdout
        self.assertEqual(len(before.splitlines()), 2)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(self.git(self.source, 'stash', 'list', '--format=%H:%gs').stdout, before)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_managed_stash_and_hidden_refs_survive_checkpoint(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'transfer.hideRefs', 'refs/custom')
        self.git(one, 'update-ref', 'refs/custom/hidden', self.base.decode())
        for value in ('first', 'second'):
            (one / 'file').write_text(value + '\n')
            self.git(one, 'stash', 'push', '-m', value)
        before = self.git(one, 'stash', 'list', '--format=%H:%gs').stdout
        self.assertEqual(len(before.splitlines()), 2)
        self.world('checkpoint', wid)
        two, _ = self.fork('two', 'S2')
        self.git(two, 'gc', '--quiet')
        self.assertEqual(self.git(two, 'stash', 'list', '--format=%H:%gs').stdout, before)
        self.assertEqual(self.git(two, 'rev-parse', 'refs/custom/hidden').stdout.strip(), self.base)
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')

    def test_explicit_empty_identity_does_not_fall_back_to_global(self):
        for key in ('user.name', 'user.email'):
            self.git(self.source, 'config', '--local', key, '')
        self.world('init', str(self.source))
        one, wid = self.fork()
        global_config = self.root / 'global-identity'
        global_config.write_text('[user]\n    name = Global Name\n    email = global@example.com\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        for key in ('user.name', 'user.email'):
            self.assertEqual(self.git(one, 'config', '--local', '--get', key).stdout, b'\n')
            self.assertEqual(self.git(one, 'config', '--get', key).stdout, b'\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.world('checkpoint', wid)

    def test_external_grafts_are_refused_without_changing_history(self):
        (self.source / 'file').write_text('next\n')
        self.git(self.source, 'commit', '-am', 'next')
        tip = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()
        graft = self.source / '.git' / 'info' / 'grafts'
        graft.write_bytes(tip + b'\n')
        before = self.git(self.source, 'rev-list', 'HEAD').stdout
        self.assertEqual(before.strip(), tip)
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(graft.read_bytes(), tip + b'\n')
        self.assertEqual(self.git(self.source, 'rev-list', 'HEAD').stdout, before)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_active_bisect_is_refused_without_changing_session(self):
        for n in range(4):
            (self.source / 'file').write_text(str(n) + '\n')
            self.git(self.source, 'commit', '-am', 'step ' + str(n))
        self.git(self.source, 'bisect', 'start', 'HEAD', self.base.decode())
        before = self.git(self.source, 'bisect', 'log').stdout
        head = self.git(self.source, 'rev-parse', 'HEAD').stdout
        self.assertTrue((self.source / '.git' / 'BISECT_START').exists())
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source), code=1)
        self.assertEqual(self.git(self.source, 'bisect', 'log').stdout, before)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout, head)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_in_progress_operation_markers_are_refused(self):
        for name in ('sequencer', 'MERGE_AUTOSTASH'):
            with self.subTest(name=name):
                marker = self.source / '.git' / name
                if name == 'sequencer':
                    marker.mkdir()
                    (marker / 'todo').write_text('pick ' + self.base.decode() + ' pending\n')
                else:
                    marker.write_bytes(self.base + b'\n')
                self.world('init', str(self.source), '--include-changes', code=1)
                self.assertTrue(marker.exists())
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                if marker.is_dir():
                    shutil.rmtree(marker)
                else:
                    marker.unlink()

    def test_conflicted_squash_merge_is_refused(self):
        self.git(self.source, 'checkout', '-b', 'side')
        (self.source / 'file').write_text('side\n')
        self.git(self.source, 'commit', '-am', 'side')
        self.git(self.source, 'checkout', 'main')
        (self.source / 'file').write_text('main\n')
        self.git(self.source, 'commit', '-am', 'main')
        self.git(self.source, 'merge', '--squash', 'side', code=1)
        self.assertFalse((self.source / '.git' / 'MERGE_HEAD').exists())
        before = self.git(self.source, 'ls-files', '--unmerged').stdout
        self.assertTrue(before)
        self.world('init', str(self.source), '--include-changes', code=1)
        self.assertEqual(self.git(self.source, 'ls-files', '--unmerged').stdout, before)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_pending_squash_message_survives_import_and_commit(self):
        self.git(self.source, 'checkout', '-b', 'side')
        (self.source / 'added').write_text('squashed content\n')
        self.git(self.source, 'add', 'added')
        self.git(self.source, 'commit', '-m', 'pending squash subject')
        self.git(self.source, 'checkout', 'main')
        self.git(self.source, 'merge', '--squash', 'side')
        message = (self.source / '.git' / 'SQUASH_MSG').read_bytes()
        staged = self.git(self.source, 'diff', '--cached').stdout
        self.assertTrue(staged)
        self.world('init', str(self.source), '--include-changes')
        self.assertEqual((self.source / '.git' / 'SQUASH_MSG').read_bytes(), message)
        shutil.rmtree(self.source)
        one, _ = self.fork()
        message_path = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'SQUASH_MSG').stdout.decode().strip()
        self.assertEqual(Path(message_path).read_bytes(), message)
        self.assertEqual(self.git(one, 'diff', '--cached').stdout, staged)
        self.git(one, '-c', 'core.editor=true', 'commit')
        self.assertIn(b'pending squash subject', self.git(one, 'log', '-1', '--format=%B').stdout)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_present_empty_squash_message_is_preserved(self):
        (self.source / '.git' / 'SQUASH_MSG').write_bytes(b'')
        self.world('init', str(self.source))
        one, _ = self.fork()
        message_path = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'SQUASH_MSG').stdout.decode().strip()
        self.assertEqual(Path(message_path).read_bytes(), b'')

    def test_move_discard_restore_checkpoint_and_gc(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        moved = self.root / 'moved'
        one.rename(moved)
        self.assertEqual(self.git(moved, 'status', '--porcelain').stdout, b'')
        self.assertIn(str(moved).encode(), self.git(moved, 'worktree', 'list', '--porcelain').stdout)
        self.world('verify', str(moved))
        self.world('checkpoint', wid)
        two, other = self.fork('two', 'S2')
        self.world('discard', wid)
        self.world('restore', wid)
        self.git(moved, 'fsck', '--full')
        self.world('discard', wid, '--now')
        self.assertFalse(moved.exists())
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.assertEqual(json.loads(self.world('inspect', other, '--json').stdout)['git']['baseline'].encode(), self.base)
        self.world('verify', 'S1')
        self.world('verify', 'S2')

    def test_detached_head_and_branch_collision(self):
        self.git(self.source, 'branch', 'world/W1')
        self.git(self.source, 'checkout', '--detach')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1-1')
        self.assertEqual(self.git(one, 'rev-parse', 'world/W1').stdout.strip(), self.base)

    def test_branch_prefix_collision_is_not_overwritten(self):
        self.git(self.source, 'branch', 'world')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world-W1')
        self.assertEqual(self.git(one, 'rev-parse', 'world').stdout.strip(), self.base)

    def test_unsupported_nested_and_unborn_are_refused(self):
        nested = self.source / 'nested'
        nested.mkdir()
        self.git(nested, 'init')
        self.world('init', str(self.source), '--include-changes', code=3)
        shutil.rmtree(nested)
        unborn = self.root / 'unborn'
        unborn.mkdir()
        self.git(unborn, 'init')
        self.world('init', str(unborn), '--include-changes', code=3)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_hard_snapshot_and_pool_refusal(self):
        init_args = ('--hard',) if sys.platform == 'darwin' else ()
        self.world('init', str(self.source), *init_args)
        self.world('pool', 'fill', 'S1', '--count', '1', code=3)
        self.assertEqual(json.loads(self.world('pool', 'status', '--json').stdout)['pool'], [])
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.git(one, 'fsck', '--full')
        self.world('verify', 'S1')

    def test_main_repository_does_not_import_other_worktree_registrations(self):
        other = self.root / 'external-worktree'
        self.git(self.source, 'worktree', 'add', '-b', 'external', str(other))
        source_index = (self.source / '.git/index').read_bytes()
        registrations = self.git(self.source, 'worktree', 'list', '--porcelain').stdout
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertNotIn(str(other).encode(), self.git(one, 'worktree', 'list', '--porcelain').stdout)
        self.assertEqual((self.source / '.git/index').read_bytes(), source_index)
        self.assertEqual(self.git(self.source, 'worktree', 'list', '--porcelain').stdout, registrations)
        self.git(other, 'status', '--porcelain')

    def test_git_failure_rolls_back_before_publication(self):
        import shlex
        self.world('init', str(self.source))
        real_git = shutil.which('git')
        wrapper = self.root / 'bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nfor arg in "$@"; do [ "$arg" = update-ref ] && exit 42; done\nexec '
                          + shlex.quote(real_git) + ' "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        target = self.root / 'failed'
        self.world('fork', '--from', 'S1', '--to', str(target), code=3)
        self.assertFalse(target.exists())
        self.assertEqual(list(self.root.glob('.wfs-fork-*')), [])
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['worlds'], [])
        self.world('verify', 'S1')
        self.env['PATH'] = self.env['PATH'].split(os.pathsep, 1)[1]
        self.fork()

    def test_git_works_in_exec_sandbox(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / 'file').write_text('sandbox commit\n')
        self.run_cmd(WORLD, 'exec', wid, '--require-sandbox', '--', 'git', 'add', 'file')
        self.run_cmd(WORLD, 'exec', wid, '--require-sandbox', '--', 'git', 'commit', '-m', 'sandbox')
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)

    def test_discard_refuses_additional_worktrees(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        external = self.root / 'dependent'
        self.git(one, 'worktree', 'add', '-b', 'dependent', str(external))
        self.world('discard', wid, '--now', '--force', code=3)
        self.assertTrue(one.exists())
        self.assertEqual(self.git(external, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.git(one, 'worktree', 'remove', str(external))
        self.world('discard', wid, '--now')
        self.assertFalse(one.exists())

    def test_plain_directories_still_work(self):
        shutil.rmtree(self.source / '.git')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertFalse((one / '.git').exists())
        self.assertFalse((one / '.world-git').exists())

    def test_environment_does_not_redirect_git_writes(self):
        sentinel = self.root / 'sentinel-index'
        sentinel.write_bytes(b'untouched')
        self.env['GIT_DIR'] = str(self.source / '.git')
        self.env['GIT_INDEX_FILE'] = str(sentinel)
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(sentinel.read_bytes(), b'untouched')
        del self.env['GIT_DIR']; del self.env['GIT_INDEX_FILE']
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

if __name__ == '__main__':
    unittest.main()
