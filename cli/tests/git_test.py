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

    def test_disabled_sparse_checkout_patterns_survive_source_deletion(self):
        (self.source / 'docs').mkdir()
        (self.source / 'docs' / 'guide').write_text('guide\n')
        self.git(self.source, 'add', 'docs')
        self.git(self.source, 'commit', '-qm', 'docs')
        self.git(self.source, 'sparse-checkout', 'set', '--no-cone', '/docs/')
        self.git(self.source, 'sparse-checkout', 'disable')
        self.assertEqual(self.git(self.source, 'config', '--get', '--type=bool', 'core.sparseCheckout').stdout.strip(), b'false')
        patterns = (self.source / '.git' / 'info' / 'sparse-checkout').read_bytes()
        self.assertIn(b'/docs/', patterns)
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, wid = self.fork()
        path = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'info/sparse-checkout').stdout.decode().strip()
        self.assertEqual(Path(path).read_bytes(), patterns)
        # The worktree-scoped switches `disable` left behind travel with the patterns.
        self.assertEqual(self.git(one, 'config', '--type=bool', 'extensions.worktreeConfig').stdout.strip(), b'true')
        for key in ('core.sparseCheckout', 'core.sparseCheckoutCone', 'index.sparse'):
            self.assertEqual(self.git(one, 'config', '--worktree', '--type=bool', key).stdout.strip(), b'false')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'rev-parse', '--is-inside-work-tree').stdout.strip(), b'true')
        repo = one / '.world-git' / 'repo.git'
        self.assertEqual(self.run_cmd('git', '--git-dir', str(repo), 'rev-parse', '--is-bare-repository').stdout.strip(), b'true')
        two, _ = self.fork('two', wid)
        path = self.git(two, 'rev-parse', '--path-format=absolute', '--git-path', 'info/sparse-checkout').stdout.decode().strip()
        self.assertEqual(Path(path).read_bytes(), patterns)
        # A later `sparse-checkout init` reuses the preserved patterns, as it would in the source.
        self.git(one, 'sparse-checkout', 'init', '--no-cone')
        self.assertEqual(self.git(one, 'sparse-checkout', 'list').stdout.strip(), b'/docs/')

    def test_sparse_pattern_change_during_mirror_aborts_publication(self):
        import shlex
        self.git(self.source, 'sparse-checkout', 'set', '--no-cone', '/file')
        self.git(self.source, 'sparse-checkout', 'disable')
        patterns = self.source / '.git' / 'info' / 'sparse-checkout'
        real_git = shutil.which('git')
        wrapper = self.root / 'sparse-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + 'printf "/raced\\n" >> ' + shlex.quote(str(patterns)) + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.assertTrue(patterns.read_bytes().endswith(b'/raced\n'))
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_valueless_sparse_checkout_is_refused_without_mutation(self):
        omitted = self.source / 'omitted'
        omitted.mkdir()
        (omitted / 'file').write_text('keep in history\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-m', 'sparse fixture')
        self.git(self.source, 'sparse-checkout', 'set', '--no-cone', '/file')
        self.git(self.source, 'config', '--worktree', '--unset-all', 'core.sparseCheckout')
        config = self.source / '.git' / 'config.worktree'
        with config.open('a') as f:
            f.write('\n[core]\n\tsparseCheckout\n')
        self.assertEqual(self.git(self.source, 'config', '--type=bool', '--get',
                                  'core.sparseCheckout').stdout.strip(), b'true')
        before = {name: (self.source / '.git' / name).read_bytes()
                  for name in ('config', 'config.worktree', 'index', 'info/sparse-checkout')}
        head = self.git(self.source, 'rev-parse', 'HEAD').stdout
        result = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertFalse(omitted.exists())
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout, head)
        for name, data in before.items():
            self.assertEqual((self.source / '.git' / name).read_bytes(), data)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_external_object_copy_space_is_budgeted(self):
        import shlex
        linked = self.root / 'linked-budget'
        self.git(self.source, 'worktree', 'add', '-b', 'budget-linked', str(linked))
        self.world('list', '--json')
        objects = self.source / '.git' / 'objects'
        oversized = objects / 'budget-fixture'
        volume = os.statvfs(self.store)
        size = volume.f_bavail * volume.f_frsize + 256 * 1024 * 1024
        with oversized.open('wb') as f:
            f.truncate(size)
        # Never permit a regressed preflight to materialize this sparse fixture.
        real_git = shutil.which('git')
        wrapper = self.root / 'budget-bin'
        wrapper.mkdir()
        called = self.root / 'clone-called'
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nfor arg in "$@"; do\n'
                          'if [ "$arg" = clone ]; then\n'
                          ': > ' + shlex.quote(str(called)) + '\nexit 91\nfi\ndone\n'
                          'exec ' + shlex.quote(real_git) + ' "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        for source in (self.source, linked):
            with self.subTest(source=source.name):
                head = self.git(source, 'rev-parse', 'HEAD').stdout
                result = self.world('init', str(source), code=3)
                self.assertIn(b'not enough free space', result.stderr)
                self.assertFalse(called.exists())
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.assertEqual(self.git(source, 'rev-parse', 'HEAD').stdout, head)
                self.assertEqual(oversized.stat().st_size, size)

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

    def test_disabled_replace_refs_stay_disabled(self):
        self.git(self.source, 'checkout', '-q', '-b', 'replacement')
        (self.source / 'file').write_text('replacement tree\n')
        self.git(self.source, 'commit', '-qam', 'replacement')
        replacement = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(self.source, 'checkout', '-q', 'main')
        self.git(self.source, 'branch', '-q', '-D', 'replacement')
        self.git(self.source, 'replace', self.base.decode(), replacement)
        self.git(self.source, 'config', 'core.useReplaceRefs', 'false')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', '--type=bool', 'core.useReplaceRefs').stdout.strip(), b'false')
        self.assertEqual(self.git(one, 'rev-parse', 'refs/replace/' + self.base.decode()).stdout.strip().decode(), replacement)
        self.assertEqual(self.git(one, 'show', 'HEAD:file').stdout, b'original\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_ambient_replace_ref_policy_is_shared(self):
        global_config = self.root / 'global-config'
        global_config.write_text('[core]\n useReplaceRefs = false\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--type=bool', 'core.useReplaceRefs').stdout.strip(), b'false')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

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

    def test_status_policy_enum_case_is_preserved(self):
        for key, value in (('core.autocrlf', 'INPUT'), ('core.safecrlf', 'WaRn')):
            self.git(self.source, 'config', key, value)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.world('checkpoint', wid)
        two, _ = self.fork('enum-child', wid)
        for path in (one, two):
            for key, value in (('core.autocrlf', b'INPUT'), ('core.safecrlf', b'WaRn')):
                self.assertEqual(self.git(path, 'config', '--get', key).stdout.strip(), value)
            self.assertEqual(self.git(path, 'status', '--porcelain').stdout, b'')

    def test_injected_status_policies_are_refused(self):
        (self.source / '.gitattributes').write_text('file filter=injected\n')
        self.git(self.source, 'add', '.gitattributes')
        self.git(self.source, 'commit', '-qm', 'file uses the injected filter')
        before = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')]
        for channel in ('count', 'parameters'):
            for key, value, reason in (('core.autocrlf', 'input', b'set as command configuration'),
                                       ('core.attributesFile', '/nonexistent-policy', b'set as command configuration'),
                                       ('filter.injected.clean', 'touch injected-filter-ran; cat', b"uses the 'injected' filter")):
                with self.subTest(channel=channel, key=key):
                    if channel == 'count':
                        self.env.update(GIT_CONFIG_COUNT='3', GIT_CONFIG_KEY_2=key, GIT_CONFIG_VALUE_2=value)
                    else:
                        self.env['GIT_CONFIG_PARAMETERS'] = "'" + key + '=' + value + "'"
                    result = self.world('init', str(self.source), code=3)
                    self.assertIn(b"would make the World's Git see files differently", result.stderr)
                    self.assertIn(reason, result.stderr)
                    self.assertFalse((self.source / 'injected-filter-ran').exists())
                    self.assertEqual(before, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')])
                    self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                    self.env['GIT_CONFIG_COUNT'] = '2'
                    for variable in ('GIT_CONFIG_KEY_2', 'GIT_CONFIG_VALUE_2', 'GIT_CONFIG_PARAMETERS'):
                        self.env.pop(variable, None)
        self.world('init', str(self.source))  # benign maintenance command config remains allowed

    def test_command_scoped_url_rewrites_are_refused(self):
        # A url.<base>.insteadOf rule given as command configuration (GIT_CONFIG_COUNT/
        # GIT_CONFIG_PARAMETERS, -c) is visible to the ambient probes during import, but belongs
        # to this invocation only: baking the rewritten URL into the World's own configuration
        # would leave the World pinned to a URL the source no longer has once the environment is
        # gone.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        for channel in ('count', 'parameters'):
            with self.subTest(channel=channel):
                key, value = 'url.ssh://tmp.invalid/.insteadOf', 'https://example.invalid/'
                if channel == 'count':
                    self.env.update(GIT_CONFIG_COUNT='3', GIT_CONFIG_KEY_2=key, GIT_CONFIG_VALUE_2=value)
                else:
                    self.env['GIT_CONFIG_PARAMETERS'] = "'" + key + '=' + value + "'"
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'set as command configuration', result.stderr)
                self.env['GIT_CONFIG_COUNT'] = '2'
                for variable in ('GIT_CONFIG_KEY_2', 'GIT_CONFIG_VALUE_2', 'GIT_CONFIG_PARAMETERS'):
                    self.env.pop(variable, None)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.world('init', str(self.source))

    def test_mirror_does_not_install_ambient_templates(self):
        import shlex
        templates = self.root / 'templates'
        (templates / 'hooks').mkdir(parents=True)
        (templates / 'info').mkdir()
        hook = templates / 'hooks' / 'pre-commit'
        hook.write_text('#!/bin/sh\ntouch template-hook-ran\n')
        hook.chmod(0o700)
        (templates / 'info' / 'attributes').write_text('file -text\n')
        real_git = shutil.which('git')
        wrapper = self.root / 'template-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nexec ' + shlex.quote(real_git) + ' -c '
                          + shlex.quote('init.templateDir=' + str(templates)) + ' "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source))
        one, _ = self.fork()
        common = one / '.world-git' / 'repo.git'
        self.assertFalse((common / 'hooks' / 'pre-commit').exists())
        attributes = common / 'info' / 'attributes'
        self.assertTrue(not attributes.exists() or attributes.read_bytes() == b'')
        (one / 'file').write_text('new commit\n')
        self.git(one, 'commit', '-am', 'template-free commit')
        self.assertFalse((one / 'template-hook-ran').exists())
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)

    def test_inactive_conditional_policy_is_refused(self):
        included = self.root / 'future-policy'
        included.write_text('[core]\n autocrlf = true\n')
        global_config = self.root / 'conditional-global'
        local = self.source / '.git' / 'config'
        original = local.read_bytes()
        for scope in ('global', 'local'):
            for condition in ('gitdir:' + str(self.root / 'future-world') + '/**',
                              'onbranch:world/**'):
                with self.subTest(scope=scope, condition=condition):
                    directive = '[includeIf "' + condition + '"]\n path = ' + str(included) + '\n'
                    if scope == 'global':
                        global_config.write_text(directive)
                        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
                    else:
                        local.write_bytes(original + directive.encode())
                    self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
                    result = self.world('init', str(self.source), code=3)
                    self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                    local.write_bytes(original)
                    self.assertIn(b"would make the World's Git see files differently", result.stderr)
                    self.assertIn(b'which sets core.autocrlf', result.stderr)
                    self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_long_branch_inspection_and_metadata_overflow(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        branch = 'long-' + 'x' * 150
        self.git(one, 'checkout', '-b', branch)
        data = json.loads(self.world('inspect', wid, '--json').stdout)
        self.assertEqual(data['git']['branch'], branch)
        self.assertIn(branch.encode(), self.world('inspect', wid).stdout)
        self.git(one, 'config', 'worldfs.baseline', 'x' * 80)
        result = self.world('inspect', wid, '--json', code=1)
        self.assertNotIn(b'"git":', result.stdout)

    def test_global_status_policy_decides_cleanliness_like_the_users_git(self):
        line = self.source / 'line.txt'
        line.write_bytes(b'committed\r\n')
        self.git(self.source, 'add', 'line.txt')
        self.git(self.source, 'commit', '-m', 'literal CRLF')
        attributes = self.root / 'global-attributes'
        attributes.write_text('*.txt text eol=lf\n')
        included = self.root / 'status-policy'
        included.write_text('[core]\n attributesFile = ' + str(attributes) + '\n')
        global_config = self.root / 'global-config'
        global_config.write_text('[include]\n path = ' + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        # Git re-reads content only when the cached stat data no longer matches (or is racy);
        # on a slow runner the commit's index is not racy, so move the mtime to force a re-read.
        stamp = line.stat().st_mtime - 10
        os.utime(line, (stamp, stamp))
        self.assertIn(b' M line.txt', self.git(self.source, 'status', '--porcelain').stdout)
        # The user's own Git (global attributes via an unconditional include) calls the file
        # modified, so a plain init must refuse it as dirty rather than call it clean.
        before = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')]
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'uncommitted changes', result.stderr)
        self.assertEqual(before, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')])
        self.assertEqual(line.read_bytes(), b'committed\r\n')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        # Carried explicitly, the World sees exactly what the source's Git sees.
        self.world('init', str(self.source), '--include-changes')
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b' M line.txt\n')

    def test_conditional_include_identity_is_pinned_in_the_world(self):
        work = self.root / 'work'
        work.mkdir()
        source = work / 'repo'
        self.git(self.root, 'init', '-q', '-b', 'main', str(source))
        (source / 'file').write_text('work\n')
        identity = self.root / 'work-identity'
        identity.write_text('[user]\n name = Work Name\n email = work@example.com\n')
        global_config = self.root / 'identity-global'
        global_config.write_text('[includeIf "gitdir:' + str(work) + '/"]\n path = ' + str(identity) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(source, 'add', 'file')
        self.git(source, 'commit', '-qm', 'base')
        self.assertEqual(self.git(source, 'config', 'user.email').stdout.strip(), b'work@example.com')
        snapshot = self.world('init', str(source)).stdout.split()[0].decode()
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        one, _ = self.fork(str(Path('elsewhere') / 'one'), snapshot)
        self.assertEqual(self.git(one, 'config', '--show-scope', 'user.email').stdout.split(),
                         [b'local', b'work@example.com'])
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'in the World')
        self.assertEqual(self.git(one, 'log', '-1', '--format=%an <%ae>').stdout.strip(),
                         b'Work Name <work@example.com>')

    def test_refusals_name_their_reason(self):
        source = self.root / 'reftable'
        self.git(self.root, 'init', '-q', '-b', 'main', '--ref-format=reftable', str(source))
        self.git(source, 'config', 'user.name', 'World Test')
        self.git(source, 'config', 'user.email', 'world@example.com')
        (source / 'file').write_text('reftable\n')
        self.git(source, 'add', 'file')
        self.git(source, 'commit', '-qm', 'base')
        result = self.world('init', str(source), code=3)
        self.assertIn(b'unsupported Git layout\n  reason: reftable ref storage', result.stderr)
        (self.source / 'sub').mkdir()
        self.git(self.source / 'sub', 'init', '-q')
        result = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: nested Git repository or submodule at ', result.stderr)

    def test_unused_ambient_filters_are_allowed_without_execution(self):
        # A machine-wide `git lfs install` defines a filter every repository can see.
        global_config = self.root / 'filter-global'
        global_config.write_text('[filter "example"]\n clean = touch ambient-filter-ran; cat\n'
                                 ' smudge = touch ambient-filter-ran; cat\n required = true\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        (self.source / 'changed').write_text('untracked\n')
        self.world('init', str(self.source), '--include-changes')
        one, _ = self.fork()
        self.git(one, 'status', '--porcelain')
        self.assertFalse((self.source / 'ambient-filter-ran').exists())
        self.assertFalse((one / 'ambient-filter-ran').exists())

    def test_relative_ambient_attribute_paths_are_refused(self):
        for key in ('core.attributesFile', 'core.excludesFile'):
            with self.subTest(key=key):
                global_config = self.root / 'relative-global'
                global_config.write_text('[core]\n ' + key.split('.')[1] + ' = ../shared-rules\n')
                self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
                result = self.world('init', str(self.source), code=3)
                self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                self.assertIn(b'in global configuration is a relative path (../shared-rules)', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_publish_rechecks_checkout_right_before_moving_the_branch(self):
        import shlex
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.git(self.source, 'branch', 'world/W1')
        real_git = shutil.which('git')
        wrapper = self.root / 'checkout-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        # After the staging fetch into the source, check the branch out in a new worktree.
        script.write_text('#!/bin/sh\nfetch=0\nfor arg in "$@"; do [ "$arg" = fetch ] && fetch=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$fetch" = 1 ] && [ ! -e ' + shlex.quote(str(self.root / 'raced-wt')) + ' ]; then\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source)) + ' worktree add -q '
                          + shlex.quote(str(self.root / 'raced-wt')) + ' world/W1 || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        result = self.world('publish', wid, code=3)
        self.assertIn(b'was checked out in the target repository while publishing; nothing was changed', result.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), self.base)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_refuses_a_branch_that_moves_during_publish(self):
        import shlex
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        real_git = shutil.which('git')
        wrapper = self.root / 'branch-move-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        marker = self.root / 'branch-moved'
        # On the first fetch, move the World's branch before the fetch itself runs.
        script.write_text('#!/bin/sh\nfetch=0\nfor arg in "$@"; do [ "$arg" = fetch ] && fetch=1; done\n'
                          + 'if [ "$fetch" = 1 ] && [ ! -e ' + shlex.quote(str(marker)) + ' ]; then\n'
                          + 'touch ' + shlex.quote(str(marker)) + '\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(one))
                          + ' -c user.name=W -c user.email=w@example.com commit -q --allow-empty -m moved || exit $?\n'
                          + 'fi\n'
                          + 'exec ' + shlex.quote(real_git) + ' "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        result = self.world('publish', wid, code=3)
        self.assertIn(b'moved from', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_refuses_mismatched_replacement_refs(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'checkout', '-q', '-b', 'replacement')
        (one / 'file').write_text('replacement tree\n')
        self.git(one, 'commit', '-qam', 'replacement')
        replacement = self.git(one, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(one, 'checkout', '-q', 'world/W1')
        self.git(one, 'branch', '-q', '-D', 'replacement')
        self.git(one, 'replace', self.base.decode(), replacement)
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        # The default target (the source the World came from) has no refs/replace at all.
        result = self.world('publish', wid, code=3)
        self.assertIn(b'replacement refs', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_refuses_target_only_replacement_refs(self):
        # The check must be symmetric: a replacement active only in the target (the World has
        # none) would show the published branch through the target's own replacement too.
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.git(self.source, 'checkout', '-q', '-b', 'replacement')
        (self.source / 'file').write_text('replacement tree\n')
        self.git(self.source, 'commit', '-qam', 'replacement')
        replacement = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(self.source, 'checkout', '-q', 'main')
        self.git(self.source, 'branch', '-q', '-D', 'replacement')
        self.git(self.source, 'replace', self.base.decode(), replacement)
        result = self.world('publish', wid, code=3)
        self.assertIn(b'identical replacement refs', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_honors_global_replace_ref_policy(self):
        # A global core.useReplaceRefs=false (shared, per the import policy) disables
        # replacements for the user's own Git in both repositories, so a replacement active
        # only in the target's local config must not be treated as active for this check.
        global_config = self.root / 'global-replace-policy'
        global_config.write_text('[core]\n useReplaceRefs = false\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        try:
            self.world('init', str(self.source))
            one, wid = self.fork()
            self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
            head = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
            self.git(self.source, 'checkout', '-q', '-b', 'replacement')
            (self.source / 'file').write_text('replacement tree\n')
            self.git(self.source, 'commit', '-qam', 'replacement')
            replacement = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
            self.git(self.source, 'checkout', '-q', 'main')
            self.git(self.source, 'branch', '-q', '-D', 'replacement')
            self.git(self.source, 'replace', self.base.decode(), replacement)
            result = self.world('publish', wid)
            self.assertIn(b'new branch world/W1', result.stdout)
            self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/world/W1').stdout.strip(), head)
        finally:
            self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'

    def test_publish_refuses_grafts(self):
        # Legacy info/grafts rewrite a commit's parents and are not disabled by
        # --no-replace-objects, so a graft in the target could make a diverged branch look
        # like shared history to the checks below it. It must be refused up front instead.
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        grafts_dir = self.source / '.git' / 'info'
        grafts_dir.mkdir(parents=True, exist_ok=True)
        (grafts_dir / 'grafts').write_text(self.base.decode() + '\n')
        result = self.world('publish', wid, code=3)
        self.assertIn(b'info/grafts', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_conditional_include_paths_expand_tilde_forms(self):
        import pwd
        user = pwd.getpwuid(os.getuid()).pw_name
        home = Path(pwd.getpwuid(os.getuid()).pw_dir)
        policy_dir = Path(tempfile.mkdtemp(prefix='.forkfs-include-', dir=str(home)))
        self.addCleanup(shutil.rmtree, policy_dir, True)
        (policy_dir / 'policy').write_text('[core]\n autocrlf = true\n')
        rel = policy_dir.relative_to(home) / 'policy'
        for form in ('~/' + str(rel), '~' + user + '/' + str(rel)):
            with self.subTest(form=form):
                global_config = self.root / 'tilde-global'
                global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ' + form + '\n')
                self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
                result = self.world('init', str(self.source), code=3)
                self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                self.assertIn(b'which sets core.autocrlf', result.stderr)
        with self.subTest(form='unresolvable nested include'):
            nested = self.root / 'outer-policy'
            nested.write_text('[include]\n path = %(prefix)/etc/forkfs-nested\n')
            global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ' + str(nested) + '\n')
            self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
            result = self.world('init', str(self.source), code=3)
            self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
            self.assertIn(b'cannot be resolved to a file', result.stderr)
        with self.subTest(form='unknown user'):
            global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ~no-such-user-forkfs/x\n')
            self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
            result = self.world('init', str(self.source), code=3)
            self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
            self.assertIn(b'cannot be resolved to a file', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_long_filter_attribute_values_are_compared_safely(self):
        (self.source / '.gitattributes').write_text('file filter=' + 'x' * 4000 + '\n')
        self.git(self.source, 'add', '.gitattributes')
        self.git(self.source, 'commit', '-qm', 'long filter name')
        self.git(self.source, 'config', 'filter.x.clean', 'cat')
        self.world('init', str(self.source))

    def test_relative_git_config_environment_is_refused(self):
        for name in ('GIT_CONFIG_GLOBAL', 'GIT_CONFIG_SYSTEM'):
            with self.subTest(name=name):
                self.env[name] = '../policy'
                if name == 'GIT_CONFIG_SYSTEM':
                    self.env['GIT_CONFIG_NOSYSTEM'] = '0'
                result = self.world('init', str(self.source), code=3)
                self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                self.env.pop('GIT_CONFIG_SYSTEM', None)
                self.env['GIT_CONFIG_NOSYSTEM'] = '1'
                self.assertIn(b'is a relative path (../policy)', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_used_ambient_filter_is_refused_without_execution(self):
        global_config = self.root / 'filter-global'
        global_config.write_text('[filter "example"]\n clean = touch ambient-filter-ran; cat\n')
        (self.source / '.gitattributes').write_text('*.bin filter=example\n')
        (self.source / 'model.bin').write_text('weights\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'model')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        result = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b"would make the World's Git see files differently", result.stderr)
        self.assertIn(b"reason: tracked file model.bin uses the 'example' filter", result.stderr)
        self.assertFalse((self.source / 'ambient-filter-ran').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_system_and_global_status_settings_are_shared(self):
        system_config = self.root / 'system-config'
        system_config.write_text('[core]\n autocrlf = false\n')
        ignores = self.root / 'global-ignore'
        ignores.write_text('*.scratch\n')
        global_config = self.root / 'global-config'
        global_config.write_text('[core]\n excludesFile = ' + str(ignores) + '\n autocrlf = input\n')
        self.env.update(GIT_CONFIG_NOSYSTEM='0', GIT_CONFIG_SYSTEM=str(system_config),
                        GIT_CONFIG_GLOBAL=str(global_config))
        (self.source / 'notes.scratch').write_text('ignored only by the global excludesFile\n')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertTrue((one / 'notes.scratch').exists())

    def test_filter_configuration_is_refused_before_execution(self):
        (self.source / '.gitattributes').write_text('file filter=example\n')
        self.git(self.source, 'add', '.gitattributes')
        self.git(self.source, 'commit', '-m', 'filter attribute')
        included = self.root / 'filter.config'
        included.write_text('[filter "example"]\n    clean = touch filter-ran; cat\n')
        self.git(self.source, 'config', '--local', 'include.path', str(included))
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b"would make the World's Git see files differently", result.stderr)
        self.assertIn(b"reason: tracked file file uses the 'example' filter", result.stderr)
        self.assertFalse((self.source / 'filter-ran').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_dangling_symbolic_ref_is_refused(self):
        self.git(self.source, 'symbolic-ref', 'refs/heads/alias', 'refs/heads/future')
        self.assertNotIn(b'alias', self.git(self.source, 'for-each-ref').stdout)
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(self.git(self.source, 'symbolic-ref', 'refs/heads/alias').stdout.strip(), b'refs/heads/future')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        # Once the target exists the alias is listed, mirrored and preserved.
        self.git(self.source, 'branch', 'future')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'symbolic-ref', 'refs/heads/alias').stdout.strip(), b'refs/heads/future')

    def test_repository_extensions_the_mirror_drops_are_refused(self):
        self.git(self.source, 'config', 'core.repositoryformatversion', '1')
        # (An extension Git itself does not know is already refused by Git before any import.)
        with self.subTest(extension='preciousObjects'):
            self.git(self.source, 'config', 'extensions.preciousObjects', 'true')
            result = self.world('init', str(self.source), code=3)
            self.assertIn(b'unsupported Git layout', result.stderr)
            self.git(self.source, 'config', '--unset', 'extensions.preciousObjects')
        with self.subTest(extension='worktreeConfig with a setting the import does not carry'):
            self.git(self.source, 'config', 'extensions.worktreeConfig', 'true')
            self.git(self.source, 'config', '--worktree', 'user.signingkey', 'ABC123')
            result = self.world('init', str(self.source), code=3)
            self.assertIn(b'unsupported Git layout', result.stderr)
            self.git(self.source, 'config', '--worktree', '--unset', 'user.signingkey')
            self.git(self.source, 'config', '--unset', 'extensions.worktreeConfig')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.world('init', str(self.source))

    def test_sha256_repository_imports(self):
        source = self.root / 'sha256'
        self.git(self.root, 'init', '-q', '-b', 'main', '--object-format=sha256', str(source))
        self.git(source, 'config', 'user.name', 'World Test')
        self.git(source, 'config', 'user.email', 'world@example.com')
        (source / 'file').write_text('sha256\n')
        self.git(source, 'add', 'file')
        self.git(source, 'commit', '-qm', 'base')
        head = self.git(source, 'rev-parse', 'HEAD').stdout.strip()
        self.assertEqual(len(head), 64)
        snapshot = self.world('init', str(source)).stdout.split()[0].decode()
        shutil.rmtree(source)
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.git(one, 'rev-parse', 'HEAD').stdout.strip(), head)
        self.assertEqual(self.git(one, 'rev-parse', '--show-object-format').stdout.strip(), b'sha256')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_reftable_source_is_refused(self):
        source = self.root / 'reftable'
        self.git(self.root, 'init', '-q', '-b', 'main', '--ref-format=reftable', str(source))
        self.git(source, 'config', 'user.name', 'World Test')
        self.git(source, 'config', 'user.email', 'world@example.com')
        (source / 'file').write_text('reftable\n')
        self.git(source, 'add', 'file')
        self.git(source, 'commit', '-qm', 'base')
        result = self.world('init', str(source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_attribute_source_overrides_are_refused(self):
        with self.subTest(source='GIT_ATTR_SOURCE'):
            env = dict(self.env)
            self.env['GIT_ATTR_SOURCE'] = self.base.decode()
            result = self.world('init', str(self.source), code=3)
            self.env = env
            self.assertIn(b'reason: GIT_ATTR_SOURCE is set', result.stderr)
        with self.subTest(source='attr.tree'):
            self.git(self.source, 'config', 'attr.tree', 'HEAD')
            result = self.world('init', str(self.source), code=3)
            self.git(self.source, 'config', '--unset', 'attr.tree')
            self.assertIn(b'reason: attr.tree is set', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.world('init', str(self.source))

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

    def test_git_locks_in_a_world_block_fork_and_checkpoint(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        repo = one / '.world-git' / 'repo.git'
        before = json.loads(self.world('list', '--json').stdout)
        for rel in ('packed-refs.lock', 'refs/heads/main.lock', 'config.lock',
                    'objects/info/commit-graphs/commit-graph-chain.lock', 'worktrees/active/index.lock'):
            with self.subTest(lock=rel):
                lock = repo / rel
                lock.parent.mkdir(parents=True, exist_ok=True)
                lock.write_text('')
                self.world('fork', '--from', wid, '--to', str(self.root / 'two'), code=1)
                self.assertFalse((self.root / 'two').exists())
                self.world('checkpoint', wid, code=1)
                self.assertTrue(lock.exists())
                self.assertEqual(json.loads(self.world('list', '--json').stdout), before)
                lock.unlink()
        two, _ = self.fork('two', wid)
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.git(two, 'pack-refs', '--all')

    def test_conflicted_notes_merge_is_refused(self):
        self.git(self.source, 'notes', 'add', '-m', 'base note')
        self.git(self.source, 'update-ref', 'refs/notes/other', 'refs/notes/commits')
        self.git(self.source, 'notes', 'add', '-f', '-m', 'ours')
        self.git(self.source, 'notes', '--ref=other', 'add', '-f', '-m', 'theirs')
        self.git(self.source, 'notes', 'merge', '-s', 'manual', 'other', code=1)
        admin = self.source / '.git'
        markers = ('NOTES_MERGE_PARTIAL', 'NOTES_MERGE_REF', 'NOTES_MERGE_WORKTREE')
        for name in markers:
            self.assertTrue((admin / name).exists(), name)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        partial = (admin / 'NOTES_MERGE_PARTIAL').read_bytes()
        self.world('init', str(self.source), '--include-changes', code=1)
        self.assertEqual((admin / 'NOTES_MERGE_PARTIAL').read_bytes(), partial)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        for keep in markers:
            with self.subTest(marker=keep):
                saved = {n: self.root / ('saved-' + n) for n in markers if n != keep}
                for n, dst in saved.items():
                    os.rename(admin / n, dst)
                self.world('init', str(self.source), '--include-changes', code=1)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                for n, dst in saved.items():
                    os.rename(dst, admin / n)
        self.git(self.source, 'notes', 'merge', '--abort')
        self.assertTrue((admin / 'NOTES_MERGE_WORKTREE').is_dir())
        self.assertEqual(list((admin / 'NOTES_MERGE_WORKTREE').iterdir()), [])
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'notes', 'show').stdout, b'ours\n')

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

    def test_orig_head_recovery_survives_source_deletion(self):
        (self.source / 'file').write_text('recover this commit\n')
        self.git(self.source, 'commit', '-am', 'recoverable tip')
        recovered = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()
        self.git(self.source, 'reset', '--hard', self.base.decode())
        self.assertEqual(self.git(self.source, 'rev-parse', 'ORIG_HEAD').stdout.strip(), recovered)
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.git(one, 'gc', '--quiet')
        self.assertEqual(self.git(one, 'rev-parse', 'ORIG_HEAD').stdout.strip(), recovered)
        self.git(one, 'reset', '--hard', 'ORIG_HEAD')
        self.assertEqual(self.git(one, 'rev-parse', 'HEAD').stdout.strip(), recovered)
        self.assertEqual((one / 'file').read_text(), 'recover this commit\n')

    def test_external_policy_added_during_mirror_aborts_publication(self):
        import shlex
        real_git = shutil.which('git')
        wrapper = self.root / 'policy-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        policy = self.root / 'external-policy'
        policy.write_text('*.log\n')
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        index = (self.source / '.git' / 'index').read_bytes()
        for key, value in (('core.excludesFile', str(policy)), ('core.attributesFile', str(policy)),
                           ('core.sparseCheckout', 'true'), ('core.splitIndex', 'true'),
                           ('extensions.partialClone', 'origin')):
            with self.subTest(key=key):
                script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                                  + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                                  + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                                  + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source))
                                  + ' config --local ' + shlex.quote(key) + ' ' + shlex.quote(value)
                                  + ' || exit $?\nfi\nexit "$result"\n')
                script.chmod(0o700)
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(self.git(self.source, 'config', '--get', key).stdout.decode().strip(), value)
                self.assertEqual((self.source / '.git' / 'index').read_bytes(), index)
                self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.git(self.source, 'config', '--unset-all', key)

    def edit_after_clean_check(self, repo, name):
        # After the first `status` run in `repo` (the admission check), edit a tracked file:
        # HEAD and the index stay the same, only the worktree bytes change before the copy.
        import shlex
        real_git = shutil.which('git')
        wrapper = self.root / ('dirty-race-bin-' + name)
        wrapper.mkdir()
        done = self.root / ('dirty-race-done-' + name)
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\ncwd=\nprev=\nstatus=0\nfor arg in "$@"; do\n'
                          + '  [ "$prev" = -C ] && cwd=$arg\n  [ "$arg" = status ] && status=1\n  prev=$arg\ndone\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$status" = 1 ] && [ "$cwd" = ' + shlex.quote(str(repo))
                          + ' ] && [ ! -e ' + shlex.quote(str(done)) + ' ]; then\n'
                          + '  : > ' + shlex.quote(str(done)) + ' || exit $?\n'
                          + '  printf "edited after the check\\n" > ' + shlex.quote(str(repo / 'file')) + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        return wrapper, done

    def test_tracked_edit_after_clean_check_is_not_published(self):
        index = (self.source / '.git' / 'index').read_bytes()
        wrapper, done = self.edit_after_clean_check(self.source, 'init')
        env = dict(self.env)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        result = self.world('init', str(self.source), code=3)
        self.env = env
        self.assertTrue(done.exists())
        self.assertIn(b'uncommitted', result.stderr.lower())
        self.assertEqual((self.source / 'file').read_text(), 'edited after the check\n')
        self.assertEqual((self.source / '.git' / 'index').read_bytes(), index)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.world('init', str(self.source), '--include-changes')

    def test_tracked_edit_after_clean_check_is_not_forked_from_world(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        before = json.loads(self.world('list', '--json').stdout)
        wrapper, done = self.edit_after_clean_check(one, 'world')
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        result = self.world('fork', '--from', wid, '--to', str(self.root / 'two'), code=3)
        self.assertTrue(done.exists())
        self.assertIn(b'uncommitted', result.stderr.lower())
        self.assertFalse((self.root / 'two').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout), before)

    def test_identity_change_during_mirror_aborts_publication(self):
        import shlex
        real_git = shutil.which('git')
        wrapper = self.root / 'identity-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source))
                          + ' config user.email changed@example.com || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        env = dict(self.env)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.env = env
        self.assertEqual(self.git(self.source, 'config', 'user.email').stdout.strip(), b'changed@example.com')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.git(one, 'config', 'user.email').stdout.strip(), b'changed@example.com')

    def test_identity_resolving_differently_in_the_copy_is_refused(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        # <world>/.world-git/repo.git/config includes ../../../identity: the World's parent.
        self.git(one, 'config', '--unset', 'user.email')
        self.git(one, 'config', 'include.path', '../../../identity')
        (self.root / 'identity').write_text('[user]\n email = here@example.com\n')
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        (elsewhere / 'identity').write_text('[user]\n email = there@example.com\n')
        self.assertEqual(self.git(one, 'config', 'user.email').stdout.strip(), b'here@example.com')
        before = json.loads(self.world('list', '--json').stdout)
        self.world('fork', '--from', wid, '--to', str(elsewhere / 'two'), code=1)
        self.assertFalse((elsewhere / 'two').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout), before)
        (elsewhere / 'identity').write_text('[user]\n email = here@example.com\n')
        two, _ = self.fork(str(Path('elsewhere') / 'two'), wid)
        self.assertEqual(self.git(two, 'config', 'user.email').stdout.strip(), b'here@example.com')

    def test_any_setting_resolving_differently_in_the_copy_is_refused(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'include.path', '../../../world-policy')
        (self.root / 'world-policy').write_text('[core]\n hooksPath = /dev/null\n')
        elsewhere = self.root / 'elsewhere'
        (elsewhere / 'hooks').mkdir(parents=True)
        hook = elsewhere / 'hooks' / 'pre-commit'
        hook.write_text('#!/bin/sh\ntouch "$(dirname "$0")/ran"\n')
        hook.chmod(0o755)
        (elsewhere / 'world-policy').write_text('[core]\n hooksPath = ' + str(elsewhere / 'hooks') + '\n')
        before = json.loads(self.world('list', '--json').stdout)
        self.world('fork', '--from', wid, '--to', str(elsewhere / 'two'), code=1)
        self.assertFalse((elsewhere / 'two').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout), before)
        (elsewhere / 'world-policy').write_text('[core]\n hooksPath = /dev/null\n')
        two, _ = self.fork(str(Path('elsewhere') / 'two'), wid)
        self.git(two, 'commit', '-q', '--allow-empty', '-m', 'no hook runs')
        self.assertFalse((elsewhere / 'hooks' / 'ran').exists())

    def test_external_hardlinks_are_reported_after_git_import(self):
        (self.source / 'shared').write_text('linked from outside\n')
        self.git(self.source, 'add', 'shared')
        self.git(self.source, 'commit', '-qm', 'shared')
        os.link(self.source / 'shared', self.root / 'outside-link')
        # A link on a Git administration file is not a file of the published tree.
        os.link(self.source / '.git' / 'description', self.root / 'outside-description')
        # Nor does a worktree file become external because its twin lived in the replaced .git.
        (self.source / 'twin').write_text('twin\n')
        self.git(self.source, 'add', 'twin')
        self.git(self.source, 'commit', '-qm', 'twin')
        os.link(self.source / 'twin', self.source / '.git' / 'twin-copy')
        result = self.world('init', str(self.source))
        self.assertIn(b'names outside this tree', result.stderr)
        snap = json.loads(self.world('list', '--json').stdout)['snapshots'][0]
        self.assertEqual((snap['hl_groups'], snap['hl_external']), (0, 1))
        self.assertIn(b'1 entries also linked from outside the tree', self.world('inspect', 'S1').stdout)

    def test_direct_ref_change_during_mirror_aborts_publication(self):
        import shlex
        self.git(self.source, 'update-ref', 'refs/custom/raced', self.base.decode())
        advanced = self.git(self.source, 'commit-tree', self.base.decode() + '^{tree}',
                            '-p', self.base.decode(), '-m', 'concurrent ref').stdout.decode().strip()
        real_git = shutil.which('git')
        wrapper = self.root / 'race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source))
                          + ' update-ref refs/custom/raced ' + shlex.quote(advanced)
                          + ' || exit $?\nfi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/custom/raced').stdout.decode().strip(), advanced)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_remotes_upstreams_and_aliases_travel_into_the_world(self):
        upstream = self.root / 'upstream.git'
        self.git(self.root, 'clone', '-q', '--bare', str(self.source), str(upstream))
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/team/repo.git')
        self.git(self.source, 'config', 'remote.origin.pushurl', 'git@example.invalid:team/repo.git')
        self.git(self.source, 'config', '--add', 'remote.origin.fetch', '+refs/tags/*:refs/tags/*')
        self.git(self.source, 'remote', 'add', 'up', '../upstream.git')
        self.git(self.source, 'fetch', '-q', 'up')
        self.git(self.source, 'config', 'branch.main.remote', 'up')
        self.git(self.source, 'config', 'branch.main.merge', 'refs/heads/main')
        self.git(self.source, 'config', 'url.https://mirror.invalid/.insteadOf', 'https://slow.invalid/')
        self.git(self.source, 'config', 'push.autoSetupRemote', 'true')
        self.git(self.source, 'config', 'alias.st', 'status --short')
        # A valueless boolean is true to Git and must stay true.
        with open(self.source / '.git' / 'config', 'a') as config:
            config.write('[remote "origin"]\n\tprune\n[branch "main"]\n\tdescription\n')
        # Settings Git would execute on its own are not carried.
        self.git(self.source, 'config', 'remote.origin.uploadpack', 'touch uploadpack-ran; git-upload-pack')
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        self.git(self.source, 'config', 'branch.main.mergeOptions', '--no-ff')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        get = lambda *key: self.git(one, 'config', *key).stdout.decode().split('\n')[:-1]
        self.assertEqual(get('--get', 'remote.origin.url'), ['https://example.invalid/team/repo.git'])
        self.assertEqual(get('--get', 'remote.origin.pushurl'), ['git@example.invalid:team/repo.git'])
        self.assertEqual(get('--get-all', 'remote.origin.fetch'),
                         ['+refs/heads/*:refs/remotes/origin/*', '+refs/tags/*:refs/tags/*'])
        self.assertEqual(get('--get', 'remote.up.url'), [str(upstream)])
        self.assertEqual(get('--get', 'branch.main.remote'), ['up'])
        self.assertEqual(get('--get', 'branch.main.merge'), ['refs/heads/main'])
        self.assertEqual(get('--get', 'url.https://mirror.invalid/.insteadof'), ['https://slow.invalid/'])
        self.assertEqual(get('--type=bool', '--get', 'push.autosetupremote'), ['true'])
        self.assertEqual(get('--type=bool', '--get', 'remote.origin.prune'), ['true'])
        # A valueless string setting stays empty rather than becoming "true".
        self.assertEqual(get('--get', 'branch.main.description'), [''])
        self.assertEqual(self.git(one, 'st').stdout, b'')
        for key in ('remote.origin.uploadpack', 'core.hooksPath', 'branch.main.mergeoptions'):
            self.git(one, 'config', '--get', key, code=1)
        # The World's own branch has no upstream, so nothing is pushed to main by accident.
        self.git(one, 'config', '--get', 'branch.world/W1.remote', code=1)
        # The relative remote still reaches the same repository after the source is gone.
        self.git(one, 'fetch', '-q', 'up')
        self.assertEqual(self.git(one, 'rev-parse', 'refs/remotes/up/main').stdout.strip(), self.base)
        self.assertFalse((one / 'uploadpack-ran').exists())

    def test_relative_remote_through_a_symlink_resolves_like_git(self):
        # A relative remote path is resolved through the filesystem, the same way Git itself
        # reaches it -- including through a symlink a ".." component in the path walks back out
        # of -- rather than collapsed lexically, which a symlink could make point somewhere else.
        other = self.root / 'other'
        other.mkdir()
        sub = other / 'sub'
        sub.mkdir()
        up = other / 'up.git'
        self.run_cmd('git', 'init', '-q', '--bare', str(up))
        link = self.source / 'link'
        link.symlink_to(sub)
        # The symlink is not part of the source's tracked tree; excluding it locally keeps
        # `git status` clean without committing it.
        with open(self.source / '.git' / 'info' / 'exclude', 'a') as exclude:
            exclude.write('/link\n')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.git(self.source, 'remote', 'add', 'origin', 'link/../up.git')
        # Sanity: Git itself resolves this through the symlink to <root>/other/up.git.
        self.git(self.source, 'ls-remote', 'origin')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.url').stdout.strip().decode(),
                         str((self.root / 'other' / 'up.git').resolve()))

    def test_missing_relative_remote_with_dotdot_is_refused(self):
        # Nothing on disk exists at this path, so there is no filesystem to resolve the ".."
        # component through -- a lexical guess could be wrong once a symlink appears later.
        self.git(self.source, 'remote', 'add', 'origin', '../missing/../nowhere.git')
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'does not exist', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_relative_remote_with_missing_leaf_through_a_symlink_resolves_like_git(self):
        # `new.git` under the symlink does not exist yet (the remote was never fetched into the
        # source), so there is no full path for `fs_realpath` to resolve directly. But `link`
        # itself exists and does resolve, and Git would still reach the remote through it once
        # fetched or pushed, so the resolution must land at the symlink's real target, not
        # lexically next to the source.
        target = self.root / 'target'
        target.mkdir()
        link = self.source / 'link'
        link.symlink_to(target)
        with open(self.source / '.git' / 'info' / 'exclude', 'a') as exclude:
            exclude.write('/link\n')
        self.git(self.source, 'remote', 'add', 'origin', 'link/new.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.url').stdout.strip().decode(),
                         str((target / 'new.git').resolve()))

    def test_relative_remote_with_missing_multilevel_path_through_a_symlink_resolves_like_git(self):
        # Neither `a` nor `a/b.git` exists under the symlink target, so the longest existing
        # prefix that can be resolved through the filesystem is the symlink itself; the rest of
        # the path (more than one missing component) is joined on lexically once that prefix is
        # real.
        target = self.root / 'target2'
        target.mkdir()
        link = self.source / 'link'
        link.symlink_to(target)
        with open(self.source / '.git' / 'info' / 'exclude', 'a') as exclude:
            exclude.write('/link\n')
        self.git(self.source, 'remote', 'add', 'origin', 'link/a/b.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.url').stdout.strip().decode(),
                         str((target / 'a' / 'b.git').resolve()))

    def test_relative_remote_through_a_dangling_symlink_is_refused(self):
        # `dead` exists as a symlink, but its target does not, so `realpath` cannot resolve it
        # and there is no way to tell from here where Git would actually follow it once the
        # target eventually exists.
        dead = self.source / 'dead'
        dead.symlink_to(self.root / 'nonexistent' / 'x')
        with open(self.source / '.git' / 'info' / 'exclude', 'a') as exclude:
            exclude.write('/dead\n')
        self.git(self.source, 'remote', 'add', 'origin', 'dead/r.git')
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'dangling symlink', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_remote_url_with_a_line_break_is_refused(self):
        # A local-path remote URL may legally contain an embedded newline -- Git accepts any
        # byte but '\0' and '/' in a path component. `git remote get-url --all` would emit it
        # verbatim, indistinguishable there from two separate URLs once split on '\n', so it is
        # refused up front rather than carried wrong.
        bare = self.root / 'up\nrepo.git'
        bare.mkdir()
        self.git(bare, 'init', '--bare', '-b', 'main')
        self.git(self.source, 'remote', 'add', 'origin', str(bare))
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'remote origin has a url containing a line break', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_remote_pushurl_with_a_line_break_is_refused(self):
        # Same as above, but for an explicit pushurl -- captured and checked separately from the
        # fetch url, and just as reachable through the push-side `git remote get-url --push`.
        bare = self.root / 'push\nrepo.git'
        bare.mkdir()
        self.git(bare, 'init', '--bare', '-b', 'main')
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        self.git(self.source, 'config', 'remote.origin.pushurl', str(bare))
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'remote origin has a pushurl containing a line break', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_stale_config_for_the_world_branch_is_not_inherited(self):
        # Carried configuration can include branch.<name>.* for a branch name that has no ref
        # yet, e.g. leftover config from a branch of that name the source once had. The new
        # World branch is always named world/W<n>, so such a stale section for that exact name
        # must not resurrect an upstream the World never had.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/r.git')
        self.git(self.source, 'config', 'branch.world/W1.remote', 'origin')
        self.git(self.source, 'config', 'branch.world/W1.merge', 'refs/heads/main')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.git(one, 'config', '--get', 'branch.world/W1.remote', code=1)
        self.git(one, 'config', '--get', 'branch.world/W1.merge', code=1)
        self.git(one, 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}', code=128)
        # The same guarantee holds for a World forked from a World, whose configuration is
        # copied wholesale from the parent, including any stale section for the child's name.
        self.git(one, 'config', 'branch.world/W2.remote', 'origin')
        two, _ = self.fork('two', wid)
        self.git(two, 'config', '--get', 'branch.world/W2.remote', code=1)
        self.git(two, 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}', code=128)

    def test_world_branch_skips_names_with_global_branch_config(self):
        # Unlike stale repository-local config (removed above), global/system
        # branch.world/W1.* configuration cannot be removed by this import; ordinary Git in
        # the World would still read it once that name exists. The generated branch must
        # therefore skip straight to the next free suffix, exactly like a colliding ref.
        global_config = self.root / 'ambient-branch-global'
        global_config.write_text('[branch "world/W1"]\n remote = origin\n merge = refs/heads/main\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1-1')
        self.git(one, 'rev-parse', '--abbrev-ref', '--symbolic-full-name', '@{upstream}', code=128)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'

    def test_rewritten_relative_remote_url_is_refused(self):
        # Whichever direction the rule rewrites, a relative remote URL it matches is refused
        # rather than carried -- reproducing Git's own insteadOf/pushInsteadOf resolution across
        # a change of location is what kept diverging from Git's actual behavior.
        for suffix in ('insteadOf', 'pushInsteadOf'):
            with self.subTest(suffix=suffix):
                self.git(self.source, 'remote', 'add', 'origin', '../up.git')
                self.git(self.source, 'config', 'url.ssh://example.invalid/.' + suffix, '../')
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'is relative and url.', result.stderr)
                self.git(self.source, 'config', '--remove-section', 'url.ssh://example.invalid/')
                self.git(self.source, 'remote', 'remove', 'origin')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_conditional_rewrite_of_relative_remote_is_refused(self):
        included = self.root / 'conditional-rewrite'
        included.write_text('[url "ssh://cond.invalid/"]\n insteadOf = ../\n')
        global_config = self.root / 'conditional-rewrite-global'
        global_config.write_text('[includeIf "gitdir:' + str(self.root) + '/"]\n path = ' + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', '../up.git')
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'is relative and url.', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_inactive_conditional_rewrite_of_relative_remote_is_refused(self):
        # The includeIf condition does not hold at the source (self.source is not under
        # self.root/'elsewhere'), so the rule is inactive here -- but it may become active once
        # the World moves, so a relative URL it would match is still refused.
        included = self.root / 'inactive-conditional-rewrite'
        included.write_text('[url "ssh://cond.invalid/"]\n insteadOf = ../\n')
        global_config = self.root / 'inactive-conditional-rewrite-global'
        global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ' + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', '../up.git')
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'is relative and url.', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_conditional_include_with_per_remote_or_branch_settings_is_refused(self):
        # A conditional include's target may set remote.<name>.url/pushurl or
        # branch.<name>.remote/merge. The condition does not hold at the source (self.source
        # is not under self.root/'elsewhere'), but it could become active once the World
        # moves there, at which point it would add a URL to a carried remote or an upstream
        # to the World's generated branch -- refused just like a status or filter setting,
        # active or not.
        targets = {
            'remote': '[remote "origin"]\n url = https://cond.invalid/x.git\n',
            'branch': '[branch "world/W1"]\n remote = origin\n',
        }
        global_config = self.root / 'per-remote-or-branch-global'
        for name, contents in targets.items():
            with self.subTest(target=name):
                included = self.root / ('per-remote-or-branch-' + name)
                included.write_text(contents)
                global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = '
                                          + str(included) + '\n')
                self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
                result = self.world('init', str(self.source), code=3)
                self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                self.assertIn(b'per-remote and per-branch settings', result.stderr)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_conditional_rewrite_of_absolute_remote_is_pinned(self):
        # A rule that lives in a conditional include active at the source -- the common
        # per-account includeIf "gitdir:~/work/" setup -- still rewrites an absolute remote URL
        # there, and may or may not apply once the World sits elsewhere. The World must still
        # reach the same endpoint the source actually used, so the effective URL is pinned rather
        # than carried raw.
        included = self.root / 'conditional-pin'
        included.write_text('[url "ssh://work.invalid/"]\n insteadOf = https://example.invalid/work/\n')
        global_config = self.root / 'conditional-pin-global'
        # Scoped to the source's own .git, not self.root, so it is inactive at the World's path.
        global_config.write_text('[includeIf "gitdir:' + str(self.source) + '/"]\n path = '
                                  + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/work/proj.git')
        # Sanity: the condition is active at the source, so Git itself already rewrites it there.
        self.assertEqual(self.git(self.source, 'remote', 'get-url', 'origin').stdout.strip(),
                          b'ssh://work.invalid/proj.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        # The World does not sit under the source, so the condition is now inactive -- but the
        # pinned literal value still reproduces what the source's Git actually used.
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.url').stdout.strip(),
                          b'ssh://work.invalid/proj.git')
        self.assertEqual(self.git(one, 'remote', 'get-url', '--push', 'origin').stdout.strip(),
                          b'ssh://work.invalid/proj.git')
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'

    def test_conditional_rewrite_of_push_only_remote_is_pinned(self):
        # A remote can carry only a pushurl and no url at all (Git supports this). A conditional
        # rewrite rule active at the source still rewrites that explicit pushurl there, so it
        # must be pinned the same way as a remote's url, not skipped for lacking a url entry.
        included = self.root / 'conditional-pin-push-only'
        included.write_text('[url "ssh://work.invalid/"]\n insteadOf = https://example.invalid/work/\n')
        global_config = self.root / 'conditional-pin-push-only-global'
        # Scoped to the source's own .git, not self.root, so it is inactive at the World's path.
        global_config.write_text('[includeIf "gitdir:' + str(self.source) + '/"]\n path = '
                                  + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'config', 'remote.pushonly.pushurl', 'https://example.invalid/work/proj.git')
        # Sanity: the condition is active at the source, so Git itself already rewrites the
        # explicit pushurl there.
        self.assertEqual(self.git(self.source, 'remote', 'get-url', '--push', 'pushonly').stdout.strip(),
                          b'ssh://work.invalid/proj.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        # The World does not sit under the source, so the condition is now inactive -- but the
        # pinned literal value still reproduces what the source's Git actually used, and no url
        # entry is synthesized for a remote that never had one.
        self.assertEqual(self.git(one, 'config', '--get-all', 'remote.pushonly.pushurl').stdout.strip(),
                          b'ssh://work.invalid/proj.git')
        self.git(one, 'config', '--get', 'remote.pushonly.url', code=1)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'

    def test_pinned_remote_url_rewritten_again_is_refused(self):
        # A rewrite pinned from the source must not be rewritten again by another rule active in
        # the source's own configuration, or the World would resolve it differently than the
        # source does.
        self.git(self.source, 'config', 'url.foo://.pushInsteadOf', 'https://example.invalid/')
        self.git(self.source, 'config', 'url.bar://.insteadOf', 'foo://')
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/up.git')
        # Sanity: Git's own rewriting is single-pass and stops at "foo://up.git" for push,
        # without chaining into the second rule -- the source's Git never reaches "bar://up.git".
        self.assertEqual(self.git(self.source, 'remote', 'get-url', '--push', 'origin').stdout.strip(),
                          b'foo://up.git')
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'would rewrite again', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_pinned_remote_keeps_explicit_pushurl(self):
        # An explicit pushurl must stay explicit once pinned: Git never falls back to a remote's
        # (possibly rewritten) url entries once it has an explicit pushurl, so leaving it
        # implicit here would expose it to a pushInsteadOf rule active at the World's own
        # location.
        self.git(self.source, 'config', 'url.ssh://same.invalid/.insteadOf', 'https://example.invalid/')
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/proj.git')
        self.git(self.source, 'config', 'remote.origin.pushurl', 'https://example.invalid/proj.git')
        # Sanity: insteadOf rewrites both the fetch and the explicit push URL here, so they agree
        # at the source even though the pushurl was set explicitly.
        self.assertEqual(self.git(self.source, 'remote', 'get-url', 'origin').stdout.strip(),
                          b'ssh://same.invalid/proj.git')
        self.assertEqual(self.git(self.source, 'remote', 'get-url', '--push', 'origin').stdout.strip(),
                          b'ssh://same.invalid/proj.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get-all', 'remote.origin.pushurl').stdout.strip(),
                          b'ssh://same.invalid/proj.git')

    def test_remote_urls_in_ambient_configuration_are_refused(self):
        # A carried remote's URL must live only in the repository-local configuration the loop
        # captures: the World reads the same global/system configuration the source does, so a
        # remote.<name>.url also set there would be visible to the World too, and pinning the
        # ambient value on top would duplicate it.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        global_config = self.root / 'remote-ambient-global'
        global_config.write_text('[remote "origin"]\n url = https://global.invalid/x.git\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'also has url in global configuration', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_remote_change_during_mirror_aborts_publication(self):
        import shlex
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/before.git')
        real_git = shutil.which('git')
        wrapper = self.root / 'remote-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source))
                          + ' remote set-url origin https://example.invalid/after.git || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_publish_copies_world_commits_back_as_a_branch(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / 'file').write_text('world change\n')
        self.git(one, 'commit', '-qam', 'world change')
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
        source_state = [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index')]
        result = self.world('publish', wid)
        self.assertIn(b'new branch world/W1', result.stdout)
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/world/W1').stdout.strip(), head)
        # Nothing else in the source changed: checkout, index, working tree, other branches.
        self.assertEqual(source_state, [(self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index')])
        self.assertEqual((self.source / 'file').read_text(), 'original\n')
        self.assertEqual(self.git(self.source, 'rev-parse', 'main').stdout.strip(), self.base)
        # A later commit is a fast-forward; uncommitted World changes are reported, not published.
        (one / 'file').write_text('second change\n')
        self.git(one, 'commit', '-qam', 'second change')
        (one / 'draft').write_text('not committed\n')
        second = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
        result = self.world('publish', wid)
        self.assertIn(b'uncommitted changes; only its commits were published', result.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), second)
        self.assertIn(b'already at', self.world('publish', wid).stdout)
        (one / 'draft').unlink()
        # A differently named branch, and a fork of the World publishing to the same source.
        self.world('publish', wid, '--branch', 'feature/from-world')
        self.assertEqual(self.git(self.source, 'rev-parse', 'feature/from-world').stdout.strip(), second)
        two, wid2 = self.fork('two', wid)
        self.git(two, 'commit', '-q', '--allow-empty', '-m', 'from the child')
        self.world('publish', wid2)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W2').stdout.strip(),
                         self.git(two, 'rev-parse', 'HEAD').stdout.strip())

    def test_publish_refuses_what_the_target_cannot_take(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
        def refused(reason, *args):
            result = self.world('publish', wid, *args, code=3)
            self.assertIn(reason, result.stderr)
        # The source's checked-out branch is never moved.
        refused(b'main is checked out in the target repository', '--branch', 'main')
        self.assertEqual(self.git(self.source, 'rev-parse', 'main').stdout.strip(), self.base)
        # A diverged branch of the same name needs --force.
        self.git(self.source, 'commit', '-q', '--allow-empty', '-m', 'source change')
        self.git(self.source, 'branch', 'world/W1', 'main')
        diverged = self.git(self.source, 'rev-parse', 'world/W1').stdout.strip()
        refused(b'not a fast-forward (--force overwrites it)')
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), diverged)
        self.world('publish', wid, '--force')
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), head)
        # An unrelated repository, a non-repository and a bad name.
        other = self.root / 'other'
        self.git(self.root, 'init', '-q', '-b', 'main', str(other))
        self.git(other, '-c', 'user.name=Other', '-c', 'user.email=other@example.com',
                 'commit', '-q', '--allow-empty', '-m', 'unrelated')
        refused(b'shares no history with the World', '--repo', str(other))
        self.assertEqual(self.git(other, 'for-each-ref', 'refs/worldfs').stdout, b'')
        self.git(other, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        clone = self.root / 'another-clone'
        self.git(self.root, 'clone', '-q', str(self.source), str(clone))
        self.world('publish', wid, '--repo', str(clone), '--branch', 'reviewed')
        self.assertEqual(self.git(clone, 'rev-parse', 'reviewed').stdout.strip(), head)
        bare = self.root / 'shared.git'
        self.git(self.root, 'clone', '-q', '--bare', str(self.source), str(bare))
        self.world('publish', wid, '--repo', str(bare), '--branch', 'reviewed')
        self.assertEqual(self.run_cmd('git', '--git-dir', str(bare), 'rev-parse', 'reviewed').stdout.strip(), head)
        plain = self.root / 'plain'
        plain.mkdir()
        refused(b'is not a Git repository', '--repo', str(plain))
        refused(b'is not a valid branch name', '--branch', 'bad..name')
        # A detached World must name the branch.
        self.git(one, 'checkout', '-q', '--detach')
        refused(b'HEAD is detached; name the branch to create with --branch')
        self.world('publish', wid, '--branch', 'detached-work')
        self.assertEqual(self.git(self.source, 'rev-parse', 'detached-work').stdout.strip(), head)

    def test_filter_named_like_an_attribute_sentinel_is_refused(self):
        for name in ('set', 'unset', 'unspecified'):
            with self.subTest(driver=name):
                marker = self.root / ('ran-' + name)
                (self.source / '.gitattributes').write_text('file filter=' + name + '\n')
                self.git(self.source, 'add', '.gitattributes')
                self.git(self.source, 'commit', '-qm', 'filter ' + name)
                self.git(self.source, 'config', 'filter.' + name + '.clean', 'touch ' + str(marker) + '; cat')
                os.utime(self.source / 'file', (1, 1))
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b"uses the '" + name.encode() + b"' filter", result.stderr)
                self.assertFalse(marker.exists())
                self.git(self.source, 'config', '--remove-section', 'filter.' + name)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_publish_leaves_other_refs_and_no_staging_ref(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.git(self.source, 'update-ref', 'refs/worldfs/publish-' + str(os.getpid()), self.base.decode())
        self.world('publish', wid)
        refs = self.git(self.source, 'for-each-ref', '--format=%(refname) %(objectname)', 'refs/worldfs').stdout.split(b'\n')
        self.assertEqual([r for r in refs if r], [b'refs/worldfs/publish-' + str(os.getpid()).encode() + b' ' + self.base])

    def test_publish_diagnostic_failure_changes_nothing(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        index = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'index').stdout.decode().strip()
        Path(index).write_bytes(b'not an index')
        self.world('publish', wid, code=3)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_refuses_a_filter_attached_after_import(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        marker = self.root / 'late-filter-ran'
        (one / '.gitattributes').write_text('file filter=late\n')
        self.git(one, 'add', '.gitattributes')
        self.git(one, 'commit', '-qm', 'attach a filter')
        self.git(one, 'config', 'filter.late.clean', 'touch ' + str(marker) + '; cat')
        os.utime(one / 'file', (1, 1))
        result = self.world('publish', wid, code=3)
        self.assertIn(b"uses the 'late' filter", result.stderr)
        self.assertFalse(marker.exists())
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_absent_source_identity_stays_absent(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        identity = self.root / 'there-identity'
        identity.write_text('[user]\n email = there@example.com\n')
        global_config = self.root / 'identity-global'
        global_config.write_text('[includeIf "gitdir:' + str(elsewhere) + '/"]\n path = ' + str(identity) + '\n')
        self.git(self.source, 'config', '--unset', 'user.email')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'config', 'user.email', code=1)
        self.world('init', str(self.source))
        one, _ = self.fork(str(Path('elsewhere') / 'one'))
        self.assertEqual(self.git(one, 'config', '--show-scope', 'user.email').stdout, b'local\t\n')

    def test_publish_refuses_a_branch_checked_out_in_a_linked_worktree(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.git(self.source, 'worktree', 'add', '-q', str(self.root / 'first-wt'), '-b', 'other')
        self.git(self.source, 'worktree', 'add', '-q', str(self.root / 'second-wt'), '-b', 'world/W1')
        result = self.world('publish', wid, code=3)
        self.assertIn(b'world/W1 is checked out in the target repository', result.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), self.base)

    def test_publish_refuses_a_symbolic_destination(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.git(self.source, 'symbolic-ref', 'refs/heads/alias', 'refs/heads/main')
        result = self.world('publish', wid, '--branch', 'alias', code=3)
        self.assertIn(b'alias is a symbolic ref in the target repository', result.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'main').stdout.strip(), self.base)
        self.assertEqual(self.git(self.source, 'symbolic-ref', 'refs/heads/alias').stdout.strip(), b'refs/heads/main')

    def test_publish_refuses_url_rewrites_of_the_world_path(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        decoy = self.root / 'decoy'
        self.git(self.root, 'clone', '-q', str(self.source), str(decoy))
        self.git(decoy, 'checkout', '-q', '-b', 'world/W1')
        self.git(decoy, '-c', 'user.name=D', '-c', 'user.email=d@example.com', 'commit', '-q', '--allow-empty', '-m', 'decoy')
        self.git(self.source, 'config', 'url.' + str(decoy) + '.insteadOf', str(one))
        result = self.world('publish', wid, code=3)
        self.assertIn(b"url.*.insteadOf rewrites the World's path", result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_publish_refuses_the_worlds_own_repository(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        result = self.world('publish', wid, '--repo', str(one / '.world-git' / 'repo.git'),
                             '--branch', 'elsewhere', code=3)
        self.assertIn(b"the target repository is the World's own repository", result.stderr)
        self.git(one, 'rev-parse', '--verify', '-q', 'refs/heads/elsewhere', code=1)

    def test_publish_refuses_a_world_whose_tree_was_replaced(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        two, _ = self.fork('two')
        self.git(two, 'commit', '-q', '--allow-empty', '-m', 'from W2')
        # Overwrite W1's directory contents with W2's, keeping W1's directory inode.
        for entry in list(one.iterdir()):
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry)
            else:
                entry.unlink()
        for entry in two.iterdir():
            if entry.is_dir() and not entry.is_symlink():
                shutil.copytree(entry, one / entry.name, symlinks=True)
            else:
                shutil.copy2(entry, one / entry.name, follow_symlinks=False)
        result = self.world('publish', wid, code=3)
        self.assertIn(b"does not carry W1's .world marker", result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_publish_refuses_a_symlinked_world_administration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        # A symlinked administration could otherwise point publish at another repository's
        # history (a decoy sharing this project's history, with its own world/W1 branch)
        # rather than refusing outright. Even when the symlink target is a faithful copy of
        # the World's own .world-git, fork/checkpoint's managed_check refuses any symlink in
        # the owned administration, including at its root.
        decoy = self.root / 'decoy'
        self.git(self.root, 'clone', '-q', str(self.source), str(decoy))
        self.git(decoy, 'checkout', '-q', '-b', 'world/W1')
        self.git(decoy, '-c', 'user.name=D', '-c', 'user.email=d@example.com', 'commit', '-q', '--allow-empty', '-m', 'decoy')
        moved = self.root / 'moved-admin'
        shutil.move(str(one / '.world-git'), str(moved))
        (one / '.world-git').symlink_to(moved)
        result = self.world('publish', wid, code=3)
        self.assertIn(b"the World's Git administration contains a symlink", result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_publish_removes_the_staging_ref_when_fetch_fails_late(self):
        import shlex
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        real_git = shutil.which('git')
        wrapper = self.root / 'late-fetch-failure-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        # The fetch itself succeeds (the staging ref is written), then the command fails.
        script.write_text('#!/bin/sh\nfetch=0\nfor arg in "$@"; do [ "$arg" = fetch ] && fetch=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$fetch" = 1 ]; then exit 128; fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('publish', wid, code=3)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_publish_drops_staging_ref_when_final_update_fails(self):
        import shlex
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        real_git = shutil.which('git')
        wrapper = self.root / 'final-update-failure-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        # The final ref transaction itself fails (e.g. another process moved the branch);
        # every other Git invocation, including the one that drops the staging ref, behaves
        # normally.
        script.write_text('#!/bin/sh\nupdate=0\nstdin=0\n'
                          + 'for arg in "$@"; do [ "$arg" = update-ref ] && update=1; '
                          + '[ "$arg" = --stdin ] && stdin=1; done\n'
                          + 'if [ "$update" = 1 ] && [ "$stdin" = 1 ]; then exit 1; fi\n'
                          + shlex.quote(real_git) + ' "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('publish', wid, code=3)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)

    def test_publish_follows_checkpoints_back_to_the_source(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'before checkpoint')
        snapshot = self.world('checkpoint', wid).stdout.split()[0].decode()
        two, wid2 = self.fork('two', snapshot)
        self.git(two, 'commit', '-q', '--allow-empty', '-m', 'after checkpoint')
        self.world('publish', wid2, '--branch', 'from-checkpoint')
        self.assertEqual(self.git(self.source, 'rev-parse', 'from-checkpoint').stdout.strip(),
                         self.git(two, 'rev-parse', 'HEAD').stdout.strip())

    def test_publish_refuses_a_world_marker_force_added_after_import(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        # Import only checks history it is about to preserve; force-adding and committing the
        # World's own marker file afterwards is the only way to get a reserved path into a
        # World's history in the first place, and publish must catch it too.
        self.git(one, 'add', '-f', '.world')
        self.git(one, '-c', 'user.name=Leo', '-c', 'user.email=leo@clapdb.com',
                 'commit', '-qm', 'force-add the World marker')
        result = self.world('publish', wid, code=3)
        self.assertIn(b'tracks the reserved path .world or .world-git', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_publish_refuses_world_git_content_even_after_a_later_clean_commit(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / '.world-git' / 'extra').write_text('smuggled\n')
        self.git(one, 'add', '-f', '.world-git/extra')
        self.git(one, '-c', 'user.name=Leo', '-c', 'user.email=leo@clapdb.com',
                 'commit', '-qm', 'force-add a file under .world-git')
        # A later, clean commit that removes the path again must not clear it from history:
        # publish walks every commit reachable from the tip, not just its tree.
        self.git(one, 'rm', '-q', '--cached', '.world-git/extra')
        (one / '.world-git' / 'extra').unlink()
        self.git(one, '-c', 'user.name=Leo', '-c', 'user.email=leo@clapdb.com',
                 'commit', '-qm', 'untrack it again')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        result = self.world('publish', wid, code=3)
        self.assertIn(b'tracks the reserved path .world or .world-git', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

    def test_fetch_head_only_tip_survives_source_deletion(self):
        remote = self.root / 'fetch-remote'
        remote.mkdir()
        self.git(remote, 'init', '-b', 'main')
        self.git(remote, 'config', 'user.name', 'Fetch Test')
        self.git(remote, 'config', 'user.email', 'fetch@example.com')
        (remote / 'fetched').write_text('fetch-only content\n')
        self.git(remote, 'add', 'fetched')
        self.git(remote, 'commit', '-m', 'fetch-only commit')
        tip = self.git(remote, 'rev-parse', 'HEAD').stdout.strip()
        self.git(self.source, 'fetch', str(remote), 'main')
        self.assertNotIn(tip, self.git(self.source, 'for-each-ref', '--format=%(objectname)').stdout)
        before = (self.source / '.git' / 'FETCH_HEAD').read_bytes()
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        shutil.rmtree(remote)
        one, _ = self.fork()
        record = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'FETCH_HEAD').stdout.decode().strip()
        self.assertEqual(Path(record).read_bytes(), before)
        self.assertEqual(self.git(one, 'rev-parse', 'FETCH_HEAD').stdout.strip(), tip)
        self.assertEqual(self.git(one, 'show', 'FETCH_HEAD:fetched').stdout, b'fetch-only content\n')

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

    def test_branch_sharing_a_tag_name_is_reported_exactly(self):
        self.git(self.source, 'tag', 'world/W1')
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1')
        self.assertEqual(self.git(one, 'rev-parse', 'refs/tags/world/W1').stdout.strip(), self.base)
        data = json.loads(self.world('inspect', wid, '--json').stdout)
        self.assertEqual(data['git']['branch'], 'world/W1')
        text = self.world('inspect', wid).stdout
        self.assertIn(b'world/W1', text)
        self.assertNotIn(b'heads/world/W1', text)
        self.git(one, 'symbolic-ref', 'HEAD', 'refs/tags/world/W1')
        data = json.loads(self.world('inspect', wid, '--json').stdout)
        self.assertEqual(data['git']['branch'], '')
        self.assertIn(b'(detached)', self.world('inspect', wid).stdout)

    def test_promisor_partial_clone_is_refused(self):
        self.git(self.source, 'config', 'uploadpack.allowFilter', 'true')
        (self.source / 'file').write_text('second\n')
        self.git(self.source, 'commit', '-am', 'second')
        partial = self.root / 'partial'
        self.git(self.root, 'clone', '--quiet', '--no-local', '--filter=blob:none',
                 self.source.as_uri(), str(partial))
        self.git(partial, 'config', 'user.name', 'World Test')
        self.git(partial, 'config', 'user.email', 'world@example.com')
        subprocess.run(['git', '-C', str(partial), 'config', '--unset', 'extensions.partialClone'],
                       env=self.env, capture_output=True)
        self.git(partial, 'config', '--get', 'extensions.partialClone', code=1)
        self.assertEqual(self.git(partial, 'status', '--porcelain').stdout, b'')
        missing = self.git(partial, 'rev-list', '--objects', '--missing=print', '--all').stdout
        self.assertIn(b'\n?', b'\n' + missing)
        promisor_packs = list((partial / '.git' / 'objects' / 'pack').glob('*.promisor'))
        self.assertTrue(promisor_packs)
        self.assertEqual(self.git(partial, 'config', '--get', 'remote.origin.promisor').stdout.strip(), b'true')
        self.assertEqual(self.git(partial, 'config', '--get', 'remote.origin.partialclonefilter').stdout.strip(),
                         b'blob:none')
        head = self.git(partial, 'rev-parse', 'HEAD').stdout.strip()

        def refused():
            result = self.world('init', str(partial), code=3)
            self.assertIn(b'unsupported Git layout', result.stderr)
            self.assertEqual(self.git(partial, 'rev-parse', 'HEAD').stdout.strip(), head)
            self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

        with self.subTest(marker='promisor-and-filter'):
            refused()
        self.git(partial, 'config', '--unset', 'remote.origin.partialclonefilter')
        with self.subTest(marker='promisor-only'):
            refused()
        self.git(partial, 'config', '--unset', 'remote.origin.promisor')
        self.git(partial, 'config', 'remote.origin.partialclonefilter', 'blob:none')
        with self.subTest(marker='filter-only'):
            refused()
        self.git(partial, 'config', '--unset', 'remote.origin.partialclonefilter')
        self.git(partial, 'config', '--get-regexp', '^remote\\..*\\.(promisor|partialclonefilter)$', code=1)
        with self.subTest(marker='promisor-pack-only'):
            refused()

    def test_many_generated_branch_collisions_still_fork(self):
        lines = ['create refs/heads/world/W1 ' + self.base.decode()]
        lines += ['create refs/heads/world/W1-%d %s' % (i, self.base.decode()) for i in range(1, 131)]
        subprocess.run(['git', '-C', str(self.source), 'update-ref', '--stdin'], env=self.env, check=True,
                       input=('\n'.join(lines) + '\n').encode())
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1-131')
        self.assertEqual(self.git(one, 'rev-parse', 'world/W1-130').stdout.strip(), self.base)

    def make_rerere_resolution(self):
        self.git(self.source, 'config', 'rerere.enabled', 'true')
        self.git(self.source, 'checkout', '-q', '-b', 'side')
        (self.source / 'file').write_text('side\n')
        self.git(self.source, 'commit', '-qam', 'side')
        self.git(self.source, 'checkout', '-q', 'main')
        (self.source / 'file').write_text('main\n')
        self.git(self.source, 'commit', '-qam', 'main')
        before = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(self.source, 'merge', 'side', code=1)
        (self.source / 'file').write_text('resolved\n')
        self.git(self.source, 'add', 'file')
        self.git(self.source, 'commit', '-qm', 'merge')
        self.git(self.source, 'reset', '-q', '--hard', before)
        self.git(self.source, 'config', '--unset', 'rerere.enabled')
        cache = self.source / '.git' / 'rr-cache'
        self.assertTrue(any(cache.rglob('postimage')))
        return cache

    def test_rerere_resolutions_survive_source_deletion(self):
        cache = self.make_rerere_resolution()
        expected = sorted((p.relative_to(cache).as_posix(), p.read_bytes()) for p in cache.rglob('*') if p.is_file())
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        path = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'rr-cache').stdout.decode().strip()
        owned = Path(path)
        self.assertEqual(sorted((p.relative_to(owned).as_posix(), p.read_bytes())
                                for p in owned.rglob('*') if p.is_file()), expected)
        merged = self.git(one, 'merge', 'side', code=1)
        self.assertIn(b'previous resolution', merged.stdout + merged.stderr)
        self.assertEqual((one / 'file').read_text(), 'resolved\n')

    def test_rerere_cache_change_during_mirror_aborts_publication(self):
        import shlex
        cache = self.make_rerere_resolution()
        postimage = next(cache.rglob('postimage'))
        real_git = shutil.which('git')
        wrapper = self.root / 'rerere-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = --mirror ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + 'printf "raced\\n" >> ' + shlex.quote(str(postimage)) + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.assertTrue(postimage.read_bytes().endswith(b'raced\n'))
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_unexpected_rerere_cache_layouts_are_refused(self):
        cache = self.source / '.git' / 'rr-cache'
        elsewhere = self.root / 'shared-rr-cache'
        elsewhere.mkdir()
        cache.symlink_to(elsewhere)
        with self.subTest(layout='symlinked cache'):
            result = self.world('init', str(self.source), code=3)
            self.assertIn(b'unsupported Git layout', result.stderr)
        cache.unlink()
        cache.mkdir()
        (cache / 'stray').write_text('not a conflict directory\n')
        with self.subTest(layout='file at top level'):
            self.world('init', str(self.source), code=3)
        (cache / 'stray').unlink()
        (cache / 'abc').mkdir()
        (cache / 'abc' / 'nested').mkdir()
        with self.subTest(layout='nested directory'):
            self.world('init', str(self.source), code=3)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        (cache / 'abc' / 'nested').rmdir()
        self.world('init', str(self.source))
        one, _ = self.fork()
        path = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'rr-cache').stdout.decode().strip()
        self.assertTrue((Path(path) / 'abc').is_dir())

    def test_branch_prefix_collision_is_not_overwritten(self):
        self.git(self.source, 'branch', 'world')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world-W1')
        self.assertEqual(self.git(one, 'rev-parse', 'world').stdout.strip(), self.base)

    def test_tracked_reserved_paths_are_refused(self):
        def refused(*args):
            result = self.world(*args, code=3)
            self.assertIn(b'unsupported Git layout', result.stderr)
        (self.source / '.world').write_text('tracked by the user\n')
        self.git(self.source, 'add', '-f', '.world')
        self.git(self.source, 'commit', '-qm', 'track .world')
        # A present `.world` makes init treat the directory as a World; the reachable case is
        # a tracked path whose file is gone from the worktree.
        (self.source / '.world').unlink()
        with self.subTest(case='.world in HEAD and index'):
            refused('init', str(self.source), '--include-changes')
        self.git(self.source, 'rm', '-q', '--cached', '.world')
        with self.subTest(case='.world in HEAD only'):
            refused('init', str(self.source), '--include-changes')
        self.git(self.source, 'commit', '-qm', 'untrack .world')
        (self.source / '.world-git').mkdir()
        (self.source / '.world-git' / 'note').write_text('staged only\n')
        self.git(self.source, 'add', '-f', '.world-git/note')
        shutil.rmtree(self.source / '.world-git')
        with self.subTest(case='.world-git in index only'):
            refused('init', str(self.source), '--include-changes')
        self.git(self.source, 'rm', '-q', '--cached', '.world-git/note')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        # Untracking is not enough while history still has .world; start again from the base.
        self.git(self.source, 'reset', '-q', '--hard', self.base.decode())
        (self.source / '.git' / 'ORIG_HEAD').unlink()
        (self.source / 'sub').mkdir()
        (self.source / 'sub' / '.world').write_text('not the root marker\n')
        self.git(self.source, 'add', '-f', 'sub/.world')
        self.git(self.source, 'commit', '-qm', 'nested .world is ordinary content')
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual((one / 'sub' / '.world').read_text(), 'not the root marker\n')
        before = json.loads(self.world('list', '--json').stdout)
        self.git(one, 'add', '-f', '.world')
        with self.subTest(case='World index picked up its own .world'):
            refused('fork', '--from', wid, '--to', str(self.root / 'two'), '--include-changes')
        self.assertFalse((self.root / 'two').exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout), before)

    def test_reserved_paths_in_preserved_history_are_refused(self):
        def refused(case):
            with self.subTest(case=case):
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'unsupported Git layout', result.stderr)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

        def commit_with(path, message):
            target = self.source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('historical\n')
            self.git(self.source, 'add', '-f', path)
            self.git(self.source, 'commit', '-qm', message)
            commit = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
            self.git(self.source, 'rm', '-rq', path.split('/')[0])
            self.git(self.source, 'commit', '-qm', 'remove ' + path)
            return commit

        # Parent of HEAD tracked .world; the current commit deleted it.
        commit_with('.world', 'add .world')
        refused('.world in an ancestor of HEAD')
        self.git(self.source, 'reset', '-q', '--hard', self.base.decode())

        # A side branch added .world-git content and was merged away with -s ours.
        self.git(self.source, 'checkout', '-q', '-b', 'side')
        commit_with('.world-git/x', 'add .world-git')
        self.git(self.source, 'checkout', '-q', 'main')
        self.git(self.source, 'merge', '-q', '-s', 'ours', '--no-edit', 'side')
        self.git(self.source, 'branch', '-q', '-D', 'side')
        refused('.world-git on a merged-away side branch')
        self.git(self.source, 'reset', '-q', '--hard', self.base.decode())
        self.git(self.source, 'reflog', 'expire', '--expire=now', '--all')

        # Only ORIG_HEAD still reaches a commit tracking .world.
        (self.source / '.world').write_text('historical\n')
        self.git(self.source, 'add', '-f', '.world')
        self.git(self.source, 'commit', '-qm', 'add .world')
        self.git(self.source, 'reset', '-q', '--hard', self.base.decode())
        self.assertFalse((self.source / '.world').exists())
        refused('.world reachable only from ORIG_HEAD')
        (self.source / '.git' / 'ORIG_HEAD').unlink()

        # Only FETCH_HEAD reaches a commit tracking .world.
        remote = self.root / 'fetch-remote'
        self.git(self.root, 'clone', '-q', str(self.source), str(remote))
        self.git(remote, 'config', 'user.name', 'Fetch Test')
        self.git(remote, 'config', 'user.email', 'fetch@example.com')
        (remote / '.world').write_text('historical\n')
        self.git(remote, 'add', '-f', '.world')
        self.git(remote, 'commit', '-qm', 'remote adds .world')
        self.git(self.source, 'fetch', '-q', str(remote), 'main')
        refused('.world reachable only from FETCH_HEAD')
        (self.source / '.git' / 'FETCH_HEAD').unlink()

        self.world('init', str(self.source))
        one, wid = self.fork()
        # A World whose own new history gains .world cannot be forked either.
        self.git(one, 'add', '-f', '.world')
        self.git(one, 'commit', '-qm', 'World commits its marker')
        self.git(one, 'rm', '-q', '--cached', '.world')
        self.git(one, 'commit', '-qm', 'World untracks its marker')
        before = json.loads(self.world('list', '--json').stdout)
        result = self.world('fork', '--from', wid, '--to', str(self.root / 'two'), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout), before)

    def test_replacement_cannot_hide_reserved_paths_in_history(self):
        # The real HEAD commit tracks .world; a replacement presents a tree without it.
        (self.source / '.world').write_text('hidden\n')
        self.git(self.source, 'add', '-f', '.world')
        self.git(self.source, 'commit', '-qm', 'real commit tracks .world')
        real = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode()
        (self.source / '.world').unlink()
        tree = self.git(self.source, 'rev-parse', self.base.decode() + '^{tree}').stdout.strip().decode()
        safe = self.git(self.source, 'commit-tree', tree, '-p', self.base.decode(), '-m', 'safe').stdout.strip().decode()
        self.git(self.source, 'replace', real, safe)
        self.git(self.source, 'reset', '-q', '--hard', 'HEAD')
        (self.source / '.git' / 'ORIG_HEAD').unlink(missing_ok=True)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip().decode(), real)
        self.assertEqual(self.git(self.source, 'ls-files', '.world').stdout, b'')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.assertFalse((self.source / '.world').exists())
        result = self.world('init', str(self.source), code=3)
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

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
