"""Git-aware worlds: real repositories, worktrees and complete workspace lifecycles."""
import json
import errno
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import shlex
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

    def install_lfs(self, repo):
        if not shutil.which('git-lfs'):
            self.skipTest('needs git-lfs')
        self.git(repo, 'lfs', 'install', '--local')
        self.git(repo, 'lfs', 'track', '*.bin')

    def lfs_object_path(self, repo, oid):
        common = self.git(repo, 'rev-parse', '--path-format=absolute', '--git-common-dir').stdout.decode().strip()
        return Path(common) / 'lfs' / 'objects' / oid[:2] / oid[2:4] / oid

    def git_hook_snapshot(self, repo):
        hooks = Path(self.git(repo, 'rev-parse', '--path-format=absolute', '--git-path', 'hooks').stdout.decode().strip())
        return {p.name: (p.stat().st_mode & 0o777, p.read_bytes()) for p in hooks.iterdir() if p.is_file()}

    @staticmethod
    def lfs_pointer(oid, size):
        return ('version https://git-lfs.github.com/spec/v1\n'
                'oid sha256:%s\nsize %d\n' % (oid, size)).encode()

    @staticmethod
    def legacy_lfs_prepush_hook():
        return (b'#!/bin/sh\n'
                b'command -v git-lfs >/dev/null 2>&1 || { echo >&2 "\\nThis repository is configured for Git LFS but \'git-lfs\' was not found on your path. If you no longer wish to use Git LFS, remove this hook by deleting the \'pre-push\' file in the hooks directory (set by \'core.hookspath\'; usually \'.git/hooks\').\\n"; exit 2; }\n'
                b'git lfs pre-push "$@"\n')

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
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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

    def test_external_object_clone_budgets_metadata_not_bytes(self):
        import ctypes
        import fcntl
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
        # Git never copies the object store: a `git clone` here fails the import.
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
        # Same-volume does not imply reflink support (for example, ext4 generally lacks it).
        # Probe the exact primitive on the store volume so the budget assertion covers both the
        # metadata-only CoW path and the full-byte copy fallback without assuming a filesystem.
        probe_src = self.store / 'clone-probe-src'
        probe_dst = self.store / 'clone-probe-dst'
        self.store.mkdir(parents=True, exist_ok=True)
        probe_src.write_bytes(b'probe')
        supports_share = False
        try:
            if sys.platform == 'darwin':
                clonefile = ctypes.CDLL(None, use_errno=True).clonefile
                clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
                clonefile.restype = ctypes.c_int
                supports_share = clonefile(os.fsencode(probe_src), os.fsencode(probe_dst), 0) == 0
            elif sys.platform.startswith('linux'):
                with probe_src.open('rb') as src, probe_dst.open('wb') as dst:
                    try:
                        fcntl.ioctl(dst.fileno(), 0x40049409, src.fileno())  # FICLONE
                        supports_share = True
                    except OSError:
                        pass
        finally:
            probe_src.unlink(missing_ok=True)
            probe_dst.unlink(missing_ok=True)

        def import_both(oversized_present=True):
            for source in (self.source, linked):
                with self.subTest(source=source.name):
                    self.world('init', str(source))
                    self.assertFalse(called.exists())
                    if oversized_present:
                        self.assertEqual(oversized.stat().st_size, size)
                    else:
                        self.assertFalse(oversized.exists())

        if supports_share:
            # A reflink-capable volume charges object-store metadata, not logical bytes.
            import_both()
            self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 2)
        else:
            # Without the filesystem's sharing primitive, budget the full copy. The sparse
            # fixture therefore refuses both imports before publication; removing it proves
            # the copy fallback itself remains usable and never invokes `git clone`.
            for source in (self.source, linked):
                with self.subTest(source=source.name, reflink=False):
                    refused = self.world('init', str(source), code=3)
                    self.assertIn(b'not enough free space', refused.stderr)
                    self.assertFalse(called.exists())
            self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
            oversized.unlink()
            import_both(oversized_present=False)
            self.assertFalse(called.exists())
            self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 2)
        one, wid = self.fork()
        self.assertFalse((one / '.world-git' / 'repo.git' / 'objects' / 'budget-fixture').exists())
        self.git(one, 'fsck', '--full')

    def mount_private_volume(self):
        """A private APFS disk image attached under self.root, detached again at cleanup: a
        second volume, which clonefile(2) cannot clone into the store's."""
        import time
        if sys.platform != 'darwin' or not shutil.which('hdiutil'):
            self.skipTest('needs hdiutil')
        image, mount = self.root / 'volume.dmg', self.root / 'volume'
        mount.mkdir()
        created = subprocess.run(['hdiutil', 'create', '-quiet', '-size', '64m', '-fs', 'APFS',
                                  '-volname', 'forkfs-objects', str(image)], capture_output=True, timeout=120)
        if created.returncode:
            self.skipTest('hdiutil create failed: %r' % created.stderr)
        attached = subprocess.run(['hdiutil', 'attach', '-plist', '-nobrowse', '-owners', 'on',
                                   '-mountpoint', str(mount), str(image)], capture_output=True, timeout=120)
        if attached.returncode:
            self.skipTest('hdiutil attach failed: %r' % attached.stderr)
        # The volume's device: detaching by it still works once the mount point is gone.
        device = next(e['dev-entry'] for e in plistlib.loads(attached.stdout)['system-entities']
                      if e.get('mount-point') == str(mount))

        def detach():
            # A freshly attached volume is busy for a while on CI runners (Spotlight and
            # fseventsd look at it, `-force` or not); the eject is retried for about a minute,
            # unmounting first once the plain and forced detaches have both failed.
            errors = []
            for attempt in range(10):
                if attempt == 4:
                    subprocess.run(['diskutil', 'unmount', 'force', str(mount)], capture_output=True, timeout=120)
                args = ['hdiutil', 'detach', '-quiet', device] + (['-force'] if attempt >= 2 else [])
                p = subprocess.run(args, capture_output=True, timeout=120)
                if p.returncode == 0:
                    return
                errors.append(p.stderr.decode(errors='replace').strip())
                time.sleep(min(2 ** attempt, 10))
            self.fail('could not detach %s (%s): %s' % (mount, device, ' / '.join(errors)))
        self.addCleanup(detach)
        self.assertNotEqual(mount.stat().st_dev, self.root.stat().st_dev)
        return mount

    def test_object_store_on_another_volume_is_copied_and_budgeted(self):
        volume = self.mount_private_volume()
        source = self.root / 'xvol-source'
        admin = volume / 'source.git'
        self.git(self.root, 'init', '-q', '-b', 'main', '--separate-git-dir', str(admin), str(source))
        self.identify(source)
        (source / 'file').write_text('packed\n')
        self.git(source, 'add', '.')
        self.git(source, 'commit', '-qm', 'packed')
        self.git(source, 'repack', '-adq')
        (source / 'file').write_text('loose\n')
        self.git(source, 'commit', '-qam', 'loose')
        self.world('list', '--json')
        # Where the objects cannot be cloned into the store's volume they are copied, and the
        # preflight budgets their full logical size.
        oversized = admin / 'objects' / 'budget-fixture'
        volume_stat = os.statvfs(self.store)
        size = volume_stat.f_bavail * volume_stat.f_frsize + 256 * 1024 * 1024
        with oversized.open('wb') as f:
            f.truncate(size)
        result = self.world('init', str(source), code=3)
        self.assertIn(b'not enough free space', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        oversized.unlink()
        self.world('init', str(source))
        shutil.rmtree(admin)
        one, _ = self.fork()
        self.git(one, 'fsck', '--full')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'log', '--format=%s').stdout, b'loose\npacked\n')

    def test_owned_objects_are_cloned_and_survive_source_deletion(self):
        self.git(self.source, 'tag', '-a', 'v1', '-m', 'v1')
        self.git(self.source, 'repack', '-adq')
        (self.source / 'file').write_text('loose commit\n')
        self.git(self.source, 'commit', '-qam', 'loose')
        (self.source / 'staged').write_text('only in the index\n')
        self.git(self.source, 'add', 'staged')
        loose = self.git(self.source, 'rev-parse', 'HEAD:file').stdout.decode().strip()
        objects = self.source / '.git' / 'objects'
        self.assertTrue((objects / loose[:2] / loose[2:]).is_file())
        packs = sorted(p.name for p in (objects / 'pack').iterdir())
        self.assertTrue(any(name.endswith('.pack') for name in packs))
        refs = self.git(self.source, 'for-each-ref', '--format=%(refname) %(objectname)').stdout
        # What Git leaves mid-write -- temporary objects and packs, a push's quarantine -- is not
        # part of the repository and is not carried into the owned one.
        junk = [objects / 'pack' / 'tmp_pack_raced', objects / loose[:2] / 'tmp_obj_raced',
                objects / 'incoming-raced' / 'pack' / 'pack-partial.pack', objects / 'info' / 'unknown']
        for path in junk:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'partial')
        self.world('init', str(self.source), '--include-changes')
        shutil.rmtree(self.source)
        one, _ = self.fork()
        owned = one / '.world-git' / 'repo.git' / 'objects'
        self.assertEqual(sorted(p.name for p in (owned / 'pack').iterdir()), packs)
        self.assertTrue((owned / loose[:2] / loose[2:]).is_file())
        for path in junk:
            self.assertFalse((owned / path.relative_to(objects)).exists(), path)
        self.assertFalse((owned / 'incoming-raced').exists())
        self.git(one, 'fsck', '--full')
        self.assertEqual(self.git(one, 'show', ':staged').stdout, b'only in the index\n')
        after = self.git(one, 'for-each-ref', '--format=%(refname) %(objectname)', '--exclude=refs/heads/world/*').stdout
        self.assertEqual(after, refs)

    def object_loss_wrapper(self, oid):
        """PATH directory whose git deletes loose object `oid` from an owned repository right
        after the import wrote its refs: a clone that raced a repack and missed an object."""
        import shlex
        real_git = shutil.which('git')
        wrapper = self.root / 'object-loss-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        loose = 'objects/' + oid[:2] + '/' + oid[2:]
        script.write_text('#!/bin/sh\nprev=\nrepo=\npack=0\nfor arg in "$@"; do\n'
                          '[ "$prev" = --git-dir ] && repo=$arg\n[ "$arg" = pack-refs ] && pack=1\nprev=$arg\ndone\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          'if [ "$result" = 0 ] && [ "$pack" = 1 ] && [ -f "$repo/' + loose + '" ]; then\n'
                          'rm -f "$repo/' + loose + '" || exit $?\nfi\nexit "$result"\n')
        script.chmod(0o700)
        return wrapper

    def test_owned_repository_missing_an_object_is_never_published(self):
        (self.source / 'file').write_text('loose only\n')
        self.git(self.source, 'commit', '-qam', 'loose')
        blob = self.git(self.source, 'rev-parse', 'HEAD:file').stdout.decode().strip()
        env = dict(self.env)
        self.env['PATH'] = str(self.object_loss_wrapper(blob)) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), code=1)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.env = env
        self.git(self.source, 'cat-file', '-e', blob)
        self.world('init', str(self.source))

    def test_concurrent_repack_is_retryable_or_complete(self):
        import shlex
        for n in range(3):
            (self.source / 'file').write_text('loose %d\n' % n)
            self.git(self.source, 'commit', '-qam', 'loose %d' % n)
        real_git = shutil.which('git')
        wrapper = self.root / 'repack-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        # Right after the owned repository is created and before its objects are cloned.
        script.write_text('#!/bin/sh\nbare=0\nfor arg in "$@"; do [ "$arg" = --bare ] && bare=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$bare" = 1 ]; then\n'
                          + shlex.quote(real_git) + ' -C ' + shlex.quote(str(self.source))
                          + ' repack -adq || exit $?\nfi\nexit "$result"\n')
        script.chmod(0o700)
        env = dict(self.env)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        raced = subprocess.run([WORLD, 'fs', 'init', str(self.source)], env=self.env, capture_output=True, timeout=60)
        self.env = env
        self.assertIn(raced.returncode, (0, 1), raced.stderr)
        snapshots = json.loads(self.world('list', '--json').stdout)['snapshots']
        if raced.returncode:
            self.assertEqual(snapshots, [])
            snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        else:
            snapshot = raced.stdout.split()[0].decode()
        shutil.rmtree(self.source)
        one, _ = self.fork('one', snapshot)
        self.git(one, 'fsck', '--full')
        self.assertEqual(self.git(one, 'log', '-1', '--format=%s').stdout, b'loose 2\n')

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

    def tree_bytes(self, root):
        """Every file and symlink under root, with its bytes: the whole tree, .git included."""
        out = {}
        for base, dirs, files in os.walk(root):
            for name in files + [d for d in dirs if (Path(base) / d).is_symlink()]:
                path = Path(base) / name
                rel = str(path.relative_to(root))
                out[rel] = os.readlink(path) if path.is_symlink() else path.read_bytes()
            for name in dirs:
                out[str((Path(base) / name).relative_to(root)) + '/'] = b''
        return out

    def make_dirty(self, root):
        """A staged change, a staged addition, an unstaged change, a deleted tracked file, new
        untracked files (one in a new directory) and ignored artifacts."""
        (root / 'file').write_text('staged\n')
        (root / 'new-staged').write_text('only in index\n')
        self.git(root, 'add', 'file', 'new-staged')
        (root / 'sub' / 'tracked').write_text('unstaged\n')
        (root / 'gone').unlink()
        (root / 'untracked').write_text('untracked\n')
        (root / 'scratch').mkdir()
        (root / 'scratch' / 'note').write_text('untracked in a new directory\n')
        (root / 'scratch' / 'trace.log').write_text('ignored in an untracked directory\n')
        (root / 'build').mkdir(exist_ok=True)
        (root / 'build' / 'model.bin').write_bytes(b'ignored artifact')

    def assert_committed(self, world, head):
        self.assertEqual(self.git(world, 'status', '--porcelain', '--untracked-files=all').stdout, b'')
        self.assertEqual(self.git(world, 'rev-parse', 'HEAD').stdout.strip(), head)
        self.assertEqual(self.git(world, 'diff', '--cached', '--name-only', 'HEAD').stdout, b'')
        self.assertEqual((world / 'file').read_text(), 'original\n')
        self.assertEqual((world / 'sub' / 'tracked').read_text(), 'tracked\n')
        self.assertEqual((world / 'gone').read_text(), 'deleted in the source\n')
        for absent in ('new-staged', 'untracked', 'scratch/note'):
            self.assertFalse((world / absent).exists(), absent)
        self.assertEqual((world / 'build' / 'model.bin').read_bytes(), b'ignored artifact')
        self.assertEqual((world / 'scratch' / 'trace.log').read_text(), 'ignored in an untracked directory\n')

    def committed_only_fixture(self):
        (self.source / '.gitignore').write_text('build/\n*.log\n')
        (self.source / 'sub').mkdir()
        (self.source / 'sub' / 'tracked').write_text('tracked\n')
        (self.source / 'gone').write_text('deleted in the source\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'more files')
        return self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()

    def test_committed_only_creates_from_head_and_leaves_the_source_alone(self):
        head = self.committed_only_fixture()
        self.make_dirty(self.source)
        # A passive squash message belongs to the staged content, which is not carried.
        (self.source / '.git' / 'SQUASH_MSG').write_text('squashed work\n')
        before = self.tree_bytes(self.source)
        self.world('init', str(self.source), code=3)
        both = self.world('init', str(self.source), '--committed-only', '--include-changes', code=2)
        self.assertIn(b'mutually exclusive', both.stderr)
        self.world('init', str(self.source), '--committed-only')
        self.assertEqual(self.tree_bytes(self.source), before)
        one, _ = self.fork()
        self.assert_committed(one, head)
        message = self.git(one, 'rev-parse', '--path-format=absolute', '--git-path', 'SQUASH_MSG').stdout.decode().strip()
        self.assertFalse(Path(message).exists())
        self.world('verify', 'S1')
        # The source keeps every staged, unstaged, deleted and untracked change it had.
        self.assertEqual(self.tree_bytes(self.source), before)
        self.assertEqual(self.git(self.source, 'show', ':file').stdout, b'staged\n')
        self.assertEqual((self.source / 'sub' / 'tracked').read_text(), 'unstaged\n')
        self.assertFalse((self.source / 'gone').exists())
        # A World commit on top of HEAD contains none of the source's uncommitted work.
        (one / 'file').write_text('world change\n')
        self.git(one, 'commit', '-qam', 'world')
        self.assertEqual(self.git(one, 'show', '--name-only', '--format=', 'HEAD').stdout, b'file\n')
        # Without a repository there is no committed version to start from.
        plain = self.root / 'plain'
        plain.mkdir()
        (plain / 'data').write_text('data\n')
        refused = self.world('init', str(plain), '--committed-only', code=3)
        self.assertIn(b'reason: --committed-only needs a Git repository', refused.stderr)

    def test_committed_only_resets_index_only_marked_files(self):
        (self.source / 'config.local').write_text('committed\n')
        self.git(self.source, 'add', 'config.local')
        self.git(self.source, 'commit', '-qm', 'local config')
        self.git(self.source, 'update-index', '--skip-worktree', 'config.local')
        self.git(self.source, 'update-index', '--assume-unchanged', 'file')
        (self.source / 'config.local').write_text('private edit\n')
        (self.source / 'file').write_text('hidden edit\n')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        index = (self.source / '.git' / 'index').read_bytes()
        snapshot = self.world('init', str(self.source), '--committed-only').stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual((one / 'config.local').read_text(), 'committed\n')
        self.assertEqual((one / 'file').read_text(), 'original\n')
        self.assertEqual(self.git(one, 'ls-files', '-v').stdout, b'H .gitignore\nH config.local\nH file\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual((self.source / 'config.local').read_text(), 'private edit\n')
        self.assertEqual((self.source / '.git' / 'index').read_bytes(), index)

    def test_committed_only_checks_head_filters_before_resetting_the_copy(self):
        # HEAD assigns a filter; the dirty source removes the assignment, so only the reset to
        # HEAD would check the file out through it.
        marker = self.root / 'head-filter-ran'
        (self.source / '.gitattributes').write_text('*.bin filter=example\n')
        (self.source / 'model.bin').write_text('weights\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'model')
        global_config = self.root / 'filter-global'
        global_config.write_text('[filter "example"]\n smudge = touch ' + str(marker) + '; cat\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        (self.source / '.gitattributes').write_text('')
        (self.source / 'model.bin').write_text('edited weights\n')
        result = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b"reason: tracked file model.bin uses the 'example' filter", result.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_committed_only_snapshot_records_only_hardlinks_it_still_has(self):
        for name in ('a', 'b', 'c', 'd'):
            (self.source / name).write_text('shared\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'twins')
        # Same content, so Git sees no change: a and b become one inode, as do c and d.
        for first, second in (('a', 'b'), ('c', 'd')):
            (self.source / second).unlink()
            os.link(self.source / first, self.source / second)
        # Writing through a changes b as well; the reset puts both back as separate files.
        with open(self.source / 'a', 'w') as f:
            f.write('changed through a hardlink\n')
        self.world('init', str(self.source), '--committed-only')
        self.world('verify', 'S1')
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        for name in ('a', 'b', 'c', 'd'):
            self.assertEqual((one / name).read_text(), 'shared\n')
        # The untouched pair is still one inode in every fork.
        self.assertEqual((one / 'c').stat().st_ino, (one / 'd').stat().st_ino)
        self.assertEqual((self.source / 'b').read_text(), 'changed through a hardlink\n')

    def test_committed_only_fork_and_checkpoint_of_a_dirty_world(self):
        head = self.committed_only_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.make_dirty(one)
        status = self.git(one, 'status', '--porcelain').stdout
        self.assertNotEqual(status, b'')
        staged = self.git(one, 'ls-files', '--stage').stdout
        self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
        self.world('fork', '--from', wid, '--to', str(self.root / 'both'), '--committed-only',
                   '--include-changes', code=2)
        two, _ = self.fork('two', wid, '--committed-only')
        self.assert_committed(two, head)
        self.assertEqual(self.git(two, 'branch', '--show-current').stdout.strip(), b'world/W2')
        self.world('checkpoint', wid, code=3)
        self.world('checkpoint', wid, '--committed-only')
        three, _ = self.fork('three', 'S2')
        self.assert_committed(three, head)
        self.world('verify', 'S2')
        # The World the copies came from keeps its uncommitted work, index included.
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, status)
        self.assertEqual(self.git(one, 'ls-files', '--stage').stdout, staged)
        self.assertEqual((one / 'untracked').read_text(), 'untracked\n')
        # A snapshot's content is already fixed.
        snap = self.world('fork', '--from', 'S1', '--to', str(self.root / 'snap'), '--committed-only', code=2)
        self.assertIn(b'--committed-only needs --from W<n>', snap.stderr)
        self.assertFalse((self.root / 'snap').exists())

    def write_hook(self, path, marker, name=None, mode=0o755):
        import shlex
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#!/bin/sh\necho "%s $(pwd -P)" >> %s\n' % (name or path.name, shlex.quote(str(marker))))
        path.chmod(mode)

    def hook_runs(self, marker):
        return marker.read_text().splitlines() if marker.exists() else []

    def commit_in(self, world, message):
        (world / 'file').write_text(message + '\n')
        self.git(world, 'commit', '-qam', message)

    def test_hooks_are_left_behind_with_a_note(self):
        marker = self.root / 'hooks-ran'
        # The template's .sample files are not hooks: no note.
        plain = self.world('init', str(self.source))
        self.assertNotIn(b'--with-hooks', plain.stderr)
        self.write_hook(self.source / '.git' / 'hooks' / 'pre-commit', marker)
        noted = self.world('init', str(self.source))
        notes = [line for line in noted.stderr.splitlines() if b'--with-hooks' in line]
        self.assertEqual(len(notes), 1, noted.stderr)
        self.assertIn(b'note:', notes[0])
        one, _ = self.fork('one', 'S2')
        self.assertFalse((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-commit').exists())
        self.commit_in(one, 'no hook')
        self.assertEqual(self.hook_runs(marker), [])
        # A repository-local core.hooksPath (husky) is noted too.
        (self.source / '.git' / 'hooks' / 'pre-commit').unlink()
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        self.assertIn(b'--with-hooks', self.world('init', str(self.source)).stderr)
        two, _ = self.fork('two', 'S3')
        self.git(two, 'config', '--get', 'core.hooksPath', code=1)

    def test_with_hooks_carries_project_hooks_without_running_them(self):
        marker = self.root / 'hooks-ran'
        hooks = self.source / '.git' / 'hooks'
        for name in ('pre-commit', 'post-checkout', 'reference-transaction', 'post-index-change'):
            self.write_hook(hooks / name, marker)
        self.write_hook(hooks / 'prepare-commit-msg', marker, mode=0o644)   # Git would not run it
        init = self.world('init', str(self.source), '--with-hooks')
        self.assertNotIn(b'--with-hooks', init.stderr)
        one, wid = self.fork()
        carried = one / '.world-git' / 'repo.git' / 'hooks'
        self.assertEqual(sorted(p.name for p in carried.iterdir()),
                         ['post-checkout', 'post-index-change', 'pre-commit', 'reference-transaction'])
        self.assertEqual((carried / 'pre-commit').read_bytes(), (hooks / 'pre-commit').read_bytes())
        self.assertTrue(os.access(carried / 'pre-commit', os.X_OK))
        # Neither the import nor the fork ran a hook, though both write refs and indexes.
        self.assertEqual(self.hook_runs(marker), [])
        self.commit_in(one, 'first')
        self.assertIn('pre-commit ' + str(one), self.hook_runs(marker))
        # Worlds forked or checkpointed from a World keep its hooks, and still run none.
        before = self.hook_runs(marker)
        two, _ = self.fork('two', wid)
        self.world('checkpoint', wid)
        three, _ = self.fork('three', 'S2')
        self.assertEqual(self.hook_runs(marker), before)
        for world in (two, three):
            self.commit_in(world, world.name)
            self.assertIn('pre-commit ' + str(world), self.hook_runs(marker))
        # --with-hooks needs a repository.
        plain = self.root / 'plain'
        plain.mkdir()
        refused = self.world('init', str(plain), '--with-hooks', code=3)
        self.assertIn(b'reason: --with-hooks needs a Git repository', refused.stderr)

    def test_with_hooks_carries_a_relative_hooks_path(self):
        marker = self.root / 'hooks-ran'
        self.write_hook(self.source / '.husky' / 'pre-commit', marker, 'husky')
        self.git(self.source, 'add', '.husky')
        self.git(self.source, 'commit', '-qm', 'husky')
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        self.world('init', str(self.source), '--with-hooks')
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'core.hooksPath').stdout.strip(), b'.husky')
        self.assertEqual(self.hook_runs(marker), [])
        self.commit_in(one, 'husky runs')
        self.assertEqual(self.hook_runs(marker), ['husky ' + str(one)])

    def test_lfs_with_in_tree_hooks_path_requires_existing_canonical_prepush(self):
        self.install_lfs(self.source)
        (self.source / 'file.bin').write_bytes(b'in-tree hook LFS fixture\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        husky = self.source / '.husky'
        husky.mkdir()
        (husky / 'pre-commit').write_text('#!/bin/sh\nexit 0\n')
        (husky / 'pre-commit').chmod(0o755)
        self.git(self.source, 'add', '.husky')
        self.git(self.source, 'commit', '-qm', 'LFS and husky')
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        refused = self.world('init', str(self.source), '--committed-only', '--with-hooks', code=3)
        self.assertIn(b'core.hooksPath has no canonical executable Git LFS pre-push hook', refused.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

        # Keep the fixture-building Git/LFS commands from installing or adjusting hooks while
        # the explicit in-tree hooksPath points into the source worktree.
        self.git(self.source, 'config', 'core.hooksPath', '/dev/null')
        # Git LFS may install its standard checkout/commit/merge hooks when its filter process
        # runs. Carry the complete generated set so an ordinary Git status in the World stays
        # clean after LFS performs that documented hook installation.
        for name in ('pre-push', 'post-checkout', 'post-commit', 'post-merge'):
            canonical = self.source / '.git' / 'hooks' / name
            self.assertTrue(canonical.is_file(), name)
            dest = husky / name
            dest.write_bytes(canonical.read_bytes())
            dest.chmod(0o755)
        legacy = self.legacy_lfs_prepush_hook()
        (husky / 'pre-push').write_bytes(legacy)
        (husky / 'pre-push').chmod(0o755)
        self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'add', '.husky')
        self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'canonical LFS pre-push')
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        self.assertEqual(self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'status', '--porcelain',
                                  '--', '.husky').stdout, b'')
        self.world('init', str(self.source), '--committed-only', '--with-hooks')
        self.assertEqual((self.source / '.husky' / 'pre-push').read_bytes(), legacy)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'core.hooksPath').stdout.strip(), b'.husky')
        self.assertTrue(os.access(one / '.husky' / 'pre-push', os.X_OK))
        self.assertEqual((one / '.husky' / 'pre-push').read_bytes(), legacy)
        # A normal Git LFS filter-process can upgrade this legacy hook as a
        # side effect. Keep the cleanliness check from mutating either copy.
        self.assertEqual(self.git(one, '-c', 'core.hooksPath=/dev/null', 'status', '--porcelain').stdout, b'')
        self.assertEqual((self.source / '.husky' / 'pre-push').read_bytes(), legacy)
        self.assertEqual((one / '.husky' / 'pre-push').read_bytes(), legacy)

    def test_with_hooks_preserves_legacy_lfs_prepush_in_git_admin(self):
        self.install_lfs(self.source)
        (self.source / 'file.bin').write_bytes(b'legacy admin hook fixture\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'legacy admin hook LFS fixture')
        legacy = self.legacy_lfs_prepush_hook()
        source_hook = self.source / '.git' / 'hooks' / 'pre-push'
        source_hook.write_bytes(legacy)
        source_hook.chmod(0o755)

        snapshot = self.world('init', str(self.source), '--with-hooks').stdout.split()[0].decode()
        one, _ = self.fork('legacy-admin-hook-world', snapshot)
        owned_hook = one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push'
        self.assertTrue(os.access(owned_hook, os.X_OK))
        self.assertEqual(owned_hook.read_bytes(), legacy)
        self.assertEqual(source_hook.read_bytes(), legacy)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_with_hooks_resolves_escaping_relative_hooks_path_from_the_source(self):
        marker = self.root / 'hooks-ran'
        shared = self.root / 'shared-hooks'
        self.write_hook(shared / 'pre-commit', marker, 'shared')
        # A decoy beside where the World will live must never run.
        self.write_hook(self.root / 'worlds' / 'shared-hooks' / 'pre-commit', marker, 'decoy')
        self.git(self.source, 'config', 'core.hooksPath', '../shared-hooks')
        self.world('init', str(self.source), '--with-hooks')
        (self.root / 'worlds').mkdir(exist_ok=True)
        one, _ = self.fork(str(Path('worlds') / 'one'))
        self.assertEqual(self.git(one, 'config', '--get', 'core.hooksPath').stdout.strip(), str(shared).encode())
        self.commit_in(one, 'shared hook runs')
        self.assertEqual(self.hook_runs(marker), ['shared ' + str(one)])

    def test_with_hooks_refuses_symlinks_under_an_in_tree_hooks_path(self):
        outside = self.root / 'outside-hooks'
        outside.mkdir()
        (outside / 'pre-commit').write_text('#!/bin/sh\nexit 0\n')
        (outside / 'pre-commit').chmod(0o755)
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        with self.subTest(case='symlinked hooks directory'):
            (self.source / '.husky').symlink_to(outside)
            result = self.world('init', str(self.source), '--with-hooks', '--include-changes', code=3)
            self.assertIn(b'core.hooksPath .husky goes through a symlink', result.stderr)
            (self.source / '.husky').unlink()
        with self.subTest(case='symlinked hook'):
            (self.source / '.husky').mkdir()
            (self.source / '.husky' / 'pre-commit').symlink_to(outside / 'pre-commit')
            result = self.world('init', str(self.source), '--with-hooks', '--include-changes', code=3)
            self.assertIn(b'hook .husky/pre-commit is a symlink', result.stderr)
            (self.source / '.husky' / 'pre-commit').unlink()
        with self.subTest(case='symlink nested below the hooks directory'):
            (self.source / '.husky' / 'lib').mkdir()
            (self.source / '.husky' / 'lib' / 'helper').symlink_to(outside / 'pre-commit')
            result = self.world('init', str(self.source), '--with-hooks', '--include-changes', code=3)
            self.assertIn(b'hook .husky/lib/helper is a symlink', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_committed_only_with_hooks_needs_a_committed_hooks_path(self):
        marker = self.root / 'hooks-ran'
        self.write_hook(self.source / '.husky' / 'pre-commit', marker, 'husky')
        self.git(self.source, 'config', 'core.hooksPath', '.husky')
        for staged in (False, True):
            with self.subTest(staged=staged):
                if staged:
                    self.git(self.source, 'add', '.husky')
                result = self.world('init', str(self.source), '--committed-only', '--with-hooks', code=3)
                self.assertIn(b'core.hooksPath .husky is not committed', result.stderr)
        self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'husky')
        # A committed directory with an uncommitted hook inside is refused too.
        self.write_hook(self.source / '.husky' / 'commit-msg', marker, 'late')
        result = self.world('init', str(self.source), '--committed-only', '--with-hooks', code=3)
        self.assertIn(b'core.hooksPath .husky has uncommitted changes', result.stderr)
        (self.source / '.husky' / 'commit-msg').unlink()
        # So is an edit that an index-only mark hides from status.
        committed = (self.source / '.husky' / 'pre-commit').read_bytes()
        for mark in ('--skip-worktree', '--assume-unchanged'):
            with self.subTest(mark=mark):
                self.git(self.source, 'update-index', mark, '.husky/pre-commit')
                (self.source / '.husky' / 'pre-commit').write_bytes(committed + b'# local edit\n')
                self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
                result = self.world('init', str(self.source), '--committed-only', '--with-hooks', code=3)
                self.assertIn(b'hook .husky/pre-commit is marked skip-worktree or assume-unchanged', result.stderr)
                self.git(self.source, 'update-index', '--no' + mark[1:], '.husky/pre-commit')
                (self.source / '.husky' / 'pre-commit').write_bytes(committed)
        snapshot = self.world('init', str(self.source), '--committed-only', '--with-hooks').stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.commit_in(one, 'husky runs')
        self.assertEqual(self.hook_runs(marker), ['husky ' + str(one)])

    def test_committed_only_needs_a_committed_hooks_path_in_a_managed_world(self):
        # A managed World carries its hooks without --with-hooks, so a hooks path configured
        # in it after creation (husky installed later) must be committed before the reset.
        marker = self.root / 'hooks-ran'
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.write_hook(one / '.husky' / 'pre-commit', marker, 'husky')
        self.git(one, 'config', 'core.hooksPath', '.husky')
        result = self.world('fork', '--from', wid, '--to', str(self.root / 'two'), '--committed-only', code=3)
        self.assertIn(b'core.hooksPath .husky is not committed', result.stderr)
        self.assertFalse((self.root / 'two').exists())
        result = self.world('checkpoint', wid, '--committed-only', code=3)
        self.assertIn(b'core.hooksPath .husky is not committed', result.stderr)
        self.git(one, 'add', '.husky')
        self.git(one, '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'husky')
        two, _ = self.fork('two', wid, '--committed-only')
        self.commit_in(two, 'husky runs')
        self.assertEqual(self.hook_runs(marker), ['husky ' + str(two)])

    def test_with_hooks_refuses_dot_dot_after_a_directory_in_the_hooks_path(self):
        # Git takes `link/..` through the symlink, which a lexical collapse would not.
        marker = self.root / 'hooks-ran'
        outside = self.root / 'outside' / 'dir'
        outside.mkdir(parents=True)
        self.write_hook(self.root / 'outside' / 'hooks' / 'pre-commit', marker, 'outside')
        self.write_hook(self.source / 'hooks' / 'pre-commit', marker, 'in-tree')
        (self.source / 'link').symlink_to(outside)
        self.git(self.source, 'config', 'core.hooksPath', 'link/../hooks')
        result = self.world('init', str(self.source), '--with-hooks', '--include-changes', code=3)
        self.assertIn(b"core.hooksPath link/../hooks has a '..' after a directory name", result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.assertEqual(self.hook_runs(marker), [])

    def test_with_hooks_refuses_a_hooks_path_inside_git_administration(self):
        marker = self.root / 'hooks-ran'
        self.write_hook(self.source / '.git' / 'custom-hooks' / 'pre-commit', marker, 'custom')
        self.git(self.source, 'config', 'core.hooksPath', '.git/custom-hooks')
        result = self.world('init', str(self.source), '--with-hooks', code=3)
        self.assertIn(b'core.hooksPath .git/custom-hooks is inside .git, which the import replaces', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_committed_only_checks_root_level_hooks(self):
        marker = self.root / 'hooks-ran'
        self.git(self.source, 'config', 'core.hooksPath', '.')
        (self.source / 'notes.txt').write_text('an ordinary untracked file is not a hook\n')
        self.write_hook(self.source / 'pre-commit', marker, 'root')
        result = self.world('init', str(self.source), '--committed-only', '--with-hooks', code=3)
        self.assertIn(b'core.hooksPath . has uncommitted changes', result.stderr)
        self.git(self.source, 'add', 'pre-commit')
        self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'root hook')
        snapshot = self.world('init', str(self.source), '--committed-only', '--with-hooks').stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertFalse((one / 'notes.txt').exists())
        # (Git itself cannot execute hooks from core.hooksPath=. -- it looks the bare hook name
        # up on PATH -- so the committed hook is checked as content, not by running it.)
        self.assertEqual((one / 'pre-commit').read_bytes(), (self.source / 'pre-commit').read_bytes())
        self.assertTrue(os.access(one / 'pre-commit', os.X_OK))

    def test_with_hooks_refuses_symlinked_hooks(self):
        marker = self.root / 'hooks-ran'
        target = self.root / 'elsewhere' / 'pre-commit'
        self.write_hook(target, marker)
        hooks = self.source / '.git' / 'hooks'
        (hooks / 'pre-commit').symlink_to(target)
        refused = self.world('init', str(self.source), '--with-hooks', code=3)
        self.assertIn(b'reason: hook pre-commit is a symlink', refused.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        # Left behind by default, with the note.
        self.assertIn(b'--with-hooks', self.world('init', str(self.source)).stderr)
        shutil.rmtree(hooks)
        hooks.symlink_to(target.parent)
        refused = self.world('init', str(self.source), '--with-hooks', code=3)
        self.assertIn(b'is a symlink or not a directory', refused.stderr)
        self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 1)
        self.assertEqual(self.hook_runs(marker), [])

    def test_hook_change_during_mirror_aborts_publication(self):
        import shlex
        marker = self.root / 'hooks-ran'
        hook = self.source / '.git' / 'hooks' / 'pre-commit'
        self.write_hook(hook, marker)
        real_git = shutil.which('git')
        wrapper = self.root / 'hook-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$mirror" = 1 ]; then\n'
                          + 'printf "exit 1\\n" >> ' + shlex.quote(str(hook)) + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), '--with-hooks', code=1)
        self.assertTrue(hook.read_text().endswith('exit 1\n'))
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

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
        (self.source / 'sub/.git/objects/info/alternates').write_text('/elsewhere/objects\n')
        result = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: nested Git repository at ' + str(self.source / 'sub').encode()
                      + b': it borrows objects from another repository', result.stderr)

    def test_unused_ambient_filters_are_allowed_without_execution(self):
        # A machine-wide `git lfs install` defines this full filter for every repository. A
        # deliberately incomplete ambient lfs filter is also harmless when no tracked file uses it.
        global_config = self.root / 'filter-global'
        global_config.write_text('[filter "lfs"]\n smudge = touch ambient-filter-ran; cat\n required = true\n'
                                 '[filter "example"]\n clean = touch ambient-filter-ran; cat\n'
                                 ' smudge = touch ambient-filter-ran; cat\n required = true\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        (self.source / 'changed').write_text('untracked\n')
        config_before = (self.source / '.git/config').read_bytes()
        index_before = (self.source / '.git/index').read_bytes()
        self.world('init', str(self.source), '--include-changes')
        self.assertEqual((self.source / '.git/config').read_bytes(), config_before)
        self.assertEqual((self.source / '.git/index').read_bytes(), index_before)
        one, _ = self.fork()
        self.git(one, 'status', '--porcelain')
        self.assertFalse((self.source / 'ambient-filter-ran').exists())
        self.assertFalse((one / 'ambient-filter-ran').exists())

    def test_dormant_carried_lfs_settings_are_location_safe_without_activation(self):
        def make_repo(name):
            repo = self.root / name
            repo.mkdir()
            self.git(repo, 'init', '-q', '-b', 'main')
            self.identify(repo)
            (repo / 'file').write_text('ordinary repository content\n')
            self.git(repo, 'add', 'file')
            self.git(repo, 'commit', '-qm', 'ordinary repository')
            return repo

        safe = make_repo('dormant-lfs-safe')
        self.git(safe, 'config', 'lfs.url', 'https://lfs.example.test/objects')
        safe_snapshot = self.world('init', str(safe)).stdout.split()[0].decode()
        one, wid = self.fork('dormant-lfs-safe-world', safe_snapshot)
        self.assertEqual(self.git(one, 'config', '--local', '--get', 'lfs.url').stdout.strip(),
                         b'https://lfs.example.test/objects')
        self.git(one, 'config', '--local', '--get', 'filter.lfs.process', code=1)
        self.assertFalse((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push').exists())
        checkpoint = self.world('checkpoint', wid)
        managed, managed_wid = self.fork('dormant-lfs-safe-managed', checkpoint.stdout.split()[0].decode())
        self.assertEqual(self.git(managed, 'config', '--local', '--get', 'lfs.url').stdout.strip(),
                         b'https://lfs.example.test/objects')
        snapshots_before = json.loads(self.world('list', '--json').stdout)['snapshots']
        self.git(managed, 'config', '--local', 'lfs.url', '../relative-lfs')
        refused = self.world('checkpoint', managed_wid, code=3)
        self.assertIn(b'must be an absolute URL', refused.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], snapshots_before)

        cases = (
            ('relative-endpoint', 'lfs.url', '../relative-lfs', b'must be an absolute URL'),
            ('relative-remote-endpoint', 'remote.origin.lfsurl', '../relative-lfs', b'must be an absolute URL'),
            ('valueless-endpoint', 'lfs.url', None, b'must be an absolute URL'),
            ('custom-storage', 'lfs.storage', '../shared-lfs', b'custom lfs.storage is not supported'),
            ('extension', 'lfs.extension.test.clean', 'git-lfs clean -- %f', b'Git LFS extensions are not supported'),
            ('custom-transfer', 'lfs.customtransfer.test.path', '/tmp/lfs-transfer', b'custom Git LFS transfer agents are not supported'),
            ('standalone-transfer', 'lfs.standalonetransferagent', 'custom-agent', b'stand-alone transfer agents are not supported'),
        )
        for name, key, value, reason in cases:
            with self.subTest(setting=key):
                repo = make_repo('dormant-lfs-' + name)
                if value is None:
                    with (repo / '.git' / 'config').open('a') as config:
                        config.write('\n[lfs]\n\turl\n')
                else:
                    self.git(repo, 'config', key, value)
                config_before = (repo / '.git' / 'config').read_bytes()
                index_before = (repo / '.git' / 'index').read_bytes()
                refused = self.world('init', str(repo), code=3)
                self.assertIn(reason, refused.stderr)
                self.assertEqual((repo / '.git' / 'config').read_bytes(), config_before)
                self.assertEqual((repo / '.git' / 'index').read_bytes(), index_before)

        for view in ('worktree', 'index', 'HEAD'):
            with self.subTest(lfsconfig_view=view):
                repo = make_repo('dormant-lfsconfig-' + view)
                (repo / '.lfsconfig').write_text('[lfs]\n url = ../relative-lfs\n')
                if view in ('index', 'HEAD'):
                    self.git(repo, 'add', '.lfsconfig')
                if view == 'HEAD':
                    self.git(repo, 'commit', '-qm', 'relative endpoint in HEAD')
                    (repo / '.lfsconfig').write_text('[lfs]\n url = https://lfs.example.test/objects\n')
                    self.git(repo, 'add', '.lfsconfig')
                elif view == 'index':
                    (repo / '.lfsconfig').write_text('[lfs]\n url = https://lfs.example.test/objects\n')
                before = (repo / '.git' / 'index').read_bytes()
                refused = self.world('init', str(repo), code=3)
                self.assertIn(b'must be an absolute URL', refused.stderr)
                self.assertEqual((repo / '.git' / 'index').read_bytes(), before)

        # Worktree-scoped settings remain outside the admitted carry set, even when endpoint
        # syntax is unsafe; do not broaden that separate policy as part of dormant validation.
        worktree = make_repo('dormant-lfs-worktree')
        self.git(worktree, 'config', 'extensions.worktreeConfig', 'true')
        self.git(worktree, 'config', '--worktree', 'lfs.url', '../relative-lfs')
        refused = self.world('init', str(worktree), code=3)
        self.assertIn(b'worktree-scoped setting lfs.url', refused.stderr)

    def test_dormant_ambient_lfs_config_is_not_carried_or_activated(self):
        global_config = self.root / 'dormant-global-lfs'
        global_config.write_text('[lfs]\n url = ../relative-lfs\n'
                                 '[lfs "extension.unused"]\n clean = touch unused-lfs-extension-ran\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, _ = self.fork('dormant-ambient-lfs-world', snapshot)
        self.git(one, 'config', '--local', '--get', 'lfs.url', code=1)
        self.git(one, 'config', '--local', '--get', 'filter.lfs.process', code=1)
        self.assertFalse((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push').exists())
        self.assertFalse((self.source / 'unused-lfs-extension-ran').exists())
        self.assertFalse((one / 'unused-lfs-extension-ran').exists())

    def test_committed_only_checks_dormant_submodule_target_lfsconfig(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        old = self.git(lib, 'rev-parse', 'HEAD').stdout.strip()
        (lib / '.lfsconfig').write_text('[lfs]\n url = ../relative-lfs\n')
        self.git(lib, 'add', '.lfsconfig')
        self.git(lib, 'commit', '-qm', 'dormant relative LFS endpoint')
        self.git(self.source, 'add', 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'record dormant LFS target')
        self.git(lib, 'checkout', '-q', old.decode())

        refused = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b'must be an absolute URL', refused.stderr)
        self.assertEqual(self.git(lib, 'rev-parse', 'HEAD').stdout.strip(), old)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_local_canonical_lfs_setup_is_preserved_for_another_branch(self):
        import hashlib
        if not shutil.which('git-lfs'):
            self.skipTest('needs git-lfs')
        payload = b'LFS content reachable only from another branch\n'
        oid = hashlib.sha256(payload).hexdigest()
        self.git(self.source, 'checkout', '-q', '-b', 'lfs-history')
        (self.source / '.gitattributes').write_text('*.bin filter=lfs\n')
        (self.source / 'history.bin').write_bytes(self.lfs_pointer(oid, len(payload)))
        self.git(self.source, 'add', '.gitattributes', 'history.bin')
        self.git(self.source, 'commit', '-qm', 'LFS pointer on preserved branch')
        self.git(self.source, 'checkout', '-q', 'main')
        for key, value in (
            ('filter.lfs.clean', 'git-lfs clean -- %f'),
            ('filter.lfs.smudge', 'git-lfs smudge -- %f'),
            ('filter.lfs.process', 'git-lfs filter-process'),
            ('filter.lfs.required', '1'),
        ):
            self.git(self.source, 'config', key, value)
        self.assertFalse((self.source / '.git' / 'lfs' / 'objects').exists())

        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'filter.lfs.process').stdout.strip(),
                         b'git-lfs filter-process')
        self.assertEqual(self.git(one, 'config', '--get', 'lfs.storage').stdout.strip(), b'lfs')
        self.assertTrue((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push').is_file())
        self.assertIn(b'lfs-history', self.git(one, 'branch', '--list').stdout)

    def test_global_canonical_lfs_setup_is_preserved_for_another_branch(self):
        import hashlib
        if not shutil.which('git-lfs'):
            self.skipTest('needs git-lfs')
        payload = b'globally configured LFS content on another branch\n'
        oid = hashlib.sha256(payload).hexdigest()
        self.git(self.source, 'checkout', '-q', '-b', 'lfs-history')
        (self.source / '.gitattributes').write_text('*.bin filter=lfs\n')
        (self.source / 'history.bin').write_bytes(self.lfs_pointer(oid, len(payload)))
        self.git(self.source, 'add', '.gitattributes', 'history.bin')
        self.git(self.source, 'commit', '-qm', 'LFS pointer on preserved branch')
        self.git(self.source, 'checkout', '-q', 'main')
        global_config = self.root / 'global-lfs-config'
        global_config.write_text('[filter "lfs"]\n'
                                 ' clean = git-lfs clean -- %f\n'
                                 ' smudge = git-lfs smudge -- %f\n'
                                 ' process = git-lfs filter-process\n'
                                 ' required = yes\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'config', '--local', '--get', 'filter.lfs.process', code=1)
        self.assertFalse((self.source / '.git' / 'lfs' / 'objects').exists())

        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--local', '--get', 'filter.lfs.process').stdout.strip(),
                         b'git-lfs filter-process')
        self.assertEqual(self.git(one, 'config', '--get', 'lfs.storage').stdout.strip(), b'lfs')
        self.assertTrue((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push').is_file())
        self.assertIn(b'lfs-history', self.git(one, 'branch', '--list').stdout)

    def test_canonical_lfs_skip_variants_are_preserved_in_managed_config(self):
        if not shutil.which('git-lfs'):
            self.skipTest('needs git-lfs')
        variants = ((True, False), (False, True), (True, True))
        for index, (skip_smudge, skip_process) in enumerate(variants):
            with self.subTest(skip_smudge=skip_smudge, skip_process=skip_process):
                source = self.root / f'lfs-skip-{index}'
                source.mkdir()
                self.git(source, 'init', '-q', '-b', 'main')
                self.identify(source)
                self.install_lfs(source)
                smudge = 'git-lfs smudge --skip -- %f' if skip_smudge else 'git-lfs smudge -- %f'
                process = 'git-lfs filter-process --skip' if skip_process else 'git-lfs filter-process'
                self.git(source, 'config', 'filter.lfs.smudge', smudge)
                self.git(source, 'config', 'filter.lfs.process', process)
                (source / 'payload.bin').write_bytes(b'captured skip variant\n')
                self.git(source, 'add', '.gitattributes', 'payload.bin')
                self.git(source, 'commit', '-qm', 'LFS skip variant')

                snapshot = self.world('init', str(source)).stdout.split()[0].decode()
                one, wid = self.fork(f'lfs-skip-one-{index}', snapshot)
                expected_smudge = smudge.encode()
                expected_process = process.encode()
                self.assertEqual(self.git(one, 'config', '--local', '--get', 'filter.lfs.smudge').stdout.strip(),
                                 expected_smudge)
                self.assertEqual(self.git(one, 'config', '--local', '--get', 'filter.lfs.process').stdout.strip(),
                                 expected_process)

                managed = self.world('checkpoint', wid).stdout.split()[0].decode()
                two, _ = self.fork(f'lfs-skip-two-{index}', managed)
                self.assertEqual(self.git(two, 'config', '--local', '--get', 'filter.lfs.smudge').stdout.strip(),
                                 expected_smudge)
                self.assertEqual(self.git(two, 'config', '--local', '--get', 'filter.lfs.process').stdout.strip(),
                                 expected_process)

    def test_include_changes_recognizes_filter_used_only_by_staged_attributes(self):
        self.install_lfs(self.source)
        (self.source / 'file').write_bytes(b'ordinary committed content\n')
        self.git(self.source, 'add', 'file')
        self.git(self.source, 'commit', '-qm', 'ordinary file')
        (self.source / '.gitattributes').write_text('file filter=lfs\n')
        self.git(self.source, 'add', '.gitattributes')
        # Deliberately make the worktree view differ from the staged/index view and HEAD.
        (self.source / '.gitattributes').write_text('')
        index_before = (self.source / '.git/index').read_bytes()
        self.world('init', str(self.source), '--include-changes')
        self.assertEqual((self.source / '.git/index').read_bytes(), index_before)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'check-attr', '--cached', 'filter', '--', 'file').stdout,
                         b'file: filter: lfs\n')
        self.assertEqual(self.git(one, 'config', '--get', 'filter.lfs.process').stdout.strip(),
                         b'git-lfs filter-process')
        self.assertTrue((one / '.world-git' / 'repo.git' / 'hooks' / 'pre-push').is_file())

    def test_include_changes_refuses_custom_filter_used_only_by_staged_attributes(self):
        import shlex
        (self.source / 'file').write_bytes(b'ordinary committed content\n')
        self.git(self.source, 'add', 'file')
        self.git(self.source, 'commit', '-qm', 'ordinary file')
        marker = self.root / 'staged-filter-ran'
        command = 'touch ' + shlex.quote(str(marker)) + '; cat'
        (self.source / '.gitattributes').write_text('file filter=example\n')
        self.git(self.source, 'add', '.gitattributes')
        (self.source / '.gitattributes').write_text('')
        # Configure only after staging the attribute: otherwise Git's `add` may refresh the
        # already-tracked file through the newly staged filter and execute this fixture.
        for field in ('clean', 'smudge', 'process'):
            self.git(self.source, 'config', 'filter.example.' + field, command)
        self.assertFalse(marker.exists())
        index_before = (self.source / '.git/index').read_bytes()
        config_before = (self.source / '.git/config').read_bytes()
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b"uses the 'example' filter", refused.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual((self.source / '.git/index').read_bytes(), index_before)
        self.assertEqual((self.source / '.git/config').read_bytes(), config_before)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_inactive_conditional_lfs_configuration_is_refused(self):
        self.install_lfs(self.source)
        (self.source / 'file.bin').write_bytes(b'conditional LFS fixture\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'LFS fixture')
        included = self.root / 'inactive-lfs-config'
        global_config = self.root / 'inactive-lfs-global'
        global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = '
                                  + str(included) + '\n')
        targets = {
            'lfs url': ('[lfs]\n url = ../relative-objects\n', b'lfs.url'),
            'LFS extension': ('[lfs "extension.foo"]\n clean = touch marker\n', b'lfs.extension.foo.clean'),
            'section-wide LFS remote': ('[remote]\n lfsdefault = origin\n', b'remote.lfsdefault'),
        }
        for name, (contents, key) in targets.items():
            with self.subTest(target=name):
                included.write_text(contents)
                self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
                refused = self.world('init', str(self.source), code=3)
                self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
                self.assertIn(b'includes', refused.stderr)
                self.assertIn(key, refused.stderr)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_lfs_import_keeps_checkout_index_and_cache_independent_of_source(self):
        import hashlib
        self.install_lfs(self.source)
        cached = b'hydrated content carried by Git LFS\n'
        checkout = b'content restored later from the copied cache\n'
        dirty_base = b'committed clean content\n'
        staged = b'staged LFS payload\n'
        payloads = {'cached.bin': cached, 'checkout.bin': checkout, 'dirty.bin': dirty_base}
        for name, data in payloads.items():
            (self.source / name).write_bytes(data)
        missing_data = b'pointer whose payload is not cached\n'
        missing_oid = hashlib.sha256(missing_data).hexdigest()
        (self.source / 'pointer-only.bin').write_bytes(self.lfs_pointer(missing_oid, len(missing_data)))
        self.git(self.source, 'add', '.gitattributes', '*.bin')
        self.git(self.source, 'commit', '-qm', 'LFS pointers')
        oids = {name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()}
        for name, oid in oids.items():
            self.assertEqual(self.lfs_object_path(self.source, oid).read_bytes(), payloads[name])
        # A valid pointer in the worktree remains a pointer even though its payload is cached;
        # this exercises a later, offline `git lfs checkout` in the World.
        checkout_pointer = self.git(self.source, 'show', 'HEAD:checkout.bin').stdout
        (self.source / 'checkout.bin').write_bytes(checkout_pointer)
        (self.source / 'dirty.bin').write_bytes(b'unstaged dirty bytes\n')
        (self.source / 'staged.bin').write_bytes(staged)
        self.git(self.source, 'add', 'staged.bin')
        staged_oid = hashlib.sha256(staged).hexdigest()
        (self.source / 'staged.bin').write_bytes(b'worktree differs from staged LFS object\n')
        expected_config = (self.source / '.git/config').read_bytes()
        expected_index = (self.source / '.git/index').read_bytes()
        expected_hooks = self.git_hook_snapshot(self.source)
        expected_cache = {p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                          for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}
        expected_status = self.git(self.source, 'status', '--porcelain').stdout
        self.world('init', str(self.source), '--include-changes')
        self.assertEqual((self.source / '.git/config').read_bytes(), expected_config)
        self.assertEqual((self.source / '.git/index').read_bytes(), expected_index)
        self.assertEqual(self.git_hook_snapshot(self.source), expected_hooks)
        self.assertEqual({p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                          for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}, expected_cache)
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, expected_status)
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual((one / 'cached.bin').read_bytes(), cached)
        self.assertEqual((one / 'checkout.bin').read_bytes(), checkout_pointer)
        self.assertEqual((one / 'dirty.bin').read_bytes(), b'unstaged dirty bytes\n')
        self.assertEqual((one / 'staged.bin').read_bytes(), b'worktree differs from staged LFS object\n')
        self.assertEqual(self.git(one, 'show', ':staged.bin').stdout,
                         self.lfs_pointer(staged_oid, len(staged)))
        repo = one / '.world-git/repo.git'
        for oid, data in [(oids['cached.bin'], cached), (oids['checkout.bin'], checkout), (staged_oid, staged)]:
            self.assertEqual(self.lfs_object_path(repo, oid).read_bytes(), data)
        self.assertFalse(self.lfs_object_path(repo, missing_oid).exists())
        self.git(one, 'lfs', 'checkout', 'checkout.bin')
        self.assertEqual((one / 'checkout.bin').read_bytes(), checkout)
        self.git(one, 'lfs', 'checkout', 'pointer-only.bin')
        self.assertEqual((one / 'pointer-only.bin').read_bytes(), self.lfs_pointer(missing_oid, len(missing_data)))
        self.git(one, 'fsck', '--full')
        self.world('checkpoint', wid, '--include-changes')
        two, _ = self.fork('two', 'S2')
        self.assertEqual((two / 'checkout.bin').read_bytes(), checkout)
        self.assertEqual(self.lfs_object_path(two / '.world-git/repo.git', staged_oid).read_bytes(), staged)
        self.git(two, 'fsck', '--full')

    def test_lfs_corrupt_cache_is_refused_without_source_mutation(self):
        import hashlib
        self.install_lfs(self.source)
        content = b'correct bytes for this pointer\n'
        oid = hashlib.sha256(content).hexdigest()
        cache = self.lfs_object_path(self.source, oid)
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(b'corrupt bytes\n')
        (self.source / 'corrupt.bin').write_bytes(self.lfs_pointer(oid, len(content)))
        self.git(self.source, 'add', '.gitattributes', 'corrupt.bin')
        self.git(self.source, 'commit', '-qm', 'corrupt LFS cache fixture')
        before = {name: (self.source / '.git' / name).read_bytes() for name in ('config', 'index')}
        hooks_before = self.git_hook_snapshot(self.source)
        self.world('init', str(self.source), code=3)
        self.assertEqual({name: (self.source / '.git' / name).read_bytes() for name in before}, before)
        self.assertEqual(self.git_hook_snapshot(self.source), hooks_before)
        self.assertEqual(cache.read_bytes(), b'corrupt bytes\n')
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_lfs_committed_only_hydrates_cached_objects_and_keeps_uncached_pointers(self):
        import hashlib
        self.install_lfs(self.source)
        cached = b'committed LFS object available offline\n'
        missing = b'committed pointer without a local payload\n'
        cached_oid, missing_oid = hashlib.sha256(cached).hexdigest(), hashlib.sha256(missing).hexdigest()
        (self.source / 'cached.bin').write_bytes(cached)
        (self.source / 'missing.bin').write_bytes(self.lfs_pointer(missing_oid, len(missing)))
        self.git(self.source, 'add', '.gitattributes', 'cached.bin', 'missing.bin')
        self.git(self.source, 'commit', '-qm', 'committed LFS pointers')
        pointer = self.git(self.source, 'show', 'HEAD:cached.bin').stdout
        (self.source / 'cached.bin').write_bytes(pointer)
        (self.source / 'untracked.bin').write_bytes(b'reset removes this\n')
        config_before = (self.source / '.git/config').read_bytes()
        index_before = (self.source / '.git/index').read_bytes()
        cache_before = self.lfs_object_path(self.source, cached_oid).read_bytes()
        self.world('init', str(self.source), '--committed-only')
        self.assertEqual((self.source / '.git/config').read_bytes(), config_before)
        self.assertEqual((self.source / '.git/index').read_bytes(), index_before)
        self.assertEqual(self.lfs_object_path(self.source, cached_oid).read_bytes(), cache_before)
        self.assertTrue((self.source / 'untracked.bin').exists())
        one, _ = self.fork()
        self.assertEqual((one / 'cached.bin').read_bytes(), cached)
        self.assertEqual((one / 'missing.bin').read_bytes(), self.lfs_pointer(missing_oid, len(missing)))
        self.assertFalse((one / 'untracked.bin').exists())

    def test_lfs_owned_cache_symlink_race_aborts_import_without_source_mutation(self):
        import shlex
        import hashlib
        self.install_lfs(self.source)
        payload = b'cache object raced into a symlink\n'
        oid = hashlib.sha256(payload).hexdigest()
        (self.source / 'race.bin').write_bytes(payload)
        self.git(self.source, 'add', '.gitattributes', 'race.bin')
        self.git(self.source, 'commit', '-qm', 'LFS race fixture')
        cache = self.lfs_object_path(self.source, oid)
        config_before = (self.source / '.git/config').read_bytes()
        index_before = (self.source / '.git/index').read_bytes()
        outside = self.root / 'outside-cache-target'
        outside.write_bytes(cache.read_bytes())
        rel = oid[:2] + '/' + oid[2:4] + '/' + oid
        script_action = ('rm -f "$repo/lfs/objects/' + rel + '" && ln -s '
                         + shlex.quote(str(outside)) + ' "$repo/lfs/objects/' + rel + '"')
        wrapper, done = self.pack_refs_wrapper('lfs-symlink-race', script_action)
        path = self.env['PATH']
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        result = self.world('init', str(self.source), code=3)
        self.env['PATH'] = path
        self.assertTrue(done.exists(), result.stderr.decode(errors='replace'))
        self.assertTrue(cache.is_file())
        self.assertIn(b'unsupported Git layout', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.assertEqual(cache.read_bytes(), outside.read_bytes())
        self.assertEqual((self.source / '.git/config').read_bytes(), config_before)
        self.assertEqual((self.source / '.git/index').read_bytes(), index_before)

    def test_canonical_lfs_pre_push_hook_uploads_to_local_bare_remote(self):
        import hashlib
        self.install_lfs(self.source)
        baseline = b'baseline remote LFS payload\n'
        (self.source / 'baseline.bin').write_bytes(baseline)
        self.git(self.source, 'add', '.gitattributes', 'baseline.bin')
        self.git(self.source, 'commit', '-qm', 'baseline LFS object')
        bare = self.root / 'lfs-remote.git'
        self.git(self.root, 'init', '--bare', '-q', str(bare))
        self.git(self.source, 'remote', 'add', 'origin', str(bare))
        self.git(self.source, 'push', '-q', '-u', 'origin', 'main')
        baseline_oid = hashlib.sha256(baseline).hexdigest()
        self.assertEqual(self.lfs_object_path(bare, baseline_oid).read_bytes(), baseline)
        self.world('init', str(self.source), '--with-hooks')
        one, wid = self.fork()
        world_payload = b'uploaded by the preserved LFS pre-push hook\n'
        (one / 'world.bin').write_bytes(world_payload)
        self.git(one, 'add', 'world.bin')
        self.git(one, 'commit', '-qm', 'world LFS object')
        self.git(one, 'push', 'origin', 'world/' + wid)
        world_oid = hashlib.sha256(world_payload).hexdigest()
        self.assertEqual(self.lfs_object_path(bare, world_oid).read_bytes(), world_payload)
        published = self.git(self.root, '--git-dir', str(bare), 'rev-parse', 'refs/heads/world/' + wid).stdout.strip()
        self.assertEqual(published, self.git(one, 'rev-parse', 'HEAD').stdout.strip())

    def test_default_lfs_hook_is_pinned_over_global_hooks_path(self):
        import hashlib
        import shlex
        self.install_lfs(self.source)
        baseline = b'baseline before global hooksPath\n'
        (self.source / 'baseline.bin').write_bytes(baseline)
        self.git(self.source, 'add', '.gitattributes', 'baseline.bin')
        self.git(self.source, 'commit', '-qm', 'baseline LFS object')
        bare = self.root / 'global-hook-remote.git'
        self.git(self.root, 'init', '--bare', '-q', str(bare))
        self.git(self.source, 'remote', 'add', 'origin', str(bare))
        self.git(self.source, '-c', 'core.hooksPath=/dev/null', 'push', '-q', '-u', 'origin', 'main')

        external = self.root / 'external-global-hooks'
        external.mkdir()
        marker = self.root / 'external-hook-ran'
        (external / 'pre-push').write_text('#!/bin/sh\nprintf ran > ' + shlex.quote(str(marker)) + '\nexit 0\n')
        (external / 'pre-push').chmod(0o700)
        global_config = self.root / 'global-hook-config'
        global_config.write_text('[core]\n hooksPath = ' + str(external) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)

        # Default import ignores the ambient hook directory and installs/pins its own canonical
        # LFS pre-push hook in the copied repository's administration.
        self.world('init', str(self.source))
        one, wid = self.fork()
        hooks_path = self.git(one, 'config', '--local', '--get', 'core.hooksPath').stdout.decode().strip()
        self.assertTrue(hooks_path)
        hook = Path(hooks_path)
        if not hook.is_absolute(): hook = one / hook
        self.assertTrue(hook.resolve().is_relative_to((one / '.world-git/repo.git').resolve()))
        payload = b'uploaded by isolated default hook\n'
        (one / 'world.bin').write_bytes(payload)
        self.git(one, 'add', 'world.bin')
        self.git(one, 'commit', '-qm', 'world LFS object')
        self.git(one, 'push', 'origin', 'world/' + wid)
        self.assertFalse(marker.exists())
        oid = hashlib.sha256(payload).hexdigest()
        self.assertEqual(self.lfs_object_path(bare, oid).read_bytes(), payload)

    def test_with_hooks_refuses_custom_lfs_pre_push_without_source_mutation(self):
        self.install_lfs(self.source)
        (self.source / 'file.bin').write_bytes(b'canonical LFS fixture\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'LFS fixture')
        config = (self.source / '.git/config').read_bytes()
        index = (self.source / '.git/index').read_bytes()
        hooks = self.git_hook_snapshot(self.source)
        cache = {p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                 for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}
        custom = self.source / '.git/hooks/pre-push'
        custom.write_bytes(self.legacy_lfs_prepush_hook() + b'# near-match custom addition\n')
        custom.chmod(0o700)
        expected_hooks = self.git_hook_snapshot(self.source)
        result = self.world('init', str(self.source), '--with-hooks', code=3)
        self.assertIn(b'pre-push', result.stderr)
        self.assertEqual((self.source / '.git/config').read_bytes(), config)
        self.assertEqual((self.source / '.git/index').read_bytes(), index)
        self.assertEqual(self.git_hook_snapshot(self.source), expected_hooks)
        self.assertEqual({p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                          for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}, cache)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_publish_transfers_lfs_objects_from_intermediate_history(self):
        import hashlib
        self.install_lfs(self.source)
        baseline = b'baseline before World import\n'
        (self.source / 'baseline.bin').write_bytes(baseline)
        self.git(self.source, 'add', '.gitattributes', 'baseline.bin')
        self.git(self.source, 'commit', '-qm', 'baseline LFS content')
        self.world('init', str(self.source))
        one, wid = self.fork()
        intermediate = b'present only in an intermediate commit\n'
        deleted_intermediate = b'deleted before the published tip\n'
        final = b'current LFS content at HEAD\n'
        (one / 'sequence.bin').write_bytes(intermediate)
        (one / 'deleted.bin').write_bytes(deleted_intermediate)
        self.git(one, 'add', 'sequence.bin', 'deleted.bin')
        self.git(one, 'commit', '-qm', 'intermediate LFS content')
        (one / 'sequence.bin').write_bytes(final)
        self.git(one, 'rm', '-q', 'deleted.bin')
        self.git(one, 'add', 'sequence.bin')
        self.git(one, 'commit', '-qm', 'final LFS content')
        intermediate_oid = hashlib.sha256(intermediate).hexdigest()
        deleted_oid = hashlib.sha256(deleted_intermediate).hexdigest()
        final_oid = hashlib.sha256(final).hexdigest()
        self.assertFalse(self.lfs_object_path(self.source, intermediate_oid).exists())
        self.assertFalse(self.lfs_object_path(self.source, deleted_oid).exists())
        self.assertFalse(self.lfs_object_path(self.source, final_oid).exists())
        # A different target ref can already reach World's tip without its LFS payloads.
        # Publication must still inspect the complete history, not subtract all target refs.
        self.git(self.source, 'fetch', '--quiet', str(one / '.world-git' / 'repo.git'),
                 f'refs/heads/world/{wid}:refs/heads/other-tip')
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/other-tip').stdout.strip(),
                         self.git(one, 'rev-parse', 'HEAD').stdout.strip())
        # Repository-local info attributes override --source=<commit> unless publication
        # evaluates attributes in an isolated repository.
        target_info_attributes = self.source / '.git' / 'info' / 'attributes'
        target_info_attributes.write_text('*.bin -filter\n')
        self.assertEqual(self.git(self.source, 'check-attr', 'filter', '--', 'sequence.bin').stdout,
                         b'sequence.bin: filter: unset\n')
        self.world('publish', wid)
        self.assertEqual(self.lfs_object_path(self.source, intermediate_oid).read_bytes(), intermediate)
        self.assertEqual(self.lfs_object_path(self.source, deleted_oid).read_bytes(), deleted_intermediate)
        self.assertEqual(self.lfs_object_path(self.source, final_oid).read_bytes(), final)
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/world/' + wid).stdout.strip(),
                         self.git(one, 'rev-parse', 'HEAD').stdout.strip())

    def test_lfs_pointer_hashing_supports_legacy_help_without_no_extensions(self):
        import hashlib
        import shlex
        self.install_lfs(self.source)
        existing = b'cached before import\n'
        (self.source / 'existing.bin').write_bytes(existing)
        self.git(self.source, 'add', '.gitattributes', 'existing.bin')
        self.git(self.source, 'commit', '-qm', 'existing cached LFS object')
        existing_oid = hashlib.sha256(existing).hexdigest()
        self.assertEqual(self.lfs_object_path(self.source, existing_oid).read_bytes(), existing)

        real_lfs = shutil.which('git-lfs')
        real_help = subprocess.run([real_lfs, 'pointer', '-h'], capture_output=True, timeout=30)
        self.assertEqual(real_help.returncode, 0, real_help.stderr)
        self.assertIn(b'--file', real_help.stdout)
        self.assertIn(b'--check', real_help.stdout)
        real_supports_no_extensions = b'--no-extensions' in real_help.stdout
        wrapper = self.root / 'legacy-git-lfs-bin'
        wrapper.mkdir()
        help_seen = self.root / 'legacy-help-seen'
        legacy_seen = self.root / 'legacy-pointer-seen'
        flag_rejected = self.root / 'legacy-flag-rejected'
        script = wrapper / 'git-lfs'
        script.write_text(
            '#!/bin/sh\n'
            'real=' + shlex.quote(real_lfs) + '\n'
            'if [ "$1" = pointer ]; then\n'
            '  shift\n'
            '  if [ "$1" = -h ] || [ "$1" = --help ]; then\n'
            '    : > ' + shlex.quote(str(help_seen)) + '\n'
            '    if [ "${FORKFS_TEST_BAD_LFS_HELP:-0}" = 1 ]; then\n'
            '      echo "usage: git-lfs pointer --file=<file>"\n'
            '      exit 0\n'
            '    fi\n'
            '    "$real" pointer "$@" | sed "s/--no-extensions//g"\n'
            '    exit $?\n'
            '  fi\n'
            '  for arg do\n'
            '    if [ "$arg" = --no-extensions ]; then\n'
            '      : > ' + shlex.quote(str(flag_rejected)) + '\n'
            '      exit 97\n'
            '    fi\n'
            '  done\n'
            '  : > ' + shlex.quote(str(legacy_seen)) + '\n' +
            ('  exec "$real" pointer --no-extensions "$@"\n' if real_supports_no_extensions else
             '  exec "$real" pointer "$@"\n') +
            'fi\n'
            'exec "$real" "$@"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']

        self.world('init', str(self.source))
        one, wid = self.fork()
        added = b'published with legacy pointer interface\n'
        added_oid = hashlib.sha256(added).hexdigest()
        (one / 'added.bin').write_bytes(added)
        self.git(one, 'add', 'added.bin')
        self.git(one, 'commit', '-qm', 'new LFS object')
        self.assertFalse(self.lfs_object_path(self.source, added_oid).exists())
        self.world('publish', wid)

        self.assertTrue(help_seen.is_file())
        self.assertTrue(legacy_seen.is_file())
        self.assertFalse(flag_rejected.exists())
        self.assertEqual(self.lfs_object_path(self.source, added_oid).read_bytes(), added)
        # The baseline object's target-cache reuse exercises the fd-backed oracle path.
        self.assertEqual(self.lfs_object_path(self.source, existing_oid).read_bytes(), existing)

        # A successful but incomplete help response is not treated as an old CLI: fail before
        # invoking the legacy pointer command when the required interface cannot be verified.
        bad_source = self.root / 'bad-pointer-help-source'
        bad_source.mkdir()
        self.git(bad_source, 'init', '-q', '-b', 'main')
        self.identify(bad_source)
        self.install_lfs(bad_source)
        (bad_source / 'bad.bin').write_bytes(b'help probe must fail closed\n')
        self.git(bad_source, 'add', '.gitattributes', 'bad.bin')
        self.git(bad_source, 'commit', '-qm', 'bad help probe fixture')
        legacy_seen.unlink()
        self.env['FORKFS_TEST_BAD_LFS_HELP'] = '1'
        try:
            refused = self.world('init', str(bad_source), code=3)
        finally:
            self.env.pop('FORKFS_TEST_BAD_LFS_HELP', None)
        self.assertIn(b'Git LFS cache object', refused.stderr)
        self.assertFalse(legacy_seen.exists())

    def test_publish_ignores_plain_pointer_text_without_lfs_filter(self):
        import hashlib
        payload = b'ordinary text, not an LFS object\n'
        oid = hashlib.sha256(payload).hexdigest()
        (self.source / 'pointer.txt').write_bytes(self.lfs_pointer(oid, len(payload)))
        self.git(self.source, 'add', 'pointer.txt')
        self.git(self.source, 'commit', '-qm', 'ordinary pointer-shaped text')
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / 'more.txt').write_text('normal commit\n')
        self.git(one, 'add', 'more.txt')
        self.git(one, 'commit', '-qm', 'ordinary change')
        self.world('publish', wid)
        self.assertFalse(self.lfs_object_path(self.source, oid).exists())
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/world/' + wid).stdout.strip(),
                         self.git(one, 'rev-parse', 'HEAD').stdout.strip())

    def test_publish_finds_pointer_blob_when_new_commit_adds_lfs_attribute(self):
        import hashlib
        payload = b'pointer text becomes tracked as LFS\n'
        oid = hashlib.sha256(payload).hexdigest()
        pointer = self.lfs_pointer(oid, len(payload))
        (self.source / 'file.bin').write_bytes(pointer)
        self.git(self.source, 'add', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'plain pointer-shaped blob')
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'filter.lfs.clean', 'git-lfs clean -- %f')
        self.git(one, 'config', 'filter.lfs.smudge', 'git-lfs smudge -- %f')
        self.git(one, 'config', 'filter.lfs.process', 'git-lfs filter-process')
        self.git(one, 'config', 'filter.lfs.required', 'true')
        self.git(one, 'config', 'lfs.storage', 'lfs')
        (one / '.gitattributes').write_text('*.bin filter=lfs\n')
        self.assertEqual(self.git(one, 'check-attr', '--source=HEAD', 'filter', '--', 'file.bin').stdout,
                         b'file.bin: filter: unspecified\n')
        self.git(one, 'add', '.gitattributes')
        self.git(one, 'commit', '-qm', 'track existing pointer as LFS')
        self.assertEqual(self.git(one, 'check-attr', '--source=HEAD', 'filter', '--', 'file.bin').stdout,
                         b'file.bin: filter: lfs\n')
        self.assertIn(b'.gitattributes', self.git(one, 'diff-tree', '--no-commit-id', '--root', '-r', '-m', '-z',
                                                   '--raw', '--no-renames', 'HEAD').stdout)
        cache = self.lfs_object_path(one, oid)
        cache.parent.mkdir(parents=True)
        cache.write_bytes(payload)
        self.world('publish', wid)
        self.assertEqual(self.lfs_object_path(self.source, oid).read_bytes(), payload)
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/heads/world/' + wid).stdout.strip(),
                         self.git(one, 'rev-parse', 'HEAD').stdout.strip())

    def test_publish_revalidates_existing_target_lfs_cache_after_manifest_scan(self):
        import hashlib
        import shlex
        self.install_lfs(self.source)
        baseline = b'baseline before target-cache race\n'
        payload = b'target cached payload\n'
        (self.source / 'baseline.bin').write_bytes(baseline)
        self.git(self.source, 'add', '.gitattributes', 'baseline.bin')
        self.git(self.source, 'commit', '-qm', 'baseline LFS content')
        oid = hashlib.sha256(payload).hexdigest()
        target_cache = self.lfs_object_path(self.source, oid)
        target_cache.parent.mkdir(parents=True, exist_ok=True)
        target_cache.write_bytes(payload)

        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, wid = self.fork('target-cache-race', snapshot)
        (one / 'new.bin').write_bytes(payload)
        self.git(one, 'add', 'new.bin')
        self.git(one, 'commit', '-qm', 'publish pointer with an existing target cache object')

        real_git = shutil.which('git')
        wrapper = self.root / 'target-lfs-cache-race-bin'
        wrapper.mkdir()
        done = self.root / 'target-lfs-cache-race-done'
        script = wrapper / 'git'
        script.write_text(
            '#!/bin/sh\nmatched=0\nfor arg in "$@"; do [ "$arg" = ' +
            shlex.quote('--file=' + str(target_cache)) + ' ] && matched=1; done\n' +
            shlex.quote(real_git) + ' "$@"\nresult=$?\n' +
            'if [ "$result" = 0 ] && [ "$matched" = 1 ] && [ ! -e ' + shlex.quote(str(done)) + ' ]; then\n' +
            '  printf %s ' + shlex.quote('X' + payload.decode()[1:]) + ' > ' + shlex.quote(str(target_cache)) + '\n' +
            '  : > ' + shlex.quote(str(done)) + ' || exit $?\nfi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']

        result = self.world('publish', wid, code=3)
        self.assertTrue(done.exists(), result.stderr.decode(errors='replace'))
        self.assertEqual(target_cache.read_bytes(), b'X' + payload[1:])
        self.assertIn(b'target Git LFS cache', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/' + wid, code=1)

    def test_publish_rechecks_name_after_linkat_eexist_lfs_cache_race(self):
        import hashlib
        import shlex
        self.install_lfs(self.source)
        payload = b'payload racing into the target LFS cache\n'
        (self.source / 'file.bin').write_bytes(payload)
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'target LFS payload')
        oid = hashlib.sha256(payload).hexdigest()
        target_cache = self.lfs_object_path(self.source, oid)
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, wid = self.fork('lfs-eexist-race', snapshot)
        target_cache.unlink()

        real_git = shutil.which('git')
        wrapper = self.root / 'lfs-eexist-race-bin'
        wrapper.mkdir()
        created = self.root / 'lfs-eexist-created'
        replaced = self.root / 'lfs-eexist-replaced'
        script = wrapper / 'git'
        script.write_text(
            '#!/bin/sh\nfilearg=\nfor arg in "$@"; do case "$arg" in --file=*) filearg=${arg#--file=};; esac; done\n' +
            shlex.quote(real_git) + ' "$@"\nresult=$?\n' +
            'if [ "$result" = 0 ] && [ ! -e ' + shlex.quote(str(created)) + ' ]; then\n' +
            '  case "$filearg" in */.wfs-*)\n' +
            '    mkdir -p ' + shlex.quote(str(target_cache.parent)) + '\n' +
            '    printf %s ' + shlex.quote(payload.decode()) + ' > ' + shlex.quote(str(target_cache)) + '\n' +
            '    : > ' + shlex.quote(str(created)) + '\n' +
            '  esac\n' +
            'elif [ "$result" = 0 ] && [ "$filearg" = /dev/stdin ] && [ -e ' + shlex.quote(str(created)) +
            ' ] && [ ! -e ' + shlex.quote(str(replaced)) + ' ]; then\n' +
            '  cp ' + shlex.quote(str(target_cache)) + ' ' + shlex.quote(str(target_cache) + '.replacement') + '\n' +
            '  mv ' + shlex.quote(str(target_cache) + '.replacement') + ' ' + shlex.quote(str(target_cache)) + '\n' +
            '  : > ' + shlex.quote(str(replaced)) + '\nfi\nexit "$result"\n')
        script.chmod(0o700)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']

        result = self.world('publish', wid, code=3)
        self.assertTrue(created.exists(), result.stderr.decode(errors='replace'))
        self.assertTrue(replaced.exists(), result.stderr.decode(errors='replace'))
        self.assertEqual(target_cache.read_bytes(), payload)
        self.assertIn(b'target Git LFS cache object', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/' + wid, code=1)

    def test_publish_refuses_relative_lfs_endpoint_in_new_history(self):
        import hashlib
        self.install_lfs(self.source)
        (self.source / 'baseline.bin').write_bytes(b'baseline LFS payload\n')
        self.git(self.source, 'add', '.gitattributes', 'baseline.bin')
        self.git(self.source, 'commit', '-qm', 'baseline LFS setup')
        self.world('init', str(self.source))
        one, wid = self.fork()
        payload = b'payload committed with relative LFS endpoint\n'
        (one / 'relative.bin').write_bytes(payload)
        self.git(one, 'add', 'relative.bin')
        (one / '.lfsconfig').write_text('[lfs]\n    url = ../relative-lfs\n')
        self.git(one, 'add', '.lfsconfig', '.gitattributes')
        self.git(one, 'commit', '-qm', 'commit relative LFS endpoint')
        oid = hashlib.sha256(payload).hexdigest()
        self.assertTrue(self.lfs_object_path(one, oid).is_file())
        (one / '.lfsconfig').write_text('[lfs]\n    url = https://lfs.example.test/objects\n')
        self.git(one, 'add', '.lfsconfig')
        self.git(one, 'commit', '-qm', 'correct LFS endpoint')

        # The tip is reachable from another target ref, but its ancestry still needs checks.
        self.git(self.source, 'fetch', '--quiet', str(one / '.world-git' / 'repo.git'),
                 f'refs/heads/world/{wid}:refs/heads/other-tip')

        cache_before = {p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                        for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}
        head_before = self.git(self.source, 'rev-parse', 'HEAD').stdout
        refused = self.world('publish', wid, code=3)
        self.assertIn(b'must be an absolute URL or absolute filesystem path', refused.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout, head_before)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/' + wid, code=1)
        self.assertEqual({p.relative_to(self.source / '.git/lfs/objects'): p.read_bytes()
                          for p in (self.source / '.git/lfs/objects').rglob('*') if p.is_file()}, cache_before)

    def test_publish_missing_or_corrupt_lfs_history_payload_does_not_move_branch(self):
        import hashlib
        for failure in ('missing', 'corrupt'):
            with self.subTest(failure=failure):
                source = self.root / ('source-' + failure)
                source.mkdir()
                self.git(source, 'init', '-q', '-b', 'main')
                self.identify(source)
                self.install_lfs(source)
                (source / 'base.bin').write_bytes(b'base LFS data\n')
                self.git(source, 'add', '.gitattributes', 'base.bin')
                self.git(source, 'commit', '-qm', 'base')
                config_before = (source / '.git/config').read_bytes()
                index_before = (source / '.git/index').read_bytes()
                cache_before = {p.relative_to(source / '.git/lfs/objects'): p.read_bytes()
                                for p in (source / '.git/lfs/objects').rglob('*') if p.is_file()}
                head_before = self.git(source, 'rev-parse', 'HEAD').stdout
                snapshot = self.world('init', str(source)).stdout.split()[0].decode()
                one, wid = self.fork('broken-' + failure, snapshot)
                middle = b'missing history payload\n'
                tip = b'new tip payload\n'
                (one / 'sequence.bin').write_bytes(middle)
                self.git(one, 'add', 'sequence.bin')
                self.git(one, 'commit', '-qm', 'intermediate LFS')
                (one / 'sequence.bin').write_bytes(tip)
                self.git(one, 'add', 'sequence.bin')
                self.git(one, 'commit', '-qm', 'tip LFS')
                oid = hashlib.sha256(middle).hexdigest()
                object_path = self.lfs_object_path(one, oid)
                self.assertEqual(object_path.read_bytes(), middle)
                if failure == 'missing':
                    object_path.unlink()
                else:
                    object_path.write_bytes(b'X' * len(middle))
                result = self.world('publish', wid, code=3)
                self.assertIn(b'LFS', result.stderr)
                self.assertEqual(self.git(source, 'for-each-ref', '--format=%(refname) %(objectname)',
                                          'refs/heads/world/').stdout, b'')
                self.assertEqual(self.git(source, 'rev-parse', 'HEAD').stdout, head_before)
                self.assertEqual((source / '.git/config').read_bytes(), config_before)
                self.assertEqual((source / '.git/index').read_bytes(), index_before)
                self.assertEqual({p.relative_to(source / '.git/lfs/objects'): p.read_bytes()
                                  for p in (source / '.git/lfs/objects').rglob('*') if p.is_file()}, cache_before)

    def test_lfs_cache_from_linked_root_and_submodule_survives_source_removal(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        self.install_lfs(lib)
        lib_payload = b'LFS content in initialized submodule\n'
        (lib / 'module.bin').write_bytes(lib_payload)
        self.git(lib, 'add', '.gitattributes', 'module.bin')
        self.git(lib, 'commit', '-qm', 'module LFS payload')
        self.git(self.root / 'origins/lib', 'config', 'receive.denyCurrentBranch', 'ignore')
        self.git(lib, 'push', 'origin', 'HEAD')
        self.git(self.source, 'add', 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'record module LFS payload')
        self.install_lfs(self.source)
        root_payload = b'LFS content from a linked source worktree\n'
        (self.source / 'root.bin').write_bytes(root_payload)
        self.git(self.source, 'add', '.gitattributes', 'root.bin')
        self.git(self.source, 'commit', '-qm', 'root LFS payload')
        linked = self.root / 'linked-lfs-source'
        self.git(self.source, 'worktree', 'add', '-qb', 'lfs-linked', str(linked))
        self.sub(linked, 'update', '-q', '--init', '--recursive')
        root_oid = self.git(linked, 'show', 'HEAD:root.bin').stdout.decode().split('sha256:')[1].splitlines()[0]
        module_oid = self.git(lib, 'show', 'HEAD:module.bin').stdout.decode().split('sha256:')[1].splitlines()[0]
        linked_lib = linked / 'libs/lib'
        linked_module_cache = self.lfs_object_path(linked_lib, module_oid)
        linked_module_cache.parent.mkdir(parents=True, exist_ok=True)
        linked_module_cache.write_bytes(lib_payload)
        self.assertEqual((linked / 'libs/lib/module.bin').read_bytes(), self.lfs_pointer(module_oid, len(lib_payload)))
        root_cache = self.lfs_object_path(linked, root_oid)
        root_cache_before, module_cache_before = root_cache.read_bytes(), linked_module_cache.read_bytes()
        root_config_before = (self.source / '.git/config').read_bytes()
        root_hooks_before = self.git_hook_snapshot(linked)
        root_index_path = Path(self.git(linked, 'rev-parse', '--path-format=absolute', '--git-path', 'index').stdout.decode().strip())
        module_common = Path(self.git(linked_lib, 'rev-parse', '--path-format=absolute', '--git-common-dir').stdout.decode().strip())
        module_index_path = Path(self.git(linked_lib, 'rev-parse', '--path-format=absolute', '--git-path', 'index').stdout.decode().strip())
        root_index_before = root_index_path.read_bytes()
        module_config_before = (module_common / 'config').read_bytes()
        module_hooks_before = self.git_hook_snapshot(linked_lib)
        module_index_before = module_index_path.read_bytes()
        self.world('init', str(linked))
        self.assertEqual((self.source / '.git/config').read_bytes(), root_config_before)
        self.assertEqual(root_index_path.read_bytes(), root_index_before)
        self.assertEqual((module_common / 'config').read_bytes(), module_config_before)
        self.assertEqual(module_index_path.read_bytes(), module_index_before)
        self.assertEqual(self.git_hook_snapshot(linked), root_hooks_before)
        self.assertEqual(self.git_hook_snapshot(linked_lib), module_hooks_before)
        self.assertEqual(root_cache.read_bytes(), root_cache_before)
        self.assertEqual(linked_module_cache.read_bytes(), module_cache_before)
        shutil.rmtree(linked)
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, _ = self.fork()
        owned_root = one / '.world-git/repo.git'
        self.assertEqual((one / 'root.bin').read_bytes(), root_payload)
        self.assertEqual((one / 'libs/lib/module.bin').read_bytes(), self.lfs_pointer(module_oid, len(lib_payload)))
        self.assertEqual(self.lfs_object_path(owned_root, root_oid).read_bytes(), root_payload)
        self.assertEqual(self.lfs_object_path(one / 'libs/lib', module_oid).read_bytes(), lib_payload)
        self.git(one, 'fsck', '--full')
        self.git(one / 'libs/lib', 'fsck', '--full')

    def test_custom_lfs_filter_commands_and_extensions_are_refused_without_execution(self):
        import shlex
        marker = self.root / 'custom-lfs-command-ran'
        (self.source / '.gitattributes').write_text('*.bin filter=lfs\n')
        (self.source / 'file.bin').write_bytes(b'plain payload\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'custom filter fixture')
        config = self.source / '.git/config'
        original_index = (self.source / '.git/index').read_bytes()
        commands = [
            ('filter.lfs.clean', 'touch ' + shlex.quote(str(marker)) + '; cat'),
            ('filter.lfs.smudge', 'touch ' + shlex.quote(str(marker)) + '; cat'),
            ('filter.lfs.process', 'touch ' + shlex.quote(str(marker)) + '; cat'),
            ('lfs.extension.test.clean', 'touch ' + shlex.quote(str(marker)) + '; cat'),
            ('lfs.extension.test.smudge', 'touch ' + shlex.quote(str(marker)) + '; cat'),
        ]
        for key, value in commands:
            with self.subTest(key=key):
                # Give LFS a valid stock driver for extension cases; the single changed value
                # under test is the malicious command or extension.
                for stock_key, stock_value in (
                        ('filter.lfs.clean', 'git-lfs clean -- %f'),
                        ('filter.lfs.smudge', 'git-lfs smudge -- %f'),
                        ('filter.lfs.process', 'git-lfs filter-process'),
                        ('filter.lfs.required', 'true')):
                    self.git(self.source, 'config', stock_key, stock_value)
                self.git(self.source, 'config', key, value)
                expected_config = config.read_bytes()
                result = self.world('init', str(self.source), code=3)
                reason = (b'Git LFS extensions are not supported' if key.startswith('lfs.extension.')
                          else b'custom or incomplete filter.lfs configuration is not supported')
                self.assertIn(reason, result.stderr)
                self.assertFalse(marker.exists())
                self.assertEqual(config.read_bytes(), expected_config)
                self.assertEqual((self.source / '.git/index').read_bytes(), original_index)
                self.git(self.source, 'config', '--file', str(config), '--unset-all', key)
                if key.startswith('lfs.extension.'):
                    self.git(self.source, 'config', '--file', str(config), '--unset-all', 'filter.lfs.clean')
                    self.git(self.source, 'config', '--file', str(config), '--unset-all', 'filter.lfs.smudge')
                    self.git(self.source, 'config', '--file', str(config), '--unset-all', 'filter.lfs.process')
                    self.git(self.source, 'config', '--file', str(config), '--unset-all', 'filter.lfs.required')
                self.assertEqual((self.source / '.git/index').read_bytes(), original_index)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.assertFalse(marker.exists())

    def test_lfs_endpoint_configuration_is_carried_and_relative_lfsconfig_refused(self):
        self.install_lfs(self.source)
        self.git(self.source, 'config', 'lfs.url', 'https://lfs.example.test/objects')
        self.git(self.source, 'config', 'remote.origin.lfsurl', 'https://lfs.example.test/upload')
        self.git(self.source, 'config', 'remote.lfsdefault', 'origin')
        self.git(self.source, 'config', 'remote.lfspushdefault', 'origin')
        (self.source / 'file.bin').write_bytes(b'endpoint fixture\n')
        self.git(self.source, 'add', '.gitattributes', 'file.bin')
        self.git(self.source, 'commit', '-qm', 'endpoint fixture')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'lfs.url').stdout.strip(),
                         b'https://lfs.example.test/objects')
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.lfsurl').stdout.strip(),
                         b'https://lfs.example.test/upload')
        self.assertEqual(self.git(one, 'config', '--get', 'remote.lfsdefault').stdout.strip(), b'origin')
        self.assertEqual(self.git(one, 'config', '--get', 'remote.lfspushdefault').stdout.strip(), b'origin')

        other = self.root / 'relative-lfsconfig'
        other.mkdir()
        self.git(other, 'init', '-q', '-b', 'main')
        self.identify(other)
        self.install_lfs(other)
        (other / '.lfsconfig').write_text('[lfs]\n    url = ../relative-objects\n')
        (other / 'file.bin').write_bytes(b'relative endpoint fixture\n')
        self.git(other, 'add', '.lfsconfig', '.gitattributes', 'file.bin')
        self.git(other, 'commit', '-qm', 'relative endpoint')
        refused = self.world('init', str(other), code=3)
        self.assertIn(b'must be an absolute URL or absolute filesystem path', refused.stderr)
        snapshots = json.loads(self.world('list', '--json').stdout)['snapshots']
        self.assertEqual([entry['id'] for entry in snapshots], ['S1'])

        staged = self.root / 'staged-relative-lfsconfig'
        staged.mkdir()
        self.git(staged, 'init', '-q', '-b', 'main')
        self.identify(staged)
        self.install_lfs(staged)
        safe = '[lfs]\n    url = https://lfs.example.test/objects\n'
        (staged / '.lfsconfig').write_text(safe)
        (staged / 'file.bin').write_bytes(b'staged endpoint fixture\n')
        self.git(staged, 'add', '.lfsconfig', '.gitattributes', 'file.bin')
        self.git(staged, 'commit', '-qm', 'safe endpoint')
        (staged / '.lfsconfig').write_text('[lfs]\n    url = ../relative-objects\n')
        self.git(staged, 'add', '.lfsconfig')
        (staged / '.lfsconfig').write_text(safe)  # WT and HEAD are safe; the index is not.
        staged_index = (staged / '.git/index').read_bytes()
        refused = self.world('init', str(staged), '--include-changes', code=3)
        self.assertIn(b'must be an absolute URL or absolute filesystem path', refused.stderr)
        self.assertEqual((staged / '.git/index').read_bytes(), staged_index)
        snapshots = json.loads(self.world('list', '--json').stdout)['snapshots']
        self.assertEqual([entry['id'] for entry in snapshots], ['S1'])

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

    def test_publish_checks_reserved_paths_through_active_replacement_refs(self):
        # An active replacement for HEAD can present a tree that tracks a reserved path even
        # though the real commit's tree does not. Publish's replacement-ref compatibility check
        # (above) requires the target to carry the identical replacement before it would let the
        # publish through at all -- so the reserved-path check must also scan history with
        # replacements honored, or a reserved path could reach the target through the replaced
        # view alone while the raw scan sees nothing.
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'world change')
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip().decode()
        # Build a replacement tree that also tracks the World's own .world marker, entirely
        # through plumbing (ls-tree/hash-object/mktree) rather than a checkout: switching
        # branches to add .world to history and back would have git delete-and-restore the file
        # in the working tree, which risks disturbing the very marker wfs_world_verify checks
        # before publish is even reached. hash-object only reads the file; it never touches it.
        entries = self.git(one, 'ls-tree', 'HEAD').stdout.decode()
        world_blob = self.git(one, 'hash-object', '-w', str(one / '.world')).stdout.strip().decode()
        mktree_input = (entries + '100644 blob ' + world_blob + '\t.world\n').encode()
        p = subprocess.run(['git', '-C', str(one), 'mktree'], input=mktree_input, env=self.env,
                            capture_output=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        payload_tree = p.stdout.strip().decode()
        replacement = self.git(one, 'commit-tree', payload_tree, '-p', self.base.decode(),
                                '-m', 'reserved replacement').stdout.strip().decode()
        self.git(one, 'replace', head, replacement)
        # Sanity: the raw commit's tree does not track the reserved path; only the active
        # replacement's does.
        self.assertEqual(self.git(one, '--no-replace-objects', 'rev-list', '-n', '1', '--full-history',
                                   head, '--', '.world').stdout, b'')
        self.assertEqual(self.git(one, '-c', 'core.useReplaceRefs=true', 'rev-list', '-n', '1',
                                   '--full-history', head, '--', '.world').stdout.strip().decode(), head)
        # The target must carry the identical replacement ref -- exactly what a real publish
        # would require before it could even reach the replacement-ref compatibility check.
        replace_ref = 'refs/replace/' + head
        self.git(self.source, 'fetch', '-q', str(one), replace_ref + ':' + replace_ref)
        result = self.world('publish', wid, code=3)
        self.assertIn(b'reserved path', result.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs').stdout, b'')

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

    def test_external_hidden_refs_are_imported(self):
        # Hidden refs are hidden from transports only; the import lists refs with for-each-ref
        # and clones the objects, so they arrive like any other ref. The hiding setting itself
        # is the source's serving policy and is not carried.
        (self.source / 'file').write_text('only under a hidden ref\n')
        self.git(self.source, 'commit', '-qam', 'hidden')
        hidden = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()
        self.git(self.source, 'update-ref', 'refs/hidden/work', hidden.decode())
        self.git(self.source, 'reset', '-q', '--hard', self.base.decode())
        self.git(self.source, 'reflog', 'expire', '--expire=now', '--all')
        for n, key in enumerate(('transfer.hideRefs', 'uploadpack.hideRefs')):
            with self.subTest(key=key):
                self.git(self.source, 'config', '--local', key, 'refs/hidden')
                before = (self.source / '.git' / 'config').read_bytes()
                snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
                self.assertEqual((self.source / '.git' / 'config').read_bytes(), before)
                one, _ = self.fork('one-%d' % n, snapshot)
                self.assertEqual(self.git(one, 'rev-parse', 'refs/hidden/work').stdout.strip(), hidden)
                self.assertEqual(self.git(one, 'show', 'refs/hidden/work:file').stdout, b'only under a hidden ref\n')
                self.git(one, 'config', '--get', key, code=1)
                self.git(one, 'fsck', '--full')
                self.git(self.source, 'config', '--local', '--unset-all', key)

    # ---- stash ----------------------------------------------------------------------------

    def make_stash_stack(self, repo, tracked='file'):
        """Three stash entries, oldest first: a worktree change, a staged change (changed again
        after staging), and a change beside an untracked file (--include-untracked)."""
        repo = Path(repo)
        (repo / tracked).write_text('worktree change\n')
        self.git(repo, 'stash', 'push', '-q', '-m', 'worktree')
        (repo / tracked).write_text('staged change\n')
        self.git(repo, 'add', tracked)
        (repo / tracked).write_text('staged, then changed again\n')
        self.git(repo, 'stash', 'push', '-q', '-m', 'staged')
        (repo / tracked).write_text('beside untracked\n')
        (repo / 'stashed-untracked').write_text('untracked\n')
        self.git(repo, 'stash', 'push', '-q', '--include-untracked', '-m', 'untracked')
        self.assertEqual(self.git(repo, 'status', '--porcelain').stdout, b'')

    def stash_view(self, repo):
        """`stash list`, every entry's commit, tree and parents (index and untracked-files
        commits included), and `stash show -p` of each entry, untracked files included."""
        listing = self.git(repo, 'stash', 'list').stdout
        entries = self.git(repo, 'log', '--walk-reflogs', '--format=%gd %H %T %P %gs', 'refs/stash', '--').stdout
        shows = [self.git(repo, 'stash', 'show', '-p', '--include-untracked', 'stash@{%d}' % n).stdout
                 for n in range(len(listing.splitlines()))]
        return listing, entries, shows

    def pack_refs_wrapper(self, name, action):
        """A `git` on PATH that runs `action` once after the first successful `pack-refs`,
        with $repo naming the new owned bare repository."""
        import shlex
        wrapper = self.root / ('pack-refs-bin-' + name)
        wrapper.mkdir()
        done = self.root / ('pack-refs-done-' + name)
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nis_pack_refs=0\nrepo=\nprev=\nfor arg in "$@"; do\n'
                          '[ "$prev" = --git-dir ] && repo=$arg\n[ "$arg" = pack-refs ] && is_pack_refs=1\nprev=$arg\ndone\n'
                          + shlex.quote(shutil.which('git')) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$is_pack_refs" = 1 ] && [ ! -e ' + shlex.quote(str(done)) + ' ]; then\n'
                          + '  : > ' + shlex.quote(str(done)) + ' || exit $?\n  ' + action + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        return wrapper, done

    def test_external_stash_stack_survives_source_deletion(self):
        self.make_stash_stack(self.source)
        view = self.stash_view(self.source)
        self.assertEqual(len(view[0].splitlines()), 3)
        log = (self.source / '.git/logs/refs/stash').read_bytes()
        self.world('init', str(self.source))
        self.assertEqual(self.stash_view(self.source), view)
        self.assertEqual((self.source / '.git/logs/refs/stash').read_bytes(), log)
        # publish moves one branch only: never the World's stash, not even a new entry.
        pub, pub_wid = self.fork('pub')
        (pub / 'file').write_text('stashed in the World\n')
        self.git(pub, 'stash', 'push', '-q', '-m', 'world entry')
        self.git(pub, 'commit', '-q', '--allow-empty', '-m', 'world change')
        self.world('publish', pub_wid)
        self.assertEqual(self.stash_view(self.source), view)
        self.assertEqual((self.source / '.git/logs/refs/stash').read_bytes(), log)
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual(self.stash_view(one), view)
        # The reflog lives in the common directory the World's worktree reads it from.
        self.assertEqual((one / '.world-git/repo.git/logs/refs/stash').read_bytes(), log)
        self.git(one, 'fsck', '--full')
        self.git(one, 'gc', '--quiet', '--prune=now')
        self.assertEqual(self.stash_view(one), view)
        self.world('checkpoint', wid)
        self.world('checkpoint', wid, '--committed-only')
        for name, source in (('two', 'S2'), ('three', 'S3'), ('four', wid)):
            copy, _ = self.fork(name, source, *(('--committed-only',) if name == 'four' else ()))
            self.assertEqual(self.stash_view(copy), view, name)
        self.git(one, 'stash', 'pop', '-q', '--index')
        self.assertEqual((one / 'stashed-untracked').read_text(), 'untracked\n')
        self.assertEqual((one / 'file').read_text(), 'beside untracked\n')
        self.assertEqual(self.git(one, 'stash', 'list', '--format=%H').stdout,
                         b''.join(line.split()[1] + b'\n' for line in view[1].splitlines()[1:]))
        self.git(one, 'reset', '-q', '--hard')
        (one / 'stashed-untracked').unlink()
        self.git(one, 'stash', 'pop', '-q', '--index')
        self.assertEqual(self.git(one, 'show', ':file').stdout, b'staged change\n')
        self.assertEqual((one / 'file').read_text(), 'staged, then changed again\n')

    def test_stash_reflog_object_missing_from_owned_clone_is_not_published(self):
        import shlex
        self.make_stash_stack(self.source)
        view = self.stash_view(self.source)
        oldest = self.git(self.source, 'rev-parse', 'stash@{2}').stdout.strip().decode()
        loose = self.source / '.git' / 'objects' / oldest[:2] / oldest[2:]
        self.assertTrue(loose.is_file(), 'fixture needs a loose, reflog-only stash commit')
        relative = str(loose.relative_to(self.source / '.git' / 'objects'))
        missing = self.root / 'owned-clone-lacked-oldest'
        # Simulate a source repack/GC race after its common object directory has been cloned:
        # remove only the oldest reflog-only stash commit from the new repository.
        remove = 'rm -f "$repo/objects/' + relative + '" && : > ' + shlex.quote(str(missing))
        path = self.env['PATH']
        # The connectivity walk covers reflog-only entries and refuses a partial owned clone.
        wrapper, done = self.pack_refs_wrapper('skip', remove)
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        result = self.world('init', str(self.source), code=3)
        self.env['PATH'] = path
        self.assertIn(b'bad object', result.stderr)
        self.assertTrue(done.exists())
        self.assertTrue(missing.exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        wrapper, done = self.pack_refs_wrapper('retry', ':')
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        self.env['PATH'] = path
        shutil.rmtree(self.source)
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.stash_view(one), view)
        self.git(one, 'fsck', '--full')

    def test_stash_entry_with_a_reserved_path_is_refused(self):
        def reserved_untracked():
            (self.source / '.world').write_text('not a marker\n')
            self.git(self.source, 'stash', 'push', '-q', '--include-untracked', '-m', 'reserved')
        def reserved_staged():
            (self.source / '.world-git').mkdir()
            (self.source / '.world-git' / 'x').write_text('staged\n')
            self.git(self.source, 'add', '-f', '.world-git/x')
            self.git(self.source, 'stash', 'push', '-q', '-m', 'reserved')
        for make in (reserved_untracked, reserved_staged):
            with self.subTest(make.__name__):
                make()
                # Buried below another entry: refs/stash itself is clean.
                (self.source / 'file').write_text('on top\n')
                self.git(self.source, 'stash', 'push', '-q', '-m', 'top')
                before = self.git(self.source, 'stash', 'list', '--format=%H %gs').stdout
                result = self.world('init', str(self.source), code=3)
                self.assertIn(b'reserved path', result.stderr)
                self.assertEqual(self.git(self.source, 'stash', 'list', '--format=%H %gs').stdout, before)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
                self.git(self.source, 'stash', 'clear')
        self.world('init', str(self.source))

    def test_stash_changed_after_capture_is_not_published(self):
        import shlex
        self.make_stash_stack(self.source)
        # Only the reflog changes: dropping stash@{1} leaves refs/stash where it was.
        top = self.git(self.source, 'rev-parse', 'refs/stash').stdout
        drop = shlex.quote(shutil.which('git')) + ' -C ' + shlex.quote(str(self.source)) + " stash drop -q 'stash@{1}'"
        wrapper, done = self.pack_refs_wrapper('drop', drop)
        path = self.env['PATH']
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        self.world('init', str(self.source), code=1)
        self.env['PATH'] = path
        self.assertTrue(done.exists())
        self.assertEqual(self.git(self.source, 'rev-parse', 'refs/stash').stdout, top)
        self.assertEqual(len(self.git(self.source, 'stash', 'list').stdout.splitlines()), 2)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        view = self.stash_view(self.source)
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.stash_view(one), view)

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
                script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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
            'submodule': '[submodule "lib"]\n update = !touch /nonexistent\n',
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

    def test_conditional_rewrite_chain_across_includes_is_refused(self):
        # A source-side conditional rule pins raw.example -> first.example (the source's own
        # Git chains no further than that). A second includeIf rule -- inactive at the source,
        # so it plays no part in that resolution -- maps first.example -> second.example. If the
        # chain guard only considered rules active in the source's ambient configuration, the
        # World would carry the pinned first.example URL unguarded, and once placed somewhere
        # the second rule activates, it would contact second.example instead.
        included_active = self.root / 'chain-active'
        included_active.write_text('[url "https://first.example/"]\n insteadOf = https://raw.example/\n')
        included_inactive = self.root / 'chain-inactive'
        included_inactive.write_text('[url "https://second.example/"]\n insteadOf = https://first.example/\n')
        global_config = self.root / 'chain-global'
        global_config.write_text(
            '[includeIf "gitdir:' + str(self.source) + '/"]\n path = ' + str(included_active) + '\n'
            '[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ' + str(included_inactive) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', 'https://raw.example/r.git')
        # Sanity: the source's own Git chains only into the first, active rule.
        self.assertEqual(self.git(self.source, 'remote', 'get-url', 'origin').stdout.strip(),
                          b'https://first.example/r.git')
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'would rewrite again', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_inactive_conditional_rewrite_of_carried_remote_is_refused(self):
        # No rule is active at the source, so this remote is carried as-is, not pinned -- but an
        # inactive includeIf rule that matches its raw URL could still activate once the World
        # moves, so it must be refused just as a pinned URL matched again would be.
        included = self.root / 'unpinned-inactive'
        included.write_text('[url "https://elsewhere.example/"]\n insteadOf = https://plain.example/\n')
        global_config = self.root / 'unpinned-inactive-global'
        global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = '
                                  + str(included) + '\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.git(self.source, 'remote', 'add', 'origin', 'https://plain.example/r.git')
        # Sanity: the rule is inactive here, so the source's own Git does not rewrite it.
        self.assertEqual(self.git(self.source, 'remote', 'get-url', 'origin').stdout.strip(),
                          b'https://plain.example/r.git')
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'matches url.', result.stderr)
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
        self.assertIn(b'remote origin has url in global configuration', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_carried_remote_with_only_ambient_url_is_refused(self):
        # A remote can be established locally by a carried key other than url/pushurl -- here,
        # remote.origin.fetch is local but remote.origin.url only exists in global
        # configuration. A remote is one unit: since this remote has a carried repository-local
        # setting at all, its URL must also come only from repository-local configuration, even
        # though there is no local url/pushurl to pin against. Without this, the World would
        # contact the raw ambient URL while the source, under a source-location includeIf rule,
        # might contact a rewritten one.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        self.git(self.source, 'config', '--unset', 'remote.origin.url')
        self.git(self.source, 'config', 'remote.origin.fetch', '+refs/heads/*:refs/remotes/origin/*')
        # Sanity: only .fetch is local now, no url/pushurl.
        self.assertEqual(self.git(self.source, 'config', '--local', '--get-regexp',
                                   '^remote\\.origin\\.').stdout.strip(),
                          b'remote.origin.fetch +refs/heads/*:refs/remotes/origin/*')
        global_config = self.root / 'remote-fetch-only-global'
        global_config.write_text('[remote "origin"]\n url = https://global.invalid/x.git\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        result = self.world('init', str(self.source), code=3)
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        self.assertIn(b'remote origin has url in global configuration', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_ambient_url_for_unrelated_remote_is_carried_normally(self):
        # A remote name with no carried repository-local settings at all is unaffected by the
        # ambient-URL refusal: only a remote that itself has carried repository-local settings
        # must keep its URL local too.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/x.git')
        global_config = self.root / 'remote-unrelated-global'
        global_config.write_text('[remote "other"]\n url = https://global.invalid/other.git\n')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        self.world('init', str(self.source))
        self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', '--get', 'remote.origin.url').stdout.strip(),
                          b'https://example.invalid/x.git')

    def test_remote_change_during_mirror_aborts_publication(self):
        import shlex
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.invalid/before.git')
        real_git = shutil.which('git')
        wrapper = self.root / 'remote-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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
        script.write_text('#!/bin/sh\nmirror=0\nfor arg in "$@"; do [ "$arg" = pack-refs ] && mirror=1; done\n'
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

    def test_unsupported_nested_repository_is_refused(self):
        nested = self.source / 'nested'
        nested.mkdir()
        self.git(nested, 'init')
        # A self-contained one is carried as files; one reaching outside its .git is refused.
        (nested / '.git/commondir').write_text('../../elsewhere\n')
        self.world('init', str(self.source), '--include-changes', code=3)
        shutil.rmtree(nested)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def pool_ready(self, snapshot='S1'):
        rows = json.loads(self.world('pool', 'status', '--json').stdout)['pool']
        return sum(r['ready'] for r in rows if r['snapshot'] == snapshot)

    def pool_fork(self, name, pooled):
        path = self.root / name
        out = self.world('fork', '--from', 'S1', '--to', str(path)).stdout
        self.assertEqual(b'(pool)' in out, pooled, out)
        return path, out.decode().split()[0]

    def test_hard_snapshot_pool(self):
        init_args = ('--hard',) if sys.platform == 'darwin' else ()
        self.world('init', str(self.source), *init_args)
        self.world('pool', 'fill', 'S1', '--count', '1')
        one, _ = self.pool_fork('one', True)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'branch', '--show-current').stdout.strip(), b'world/W1')
        self.git(one, 'fsck', '--full')
        two, _ = self.pool_fork('two', False)
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.world('verify', 'S1')

    def test_git_snapshot_pool_hands_out_ordinary_git_worlds(self):
        self.world('init', str(self.source))
        self.world('pool', 'fill', 'S1', '--count', '2')
        self.assertEqual(self.pool_ready(), 2)
        self.world('verify', 'S1')
        one, w1 = self.pool_fork('one', True)
        two, w2 = self.pool_fork('two', True)
        self.assertEqual(self.pool_ready(), 0)
        three, w3 = self.pool_fork('three', False)   # the ordinary path, for comparison
        for world, wid in ((one, w1), (two, w2), (three, w3)):
            self.assertEqual(self.git(world, 'status', '--porcelain').stdout, b'')
            self.assertEqual(self.git(world, 'branch', '--show-current').stdout.decode().strip(), 'world/' + wid)
            info = json.loads(self.world('inspect', wid, '--json').stdout)['git']
            self.assertEqual(info['baseline'].encode(), self.base)
            self.assertEqual(info['branch'], 'world/' + wid)
            self.assertEqual(info['git_dir'], str(world / '.world-git/repo.git'))
            self.assertIn(str(world).encode(), self.git(world, 'worktree', 'list', '--porcelain').stdout)
            self.git(world, 'fsck', '--full')
        # A handed-out World is configured exactly like an ordinary fork.
        config = lambda w: self.git(w, 'config', '--local', '--list').stdout
        self.assertEqual(config(one), config(three))
        self.assertEqual(config(two), config(three))
        # Independent refs, indexes and commits.
        (one / 'file').write_text('one\n')
        self.git(one, 'commit', '-qam', 'one')
        (two / 'new').write_text('two\n')
        self.git(two, 'add', 'new')
        self.assertEqual(self.git(two, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.assertEqual(self.git(three, 'status', '--porcelain').stdout, b'')
        self.git(two, 'rev-parse', '--verify', 'refs/heads/world/W1', code=128)
        self.git(one, 'rev-parse', '--verify', 'refs/heads/world/W2', code=128)
        self.assertEqual(self.git(self.source, 'rev-parse', 'HEAD').stdout.strip(), self.base)
        self.world('verify', 'S1')

    def test_git_pool_setup_failure_discards_the_entry(self):
        import shlex
        self.world('init', str(self.source))
        entries = self.store / 'pool' / 'S1'
        listing = lambda: sorted(p.name for p in entries.iterdir()) if entries.exists() else []
        self.world('pool', 'fill', 'S1', '--count', '1')
        self.assertEqual(len(listing()), 1)
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
        # The entry the failed setup touched is gone, not back in the pool.
        self.assertEqual(self.pool_ready(), 0)
        self.assertEqual(listing(), [])
        self.env['PATH'] = self.env['PATH'].split(os.pathsep, 1)[1]
        one, _ = self.pool_fork('one', False)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        # An entry whose setup refuses it is dropped, and the fork is served by a fresh clone.
        self.world('pool', 'fill', 'S1', '--count', '1')
        [entry] = listing()
        (entries / entry / '.git').write_text('gitdir: elsewhere\n')
        two, wid = self.pool_fork('two', False)
        self.assertEqual((two / '.git').read_text(), 'gitdir: .world-git/repo.git/worktrees/active\n')
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(two, 'branch', '--show-current').stdout.decode().strip(), 'world/' + wid)
        self.assertEqual(self.pool_ready(), 0)
        self.assertEqual(listing(), [])
        # So is an entry whose marker disappeared: it would otherwise publish without a worktree.
        self.world('pool', 'fill', 'S1', '--count', '1')
        [entry] = listing()
        (entries / entry / '.git').unlink()
        three, wid = self.pool_fork('three', False)
        self.assertEqual((three / '.git').read_text(), 'gitdir: .world-git/repo.git/worktrees/active\n')
        self.assertEqual(self.git(three, 'branch', '--show-current').stdout.decode().strip(), 'world/' + wid)
        self.assertEqual(self.pool_ready(), 0)
        self.assertEqual(listing(), [])
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

    # ---- world exec: the World's Git hooks and command-running settings ----

    def exec_sh(self, wid, script, *opts, code=0):
        return self.run_cmd(WORLD, 'exec', wid, *opts, '--', '/bin/sh', '-c', script, code=code)

    def assert_denied(self, wid, script):
        """`script` fails inside the sandbox (EPERM under seatbelt, EROFS/EBUSY under bwrap)."""
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c', script),
                           env=self.env, capture_output=True, timeout=60)
        self.assertNotEqual(p.returncode, 0, (script, p.stdout, p.stderr))
        return p

    def test_exec_sandbox_denies_hooks_but_not_configuration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        repo = one / '.world-git/repo.git'
        hooks = repo / 'hooks'
        hooks.mkdir(exist_ok=True)
        (hooks / 'post-commit').write_text('#!/bin/sh\necho kept-hook-ran\n')
        (hooks / 'post-commit').chmod(0o755)
        dot_git = (one / '.git').read_bytes()
        self.assert_denied(wid, 'echo evil > .world-git/repo.git/hooks/pre-commit')
        self.assert_denied(wid, 'echo evil >> .world-git/repo.git/hooks/post-commit')
        self.assert_denied(wid, 'mkdir hooks2 && mv .world-git/repo.git/hooks hooks-old')
        self.assert_denied(wid, 'rm -rf .world-git/repo.git/hooks')
        # Nor can the command swap the repository, or where the World's `.git` points.
        self.assert_denied(wid, 'mv .world-git/repo.git .world-git/other')
        self.assert_denied(wid, 'mv .world-git/repo.git/worktrees/active .world-git/repo.git/worktrees/x')
        self.assert_denied(wid, 'echo "gitdir: /tmp" > .git')
        self.assert_denied(wid, 'echo /tmp > .world-git/repo.git/worktrees/active/commondir')
        self.assertFalse((hooks / 'pre-commit').exists())
        self.assertEqual((hooks / 'post-commit').read_text(), '#!/bin/sh\necho kept-hook-ran\n')
        self.assertEqual((one / '.git').read_bytes(), dot_git)
        # Everyday work, including Git writing its own configuration, is unaffected, and the
        # World's hooks still run inside the exec.
        p = self.exec_sh(wid, 'echo work > file && git add file && git commit -qm work && '
                              'git config user.email agent@example.com && '
                              'git config branch.main.description x', '--require-sandbox')
        self.assertIn(b'kept-hook-ran', p.stdout + p.stderr)
        self.assertNotIn(b'WARNING', p.stderr)
        self.assertEqual(self.git(one, 'config', 'user.email').stdout.strip(), b'agent@example.com')
        self.assertEqual(self.git(one, 'log', '-1', '--format=%s').stdout.strip(), b'work')
        # A setting that makes Git run a command is written, then reported, with the exit code kept.
        p = self.exec_sh(wid, "git config core.fsmonitor 'touch /tmp/x' && exit 7", '--require-sandbox', code=7)
        self.assertIn(b"world: WARNING: exec changed a Git setting that runs commands: "
                      b"local core.fsmonitor: (unset) -> touch /tmp/x\n", p.stderr)
        p = self.exec_sh(wid, "git config --unset core.fsmonitor && git config alias.x '!sh -c evil' && "
                              "git config alias.co checkout", '--require-sandbox')
        self.assertIn(b'local core.fsmonitor: touch /tmp/x -> (unset)\n', p.stderr)
        self.assertIn(b'local alias.x: (unset) -> !sh -c evil\n', p.stderr)
        self.assertIn(b'local alias.co: (unset) -> checkout', p.stderr)
        # A core.hooksPath outside the tree is guarded like the hooks directory; one inside the
        # tree (husky's .husky) stays writable, but its changes are still reported.
        shared = self.root / 'shared-hooks'
        shared.mkdir()
        self.git(one, 'config', 'core.hooksPath', str(shared))
        self.assert_denied(wid, 'echo evil > ' + shlex.quote(str(shared / 'pre-commit')))
        self.git(one, 'config', 'core.hooksPath', '.husky')
        p = self.exec_sh(wid, 'mkdir -p .husky && echo lint > .husky/pre-commit', '--require-sandbox')
        self.assertIn(b'exec added a Git hook: .husky/pre-commit', p.stderr)
        self.git(one, 'config', '--unset', 'core.hooksPath')
        # A World path with regex metacharacters still matches only itself.
        odd, oid = self.fork('w.i+r(d)[x]{2}$^|?*')
        (odd / '.world-git/repo.git/hooks').mkdir(exist_ok=True)
        self.assert_denied(oid, 'echo evil > .world-git/repo.git/hooks/pre-commit')
        if sys.platform != 'darwin':
            return  # bwrap binds paths, and everything outside the World is read-only anyway
        lookalike = self.root / 'wXiirdxx/.world-git/repo.git/worktrees/active/modules/m/hooks'
        lookalike.mkdir(parents=True)
        self.exec_sh(oid, 'echo fine > ' + shlex.quote(str(lookalike / 'pre-commit')), '--require-sandbox')

    def test_exec_reports_hooks_and_settings_without_sandbox(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        p = self.exec_sh(wid, 'mkdir -p .world-git/repo.git/hooks && '
                              'printf "#!/bin/sh\\n" > .world-git/repo.git/hooks/pre-commit && '
                              'git config credential.helper "!f() { cat ~/.token; }; f" && '
                              'git config --add credential.helper store && exit 3', '--no-sandbox', code=3)
        self.assertIn(b'world: WARNING: exec added a Git hook: .world-git/repo.git/hooks/pre-commit\n', p.stderr)
        self.assertIn(b'world: WARNING: exec changed a Git setting that runs commands: local credential.helper: '
                      b'(unset) -> !f() { cat ~/.token; }; f, store\n', p.stderr)
        p = self.exec_sh(wid, 'echo "echo changed" >> .world-git/repo.git/hooks/pre-commit', '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed a Git hook: .world-git/repo.git/hooks/pre-commit\n', p.stderr)
        p = self.exec_sh(wid, 'rm .world-git/repo.git/hooks/pre-commit && touch .world-git/repo.git/hooks/x.sample',
                         '--no-sandbox')
        self.assertEqual(p.stderr.count(b'WARNING'), 1, p.stderr)
        self.assertIn(b'world: WARNING: exec removed a Git hook: .world-git/repo.git/hooks/pre-commit\n', p.stderr)
        # Effective configuration: a change to the global file the World includes is reported too.
        glob = self.root / 'global-config'
        glob.write_text('')
        self.env['GIT_CONFIG_GLOBAL'] = str(glob)
        p = self.exec_sh(wid, 'git config --global core.pager "less; evil"', '--no-sandbox')
        self.assertIn(b'global core.pager: (unset) -> less; evil\n', p.stderr)
        # A rewritten `.git` is reported: Git would find another repository and its hooks.
        p = self.exec_sh(wid, 'cp .git .git.orig && echo "gitdir: /tmp/elsewhere" > .git', '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed where Git finds a repository: .git '
                      b'(a file naming its repository -> a file naming another repository)\n', p.stderr)
        (one / '.git.orig').rename(one / '.git')
        # So is replacing it with another entry type: a new repository (whose settings the
        # World's own administration does not show), a symlink, or nothing at all.
        p = self.exec_sh(wid, "mv .git .git.saved && git init -q . && git config core.fsmonitor 'echo bad'",
                         '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed where Git finds a repository: .git '
                      b'(a file naming its repository -> a directory)\n', p.stderr)
        shutil.rmtree(one / '.git')
        p = self.exec_sh(wid, 'ln -s .git.saved .git', '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed where Git finds a repository: .git (missing -> a symlink)\n',
                      p.stderr)
        p = self.exec_sh(wid, 'rm .git && mv .git.saved .git', '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed where Git finds a repository: .git '
                      b'(a symlink -> a file naming its repository)\n', p.stderr)
        p = self.exec_sh(wid, 'mv .git .git.saved', '--no-sandbox')
        self.assertIn(b'world: WARNING: exec removed .git, which told Git where a repository is\n', p.stderr)
        (one / '.git.saved').rename(one / '.git')

    def test_exec_warns_about_preexisting_redirected_pointer_coverage(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        alternate = self.root / 'alternate-repository'
        self.git(self.root, 'init', str(alternate))
        dotgit = one / '.git'
        original = dotgit.read_bytes()
        dotgit.write_text('gitdir: ' + str(alternate / '.git') + '\n')
        redirected = dotgit.read_bytes()
        p = self.exec_sh(wid, "git config core.sshCommand 'touch redirected-helper-ran'; exit 7",
                         '--no-sandbox', code=7)
        self.assertIn(b'WARNING: Git guard coverage is incomplete before exec:', p.stderr)
        self.assertIn(b'WARNING: Git guard coverage is incomplete after exec:', p.stderr)
        self.assertIn(b'target Git hooks and settings were not fully inspected', p.stderr)
        self.assertEqual(dotgit.read_bytes(), redirected)
        self.assertEqual(self.git(alternate, 'config', 'core.sshCommand').stdout.strip(),
                         b'touch redirected-helper-ran')
        self.assertNotIn(b'local core.sshcommand:', p.stderr)
        self.assertFalse((one / 'redirected-helper-ran').exists())
        # Coverage warnings must not discard owned snapshots or the repair report.
        p = self.exec_sh(wid, 'printf %s ' + shlex.quote(original.decode()) + ' > .git; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'WARNING: Git guard coverage is incomplete before exec:', p.stderr)
        self.assertNotIn(b'Git guard coverage is incomplete after exec:', p.stderr)
        self.assertIn(b'exec changed where Git finds a repository: .git ', p.stderr)
        self.assertEqual(dotgit.read_bytes(), original)
        # Missing/foreign mandatory commondir must warn before metadata queries,
        # including when those queries cannot produce an initial snapshot.
        commondir = one / '.world-git/repo.git/worktrees/active/commondir'
        original_common = commondir.read_bytes()
        for contents in (str(alternate / '.git').encode() + b'\n', None):
            with self.subTest(commondir=contents):
                if contents is None:
                    commondir.unlink()
                else:
                    commondir.write_bytes(contents)
                p = self.exec_sh(wid, 'touch command-completed; exit 7', '--no-sandbox', code=7)
                self.assertIn(b'WARNING: Git guard coverage is incomplete before exec:', p.stderr)
                self.assertTrue((one / 'command-completed').exists())
                (one / 'command-completed').unlink()
                commondir.write_bytes(original_common)

    def test_exec_refuses_preexisting_redirected_git_pointers(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        alternate = self.root / 'alternate.git'
        self.git(self.root, 'init', '--bare', str(alternate))
        dotgit = one / '.git'
        commondir = one / '.world-git/repo.git/worktrees/active/commondir'
        for pointer, prefix in ((dotgit, b'gitdir: '), (commondir, b'')):
            original = pointer.read_bytes()
            saved = pointer.with_name(pointer.name + '.saved')
            saved.write_bytes(original)
            cases = ('redirected-file', 'directory', 'symlink', 'embedded-newline', 'nul', 'oversized')
            for case in cases:
                with self.subTest(pointer=pointer.name, case=case):
                    pointer.unlink()
                    if case == 'directory':
                        pointer.mkdir()
                    elif case == 'symlink':
                        pointer.symlink_to(saved)
                    elif case == 'redirected-file':
                        pointer.write_bytes(prefix + str(alternate).encode() + b'\n')
                    elif case == 'embedded-newline':
                        pointer.write_bytes(original.rstrip(b'\r\n') + b'\n/alternate\n')
                    elif case == 'nul':
                        pointer.write_bytes(original.rstrip(b'\r\n') + b'\0ignored\n')
                    else:
                        pointer.write_bytes(original.rstrip(b'\r\n') + b'x' * 8192)
                    p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                    self.assertIn(b'Git repository pointer is missing, redirected or unsupported', p.stderr)
                    self.assertFalse((one / 'should-not-run').exists())
                    if pointer.is_dir() and not pointer.is_symlink():
                        pointer.rmdir()
                    else:
                        pointer.unlink()
                    pointer.write_bytes(original)
            saved.unlink()
        # Trailing CR/LF and a canonical absolute target are valid Git pointer syntax.
        dotgit.write_text('gitdir: ' + str(one / '.world-git/repo.git/worktrees/active') + '\r\n')
        self.exec_sh(wid, ':', '--require-sandbox')

    def test_exec_refuses_hardlinked_git_pointers_but_allows_observational_repair(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        pointers = (one / '.git', one / '.world-git/repo.git/worktrees/active/commondir')
        for pointer in pointers:
            with self.subTest(pointer=pointer.name):
                original = pointer.read_bytes()
                alias = one / 'pointer-alias'
                os.link(pointer, alias)
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'Git repository pointer is missing, redirected or unsupported', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                self.assertEqual(pointer.read_bytes(), original)
                # A valid but changed pointer can still be inspected and repaired via an
                # alias with --no-sandbox; observational parsing must not reject its links.
                alias.write_bytes(original + b'\r\n')
                script = 'printf %s ' + shlex.quote(original.decode()) + ' > pointer-alias; exit 7'
                p = self.exec_sh(wid, script, '--no-sandbox', code=7)
                self.assertIn(b'exec changed where Git finds a repository: ' +
                              str(pointer.relative_to(one)).encode(), p.stderr)
                self.assertEqual(pointer.read_bytes(), original)
                alias.unlink()

    def test_exec_reports_redirected_worktree_hooks_and_archive_command(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'extensions.worktreeConfig', 'true')
        self.git(one, 'config', '--worktree', 'core.bare', 'false')
        self.git(one, 'config', 'core.hooksPath', '.husky')
        hidden = one / '.world-git/hidden'
        hidden.mkdir()
        script = ('git config --worktree core.worktree ' + shlex.quote(str(hidden)) +
                  ' && mkdir -p .world-git/hidden/.husky && '
                  'echo evil > .world-git/hidden/.husky/pre-commit && exit 7')
        p = self.exec_sh(wid, script, '--no-sandbox', code=7)
        self.assertEqual(self.git(one, 'rev-parse', '--show-toplevel').stdout.strip(), str(hidden).encode())
        self.assertIn(b'worktree core.worktree:', p.stderr)
        self.assertIn(b'exec added a Git hook: .world-git/hidden/.husky/pre-commit', p.stderr)
        self.assertNotIn(b'Git guard coverage is incomplete', p.stderr)  # optional checkout .git is absent
        self.assert_denied(wid, 'echo changed > .world-git/hidden/.husky/pre-commit')
        # Git ignores command-scope core.worktree for checkout setup; the guard must
        # protect the Git-resolved checkout rather than the last config-list value.
        external = self.root / 'external-checkout'
        (external / '.husky').mkdir(parents=True)
        self.env.update(GIT_CONFIG_COUNT='3', GIT_CONFIG_KEY_2='core.worktree',
                        GIT_CONFIG_VALUE_2=str(external))
        self.assertEqual(self.git(one, 'rev-parse', '--show-toplevel').stdout.strip(), str(hidden).encode())
        self.assert_denied(wid, 'echo changed > .world-git/hidden/.husky/pre-commit')
        p = self.exec_sh(wid, "git config tar.custom.command 'sh -c evil'", '--no-sandbox')
        self.assertIn(b'local tar.custom.command: (unset) -> sh -c evil', p.stderr)
        p = self.exec_sh(wid, "git config gc.recentObjectsHook 'sh -c evil'", '--no-sandbox')
        self.assertIn(b'local gc.recentobjectshook: (unset) -> sh -c evil', p.stderr)

    def test_exec_refuses_incomplete_git_administration_scan(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        modules = one / '.world-git/repo.git/worktrees/active/modules'
        # Legal short components exceed the discovery depth without exceeding PATH_MAX.
        deep = modules.joinpath(*(['a'] * 34))
        deep.mkdir(parents=True)
        (deep / 'HEAD').write_text('ref: refs/heads/main\n')
        (deep / 'objects').mkdir()
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'incomplete Git administration scan', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        p = self.exec_sh(wid, 'exit 7', '--no-sandbox', code=7)
        self.assertIn(b'incomplete Git administration scan', p.stderr)
        shutil.rmtree(modules)
        # macOS PATH_MAX equals the guard's buffer; Linux allows this longer path.
        if sys.platform == 'darwin':
            return
        # A short-depth path can independently exceed the guard's fixed path buffers.
        long_path = modules.joinpath(*(['b' * 180] * 6))
        long_path.mkdir(parents=True)
        p = self.exec_sh(wid, 'exit 7', '--no-sandbox', code=7)
        self.assertIn(b'incomplete Git administration scan', p.stderr)

    def test_exec_refuses_hardlinked_guarded_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/pre-commit'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\nexit 0\n')
        hook.chmod(0o755)
        alias = one / 'writable-hook-alias'
        os.link(hook, alias)
        original = hook.read_bytes()
        p = self.exec_sh(wid, 'touch should-not-run; echo changed > writable-hook-alias',
                         '--require-sandbox', code=3)
        self.assertIn(b'hardlinked Git hooks cannot be guarded', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        self.assertEqual(hook.read_bytes(), original)
        p = self.exec_sh(wid, 'echo changed > writable-hook-alias; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'hardlinked Git hooks cannot be guarded', p.stderr)
        self.assertEqual(hook.read_bytes(), b'changed\n')

    def test_exec_refuses_symlinked_guarded_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        target = self.root / 'external-hook'
        target.write_text('#!/bin/sh\nexit 0\n')
        target.chmod(0o755)
        hook = one / '.world-git/repo.git/hooks/pre-commit'
        hook.parent.mkdir(exist_ok=True)
        hook.symlink_to(target)
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'symlinked Git hooks cannot be guarded', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        p = self.exec_sh(wid, 'exit 7', '--no-sandbox', code=7)
        self.assertIn(b'symlinked Git hooks cannot be guarded', p.stderr)

        # The default hooks directory itself must not redirect outside its policy either.
        shutil.rmtree(hook.parent)
        external = self.root / 'external-hooks'
        external.mkdir()
        (external / 'pre-commit').write_text(target.read_text())
        hook.parent.symlink_to(external, target_is_directory=True)
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'symlinked Git hooks cannot be guarded', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_reports_nested_hook_support_files_without_changing_wrapper(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        roots = (one / '.world-git/repo.git/hooks', one / '.project-hooks')
        self.git(one, 'config', 'core.hooksPath', '.project-hooks')
        wrappers = []
        for root in roots:
            (root / 'lib/deep').mkdir(parents=True, exist_ok=True)
            (root / 'helpers.sample').mkdir()
            wrapper = root / 'pre-commit'
            wrapper.write_text('#!/bin/sh\n. "$(dirname "$0")/lib/deep/helper"\n')
            wrapper.chmod(0o755)
            wrappers.append((wrapper, wrapper.read_bytes()))
            (root / 'lib/deep/helper').write_text('echo before\n')
            (root / 'lib/deep/remove-me').write_text('old\n')
            (root / 'lib/deep/nested.sample').write_text('old\n')
            (root / 'helpers.sample/helper').write_text('old\n')
            (root / 'ignored.sample').write_text('old template\n')
        commands = []
        for root in roots:
            for relative in ('lib/deep/helper', 'lib/deep/nested.sample', 'helpers.sample/helper'):
                commands.append('printf %s ' + shlex.quote('echo after!\n') + ' > ' + shlex.quote(str(root / relative)))
            commands.extend(('echo added > ' + shlex.quote(str(root / 'lib/deep/new-helper')),
                             'rm ' + shlex.quote(str(root / 'lib/deep/remove-me')),
                             'echo template > ' + shlex.quote(str(root / 'ignored.sample'))))
        p = self.exec_sh(wid, ' && '.join(commands) + '; exit 7', '--no-sandbox', code=7)
        for root in roots:
            prefix = str(root.relative_to(one)).encode() + b'/'
            for relative in (b'lib/deep/helper', b'lib/deep/nested.sample', b'helpers.sample/helper'):
                self.assertIn(b'exec changed a Git hook: ' + prefix + relative, p.stderr)
            self.assertIn(b'exec added a Git hook: ' + prefix + b'lib/deep/new-helper', p.stderr)
            self.assertIn(b'exec removed a Git hook: ' + prefix + b'lib/deep/remove-me', p.stderr)
            self.assertNotIn(prefix + b'ignored.sample', p.stderr)
        for wrapper, original in wrappers:
            self.assertEqual(wrapper.read_bytes(), original)
            self.assertNotIn(b'a Git hook: ' + str(wrapper.relative_to(one)).encode() + b'\n', p.stderr)
        # Project hook support stays writable with a sandbox; its bytes are still reported.
        p = self.exec_sh(wid, 'echo edited > .project-hooks/lib/deep/helper', '--require-sandbox')
        self.assertIn(b'exec changed a Git hook: .project-hooks/lib/deep/helper', p.stderr)

    def test_exec_refuses_nested_hook_aliases_and_excess_depth(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        helpers = one / '.world-git/repo.git/hooks/lib'
        helpers.mkdir(parents=True, exist_ok=True)
        outside = one / 'external-helper'
        outside.write_text('payload\n')
        for kind in ('symlink-file', 'symlink-directory', 'hardlink'):
            with self.subTest(kind=kind):
                entry = helpers / 'unsupported'
                if kind == 'hardlink':
                    os.link(outside, entry)
                else:
                    entry.symlink_to(one if kind == 'symlink-directory' else outside,
                                     target_is_directory=kind == 'symlink-directory')
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'linked Git hooks cannot be guarded', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                entry.unlink()
        deep = helpers
        for _ in range(33):
            deep = deep / 'd'
            deep.mkdir()
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'Git hook directory depth limit exceeded', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_large_sparse_hook_invalidates_capture_promptly(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/pre-commit'
        hook.parent.mkdir(exist_ok=True)
        script = (shlex.quote(sys.executable) + ' -c ' + shlex.quote(
            "with open('.world-git/repo.git/hooks/pre-commit', 'wb') as f: f.truncate(1 << 40)") +
            '; exit 7')
        p = subprocess.run((WORLD, 'exec', wid, '--no-sandbox', '--', '/bin/sh', '-c', script),
                           env=self.env, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 7, p.stderr)
        self.assertEqual(hook.stat().st_size, 1 << 40)
        self.assertIn(b'64 MiB capture budget', p.stderr)
        self.assertIn(b'WARNING: exec left', p.stderr)
        self.assertIn(b'Git hooks and settings uninspected after the command', p.stderr)
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'incomplete Git hooks or administration capture', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_warns_when_new_project_hook_exceeds_capture_budget(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.hooksPath', '.hidden-hooks')
        (one / '.gitignore').write_text('.hidden-hooks/\n')
        script = (shlex.quote(sys.executable) + ' -c ' + shlex.quote(
            "import os; os.mkdir('.hidden-hooks'); "
            "f = open('.hidden-hooks/pre-commit', 'wb'); f.truncate(1 << 40); f.close()") + '; exit 7')
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c', script),
                           env=self.env, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 7, p.stderr)
        self.assertEqual((one / '.hidden-hooks/pre-commit').stat().st_size, 1 << 40)
        self.assertIn(b'WARNING: exec left', p.stderr)
        self.assertIn(b'Git hooks and settings uninspected after the command', p.stderr)
        self.assertIn(b'64 MiB capture budget', p.stderr)

    def test_exec_reports_bundle_uri_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        p = self.exec_sh(wid, 'git config fetch.bundleURI https://example.invalid/bootstrap.bundle; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local fetch.bundleuri: (unset) -> https://example.invalid/bootstrap.bundle', p.stderr)

    def test_exec_reports_dormant_worktree_config_and_its_includes(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        root_admin = one / '.world-git/repo.git/worktrees/active'
        child_admin = root_admin / 'modules/lib-module'
        for repo in (one, one / 'libs/lib'):
            self.git(repo, 'config', 'extensions.worktreeConfig', 'false')
        root_config = root_admin / 'config.worktree'
        child_config = child_admin / 'config.worktree'
        p = self.exec_sh(wid, "printf '[core]\\n sshCommand = touch dormant-worktree-ran\\n' > " +
                         shlex.quote(str(root_config)) + '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'exec added Git worktree configuration: .world-git/repo.git/worktrees/active/config.worktree',
                      p.stderr)
        self.assertNotIn(b'worktree core.sshcommand:', p.stderr)
        root_config.write_text('[includeIf "onbranch:never-active"]\n path = dormant-parent\n')
        policy = root_admin / 'dormant-parent'
        nested = root_admin / 'dormant-leaf'
        policy.write_text('[include]\n path = dormant-leaf\n')
        nested.write_text('[core]\n sshCommand = touch dormant-worktree-ran\n')
        p = self.exec_sh(wid, "printf '\\n[alias]\\n hidden = !touch dormant-worktree-ran\\n' >> " +
                         shlex.quote(str(nested)) + " && printf '[core]\\n fsmonitor = touch dormant-worktree-ran\\n' > " +
                         shlex.quote(str(child_config)), '--require-sandbox')
        self.assertIn(b'exec changed a Git include target: .world-git/repo.git/worktrees/active/dormant-leaf', p.stderr)
        self.assertIn(b'exec added Git worktree configuration: .world-git/repo.git/worktrees/active/modules/lib-module/config.worktree',
                      p.stderr)
        p = self.exec_sh(wid, 'rm ' + shlex.quote(str(child_config)), '--require-sandbox')
        self.assertIn(b'exec removed Git worktree configuration:', p.stderr)
        self.assertFalse((one / 'dormant-worktree-ran').exists())
        self.assertFalse((one / 'libs/lib/dormant-worktree-ran').exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'Seatbelt protected hooks ancestor pins')
    def test_exec_pins_external_and_default_hook_ancestors(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        shared = self.root / 'shared'
        hooks = shared / 'hooks'
        hooks.mkdir(parents=True)
        (shared / 'cancelled').mkdir()
        hook = hooks / 'pre-commit'
        hook.write_text('#!/bin/sh\nexit 0\n')
        original = hook.read_bytes()
        self.git(one, 'config', 'core.hooksPath', str(shared / 'cancelled/../hooks'))
        self.assert_denied(wid, 'mv ' + shlex.quote(str(shared)) + ' ' + shlex.quote(str(self.root / 'shared-old')))
        self.assert_denied(wid, 'mv ' + shlex.quote(str(shared / 'cancelled')) + ' ' +
                           shlex.quote(str(shared / 'cancelled-old')))
        self.exec_sh(wid, 'echo allowed > ' + shlex.quote(str(shared / 'sibling')), '--require-sandbox')
        self.assertEqual(hook.read_bytes(), original)
        self.assertTrue((shared / 'sibling').exists())
        self.git(one, 'config', '--unset', 'core.hooksPath')
        # Default hooks remain protected even if an ancestor outside administration moves.
        moved = self.root / 'moved-world'
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c',
                            'mv ' + shlex.quote(str(one)) + ' ' + shlex.quote(str(moved))),
                           env=self.env, capture_output=True, timeout=60)
        if moved.exists():
            moved.rename(one)
        self.assertNotEqual(p.returncode, 0, p.stderr)
        missing = shared / 'not-created/hooks'
        self.git(one, 'config', 'core.hooksPath', str(missing))
        self.assert_denied(wid, 'mkdir ' + shlex.quote(str(missing.parent)))
        self.assertFalse(missing.parent.exists())

    def test_exec_reports_dormant_conditional_include_targets(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/dormant-policy'
        child = one / '.world-git/dormant-child'
        absent = one / '.world-git/dormant-new'
        policy.write_text('[includeIf "onbranch:never-active"]\n path = dormant-child\n'
                          '[include]\n path = dormant-new\n')
        child.write_text('[core]\n sshCommand = touch include-command-ran\n')
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(policy))
        self.assertNotIn(b'include-command-ran', self.git(one, 'config', '--list', '--includes').stdout)
        p = self.exec_sh(wid, "printf '[core]\\n sshCommand = touch changed-command-ran\\n' > " +
                         shlex.quote(str(child)) + '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'exec changed a Git include target: .world-git/dormant-child', p.stderr)
        self.assertNotIn(b'local core.sshcommand:', p.stderr)
        p = self.exec_sh(wid, "printf '[alias]\\n payload = !touch added-command-ran\\n' > " +
                         shlex.quote(str(absent)), '--require-sandbox')
        self.assertIn(b'exec added a Git include target: .world-git/dormant-new', p.stderr)
        # Content comparison deliberately also reports benign edits in dormant files.
        p = self.exec_sh(wid, "printf '\\n[user]\\n name = Changed\\n' >> " +
                         shlex.quote(str(policy)), '--require-sandbox')
        self.assertIn(b'exec changed a Git include target: .world-git/dormant-policy', p.stderr)
        p = self.exec_sh(wid, 'rm ' + shlex.quote(str(child)), '--require-sandbox')
        self.assertIn(b'exec removed a Git include target: .world-git/dormant-child', p.stderr)
        for marker in ('include-command-ran', 'changed-command-ran', 'added-command-ran'):
            self.assertFalse((one / marker).exists())

    def test_exec_protects_and_reports_dormant_include_hooks(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/dormant-hook-policy'
        nested = one / '.world-git/dormant-hook-child'
        policy.write_text('[includeIf "onbranch:also-never"]\n path = dormant-hook-child\n')
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(policy))
        # Both a writable administration subtree and an external directory need the
        # dormant policy's hooks protection (the latter is normally writable on macOS).
        paths = (one / '.world-git/custom/dormant-hooks', self.root / 'external-dormant-hooks')
        for hooks in paths:
            hooks.mkdir(parents=True)
            hook = hooks / 'pre-commit'
            hook.write_text('#!/bin/sh\ntouch dormant-hook-ran\n')
            hook.chmod(0o755)
            self.git(one, 'config', '--file', str(nested), '--add', 'core.hooksPath', str(hooks))
        unchanged = nested.read_bytes()
        self.assertNotIn(b'dormant-hooks', self.git(one, 'config', '--list', '--includes').stdout)
        for hooks in paths:
            hook = hooks / 'pre-commit'
            original = hook.read_bytes()
            # The marker proves a denial occurs in the child rather than initial capture.
            p = self.assert_denied(wid, 'touch sandbox-started; echo changed > ' + shlex.quote(str(hook)))
            self.assertTrue((one / 'sandbox-started').exists(), p.stderr)
            (one / 'sandbox-started').unlink()
            self.assertEqual(hook.read_bytes(), original)
            p = self.exec_sh(wid, 'echo changed >> ' + shlex.quote(str(hook)) + '; exit 7',
                             '--no-sandbox', code=7)
            self.assertIn(b'exec changed a Git hook:', p.stderr)
            self.assertIn(str(hooks.name).encode() + b'/pre-commit', p.stderr)
            self.assertNotIn(b'exec changed a Git include target:', p.stderr)
        self.assertEqual(nested.read_bytes(), unchanged)
        self.assertFalse((one / 'dormant-hook-ran').exists())

    def test_exec_dormant_hooks_use_each_checkout_and_receive_gitdir(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/shared-dormant-hooks'
        policy.write_text('[core]\n hooksPath = .dormant-hooks\n')
        repos = (one, one / 'libs/lib')
        for repo in repos:
            self.git(repo, 'config', 'includeIf.onbranch:never-active.path', str(policy))
        # Same include file, distinct repository contexts. Missing project directories
        # remain editable, but newly planted hooks must be observed in both checkouts.
        p = self.exec_sh(wid, 'mkdir .dormant-hooks libs/lib/.dormant-hooks; '
                         'echo root > .dormant-hooks/pre-commit; '
                         'echo child > libs/lib/.dormant-hooks/pre-commit; exit 7',
                         '--require-sandbox', code=7)
        self.assertIn(b'exec added a Git hook: .dormant-hooks/pre-commit', p.stderr)
        self.assertIn(b'exec added a Git hook: libs/lib/.dormant-hooks/pre-commit', p.stderr)
        self.assertNotIn(b'exec changed a Git include target:', p.stderr)
        admin = one / '.world-git/repo.git/worktrees/active'
        for gitdir in (admin, admin / 'modules/lib-module'):
            hooks = gitdir / '.dormant-hooks'
            hooks.mkdir()
            hook = hooks / 'pre-receive'
            hook.write_text('#!/bin/sh\nexit 0\n')
            p = self.assert_denied(wid, 'touch sandbox-started; echo changed > ' + shlex.quote(str(hook)))
            self.assertTrue((one / 'sandbox-started').exists(), p.stderr)
            (one / 'sandbox-started').unlink()
            self.assertEqual(hook.read_text(), '#!/bin/sh\nexit 0\n')

    def test_exec_dormant_hooks_cover_raw_alternate_worktree(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        admin = one / '.world-git/repo.git/worktrees/active'
        policy = one / '.world-git/dormant-alternate'
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(policy))
        # Git setup treats both path expansions literally in core.worktree, unlike
        # hooksPath. Capture must not apply --type=path to these candidate bases.
        for raw in ('~literal-worktree', '%(prefix)/literal-worktree'):
            with self.subTest(worktree=raw):
                hooks = admin / raw / '.alternate-hooks'
                hooks.mkdir(parents=True)
                hook = hooks / 'pre-commit'
                hook.write_text('#!/bin/sh\nexit 0\n')
                policy.write_text('[core]\n worktree = ' + raw + '\n hooksPath = .alternate-hooks\n')
                p = self.assert_denied(wid, 'touch sandbox-started; echo changed > ' + shlex.quote(str(hook)))
                self.assertTrue((one / 'sandbox-started').exists(), p.stderr)
                (one / 'sandbox-started').unlink()
                self.assertEqual(hook.read_text(), '#!/bin/sh\nexit 0\n')
                p = self.exec_sh(wid, 'echo changed >> ' + shlex.quote(str(hook)) + '; exit 7',
                                 '--no-sandbox', code=7)
                self.assertIn((raw + '/.alternate-hooks/pre-commit').encode(), p.stderr)
                self.assertIn(b'exec changed a Git hook:', p.stderr)
                self.assertNotIn(b'exec changed a Git include target:', p.stderr)

    def test_exec_dormant_empty_hooks_path_observes_execution_directory(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/dormant-empty-hooks'
        policy.write_text('[core]\n hooksPath =\n')
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(policy))
        p = self.exec_sh(wid, 'echo changed >> file; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'exec changed a Git hook: file', p.stderr)
        self.assertNotIn(b'exec changed a Git include target:', p.stderr)

    def test_exec_dormant_include_resolves_parent_alias_before_dotdot(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        admin = one / '.world-git'
        (admin / 'include-dir/deep').mkdir(parents=True)
        (admin / 'include-alias').symlink_to('include-dir/deep', target_is_directory=True)
        policy = admin / 'include-dir/policy'
        child = admin / 'include-dir/child'
        policy.write_text('[include]\n path = child\n')
        child.write_text('[alias]\n hidden = !touch nested-helper-ran\n')
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(admin / 'include-alias/../policy'))
        self.git(one, 'config', 'includeIf.onbranch:also-never.path', str(policy))
        p = self.exec_sh(wid, "printf '\\n[user]\\n name = changed\\n' >> " + shlex.quote(str(child)),
                         '--require-sandbox')
        warning = b'exec changed a Git include target: .world-git/include-dir/child'
        self.assertEqual(p.stderr.count(warning), 1, p.stderr)
        self.assertFalse((one / 'nested-helper-ran').exists())

    def test_exec_refuses_nonregular_dormant_include_targets(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        target = one / '.world-git/dormant-target'
        self.git(one, 'config', 'includeIf.onbranch:never-active.path', str(target))
        for kind in ('symlink', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink':
                    target.symlink_to(one / 'file')
                else:
                    os.mkfifo(target)
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'nonregular Git include target', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                target.unlink()

    def test_exec_reports_global_maintenance_registration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch scheduled-helper-ran')
        self.git(one, 'config', 'maintenance.prefetch.enabled', 'true')
        self.git(one, 'config', 'maintenance.prefetch.schedule', 'hourly')
        home = self.root / 'maintenance-home'
        home.mkdir()
        self.env['HOME'] = str(home)
        self.env['GIT_CONFIG_GLOBAL'] = str(home / '.gitconfig')
        p = self.exec_sh(wid, 'git config --global --add maintenance.repo ' + shlex.quote(str(one)) +
                         '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'global maintenance.repo: (unset) -> ' + str(one).encode(), p.stderr)
        self.assertNotIn(b'local maintenance.prefetch.enabled:', p.stderr)
        self.assertNotIn(b'local maintenance.prefetch.schedule:', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertEqual(self.git(one, 'config', '--global', '--get-all', 'maintenance.repo').stdout.strip(),
                         str(one).encode())
        self.assertFalse((one / 'scheduled-helper-ran').exists())

    def test_exec_reports_activating_maintenance_commands(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch maintenance-helper-ran')
        hook = one / '.world-git/repo.git/hooks/pre-auto-gc'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\ntouch maintenance-hook-ran\n')
        hook.chmod(0o755)
        values = {'receive.autogc': 'true', 'maintenance.auto': 'true',
                  'gc.auto': '1', 'gc.autoPackLimit': '1', 'maintenance.strategy': 'incremental',
                  'maintenance.gc.enabled': 'true', 'maintenance.prefetch.enabled': 'true',
                  'maintenance.prefetch.schedule': 'hourly', 'maintenance.gc.schedule': 'daily'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + value
                                        for key, value in values.items()) + '; exit 7', '--no-sandbox', code=7)
        for key, value in values.items():
            self.assertIn(b'local ' + key.lower().encode() + b': (unset) -> ' + value.encode(), p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertFalse((one / 'maintenance-helper-ran').exists())
        self.assertFalse((one / 'maintenance-hook-ran').exists())

    def test_exec_reports_enabling_previously_rejected_update_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/update'
        hook.parent.mkdir(exist_ok=True)
        marker = one / 'retained-update-hook-ran'
        hook.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        hook.chmod(0o755)
        original = hook.read_bytes()
        # Both receive-side rejection gates run before the retained update hook.
        # Changing or removing them can permit forced updates or branch deletions.
        for key in ('receive.denyNonFastForwards', 'receive.denyDeletes'):
            for change, after in ((key + ' false', 'false'), ('--unset ' + key, '(unset)')):
                with self.subTest(change=change):
                    self.git(one, 'config', key, 'true')
                    p = self.exec_sh(wid, 'git config ' + change + '; exit 7',
                                     '--no-sandbox', code=7)
                    self.assertIn(b'local ' + key.lower().encode() + b': true -> ' + after.encode(), p.stderr)
                    self.assertNotIn(b'a Git hook:', p.stderr)
                    self.assertEqual(hook.read_bytes(), original)
                    self.assertFalse(marker.exists())

        # Unlike denyDeletes, removing denyDeleteCurrent defaults to refusal.
        # Explicit false permits the current-branch deletion with this retained policy.
        self.git(one, 'config', 'receive.denyCurrentBranch', 'ignore')
        self.git(one, 'config', 'receive.denyDeleteCurrent', 'true')
        p = self.exec_sh(wid, 'git config receive.denyDeleteCurrent false; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local receive.denydeletecurrent: true -> false', p.stderr)
        self.assertNotIn(b'local receive.denycurrentbranch:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertEqual(hook.read_bytes(), original)
        self.assertFalse(marker.exists())

    def test_exec_reports_enabling_promisor_and_checkout_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch promisor-helper-ran')
        self.git(one, 'config', 'core.repositoryformatversion', '1')
        hook = one / '.world-git/repo.git/hooks/push-to-checkout'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\ntouch checkout-hook-ran\n')
        hook.chmod(0o755)
        p = self.exec_sh(wid, 'git config remote.dormant.promisor true && '
                         'git config remote.dormant.partialCloneFilter blob:none && '
                         'git config extensions.partialClone dormant && '
                         'git config receive.denyCurrentBranch updateInstead; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local remote.dormant.promisor: (unset) -> true', p.stderr)
        self.assertIn(b'local remote.dormant.partialclonefilter: (unset) -> blob:none', p.stderr)
        self.assertIn(b'local extensions.partialclone: (unset) -> dormant', p.stderr)
        self.assertIn(b'local receive.denycurrentbranch: (unset) -> updateInstead', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertFalse((one / 'promisor-helper-ran').exists())
        self.assertFalse((one / 'checkout-hook-ran').exists())

    def test_exec_reports_unhiding_refs_with_retained_transfer_commands(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        # uploadpack.packObjectsHook is honored only in protected configuration;
        # use the test's isolated global file, not an ignored local definition.
        self.env['GIT_CONFIG_GLOBAL'] = str(self.root / 'hidden-refs-global-config')
        pack_command = 'touch ' + shlex.quote(str(one / 'retained-pack-hook-ran')) + '; git pack-objects'
        self.git(one, 'config', '--global', 'uploadpack.packObjectsHook', pack_command)
        hook = one / '.world-git/repo.git/hooks/update'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(one / 'retained-hidden-ref-hook-ran')) + '\n')
        hook.chmod(0o755)
        original = hook.read_bytes()
        keys = ('uploadpack.hideRefs', 'receive.hideRefs', 'transfer.hideRefs')
        for after in ('(unset)', '!refs/heads/main'):
            with self.subTest(after=after):
                for key in keys:
                    self.git(one, 'config', key, 'refs/heads/main')
                commands = ('git config --unset ' + key if after == '(unset)' else
                            'git config ' + key + ' ' + shlex.quote(after) for key in keys)
                p = self.exec_sh(wid, ' && '.join(commands) + '; exit 7', '--no-sandbox', code=7)
                for key in keys:
                    self.assertIn(b'local ' + key.lower().encode() + b': refs/heads/main -> ' +
                                  after.encode(), p.stderr)
                self.assertNotIn(b'global uploadpack.packobjectshook:', p.stderr)
                self.assertNotIn(b'a Git hook:', p.stderr)
                self.assertEqual(self.git(one, 'config', '--global', 'uploadpack.packObjectsHook').stdout.strip(),
                                 pack_command.encode())
                self.assertEqual(hook.read_bytes(), original)
                self.assertFalse((one / 'retained-pack-hook-ran').exists())
                self.assertFalse((one / 'retained-hidden-ref-hook-ran').exists())

    def test_exec_reports_enabling_remote_archive_command(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'tar.custom.command', 'touch archive-command-ran; cat')
        self.git(one, 'config', 'tar.custom.remote', 'false')
        p = self.exec_sh(wid, 'git config tar.custom.remote true && '
                         'git config uploadarchive.allowUnreachable true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local tar.custom.remote: false -> true', p.stderr)
        self.assertIn(b'local uploadarchive.allowunreachable: (unset) -> true', p.stderr)
        self.assertNotIn(b'local tar.custom.command:', p.stderr)
        self.assertFalse((one / 'archive-command-ran').exists())

    def test_exec_reports_alias_and_remote_helper_configuration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        p = self.exec_sh(wid, "git config alias.probe '-c core.sshCommand=./payload ls-remote origin' && "
                         "git config remote.agent.url 'ext::sh -c evil' && "
                         "git config remote.agent.pushurl 'custom://repository' && "
                         "git config submodule.agent.url 'ext::sh -c evil' && "
                         "git config 'url.ext::sh -c evil.insteadOf' 'https://example.com/' && "
                         "git config 'url.custom://repository.pushInsteadOf' 'work:'", '--no-sandbox')
        for key in (b'alias.probe', b'remote.agent.url', b'remote.agent.pushurl', b'submodule.agent.url',
                    b'url.ext::sh -c evil.insteadof', b'url.custom://repository.pushinsteadof'):
            self.assertIn(b'local ' + key + b': (unset) -> ', p.stderr)
        p = self.exec_sh(wid, "git config remote.ordinary.url 'https://example.org/repo' && "
                         "git config remote.ordinary.pushurl 'git@example.org:repo' && "
                         "git config submodule.ordinary.url 'https://example.org/library' && "
                         "git config branch.sort refname && git config tag.sort version:refname", '--no-sandbox')
        self.assertIn(b'local submodule.ordinary.url: (unset) -> https://example.org/library', p.stderr)
        self.assertIn(b'local remote.ordinary.url: (unset) -> https://example.org/repo', p.stderr)
        self.assertIn(b'local remote.ordinary.pushurl: (unset) -> git@example.org:repo', p.stderr)
        self.assertEqual(p.stderr.count(b'WARNING'), 3, p.stderr)
        self.assertNotIn(b'local branch.sort:', p.stderr)
        self.assertNotIn(b'local tag.sort:', p.stderr)

    def test_exec_reports_ssh_and_rewritten_endpoint_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.sshCommand', 'touch ssh-command-ran')
        self.git(one, 'config', 'url.ext::touch rewrite-helper-ran.insteadOf', 'https://rewritten.invalid/')
        p = self.exec_sh(wid, "git config remote.ssh.url ssh://host/repo && "
                         "git config remote.scp.pushurl git@host:repo && "
                         "git config 'url.ssh://host/.insteadOf' https://old.invalid/ && "
                         "git config remote.rewritten.url https://rewritten.invalid/repo; exit 7",
                         '--no-sandbox', code=7)
        for key in ('remote.ssh.url', 'remote.scp.pushurl', 'url.ssh://host/.insteadof', 'remote.rewritten.url'):
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ', p.stderr)
        self.assertNotIn(b'local core.sshcommand:', p.stderr)
        self.assertNotIn(b'local url.ext::touch rewrite-helper-ran.insteadof:', p.stderr)
        self.assertFalse((one / 'ssh-command-ran').exists())
        self.assertFalse((one / 'rewrite-helper-ran').exists())
        p = self.exec_sh(wid, "git config remote.ssh.fetch '+refs/heads/*:refs/remotes/ssh/*' && "
                         "git config branch.main.description unchanged-policy", '--no-sandbox')
        self.assertIn(b'local remote.ssh.fetch: (unset) -> +refs/heads/*:refs/remotes/ssh/*', p.stderr)
        self.assertNotIn(b'branch.main.description:', p.stderr)

    def test_exec_refuses_symlinked_worktrees_locator(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        worktrees = one / '.world-git/repo.git/worktrees'
        saved = worktrees.with_name('saved-worktrees')
        worktrees.rename(saved)
        worktrees.symlink_to('saved-worktrees', target_is_directory=True)
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'incomplete Git administration scan', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        self.assertTrue(worktrees.is_symlink())
        p = self.exec_sh(wid, 'rm .world-git/repo.git/worktrees && '
                         'mv .world-git/repo.git/saved-worktrees .world-git/repo.git/worktrees; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'incomplete Git administration scan', p.stderr)
        self.assertTrue(worktrees.is_dir())
        self.assertFalse(worktrees.is_symlink())

    def test_exec_reports_replacement_refs_activating_retained_filter(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(one, 'checkout', '-q', '-b', 'filter-replacement')
        (one / '.gitattributes').write_text('file filter=retained\n')
        (one / 'file').write_text('replacement contents\n')
        self.git(one, 'add', '.gitattributes', 'file')
        self.git(one, 'commit', '-qm', 'replacement with dormant filter attributes')
        replacement = self.git(one, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(one, 'checkout', '-q', 'main')
        self.git(one, 'config', 'core.useReplaceRefs', 'false')
        self.git(one, 'replace', head, replacement)
        self.git(one, 'config', 'filter.retained.smudge', 'touch replacement-filter-ran; cat')
        original = (one / 'file').read_bytes()
        # Activation changes only configuration: neither reset nor the retained
        # smudge command is run by this inspection/reporting regression.
        p = self.exec_sh(wid, 'git config core.useReplaceRefs true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local core.usereplacerefs: false -> true', p.stderr)
        self.assertNotIn(b'local filter.retained.smudge:', p.stderr)
        self.assertEqual(self.git(one, 'config', 'filter.retained.smudge').stdout.strip(),
                         b'touch replacement-filter-ran; cat')
        self.assertEqual(self.git(one, 'rev-parse', 'refs/replace/' + head).stdout.strip().decode(), replacement)
        self.assertEqual(self.git(one, 'show', 'HEAD:.gitattributes').stdout, b'file filter=retained\n')
        self.assertEqual((one / 'file').read_bytes(), original)
        self.assertFalse((one / 'replacement-filter-ran').exists())

    def test_exec_reports_attributes_file_activating_existing_filter(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'filter.hidden.clean', 'touch filter-ran; cat')
        attributes = one / '.world-git/hidden-attributes'
        attributes.write_text('* filter=hidden\n')
        p = self.exec_sh(wid, 'git config core.attributesFile ' + shlex.quote(str(attributes)), '--no-sandbox')
        self.assertIn(b'local core.attributesfile: (unset) -> ' + str(attributes).encode(), p.stderr)
        self.assertFalse((one / 'filter-ran').exists())

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux bind mount policy')
    def test_exec_readonly_hooks_path_overrides_locator_pin(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        modules = one / '.world-git/repo.git/worktrees/active/modules'
        (modules / 'guarded/child').mkdir(parents=True)
        (modules / 'guarded-foo').mkdir()
        self.git(one, 'config', 'core.hooksPath', str(modules))
        self.assert_denied(wid, 'echo evil > .world-git/repo.git/worktrees/active/modules/pre-commit')
        self.assertFalse((modules / 'pre-commit').exists())

        # Read-only ancestors must also dominate descendant locator pins.
        self.assert_denied(wid, 'echo evil > .world-git/repo.git/worktrees/active/modules/guarded/child/payload')
        self.assertFalse((modules / 'guarded/child/payload').exists())
        self.exec_sh(wid, 'echo fine > ordinary-work', '--require-sandbox')
        self.assertEqual((one / 'ordinary-work').read_text(), 'fine\n')
        # A lexically intervening sibling must not hide the read-only ancestor, and
        # should retain its independent writable locator bind.
        self.git(one, 'config', 'core.hooksPath', str(modules / 'guarded'))
        self.assert_denied(wid, 'echo evil > .world-git/repo.git/worktrees/active/modules/guarded/child/payload')
        self.assertFalse((modules / 'guarded/child/payload').exists())
        self.exec_sh(wid, 'echo fine > .world-git/repo.git/worktrees/active/modules/guarded-foo/payload',
                     '--require-sandbox')

    def test_exec_reports_ignored_checkout_attributes_and_aliases(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        marker = one / 'filter-ran'
        command = 'touch ' + shlex.quote(str(marker)) + '; cat'
        for checkout in (one, one / 'libs/lib'):
            self.git(checkout, 'config', 'filter.retained.smudge', command)
            self.git(checkout, 'config', 'filter.second.smudge', command)
            (checkout / '.gitignore').write_text('.gitattributes\nhidden/\nroute\nloop\ndangling\n')
        paths = ('.gitattributes', 'hidden/new/.gitattributes',
                 'hidden/.world-git/.gitattributes', 'libs/lib/.gitattributes')
        script = '; '.join('mkdir -p ' + shlex.quote(str((one / path).parent)) +
                           '; printf %s ' + shlex.quote('* filter=retained\n') + ' > ' + shlex.quote(path)
                           for path in paths)
        p = self.exec_sh(wid, script + '; exit 7', '--no-sandbox', code=7)
        for path in paths:
            self.assertIn(b'exec added Git repository attributes: ' + path.encode(), p.stderr)
        # Both trees are already captured; changing only the ignored alias must warn.
        (one / 'A').mkdir()
        (one / 'B').mkdir()
        for target, value in (('A', 'retained'), ('B', 'second')):
            (one / target / '.gitattributes').write_text('* filter=' + value + '\n')
        (one / 'route').symlink_to('A', target_is_directory=True)
        (one / 'loop').symlink_to('.', target_is_directory=True)
        (one / 'dangling').symlink_to('missing', target_is_directory=True)
        external = self.root / 'attribute-target'
        external.mkdir()
        (external / '.gitattributes').write_text('* filter=retained\n')
        (one / 'outside').symlink_to(external, target_is_directory=True)
        # Literal administration is excluded, but a project alias into it is observable.
        admin_target = one / '.world-git/attribute-target'
        admin_target.mkdir()
        (admin_target / '.gitattributes').write_text('* filter=retained\n')
        (one / 'admin-route').symlink_to(admin_target, target_is_directory=True)
        changed = [str(one / path) for path in paths] + [str(external / '.gitattributes'),
                                                        str(admin_target / '.gitattributes')]
        script = 'rm route; ln -s B route; ' + '; '.join(
            'printf %s ' + shlex.quote('* filter=second\n') + ' > ' + shlex.quote(path)
            for path in changed)
        p = self.exec_sh(wid, script + '; exit 7', '--no-sandbox', code=7)
        for path in paths:
            self.assertEqual(p.stderr.count(b'exec changed Git repository attributes: ' + path.encode() + b' '), 1)
        self.assertIn(b'exec changed a Git attributes directory alias: route', p.stderr)
        self.assertIn(b'exec changed Git repository attributes: ' + str(external / '.gitattributes').encode(), p.stderr)
        self.assertIn(b'exec changed Git repository attributes: .world-git/attribute-target/.gitattributes', p.stderr)
        self.assertNotIn(b'local filter.retained.smudge:', p.stderr)
        p = self.exec_sh(wid, 'rm ' + ' '.join(shlex.quote(path) for path in paths) +
                         '; rm route dangling; exit 7', '--no-sandbox', code=7)
        for path in paths:
            self.assertIn(b'exec removed Git repository attributes: ' + path.encode(), p.stderr)
        self.assertIn(b'exec removed a Git attributes directory alias: dangling', p.stderr)
        self.assertFalse(marker.exists())

    def test_exec_guards_independent_nested_repositories(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / '.gitignore').write_text('ignored/\n')
        checkouts = (one / 'ignored/vendor', one / 'libs/lib/vendor', one / 'bare-vendor')
        for checkout in checkouts:
            checkout.mkdir(parents=True)
            if checkout.name == 'bare-vendor':
                self.git(checkout, 'init', '--bare', '.git')
            else:
                self.git(checkout, 'init')
            hooks = checkout / '.git/hooks'
            hooks.mkdir(exist_ok=True)
            (hooks / 'pre-commit').write_text('#!/bin/sh\nexit 0\n')
            (hooks / 'pre-commit').chmod(0o755)
        custom = checkouts[0] / '.git/custom/pre-commit'
        custom.parent.mkdir()
        custom.write_text('#!/bin/sh\nexit 0\n')
        custom.chmod(0o755)
        self.git(checkouts[0], 'config', 'core.hooksPath', '.git/custom')
        # Only the root selects this second directory; nested capture cannot mask
        # a missed administrative classification while checkout markers are pending.
        root_custom = checkouts[0] / '.git/root-custom/pre-commit'
        root_custom.parent.mkdir()
        root_custom.write_bytes(custom.read_bytes())
        root_custom.chmod(0o755)
        self.git(one, 'config', 'core.hooksPath', 'ignored/vendor/.git/root-custom')
        linked = one / '.claude/worktrees/agent-x'
        self.git(one, 'worktree', 'add', '--detach', str(linked), 'HEAD')
        script = '; '.join('git --git-dir=' + shlex.quote(str(path / '.git')) +
                           ' config core.sshCommand retained-ssh' for path in checkouts)
        script += '; git -C ' + shlex.quote(str(linked)) + ' config core.askPass retained-askpass'
        p = self.exec_sh(wid, script + '; exit 7', '--no-sandbox', code=7)
        for checkout in checkouts:
            label = str(checkout.relative_to(one)).encode()
            self.assertIn(b'nested ' + label + b' local core.sshcommand: (unset) -> retained-ssh', p.stderr)
        self.assertIn(b'nested .claude/worktrees/agent-x local core.askpass: (unset) -> retained-askpass', p.stderr)
        hook = checkouts[0] / '.git/hooks/pre-commit'
        original = hook.read_bytes()
        self.assert_denied(wid, 'echo changed >> ' + shlex.quote(str(hook)))
        self.assertEqual(hook.read_bytes(), original)
        self.assert_denied(wid, 'echo changed >> ' + shlex.quote(str(custom)))
        self.assertEqual(custom.read_bytes(), original)
        self.assert_denied(wid, 'echo changed >> ' + shlex.quote(str(root_custom)))
        self.assertEqual(root_custom.read_bytes(), original)
        p = self.exec_sh(wid, 'echo changed >> ' + shlex.quote(str(hook)) + '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'exec changed a Git hook: ignored/vendor/.git/hooks/pre-commit', p.stderr)
        # A newly planted persistent common-directory redirect must be reported on Linux,
        # while macOS can deny creation of the absent literal locator.
        redirect = checkouts[0] / '.git/commondir'
        if sys.platform == 'darwin':
            self.exec_sh(wid, 'printf %s ' + shlex.quote(str(one / '.world-git/repo.git')) +
                         ' > ' + shlex.quote(str(redirect)), '--require-sandbox', code=1)
            self.assertFalse(redirect.exists())
        else:
            p = self.exec_sh(wid, 'printf %s ' + shlex.quote(str(one / '.world-git/repo.git')) +
                             ' > ' + shlex.quote(str(redirect)) + '; exit 7', '--require-sandbox', code=7)
            self.assertIn(b'WARNING', p.stderr)
            self.assertIn(b'unsupported nested Git common directory', p.stderr)
            redirect.unlink()
        # Unknown externally routed administration must fail before user code starts.
        bad = one / 'unsupported'
        bad.mkdir()
        (bad / '.git').write_text('gitdir: ' + str(self.source / '.git') + '\n')
        p = self.exec_sh(wid, 'touch command-ran', '--require-sandbox', code=3)
        self.assertIn(b'unsupported nested Git administration pointer', p.stderr)
        self.assertFalse((one / 'command-ran').exists())

    def test_exec_guards_nested_repository_in_plain_world(self):
        plain = self.root / 'plain-nested'
        plain.mkdir()
        self.world('init', str(plain))
        one, wid = self.fork()
        self.exec_sh(wid, 'echo plain > file', '--require-sandbox')
        checkout = one / 'vendor'
        checkout.mkdir()
        self.git(checkout, 'init')
        p = self.exec_sh(wid, 'git -C vendor config core.sshCommand retained-ssh; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'nested vendor local core.sshcommand: (unset) -> retained-ssh', p.stderr)
        self.assertNotIn(b'Git guard unavailable', p.stderr)

    def test_exec_reports_fsck_policy_and_skiplist_sources(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        common = one / '.world-git/repo.git'
        hooks = common / 'hooks'
        hooks.mkdir(exist_ok=True)
        marker = one / 'fsck-hook-ran'
        original = ('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n').encode()
        for name in ('update', 'reference-transaction'):
            (hooks / name).write_bytes(original)
            (hooks / name).chmod(0o755)
        self.git(one, 'config', 'receive.fsckObjects', 'true')
        self.git(one, 'config', 'fetch.fsckObjects', 'true')
        for section in ('receive', 'fetch'):
            self.git(one, 'config', section + '.fsck.missingEmail', 'error')
        self.git(one, 'config', 'receive.fsck.skipList', 'skiplist')
        external = self.root / 'dormant-skiplist'
        policy = common / 'dormant-fsck-policy'
        policy.write_text('[fetch "fsck"]\n skipList = ' + str(external) + '\n')
        self.git(one, 'config', 'includeIf.onbranch:never-fsck.path', str(policy))
        p = self.exec_sh(wid, 'git config receive.fsck.missingEmail ignore; '
                         'git config fetch.fsck.missingEmail warn; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local receive.fsck.missingemail: error -> ignore', p.stderr)
        self.assertIn(b'local fetch.fsck.missingemail: error -> warn', p.stderr)
        # Relative receive paths include the common bare repository's execution base.
        targets = (common / 'skiplist', one / 'skiplist', external)
        for action, value in (('added', self.base.decode() + '\n'),
                              ('changed', '# policy changed\n' + self.base.decode() + '\n'), ('removed', None)):
            script = '; '.join('rm ' + shlex.quote(str(path)) if value is None else
                               'printf %s ' + shlex.quote(value) + ' > ' + shlex.quote(str(path))
                               for path in targets)
            p = self.exec_sh(wid, script + '; exit 7', '--no-sandbox', code=7)
            for path in targets:
                label = str(path.relative_to(one)) if path.is_relative_to(one) else str(path)
                self.assertIn(b'exec ' + action.encode() + b' Git fsck skipList: ' + label.encode(), p.stderr)
            self.assertNotIn(b'a Git hook:', p.stderr)
            self.assertNotIn(b'local receive.fsck.skiplist:', p.stderr)
            self.assertFalse(marker.exists())
        for name in ('update', 'reference-transaction'):
            self.assertEqual((hooks / name).read_bytes(), original)
        external.symlink_to(policy)
        p = self.exec_sh(wid, 'touch command-ran', '--require-sandbox', code=3)
        self.assertIn(b'nonregular Git fsck skipList cannot be inspected', p.stderr)
        self.assertFalse((one / 'command-ran').exists())
        external.unlink()
        self.git(one, 'config', 'receive.fsck.skipList', 'path,split')
        p = self.exec_sh(wid, 'touch command-ran', '--require-sandbox', code=3)
        self.assertIn(b'unsupported Git fsck skipList path', p.stderr)
        self.assertFalse((one / 'command-ran').exists())

    def test_exec_reports_receive_validation_gates_with_retained_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/update'
        hook.parent.mkdir(exist_ok=True)
        original = b'#!/bin/sh\ntouch update-hook-ran\n'
        hook.write_bytes(original)
        hook.chmod(0o755)
        self.git(one, 'config', 'receive.shallowUpdate', 'false')
        self.git(one, 'config', 'receive.fsckObjects', 'true')
        self.git(one, 'config', 'receive.maxInputSize', '1')
        self.git(one, 'config', 'receive.advertiseAtomic', 'false')
        self.git(one, 'config', 'receive.advertisePushOptions', 'false')
        self.git(one, 'config', 'push.followTags', 'false')
        self.git(one, 'config', 'push.useForceIfIncludes', 'true')
        self.git(one, 'config', 'push.pushOption', 'ci.skip')
        # Removing the option can bypass the unchanged receiver's capability rejection.
        p = self.exec_sh(wid, 'git config --unset-all push.pushOption; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local push.pushoption: ci.skip -> (unset)', p.stderr)
        self.assertNotIn(b'local receive.advertisepushoptions:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertEqual(hook.read_bytes(), original)
        self.assertFalse((one / 'update-hook-ran').exists())
        p = self.exec_sh(wid, 'git config receive.shallowUpdate true; '
                         'git config receive.fsckObjects false; git config receive.maxInputSize 0; '
                         'git config receive.advertiseAtomic true; git config receive.advertisePushOptions true; '
                         'git config push.followTags true; git config push.useForceIfIncludes false; '
                         'git config receive.certNonceSeed test-seed; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local receive.shallowupdate: false -> true', p.stderr)
        self.assertIn(b'local receive.fsckobjects: true -> false', p.stderr)
        self.assertIn(b'local receive.maxinputsize: 1 -> 0', p.stderr)
        self.assertIn(b'local receive.advertiseatomic: false -> true', p.stderr)
        self.assertIn(b'local receive.advertisepushoptions: false -> true', p.stderr)
        self.assertIn(b'local push.followtags: false -> true', p.stderr)
        self.assertIn(b'local push.useforceifincludes: true -> false', p.stderr)
        self.assertIn(b'local receive.certnonceseed: (unset) -> test-seed', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertEqual(hook.read_bytes(), original)
        self.assertFalse((one / 'update-hook-ran').exists())

    def test_exec_reports_repository_attributes_activating_unchanged_filters(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        repositories = ((one, one / '.world-git/repo.git'),
                        (one / 'libs/lib', one / '.world-git/repo.git/worktrees/active/modules/lib-module'))
        for checkout, common in repositories:
            with self.subTest(repository=checkout):
                for name in ('hidden', 'second'):
                    self.git(checkout, 'config', 'filter.' + name + '.clean', 'touch filter-ran; cat')
                attributes = common / 'info/attributes'
                attributes.parent.mkdir(exist_ok=True)
                self.assertFalse(attributes.exists())
                rel = str(attributes.relative_to(one)).encode()
                p = self.exec_sh(wid, 'printf %s ' + shlex.quote('* filter=hidden\n') + ' > ' +
                                 shlex.quote(str(attributes)) + '; exit 7', '--no-sandbox', code=7)
                self.assertIn(b'exec added Git repository attributes: ' + rel, p.stderr)
                self.assertNotIn(b'local filter.hidden.clean:', p.stderr)
                alias = one / 'attributes-alias'
                os.link(attributes, alias)
                # Same-size, same-mtime replacement through a hardlink must still be seen.
                code = ('import os\np = ' + repr(str(alias)) + '\nst = os.stat(p)\n'
                        "with open(p, 'wb') as f: f.write(b'* filter=second\\n')\n"
                        'os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))\n')
                p = self.exec_sh(wid, shlex.quote(sys.executable) + ' -c ' + shlex.quote(code) + '; exit 7',
                                 '--no-sandbox', code=7)
                self.assertIn(b'exec changed Git repository attributes: ' + rel, p.stderr)
                alias.unlink()
                p = self.exec_sh(wid, 'rm ' + shlex.quote(str(attributes)) + '; exit 7', '--no-sandbox', code=7)
                self.assertIn(b'exec removed Git repository attributes: ' + rel, p.stderr)
                self.assertFalse((one / 'filter-ran').exists())
                self.assertFalse((checkout / 'filter-ran').exists())

    def test_exec_reports_dormant_attributes_sources(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        marker = one / 'dormant-filter-ran'
        self.git(one, 'config', 'filter.retained.smudge', 'touch ' + shlex.quote(str(marker)) + '; cat')
        policy = one / '.world-git/dormant-attributes-config'
        external = self.root / 'dormant-attributes'
        relative = one / '.world-git/dormant-attributes'
        policy.write_text('[core]\n attributesFile = ' + str(external) +
                          '\n attributesFile = .world-git/dormant-attributes\n')
        self.git(one, 'config', 'includeIf.onbranch:never-selected-attribute-branch.path', str(policy))
        original = policy.read_bytes()
        targets = (external, relative)
        for action, value in (('added', '* filter=retained\n'), ('changed', '*.txt filter=retained\n'),
                              ('removed', None)):
            script = '; '.join('rm ' + shlex.quote(str(path)) if value is None else
                               'printf %s ' + shlex.quote(value) + ' > ' + shlex.quote(str(path))
                               for path in targets)
            p = self.exec_sh(wid, script + '; exit 7', '--no-sandbox', code=7)
            for path in targets:
                label = str(path.relative_to(one)) if path.is_relative_to(one) else str(path)
                self.assertIn(b'exec ' + action.encode() + b' Git repository attributes: ' + label.encode(), p.stderr)
            self.assertNotIn(b'local filter.retained.smudge:', p.stderr)
            self.assertNotIn(b'a Git include target:', p.stderr)
            self.assertEqual(policy.read_bytes(), original)
            self.assertFalse(marker.exists())
        # The same unsupported-source policy applies even while the condition is false.
        external.symlink_to(policy)
        p = self.exec_sh(wid, 'touch command-ran', '--require-sandbox', code=3)
        self.assertIn(b'nonregular Git repository attributes cannot be inspected', p.stderr)
        self.assertFalse((one / 'command-ran').exists())

    def test_exec_reports_effective_user_attributes_once_per_path(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'filter.hidden.clean', 'touch filter-ran; cat')
        self.git(one / 'libs/lib', 'config', 'filter.hidden.clean', 'touch filter-ran; cat')
        xdg = self.root / 'xdg'
        shared = xdg / 'git/attributes'
        shared.parent.mkdir(parents=True)
        shared.write_text('')
        self.env.update(XDG_CONFIG_HOME=str(xdg), GIT_ATTR_NOSYSTEM='1')
        self.git(one, 'var', 'GIT_ATTR_SYSTEM', code=1)
        p = self.exec_sh(wid, 'printf %s ' + shlex.quote('* filter=hidden\n') + ' > ' +
                         shlex.quote(str(shared)) + '; exit 7', '--no-sandbox', code=7)
        warning = b'exec changed Git repository attributes: ' + str(shared).encode()
        self.assertEqual(p.stderr.count(warning), 1, p.stderr)  # root and submodules share it
        relative = one / '.world-git/custom-attributes'
        relative.write_text('')
        self.git(one, 'config', 'core.attributesFile', '.world-git/custom-attributes')
        p = self.exec_sh(wid, 'printf %s ' + shlex.quote('* filter=hidden\n') +
                         ' > .world-git/custom-attributes', '--no-sandbox')
        self.assertIn(b'exec changed Git repository attributes: .world-git/custom-attributes', p.stderr)
        # Each repository resolves its own configured value; Git expands ~ using HOME.
        home_dir = self.root / 'home'
        home_dir.mkdir()
        self.env['HOME'] = str(home_dir)
        user_attributes = home_dir / 'attributes'
        lib_attributes = home_dir / 'lib-attributes'
        user_attributes.write_text('')
        lib_attributes.write_text('')
        self.git(one, 'config', 'core.attributesFile', '~/attributes')
        self.git(one / 'libs/lib', 'config', 'core.attributesFile', '~/lib-attributes')
        script = ' && '.join('printf %s ' + shlex.quote('* filter=hidden\n') + ' > ' + shlex.quote(str(path))
                             for path in (user_attributes, lib_attributes))
        p = self.exec_sh(wid, script, '--no-sandbox')
        for path in (user_attributes, lib_attributes):
            self.assertEqual(p.stderr.count(b'exec changed Git repository attributes: ' + str(path).encode()), 1,
                             p.stderr)
        self.assertFalse((one / 'filter-ran').exists())
        self.assertFalse((one / 'libs/lib/filter-ran').exists())

    def test_exec_allows_disabled_user_attributes(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.attributesFile', '/dev/null')
        p = self.exec_sh(wid, 'touch command-ran', '--require-sandbox')
        self.assertTrue((one / 'command-ran').exists())
        self.assertNotIn(b'WARNING', p.stderr)

    def test_exec_refuses_nonregular_repository_attributes(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        attributes = one / '.world-git/repo.git/info/attributes'
        attributes.parent.mkdir(exist_ok=True)
        target = self.root / 'external-attributes'
        target.write_text('* filter=hidden\n')
        for kind in ('symlink', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink':
                    attributes.symlink_to(target)
                else:
                    os.mkfifo(attributes)
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'nonregular Git repository attributes cannot be inspected', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                p = self.exec_sh(wid, 'exit 7', '--no-sandbox', code=7)
                self.assertIn(b'nonregular Git repository attributes cannot be inspected', p.stderr)
                attributes.unlink()

    def test_exec_reports_config_value_boundaries_and_implicit_boolean(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'alias.boundary', 'status\x1e!payload')
        p = self.exec_sh(wid, "git config --unset-all alias.boundary && "
                         "git config --add alias.boundary status && git config --add alias.boundary '!payload'; exit 7",
                         '--no-sandbox', code=7)
        self.assertIn(rb'local alias.boundary: status\x1e!payload -> status, !payload', p.stderr)
        self.assertNotIn(b'\x1e', p.stderr)
        self.git(one, 'config', '--replace-all', 'alias.boundary', 'left\x1d\x1e\\right')
        script = ('git config --unset-all alias.boundary && git config --add alias.boundary ' +
                  shlex.quote('left\x1d') + ' && git config --add alias.boundary ' + shlex.quote('\\right'))
        p = self.exec_sh(wid, script, '--no-sandbox')
        self.assertIn(rb'local alias.boundary: left\x1d\x1e\\right -> left\x1d, \\right', p.stderr)
        self.assertNotIn(b'\x1d', p.stderr)
        self.assertNotIn(b'\x1e', p.stderr)
        # Git interprets a bare boolean as true and an explicit empty value as false.
        config = one / '.world-git/repo.git/config'
        with config.open('a') as f:
            f.write('\n[commit]\n gpgSign\n')
        self.assertEqual(self.git(one, 'config', '--bool', 'commit.gpgsign').stdout.strip(), b'true')
        p = self.exec_sh(wid, "git config commit.gpgsign ''", '--no-sandbox')
        self.assertIn(b'local commit.gpgsign: (implicit) -> ""', p.stderr)
        self.assertEqual(self.git(one, 'config', '--bool', 'commit.gpgsign').stdout.strip(), b'false')

    def test_exec_preserves_config_identity_field_boundaries(self):
        origin = self.origin('identity-lib')
        other_name = 'a\x1flocal\x1fcredential.x'
        self.sub(self.source, 'add', '-q', '--name', 'a', str(origin), 'libs/one')
        self.sub(self.source, 'add', '-q', '--name', other_name, str(origin), 'libs/two')
        self.git(self.source, 'commit', '-qm', 'identity fixtures')
        self.world('init', str(self.source))
        one, wid = self.fork()
        key = 'credential.x\x1flocal\x1fcredential.helper'
        value = '!touch helper-ran'
        self.git(one / 'libs/one', 'config', key, value)
        script = ('git -C libs/one config --unset ' + shlex.quote(key) +
                  ' && git -C libs/two config credential.helper ' + shlex.quote(value))
        p = self.exec_sh(wid, script, '--no-sandbox')
        self.assertIn(rb'submodule a local credential.x\x1flocal\x1fcredential.helper: !touch helper-ran -> (unset)',
                      p.stderr)
        self.assertIn(rb'submodule a\x1flocal\x1fcredential.x local credential.helper: (unset) -> !touch helper-ran',
                      p.stderr)
        self.assertNotIn(b'\x1f', p.stderr)
        self.assertFalse((one / 'libs/one/helper-ran').exists())
        self.assertFalse((one / 'libs/two/helper-ran').exists())

    def test_exec_reports_selecting_sendemail_identity(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'sendemail.work.sendmailcmd', 'touch sendmail-ran')
        self.git(one, 'config', 'sendemail.work.cccmd', 'touch sendmail-ran')
        self.git(one, 'config', 'core.editor', 'touch sendmail-ran')
        self.git(one, 'config', 'imap.tunnel', 'touch sendmail-ran')
        p = self.exec_sh(wid, 'git config sendemail.identity work; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local sendemail.identity: (unset) -> work', p.stderr)
        self.assertNotIn(b'local sendemail.work.sendmailcmd:', p.stderr)
        values = {'sendemail.work.annotate': 'true', 'sendemail.work.suppresscc': 'none',
                  'sendemail.work.validate': 'true', 'sendemail.work.useimaponly': 'false',
                  'sendemail.work.imapsentfolder': 'Sent'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + value
                                        for key, value in values.items()), '--no-sandbox')
        for key, value in values.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + value.encode(), p.stderr)
        for key in (b'sendemail.work.sendmailcmd', b'sendemail.work.cccmd', b'core.editor', b'imap.tunnel'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        self.assertFalse((one / 'sendmail-ran').exists())

    def test_exec_reports_disabling_confirmation_for_retained_mail_commands(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        for prefix in ('sendemail', 'sendemail.work'):
            self.git(one, 'config', prefix + '.sendmailCmd', 'touch retained-mail-command-ran')
            self.git(one, 'config', prefix + '.confirm', 'always')
        self.git(one, 'config', 'sendemail.identity', 'work')
        p = self.exec_sh(wid, 'git config sendemail.confirm never && '
                         'git config sendemail.work.confirm never; exit 7', '--no-sandbox', code=7)
        for prefix in ('sendemail', 'sendemail.work'):
            self.assertIn(b'local ' + prefix.encode() + b'.confirm: always -> never', p.stderr)
            self.assertNotIn(b'local ' + prefix.encode() + b'.sendmailcmd:', p.stderr)
            self.assertEqual(self.git(one, 'config', prefix + '.sendmailCmd').stdout.strip(),
                             b'touch retained-mail-command-ran')
        self.assertNotIn(b'local sendemail.identity:', p.stderr)
        self.assertFalse((one / 'retained-mail-command-ran').exists())

    def test_exec_refuses_hooks_path_aliases_and_guards_missing_direct_path(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        external = self.root / 'external-config'
        external.mkdir()
        (one / 'hook-link').symlink_to(external, target_is_directory=True)
        for path in ('hook-link/new/hooks', 'hook-link/../other-hooks'):
            self.git(one, 'config', 'core.hooksPath', path)
            p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
            self.assertIn(b'could not resolve Git hooks path', p.stderr)
            self.assertFalse((one / 'should-not-run').exists())
        (one / 'dangling-hooks').symlink_to(external / 'absent', target_is_directory=True)
        self.git(one, 'config', 'core.hooksPath', 'dangling-hooks/new')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'could not resolve Git hooks path', p.stderr)
        self.git(one, 'config', 'core.hooksPath', 'absent/../ambiguous-hooks')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'could not resolve Git hooks path', p.stderr)
        (one / 'ordinary-file').write_text('not a directory')
        self.git(one, 'config', 'core.hooksPath', 'ordinary-file/hooks')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'could not resolve Git hooks path', p.stderr)
        local = one / 'local-hooks'
        local.mkdir()
        (one / 'local-alias').symlink_to(local, target_is_directory=True)
        self.git(one, 'config', 'core.hooksPath', 'local-alias')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'could not resolve Git hooks path', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        direct = external / 'new-hooks'
        self.git(one, 'config', 'core.hooksPath', str(direct))
        self.assert_denied(wid, 'mkdir ' + shlex.quote(str(direct)))
        self.assertFalse(direct.exists())
        # Existing parent/.. semantics remain valid without a mutable alias.
        parent = external / 'existing'
        parent.mkdir()
        self.git(one, 'config', 'core.hooksPath', str(parent / '../new-hooks'))
        self.assert_denied(wid, 'mkdir ' + shlex.quote(str(direct)))
        self.assertFalse(direct.exists())

    def test_exec_reports_enabling_skipped_remotes(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch skipped-helper-ran')
        for key in ('remote.dormant.skipDefaultUpdate', 'remote.dormant.skipFetchAll'):
            self.git(one, 'config', key, 'true')
        p = self.exec_sh(wid, 'git config remote.dormant.skipDefaultUpdate false && '
                         'git config remote.dormant.skipFetchAll false && '
                         'git config fetch.all true && git config remotes.default dormant; exit 7',
                         '--no-sandbox', code=7)
        for key in (b'remote.dormant.skipdefaultupdate', b'remote.dormant.skipfetchall'):
            self.assertIn(b'local ' + key + b': true -> false', p.stderr)
        self.assertIn(b'local fetch.all: (unset) -> true', p.stderr)
        self.assertIn(b'local remotes.default: (unset) -> dormant', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertFalse((one / 'skipped-helper-ran').exists())

    def test_exec_reports_ignored_in_tree_hooks(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.hooksPath', '.hidden-hooks')
        (one / '.gitignore').write_text('.hidden-hooks/\n')
        self.assertFalse((one / '.hidden-hooks').exists())
        p = self.exec_sh(wid, "mkdir .hidden-hooks && printf '#!/bin/sh\\ntouch hook-ran\\n' "
                         "> .hidden-hooks/pre-commit && chmod +x .hidden-hooks/pre-commit; exit 7",
                         '--require-sandbox', code=7)
        self.assertIn(b'exec added a Git hook: .hidden-hooks/pre-commit', p.stderr)
        self.assertNotIn(b'.hidden-hooks', self.git(one, 'status', '--porcelain').stdout)
        p = self.exec_sh(wid, 'echo changed >> .hidden-hooks/pre-commit', '--require-sandbox')
        self.assertIn(b'exec changed a Git hook: .hidden-hooks/pre-commit', p.stderr)
        p = self.exec_sh(wid, 'rm .hidden-hooks/pre-commit', '--require-sandbox')
        self.assertIn(b'exec removed a Git hook: .hidden-hooks/pre-commit', p.stderr)
        self.assertFalse((one / 'hook-ran').exists())
        tracked = one / '.tracked-hooks'
        tracked.mkdir()
        (tracked / 'pre-commit').write_text('#!/bin/sh\ntouch hook-ran\n')
        self.git(one, 'add', '.tracked-hooks/pre-commit')
        self.git(one, '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'tracked hook')
        self.git(one, 'config', 'core.hooksPath', '.tracked-hooks')
        p = self.exec_sh(wid, 'echo changed >> .tracked-hooks/pre-commit', '--require-sandbox')
        self.assertIn(b'exec changed a Git hook: .tracked-hooks/pre-commit', p.stderr)

    def test_exec_reports_remote_and_merge_strategy_selectors(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch selected-remote-ran')
        strategy_dir = one / 'strategy-bin'
        strategy_dir.mkdir()
        strategy = strategy_dir / 'git-merge-payload'
        strategy.write_text('#!/bin/sh\ntouch selected-strategy-ran\n')
        strategy.chmod(0o755)
        self.env['PATH'] = str(strategy_dir) + os.pathsep + self.env['PATH']
        branch = self.git(one, 'branch', '--show-current').stdout.decode().strip()
        values = {'pull.twohead': 'payload', 'pull.octopus': 'payload',
                  'branch.' + branch + '.mergeoptions': '-s payload',
                  'branch.' + branch + '.remote': 'dormant',
                  'branch.' + branch + '.pushremote': 'dormant', 'remote.pushdefault': 'dormant'}
        p = self.exec_sh(wid, ' && '.join('git config ' + shlex.quote(key) + ' ' + shlex.quote(val)
                                        for key, val in values.items()) + '; exit 7', '--no-sandbox', code=7)
        for key, val in values.items():
            self.assertIn(b'local ' + key.encode() + b': ', p.stderr)
            self.assertIn(b' -> ' + val.encode() + b'\n', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertFalse((one / 'selected-remote-ran').exists())
        self.assertFalse((one / 'selected-strategy-ran').exists())

    def test_exec_reports_autostash_hook_and_driver_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/post-rewrite'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\ntouch autostash-hook-ran\n')
        hook.chmod(0o755)
        self.git(one, 'config', 'merge.retained.driver', 'touch autostash-driver-ran')
        self.git(one, 'config', 'merge.default', 'retained')
        keys = ('rebase.autoStash', 'pull.autoStash', 'merge.autoStash')
        for key in keys:
            self.git(one, 'config', key, 'false')
        (one / 'file').write_text('dirty worktree\n')
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' true' for key in keys) + '; exit 7',
                         '--no-sandbox', code=7)
        for key in keys:
            self.assertIn(b'local ' + key.lower().encode() + b': false -> true', p.stderr)
        self.assertNotIn(b'local merge.retained.driver:', p.stderr)
        self.assertNotIn(b'local merge.default:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertFalse((one / 'autostash-hook-ran').exists())
        self.assertFalse((one / 'autostash-driver-ran').exists())

    def test_exec_reports_checkout_guess_and_rebase_hook_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hooks = one / '.world-git/repo.git/hooks'
        hooks.mkdir(exist_ok=True)
        for name in ('post-checkout', 'post-rewrite'):
            hook = hooks / name
            hook.write_text('#!/bin/sh\ntouch retained-hook-ran\n')
            hook.chmod(0o755)
        self.git(one, 'config', 'checkout.guess', 'false')
        self.git(one, 'config', 'pull.rebase', 'false')
        branch = self.git(one, 'branch', '--show-current').stdout.decode().strip()
        self.git(one, 'config', 'branch.' + branch + '.rebase', 'false')
        p = self.exec_sh(wid, 'git config checkout.guess true && git config checkout.defaultRemote origin && '
                         'git config pull.rebase true && '
                         'git config ' + shlex.quote('branch.' + branch + '.rebase') + ' true; exit 7',
                         '--no-sandbox', code=7)
        for key in (b'checkout.guess', b'pull.rebase', b'branch.' + branch.encode() + b'.rebase'):
            self.assertIn(b'local ' + key + b': false -> true', p.stderr)
        self.assertIn(b'local checkout.defaultremote: (unset) -> origin', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertFalse((one / 'retained-hook-ran').exists())

    def test_exec_reports_pull_fastforward_and_lfs_endpoint_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'credential.helper', '!touch endpoint-helper-ran')
        self.git(one, 'config', 'merge.retained.driver', 'touch pull-driver-ran')
        self.git(one, 'config', 'merge.default', 'retained')
        self.git(one, 'config', 'merge.ff', 'only')
        self.git(one, 'config', 'pull.ff', 'only')
        endpoints = {'lfs.url': 'https://example.invalid/download',
                     'lfs.pushurl': 'https://example.invalid/upload',
                     'remote.origin.lfsurl': 'https://example.invalid/remote-download',
                     'remote.origin.lfspushurl': 'https://example.invalid/remote-upload'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + shlex.quote(value)
                                        for key, value in endpoints.items()) +
                         ' && git config pull.ff true; exit 7', '--no-sandbox', code=7)
        for key, value in endpoints.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + value.encode(), p.stderr)
        self.assertIn(b'local pull.ff: only -> true', p.stderr)
        for key in (b'credential.helper', b'merge.retained.driver', b'merge.default', b'merge.ff'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        # Without explicit endpoint overrides, gitprotocol can select an unchanged
        # URL-scoped access policy for the git:// remote's derived LFS endpoint.
        for key in endpoints:
            self.git(one, 'config', '--unset', key)
        self.git(one, 'config', 'remote.origin.url', 'git://example.invalid/repo')
        self.git(one, 'config', 'lfs.http://example.invalid/repo/info/lfs.access', 'basic')
        self.git(one, 'config', 'lfs.gitprotocol', 'https')
        p = self.exec_sh(wid, 'git config lfs.gitprotocol http', '--no-sandbox')
        self.assertIn(b'local lfs.gitprotocol: https -> http', p.stderr)
        self.assertNotIn(b'local lfs.http://example.invalid/repo/info/lfs.access:', p.stderr)
        self.assertNotIn(b'local credential.helper:', p.stderr)
        self.assertFalse((one / 'endpoint-helper-ran').exists())
        self.assertFalse((one / 'pull-driver-ran').exists())

    def test_exec_reports_ignored_lfsconfig_in_root_and_submodule(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        repos = (one, one / 'libs/lib')
        for repo in repos:
            with (repo / '.gitignore').open('a') as f:
                f.write('\n.lfsconfig\n')
            self.git(repo, 'config', 'credential.helper', '!touch ' + shlex.quote(str(one / 'lfs-credential-ran')))
        paths = ('.lfsconfig', 'libs/lib/.lfsconfig')
        for operation in ('add', 'change', 'remove'):
            with self.subTest(operation=operation):
                if operation == 'remove':
                    commands = ['rm ' + path for path in paths]
                else:
                    url = 'https://example.invalid/' + operation
                    content = '[lfs]\n url = ' + url + '\n'
                    commands = ['printf %s ' + shlex.quote(content) + ' > ' + path for path in paths]
                p = self.exec_sh(wid, ' && '.join(commands) + '; exit 7', '--require-sandbox', code=7)
                verb = {'add': 'added', 'change': 'changed', 'remove': 'removed'}[operation]
                for path in paths:
                    self.assertIn(('exec ' + verb + ' Git LFS configuration: ' + path).encode(), p.stderr)
                self.assertNotIn(b'credential.helper:', p.stderr)
                self.assertFalse((one / 'lfs-credential-ran').exists())

    def test_exec_reports_lfsconfig_includes_and_ignores_unsupported_hook_setting(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/lfs-policy'
        policy.write_text('[lfs]\n url = https://example.invalid/before\n')
        # Keep the external alias readable without traversing gated store snapshots.
        ignored_hooks = self.root / 'ignored-lfs-hooks'
        ignored_hooks.mkdir()
        (one / 'ignored-hook-alias').symlink_to(ignored_hooks, target_is_directory=True)
        (one / '.lfsconfig').write_text('[include]\n path = .world-git/lfs-policy\n'
                                      '[core]\n hooksPath = ignored-hook-alias\n')
        p = self.exec_sh(wid, "printf '[lfs]\\n url = https://example.invalid/after\\n' > .world-git/lfs-policy",
                         '--require-sandbox')
        self.assertIn(b'exec changed a Git include target: .world-git/lfs-policy', p.stderr)
        self.assertNotIn(b'Git LFS configuration:', p.stderr)
        self.assertNotIn(b'Git guard unavailable', p.stderr)

    def test_exec_reports_gitmodules_sources_without_running_helpers(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        paths = ('.gitmodules', 'libs/lib/.gitmodules')
        payload = '[submodule "future"]\n path = future\n url = ext::touch module-helper-ran\n'
        p = self.exec_sh(wid, '; '.join('printf %s ' + shlex.quote(payload) + ' >> ' + shlex.quote(path)
                                      for path in paths) + '; exit 7', '--no-sandbox', code=7)
        for path in paths:
            self.assertIn(b'Git submodule configuration: ' + path.encode(), p.stderr)
        config = one / '.gitmodules'
        config.unlink()
        replacement = one / '.world-git/module-blob'
        # Includes in .gitmodules are ignored by Git and must not be traversed.
        replacement.write_text(payload + '[include]\n path = ' + str(one / '.world-git/module-fifo') + '\n')
        os.mkfifo(one / '.world-git/module-fifo')
        oid = self.git(one, 'hash-object', '-w', str(replacement)).stdout.strip().decode()
        marker = one / 'module-fsmonitor-ran'
        monitor = one / '.world-git/module-monitor'
        monitor.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        monitor.chmod(0o755)
        self.git(one, 'config', 'core.fsmonitor', str(monitor))
        p = self.exec_sh(wid, 'git -c core.fsmonitor=false update-index --cacheinfo 100644,' + oid +
                         ',.gitmodules; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'Git submodule configuration: .world-git/repo.git/worktrees/active (:.gitmodules)', p.stderr)
        tree = self.git(one, '-c', 'core.fsmonitor=false', 'write-tree').stdout.strip().decode()
        head = self.git(one, 'commit-tree', tree, '-p', 'HEAD', '-m', 'module fallback').stdout.strip().decode()
        self.git(one, '-c', 'core.fsmonitor=false', 'update-index', '--force-remove', '.gitmodules')
        p = self.exec_sh(wid, 'git update-ref HEAD ' + head + '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'Git submodule configuration: .world-git/repo.git/worktrees/active (HEAD:.gitmodules)', p.stderr)
        self.assertNotIn(b'Git guard unavailable', p.stderr)
        self.assertFalse(marker.exists())
        for checkout in (one, one / 'libs/lib'):
            self.assertFalse((checkout / 'module-helper-ran').exists())

    def test_exec_reports_lfsconfig_index_head_and_blob_includes_without_fsmonitor(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        policy = one / '.world-git/lfs-blob-policy'
        policy.write_text('[lfs]\n fetchinclude = before/**\n')
        config = one / '.lfsconfig'
        include = '[include]\n path = ' + str(policy) + '\n'
        config.write_text(include + '[lfs]\n url = https://example.invalid/before\n')
        self.git(one, 'add', '.lfsconfig')
        self.git(one, 'commit', '-qm', 'LFS fallback configuration')
        config.unlink()
        new_config = one / '.world-git/new-lfs-blob'
        new_config.write_text(include + '[lfs]\n url = https://example.invalid/after\n')
        oid = self.git(one, 'hash-object', '-w', str(new_config)).stdout.strip().decode()
        marker = one / 'fsmonitor-inspection-ran'
        fsmonitor = one / '.world-git/retained-fsmonitor'
        fsmonitor.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        fsmonitor.chmod(0o755)
        self.git(one, 'config', 'core.fsmonitor', str(fsmonitor))
        self.git(one, 'config', 'credential.helper', '!touch ' + shlex.quote(str(one / 'lfs-credential-ran')))
        # The command itself also disables fsmonitor, leaving its retained definition
        # available for the guard's separate, unmodified configuration snapshot.
        p = self.exec_sh(wid, 'git -c core.fsmonitor=false update-index --cacheinfo 100644,' + oid +
                         ',.lfsconfig; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'Git LFS configuration: .world-git/repo.git/worktrees/active (:.lfsconfig)', p.stderr)
        self.assertNotIn(b'local core.fsmonitor:', p.stderr)
        self.assertFalse(marker.exists())
        tree = self.git(one, '-c', 'core.fsmonitor=false', 'write-tree').stdout.strip().decode()
        new_head = self.git(one, 'commit-tree', tree, '-p', 'HEAD', '-m', 'alternate LFS fallback').stdout.strip().decode()
        self.git(one, '-c', 'core.fsmonitor=false', 'update-index', '--force-remove', '.lfsconfig')
        p = self.exec_sh(wid, 'git update-ref HEAD ' + new_head + '; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'Git LFS configuration: .world-git/repo.git/worktrees/active (HEAD:.lfsconfig)', p.stderr)
        self.assertFalse(marker.exists())
        p = self.exec_sh(wid, "printf '[lfs]\\n fetchinclude = after/**\\n' > .world-git/lfs-blob-policy",
                         '--require-sandbox')
        self.assertIn(b'exec changed a Git include target: .world-git/lfs-blob-policy', p.stderr)
        self.assertNotIn(b'Git LFS configuration:', p.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse((one / 'lfs-credential-ran').exists())
        self.assertFalse(config.exists())

    def test_exec_refuses_nonregular_lfsconfig(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        config = one / '.lfsconfig'
        for kind in ('symlink', 'fifo'):
            with self.subTest(kind=kind):
                if kind == 'symlink':
                    config.symlink_to(one / 'file')
                else:
                    os.mkfifo(config)
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'nonregular Git include target cannot be inspected', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                config.unlink()

    def test_exec_reports_redirect_policy_activating_retained_credentials(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        helper = '!touch ' + shlex.quote(str(one / 'redirect-credential-ran'))
        self.git(one, 'config', 'credential.helper', helper)
        keys = ('http.followRedirects', 'http.https://example.invalid/repo.followRedirects')
        for key in keys:
            self.git(one, 'config', key, 'false')
        p = self.exec_sh(wid, 'git config http.followRedirects true && '
                         'git config http.https://example.invalid/repo.followRedirects initial; exit 7',
                         '--no-sandbox', code=7)
        for key, value in zip(keys, ('true', 'initial')):
            self.assertIn(b'local ' + key.lower().encode() + b': false -> ' + value.encode(), p.stderr)
        self.assertNotIn(b'local credential.helper:', p.stderr)
        self.assertEqual(self.git(one, 'config', 'credential.helper').stdout.strip(), helper.encode())
        self.assertFalse((one / 'redirect-credential-ran').exists())

    def test_exec_reports_lfs_fetch_filters_activating_retained_agent(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'lfs.customtransfer.retained.path', '/bin/sh')
        self.git(one, 'config', 'lfs.customtransfer.retained.args', '-c "touch retained-lfs-agent-ran"')
        self.git(one, 'config', 'lfs.standaloneTransferAgent', 'retained')
        self.git(one, 'config', 'lfs.fetchInclude', 'never-matching/**')
        self.git(one, 'config', 'lfs.fetchExclude', '*')
        p = self.exec_sh(wid, "git config lfs.fetchInclude '*' && git config --unset lfs.fetchExclude; exit 7",
                         '--no-sandbox', code=7)
        self.assertIn(b'local lfs.fetchinclude: never-matching/** -> *', p.stderr)
        self.assertIn(b'local lfs.fetchexclude: * -> (unset)', p.stderr)
        for key in (b'lfs.customtransfer.retained.path', b'lfs.customtransfer.retained.args',
                    b'lfs.standalonetransferagent'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        self.assertFalse((one / 'retained-lfs-agent-ran').exists())

    def test_exec_reports_lfs_access_and_default_upstream_merge_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'credential.helper', '!touch lfs-credential-ran')
        self.git(one, 'config', 'merge.retained.driver', 'touch retained-merge-ran')
        info = one / '.world-git/repo.git/info'
        info.mkdir(exist_ok=True)
        (info / 'attributes').write_text('* merge=retained\n')
        branch = self.git(one, 'branch', '--show-current').stdout.decode().strip()
        self.git(one, 'config', 'branch.' + branch + '.remote', '.')
        self.git(one, 'config', 'branch.' + branch + '.merge', 'refs/heads/upstream')
        self.git(one, 'config', 'merge.defaultToUpstream', 'false')
        self.git(one, 'config', 'merge.ff', 'only')
        p = self.exec_sh(wid, 'git config lfs.access basic && '
                         'git config lfs.https://example.invalid/repo.access basic && '
                         'git config merge.defaultToUpstream true && git config merge.ff true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local lfs.access: (unset) -> basic', p.stderr)
        self.assertIn(b'local lfs.https://example.invalid/repo.access: (unset) -> basic', p.stderr)
        self.assertIn(b'local merge.defaulttoupstream: false -> true', p.stderr)
        self.assertIn(b'local merge.ff: only -> true', p.stderr)
        for key in (b'credential.helper', b'merge.retained.driver', b'branch.' + branch.encode() + b'.merge'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        self.assertNotIn(b'Git repository attributes:', p.stderr)
        self.assertFalse((one / 'lfs-credential-ran').exists())
        self.assertFalse((one / 'retained-merge-ran').exists())

    def test_exec_reports_interactive_and_proactive_credential_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.askPass', 'touch askpass-command-ran')
        self.git(one, 'config', 'credential.helper', '!touch credential-command-ran')
        self.git(one, 'config', 'credential.interactive', 'false')
        p = self.exec_sh(wid, 'git config credential.interactive true && '
                         'git config http.proactiveAuth basic && '
                         'git config http.https://example.invalid/repo.proactiveAuth basic; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local credential.interactive: false -> true', p.stderr)
        self.assertIn(b'local http.proactiveauth: (unset) -> basic', p.stderr)
        self.assertIn(b'local http.https://example.invalid/repo.proactiveauth: (unset) -> basic', p.stderr)
        self.assertNotIn(b'local core.askpass:', p.stderr)
        self.assertNotIn(b'local credential.helper:', p.stderr)
        self.assertFalse((one / 'askpass-command-ran').exists())
        self.assertFalse((one / 'credential-command-ran').exists())

    def test_exec_reports_enabling_builtin_submodule_update(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        child = one / 'libs/lib'
        self.git(child, 'config', 'filter.retained.smudge', 'touch retained-filter-ran; cat')
        child_admin = one / '.world-git/repo.git/worktrees/active/modules/lib-module'
        info = child_admin / 'info'
        info.mkdir(exist_ok=True)
        (info / 'attributes').write_text('*.txt filter=retained\n')
        self.git(one, 'config', 'submodule.lib-module.update', 'none')
        p = self.exec_sh(wid, 'git config submodule.lib-module.update checkout; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local submodule.lib-module.update: none -> checkout', p.stderr)
        self.assertNotIn(b'local filter.retained.smudge:', p.stderr)
        self.assertNotIn(b'Git repository attributes:', p.stderr)
        # Removing the selector also restores Git's built-in default mode.
        self.git(one, 'config', 'submodule.lib-module.update', 'none')
        p = self.exec_sh(wid, 'git config --unset submodule.lib-module.update', '--no-sandbox')
        self.assertIn(b'local submodule.lib-module.update: none -> (unset)', p.stderr)
        self.assertFalse((child / 'retained-filter-ran').exists())
        self.assertFalse((one / 'retained-filter-ran').exists())

    def test_exec_reports_enabling_preconfigured_push_transport(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.dormant.url', 'ext::touch push-helper-ran')
        self.git(one, 'config', 'remote.pushDefault', 'dormant')
        self.git(one, 'config', 'push.default', 'nothing')
        p = self.exec_sh(wid, 'git config push.default current && '
                         "git config remote.dormant.push 'HEAD:refs/heads/main' && "
                         'git config remote.dormant.mirror true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local push.default: nothing -> current', p.stderr)
        self.assertIn(b'local remote.dormant.push: (unset) -> HEAD:refs/heads/main', p.stderr)
        self.assertIn(b'local remote.dormant.mirror: (unset) -> true', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertNotIn(b'local remote.pushdefault:', p.stderr)
        # Upstream setup gates apply to simple/upstream, unlike push.default=current.
        branch = self.git(one, 'branch', '--show-current').stdout.decode().strip()
        self.git(one, 'config', '--unset', 'remote.dormant.push')
        self.git(one, 'config', '--unset', 'remote.dormant.mirror')
        self.git(one, 'config', 'push.default', 'upstream')
        self.git(one, 'config', 'branch.' + branch + '.remote', 'dormant')
        p = self.exec_sh(wid, 'git config push.autoSetupRemote true', '--no-sandbox')
        self.assertIn(b'local push.autosetupremote: (unset) -> true', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.git(one, 'config', '--unset', 'push.autoSetupRemote')
        p = self.exec_sh(wid, 'git config ' + shlex.quote('branch.' + branch + '.merge') +
                         ' refs/heads/main', '--no-sandbox')
        self.assertIn(b'local branch.' + branch.encode() + b'.merge: (unset) -> refs/heads/main', p.stderr)
        self.assertNotIn(b'local remote.dormant.url:', p.stderr)
        self.assertNotIn(b'local remote.pushdefault:', p.stderr)
        self.assertFalse((one / 'push-helper-ran').exists())

    def test_exec_reports_proxy_password_helper_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.askpass', 'touch proxy-helper-ran')
        proxy = 'http://username@proxy.invalid:8080'
        keys = ('http.proxy', 'http.https://example.invalid/repo.proxy', 'remote.origin.proxy')
        p = self.exec_sh(wid, ' && '.join('git config ' + shlex.quote(key) + ' ' + shlex.quote(proxy)
                                        for key in keys) + '; exit 7', '--no-sandbox', code=7)
        for key in keys:
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + proxy.encode(), p.stderr)
        self.assertNotIn(b'local core.askpass:', p.stderr)
        self.assertFalse((one / 'proxy-helper-ran').exists())

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux hooks ancestor mount pins')
    def test_exec_pins_linux_custom_hook_ancestors_without_reopening_external_paths(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        custom = one / '.world-git/custom'
        hooks = custom / 'hooks'
        hooks.mkdir(parents=True)
        (custom / 'cancelled').mkdir()
        hook = hooks / 'pre-commit'
        hook.write_text('#!/bin/sh\nexit 0\n')
        original = hook.read_bytes()
        self.git(one, 'config', 'core.hooksPath', str(custom / 'cancelled/../hooks'))
        old = custom.with_name('old-custom')
        self.assert_denied(wid, 'mv ' + shlex.quote(str(custom)) + ' ' + shlex.quote(str(old)) +
                           ' && mkdir -p ' + shlex.quote(str(hooks)) + ' && echo evil > ' + shlex.quote(str(hook)))
        self.assertFalse(old.exists())
        self.assertEqual(hook.read_bytes(), original)
        self.assert_denied(wid, 'mv ' + shlex.quote(str(custom / 'cancelled')) + ' ' +
                           shlex.quote(str(custom / 'old-cancelled')))
        self.assertFalse((custom / 'old-cancelled').exists())
        self.exec_sh(wid, 'echo allowed > ' + shlex.quote(str(custom / 'sibling')), '--require-sandbox')
        self.assertEqual((custom / 'sibling').read_text().strip(), 'allowed')
        external = self.root / 'external-hooks-parent'
        (external / 'hooks').mkdir(parents=True)
        sibling = external / 'sibling'
        sibling.write_text('unchanged')
        self.git(one, 'config', 'core.hooksPath', str(external / 'hooks'))
        self.assert_denied(wid, 'echo evil > ' + shlex.quote(str(sibling)))
        self.assertEqual(sibling.read_text(), 'unchanged')

    def test_exec_reports_certificate_and_signing_helper_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'core.askpass', 'touch certificate-helper-ran')
        self.git(one, 'config', 'gpg.format', 'ssh')
        self.git(one, 'config', 'gpg.ssh.defaultKeyCommand', 'touch default-key-helper-ran')
        self.git(one, 'config', 'user.signingKey', 'existing-key')
        cert_keys = ('http.sslCert', 'http.proxySSLCert',
                     'http.https://example.invalid/repo.sslCert',
                     'http.https://example.invalid/repo.proxySSLCert')
        for key in cert_keys:
            self.git(one, 'config', key, 'existing-certificate.pem')
        values = {'http.sslCertPasswordProtected': 'true',
                  'http.proxySSLCertPasswordProtected': 'true',
                  'http.https://example.invalid/repo.sslCertPasswordProtected': 'true',
                  'http.https://example.invalid/repo.proxySSLCertPasswordProtected': 'true'}
        p = self.exec_sh(wid, ' && '.join('git config ' + shlex.quote(key) + ' ' + value
                                        for key, value in values.items()) +
                         ' && git config --unset user.signingKey; exit 7', '--no-sandbox', code=7)
        for key, value in values.items():
            self.assertIn(b'local ' + key.lower().encode() + b': (unset) -> ' + value.encode(), p.stderr)
        self.assertIn(b'local user.signingkey: existing-key -> (unset)', p.stderr)
        for key in (b'core.askpass', b'gpg.format', b'gpg.ssh.defaultkeycommand'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        for key in cert_keys:
            self.assertNotIn(b'local ' + key.lower().encode() + b':', p.stderr)
            self.git(one, 'config', '--unset', key)
        p = self.exec_sh(wid, ' && '.join('git config ' + shlex.quote(key) + ' existing-certificate.pem'
                                        for key in cert_keys), '--no-sandbox')
        for key in cert_keys:
            self.assertIn(b'local ' + key.lower().encode() + b': (unset) -> existing-certificate.pem', p.stderr)
        self.assertNotIn(b'local core.askpass:', p.stderr)
        for key in values:
            self.assertNotIn(b'local ' + key.lower().encode() + b':', p.stderr)
        self.assertFalse((one / 'certificate-helper-ran').exists())
        self.assertFalse((one / 'default-key-helper-ran').exists())

    def test_exec_reports_lfs_transfer_and_autocorrect_selectors(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        commands = one / 'command-bin'
        commands.mkdir()
        transfer = commands / 'transfer-agent'
        corrected = commands / 'git-customcommand'
        for program in (transfer, corrected):
            program.write_text('#!/bin/sh\ntouch selected-command-ran\n')
            program.chmod(0o755)
        self.env['PATH'] = str(commands) + os.pathsep + self.env['PATH']
        self.git(one, 'config', 'lfs.customtransfer.payload.path', str(transfer))
        self.git(one, 'config', 'lfs.basictransfersonly', 'true')
        values = {'lfs.standalonetransferagent': 'payload',
                  'lfs.https://example.invalid/repo.standalonetransferagent': 'payload',
                  'help.autocorrect': 'immediate',
                  'lfs.customtransfer.payload.args': '--mode=custom',
                  'lfs.customtransfer.payload.direction': 'both'}
        p = self.exec_sh(wid, ' && '.join('git config ' + shlex.quote(key) + ' ' + shlex.quote(value)
                                        for key, value in values.items()) +
                         ' && git config lfs.basictransfersonly false; exit 7', '--no-sandbox', code=7)
        for key, value in values.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + value.encode(), p.stderr)
        self.assertIn(b'local lfs.basictransfersonly: true -> false', p.stderr)
        self.assertNotIn(b'local lfs.customtransfer.payload.path:', p.stderr)
        self.assertFalse((one / 'selected-command-ran').exists())

    def test_exec_reports_selecting_preconfigured_help_commands(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'browser.payload.cmd', 'touch selected-help-ran')
        self.git(one, 'config', 'man.payload.cmd', 'touch selected-help-ran')
        values = {'help.browser': 'payload', 'help.format': 'web',
                  'instaweb.browser': 'payload', 'man.viewer': 'payload'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + val for key, val in values.items()),
                         '--no-sandbox')
        for key, val in values.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + val.encode(), p.stderr)
        self.assertNotIn(b'local browser.payload.cmd:', p.stderr)
        self.assertNotIn(b'local man.payload.cmd:', p.stderr)
        self.assertFalse((one / 'selected-help-ran').exists())

    def test_exec_reports_enabling_submodule_status(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        child = one / 'libs/lib'
        for key in ('core.fsmonitor', 'diff.external', 'gpg.program'):
            self.git(child, 'config', key, 'touch child-program-ran')
        self.git(child, 'config', 'log.showSignature', 'true')
        self.git(one, 'config', 'submodule.lib-module.ignore', 'all')
        p = self.exec_sh(wid, 'git config submodule.lib-module.ignore none && '
                         'git config diff.ignoreSubmodules none && git config diff.submodule diff && '
                         'git config status.submoduleSummary true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local submodule.lib-module.ignore: all -> none', p.stderr)
        self.assertIn(b'local diff.ignoresubmodules: (unset) -> none', p.stderr)
        self.assertIn(b'local diff.submodule: (unset) -> diff', p.stderr)
        self.assertIn(b'local status.submodulesummary: (unset) -> true', p.stderr)
        for key in (b'core.fsmonitor', b'diff.external', b'gpg.program', b'log.showsignature'):
            self.assertNotIn(b'local ' + key + b':', p.stderr)
        self.assertFalse((one / 'child-program-ran').exists())
        self.assertFalse((child / 'child-program-ran').exists())

    def test_exec_reports_three_way_am_activating_retained_merge_driver(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        command = 'touch ' + shlex.quote(str(one / 'am-merge-driver-ran'))
        self.git(one, 'config', 'merge.retained.driver', command)
        self.git(one, 'config', 'am.threeWay', 'false')
        attributes = one / '.world-git/repo.git/info/attributes'
        attributes.parent.mkdir(exist_ok=True)
        attributes.write_text('file merge=retained\n')
        original = attributes.read_bytes()
        p = self.exec_sh(wid, 'git config am.threeWay true; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local am.threeway: false -> true', p.stderr)
        self.assertNotIn(b'local merge.retained.driver:', p.stderr)
        self.assertNotIn(b'Git repository attributes:', p.stderr)
        self.assertEqual(attributes.read_bytes(), original)
        self.assertEqual(self.git(one, 'config', 'merge.retained.driver').stdout.strip(), command.encode())
        self.assertFalse((one / 'am-merge-driver-ran').exists())

    def test_exec_reports_hook_and_object_validation_activation(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        marker = one / 'retained-hook-ran'
        command = 'touch ' + shlex.quote(str(marker))
        self.git(one, 'config', 'hook.retained.command', command)
        self.git(one, 'config', 'hook.retained.enabled', 'false')
        self.git(one, 'config', 'hook.pre-commit.enabled', 'false')
        self.git(one, 'config', 'fetch.fsckObjects', 'true')
        self.git(one, 'config', 'transfer.fsckObjects', 'true')
        hooks = one / '.world-git/repo.git/hooks'
        hooks.mkdir(exist_ok=True)
        original = ('#!/bin/sh\n' + command + '\n').encode()
        for name in ('pre-commit', 'reference-transaction'):
            (hooks / name).write_bytes(original)
            (hooks / name).chmod(0o755)
        p = self.exec_sh(wid, 'git config hook.retained.event pre-commit; '
                         'git config hook.retained.enabled true; git config hook.pre-commit.enabled true; '
                         'git config fetch.fsckObjects false; git config transfer.fsckObjects false; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local hook.retained.event: (unset) -> pre-commit', p.stderr)
        for key in ('hook.retained.enabled', 'hook.pre-commit.enabled'):
            self.assertIn(b'local ' + key.encode() + b': false -> true', p.stderr)
        for key in ('fetch.fsckobjects', 'transfer.fsckobjects'):
            self.assertIn(b'local ' + key.encode() + b': true -> false', p.stderr)
        self.assertNotIn(b'local hook.retained.command:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        for name in ('pre-commit', 'reference-transaction'):
            self.assertEqual((hooks / name).read_bytes(), original)
        self.assertFalse(marker.exists())

    def test_exec_reports_fetch_refspec_activating_retained_reference_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.retained.url', str(self.source))
        self.assertEqual(self.git(one, 'config', '--get-all', 'remote.retained.fetch', code=1).stdout, b'')
        hook = one / '.world-git/repo.git/hooks/reference-transaction'
        hook.parent.mkdir(exist_ok=True)
        marker = one / 'fetch-reference-hook-ran'
        hook.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        hook.chmod(0o755)
        original = hook.read_bytes()
        refspec = '+refs/heads/*:refs/remotes/retained/*'
        p = self.exec_sh(wid, 'git config remote.retained.fetch ' + shlex.quote(refspec) + '; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local remote.retained.fetch: (unset) -> ' + refspec.encode(), p.stderr)
        self.assertNotIn(b'local remote.retained.url:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertEqual(hook.read_bytes(), original)
        self.assertFalse(marker.exists())
        self.assertEqual(self.git(one, 'for-each-ref', '--format=%(refname)', 'refs/remotes/retained/').stdout, b'')
        self.git(one, 'config', '--', 'remote.retained.tagOpt', '--no-tags')
        p = self.exec_sh(wid, 'git config -- remote.retained.tagOpt --tags; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local remote.retained.tagopt: --no-tags -> --tags', p.stderr)
        self.assertNotIn(b'local remote.retained.url:', p.stderr)
        self.assertNotIn(b'a Git hook:', p.stderr)
        self.assertEqual(hook.read_bytes(), original)
        self.assertFalse(marker.exists())

    def test_exec_reports_pruning_activating_retained_reference_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'remote.origin.url', str(self.source))
        self.git(one, 'config', 'remote.origin.fetch', '+refs/heads/*:refs/remotes/origin/*')
        self.git(one, 'update-ref', 'refs/remotes/origin/stale', 'HEAD')
        self.git(one, 'tag', 'stale')
        hook = one / '.world-git/repo.git/hooks/reference-transaction'
        hook.parent.mkdir(exist_ok=True)
        marker = one / 'prune-reference-hook-ran'
        hook.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        hook.chmod(0o755)
        original = hook.read_bytes()
        for key in ('fetch.prune', 'remote.origin.prune', 'fetch.pruneTags', 'remote.origin.pruneTags'):
            self.git(one, 'config', key, 'false')
        # Tag pruning is tested only after prune itself is enabled and retained.
        for keys in (('fetch.prune', 'remote.origin.prune'),
                     ('fetch.pruneTags', 'remote.origin.pruneTags')):
            p = self.exec_sh(wid, ' && '.join('git config ' + key + ' true' for key in keys) + '; exit 7',
                             '--no-sandbox', code=7)
            for key in keys:
                self.assertIn(b'local ' + key.lower().encode() + b': false -> true', p.stderr)
            self.assertNotIn(b'a Git hook:', p.stderr)
            self.assertNotIn(b'local remote.origin.url:', p.stderr)
            self.assertEqual(hook.read_bytes(), original)
            self.assertFalse(marker.exists())
        # Configuration inspection neither fetches nor prunes the prepared stale refs.
        self.assertEqual(self.git(one, 'rev-parse', 'refs/remotes/origin/stale').stdout.strip(), self.base)
        self.assertEqual(self.git(one, 'rev-parse', 'refs/tags/stale').stdout.strip(), self.base)

    def test_exec_reports_selecting_preconfigured_merge_driver(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'merge.payload.driver', 'touch merge-driver-ran')
        self.git(one, 'config', 'filter.payload.clean', 'touch merge-filter-ran; cat')
        info = one / '.world-git/repo.git/info'
        info.mkdir(exist_ok=True)
        (info / 'attributes').write_text('* filter=payload\n')
        p = self.exec_sh(wid, 'git config merge.outer.recursive payload && '
                         'git config merge.default payload && git config merge.renormalize true; exit 7',
                         '--no-sandbox', code=7)
        self.assertIn(b'local merge.outer.recursive: (unset) -> payload', p.stderr)
        self.assertIn(b'local merge.default: (unset) -> payload', p.stderr)
        self.assertIn(b'local merge.renormalize: (unset) -> true', p.stderr)
        self.assertNotIn(b'local merge.payload.driver:', p.stderr)
        self.assertNotIn(b'local filter.payload.clean:', p.stderr)
        self.assertNotIn(b'Git repository attributes:', p.stderr)
        self.assertFalse((one / 'merge-driver-ran').exists())
        self.assertFalse((one / 'merge-filter-ran').exists())

    def test_exec_reports_selecting_preconfigured_diff_and_merge_tools(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'difftool.payload.cmd', 'touch selected-tool-ran')
        self.git(one, 'config', 'mergetool.payload.cmd', 'touch selected-tool-ran')
        keys = ('diff.tool', 'diff.guitool', 'merge.tool', 'merge.guitool')
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' payload' for key in keys), '--no-sandbox')
        for key in keys:
            self.assertIn(b'local ' + key.encode() + b': (unset) -> payload', p.stderr)
        self.assertNotIn(b'local difftool.payload.cmd:', p.stderr)
        self.assertNotIn(b'local mergetool.payload.cmd:', p.stderr)
        p = self.exec_sh(wid, 'git config difftool.guiDefault auto && git config mergetool.guiDefault true',
                         '--no-sandbox')
        self.assertIn(b'local difftool.guidefault: (unset) -> auto', p.stderr)
        self.assertIn(b'local mergetool.guidefault: (unset) -> true', p.stderr)
        self.assertNotIn(b'local diff.guitool:', p.stderr)
        self.assertNotIn(b'local merge.guitool:', p.stderr)
        self.assertFalse((one / 'selected-tool-ran').exists())

    def test_exec_reports_disabling_prompts_for_retained_tools(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        for kind, selector in (('difftool', 'diff.tool'), ('mergetool', 'merge.tool')):
            command = 'touch retained-' + kind + '-ran'
            self.git(one, 'config', kind + '.payload.cmd', command)
            self.git(one, 'config', selector, 'payload')
            self.git(one, 'config', kind + '.prompt', 'true')
        # With EOF on stdin a true prompt prevents launching these selected commands;
        # false allows them. Inspection itself must execute neither tool.
        p = self.exec_sh(wid, 'git config difftool.prompt false && '
                         'git config mergetool.prompt false; exit 7', '--no-sandbox', code=7)
        for kind, selector in (('difftool', 'diff.tool'), ('mergetool', 'merge.tool')):
            self.assertIn(b'local ' + kind.encode() + b'.prompt: true -> false', p.stderr)
            self.assertNotIn(b'local ' + kind.encode() + b'.payload.cmd:', p.stderr)
            self.assertNotIn(b'local ' + selector.encode() + b':', p.stderr)
            self.assertEqual(self.git(one, 'config', kind + '.payload.cmd').stdout.strip(),
                             ('touch retained-' + kind + '-ran').encode())
            self.assertFalse((one / ('retained-' + kind + '-ran')).exists())

    def test_exec_reports_activating_unchanged_proc_receive_hook(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        hook = one / '.world-git/repo.git/hooks/proc-receive'
        hook.parent.mkdir(exist_ok=True)
        hook.write_text('#!/bin/sh\ntouch proc-receive-ran\n')
        hook.chmod(0o755)
        p = self.exec_sh(wid, 'git config receive.procReceiveRefs refs/for/; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local receive.procreceiverefs: (unset) -> refs/for/', p.stderr)
        self.assertNotIn(b'exec changed a Git hook:', p.stderr)
        self.assertFalse((one / 'proc-receive-ran').exists())

    def test_exec_reports_activating_unchanged_submodule_update_commands(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', 'submodule.lib-module.update', '!touch update-command-ran')
        self.git(one, 'config', 'submodule.vendor/unused.update', '!touch update-command-ran')
        self.git(one, 'config', 'submodule.lib-module.active', 'false')
        self.git(one, 'config', '--replace-all', 'submodule.active', ':(exclude)**')
        p = self.exec_sh(wid, 'git config submodule.lib-module.active true && '
                         'git config --replace-all submodule.active vendor/unused; exit 7', '--no-sandbox', code=7)
        self.assertIn(b'local submodule.lib-module.active: false -> true', p.stderr)
        self.assertIn(b'local submodule.active: :(exclude)** -> vendor/unused', p.stderr)
        self.assertNotIn(b'local submodule.lib-module.update:', p.stderr)
        self.assertNotIn(b'local submodule.vendor/unused.update:', p.stderr)
        # Without either active selector, adding even an ordinary URL activates a module.
        self.git(one, 'config', '--unset-all', 'submodule.active')
        self.git(one, 'config', '--unset', 'submodule.lib-module.active')
        url = self.git(one, 'config', 'submodule.lib-module.url').stdout.decode().strip()
        self.git(one, 'config', '--unset', 'submodule.lib-module.url')
        p = self.exec_sh(wid, 'git config submodule.lib-module.url ' + shlex.quote(url), '--no-sandbox')
        self.assertIn(b'local submodule.lib-module.url: (unset) -> ' + url.encode(), p.stderr)
        self.assertNotIn(b'local submodule.lib-module.update:', p.stderr)
        # Recursion selectors can activate an unchanged child's fetch/push helper.
        self.git(one / 'libs/lib', 'config', 'remote.origin.url', 'ext::touch fetch-helper-ran')
        values = {'submodule.recurse': 'true', 'fetch.recursesubmodules': 'on-demand',
                  'push.recursesubmodules': 'on-demand',
                  'submodule.lib-module.fetchrecursesubmodules': 'true'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + val for key, val in values.items()),
                         '--no-sandbox')
        for key, val in values.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + val.encode(), p.stderr)
        self.assertNotIn(b'local remote.origin.url:', p.stderr)
        for directory in (one, one / 'libs/lib', one / 'vendor/unused'):
            self.assertFalse((directory / 'update-command-ran').exists())
            self.assertFalse((directory / 'fetch-helper-ran').exists())

    def test_exec_reports_activating_preconfigured_signing_program(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        program = one / 'signing-program'
        program.write_text('#!/bin/sh\ntouch signing-program-ran\n')
        program.chmod(0o755)
        self.git(one, 'config', 'gpg.program', str(program))
        self.git(one, 'config', 'gpg.ssh.program', str(program))
        values = {'commit.gpgsign': 'true', 'tag.gpgsign': 'true',
                  'tag.forcesignannotated': 'true', 'push.gpgsign': 'if-asked', 'gpg.format': 'ssh'}
        p = self.exec_sh(wid, ' && '.join('git config ' + key + ' ' + val for key, val in values.items()),
                         '--no-sandbox')
        for key, val in values.items():
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + val.encode(), p.stderr)
        self.assertNotIn(b'local gpg.program:', p.stderr)
        self.assertNotIn(b'local gpg.ssh.program:', p.stderr)
        p = self.exec_sh(wid, "git config log.showSignature true && git config merge.verifySignatures true && "
                         "git config format.pretty signature && git config pretty.signature '%G?' && "
                         "git config rebase.instructionFormat '%G?' && "
                         "git config format.commitListFormat 'log:%G?' && git config format.coverLetter true && "
                         "git config branch.sort signature:grade && git config tag.sort signature:grade", '--no-sandbox')
        for key, value in (('log.showsignature', 'true'), ('merge.verifysignatures', 'true'),
                           ('format.pretty', 'signature'), ('pretty.signature', '%G?'), ('rebase.instructionformat', '%G?'),
                           ('format.commitlistformat', 'log:%G?'), ('format.coverletter', 'true'),
                           ('branch.sort', 'signature:grade'), ('tag.sort', 'signature:grade')):
            self.assertIn(b'local ' + key.encode() + b': (unset) -> ' + value.encode(), p.stderr)
        self.assertNotIn(b'local gpg.program:', p.stderr)
        self.assertNotIn(b'local gpg.ssh.program:', p.stderr)
        self.assertFalse((one / 'signing-program-ran').exists())

    def test_exec_reports_missing_or_replaced_active_git_administration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        active = one / '.world-git/repo.git/worktrees/active'
        saved = active.with_name('saved-active')
        rel = '.world-git/repo.git/worktrees/active'
        operations = ('mv ' + rel + ' ' + rel.replace('/active', '/saved-active'),
                      'mv ' + rel + ' ' + rel.replace('/active', '/saved-active') + '; ln -s saved-active ' + rel,
                      'mv ' + rel + ' ' + rel.replace('/active', '/saved-active') + '; echo replaced > ' + rel,
                      'rm -rf ' + rel)
        for operation in operations:
            with self.subTest(operation=operation):
                p = self.exec_sh(wid, operation + '; exit 7', '--no-sandbox', code=7)
                self.assertIn(b'WARNING: exec left', p.stderr)
                self.assertIn(b'Git administration replaced or incomplete', p.stderr)
                p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
                self.assertIn(b'incomplete Git administration scan', p.stderr)
                self.assertFalse((one / 'should-not-run').exists())
                if saved.exists():
                    if active.is_symlink() or active.exists():
                        active.unlink()
                    saved.rename(active)

    def test_exec_reports_disappearing_git_administration(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        for operation in ('mv .world-git .saved-admin',
                          'mv .world-git .saved-admin; ln -s .saved-admin .world-git',
                          'mv .world-git .saved-admin; echo replacement > .world-git',
                          'mv .world-git .saved-admin; mkdir .world-git',
                          'rm -rf .world-git'):
            with self.subTest(operation=operation):
                p = self.exec_sh(wid, operation + '; echo "gitdir: /tmp/elsewhere" > .git; exit 7',
                                 '--no-sandbox', code=7)
                self.assertIn(b'WARNING: exec ', p.stderr)
                self.assertIn(b'Git administration', p.stderr)
                self.assertIn(b'could not be inspected', p.stderr)
                if (one / '.saved-admin').exists():
                    admin = one / '.world-git'
                    if admin.is_symlink() or admin.is_file():
                        admin.unlink()
                    elif admin.exists():
                        shutil.rmtree(admin)
                    (one / '.saved-admin').rename(one / '.world-git')
                    (one / '.git').write_text('gitdir: .world-git/repo.git/worktrees/active\n')

    def test_exec_config_capture_ignores_writable_path_git(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        wrapper = one / 'bin'
        wrapper.mkdir()
        marker = one / 'untrusted-git-ran'
        fake = wrapper / 'git'
        payload = '#!/bin/sh\necho ran >> ' + shlex.quote(str(marker)) + '\nexit 99\n'
        fake.write_text(payload)
        fake.chmod(0o755)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.exec_sh(wid, ':', '--no-sandbox')
        self.assertFalse(marker.exists())
        # The command can install the executable after the initial capture too.
        fake.unlink()
        script = 'printf %s ' + shlex.quote(payload) + ' > bin/git && chmod +x bin/git'
        self.exec_sh(wid, script, '--no-sandbox')
        self.assertFalse(marker.exists())

    def test_exec_config_fifo_include_has_bounded_capture(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        fifo = one / 'blocked-config'
        os.mkfifo(fifo)
        script = shlex.quote(shutil.which('git', path=self.env['PATH'])) + ' config include.path ' + shlex.quote(str(fifo)) + '; exit 7'
        p = subprocess.run((WORLD, 'exec', wid, '--no-sandbox', '--', '/bin/sh', '-c', script),
                           env=self.env, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 7, p.stderr)
        self.assertIn(b'WARNING: exec left', p.stderr)
        self.assertIn(b'Git hooks and settings uninspected after the command', p.stderr)
        # A blocked initial capture also returns, without suppressing the requested command.
        p = subprocess.run((WORLD, 'exec', wid, '--no-sandbox', '--', '/bin/sh', '-c', 'exit 9'),
                           env=self.env, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 9, p.stderr)
        self.assertIn(b'could not read', p.stderr)

        # An unavailable initial query must not silently omit hook protections.
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c',
                            'touch should-not-run'), env=self.env, capture_output=True, timeout=15)
        self.assertEqual(p.returncode, 3, p.stderr)
        self.assertIn(b'Git guard unavailable', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        fifo.unlink()
        fifo.write_text('[broken config\n')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'Git guard unavailable', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_post_capture_signal_cancels_query_and_releases_lock(self):
        import signal
        import time
        self.world('init', str(self.source))
        one, wid = self.fork()
        fifo = one / 'blocked-post-config'
        os.mkfifo(fifo)
        script = 'git config include.path ' + shlex.quote(str(fifo))
        proc = subprocess.Popen((WORLD, 'exec', wid, '--no-sandbox', '--', '/bin/sh', '-c', script),
                                env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        writer = None
        try:
            # A successful writer open proves the post-command inspector has opened the
            # include; hold it empty so the query stays blocked until cancellation.
            deadline = time.monotonic() + 10
            while writer is None and time.monotonic() < deadline:
                try:
                    writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                except OSError as e:
                    if e.errno != errno.ENXIO:
                        raise
                    if proc.poll() is not None:
                        break
                    time.sleep(0.02)
            self.assertIsNotNone(writer, 'post-command Git query did not open the FIFO')
            proc.send_signal(signal.SIGTERM)
            stdout, stderr = proc.communicate(timeout=3)
            self.assertEqual(proc.returncode, 128 + signal.SIGTERM, (stdout, stderr))
            # Bubblewrap's namespace teardown can finish after its outer monitor is
            # reaped. Keep the original writer open so an orphan cannot exit on EOF
            # and falsely satisfy the check; allow bounded asynchronous descriptor cleanup.
            deadline = time.monotonic() + 2
            while True:
                try:
                    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                except OSError as e:
                    self.assertEqual(e.errno, errno.ENXIO)
                    break
                else:
                    os.close(fd)
                if time.monotonic() >= deadline:
                    self.fail('configuration helper still holds the FIFO open')
                time.sleep(0.02)
            os.close(writer)
            writer = None
        finally:
            if writer is not None:
                os.close(writer)
            # Release a failed test's reader without leaving any query blocked.
            fifo.unlink()
            fifo.write_text('')
            if proc.poll() is None:
                proc.terminate()
            proc.communicate(timeout=10)
        self.git(one, 'config', '--unset', 'include.path')
        self.exec_sh(wid, ':', '--no-sandbox')  # the previous exec lock was released

    def test_exec_config_inherited_command_scope_and_expanded_hooks(self):
        import pwd
        self.world('init', str(self.source))
        one, wid = self.fork()
        shared = self.root / 'command-hooks'
        shared.mkdir()
        user = pwd.getpwuid(os.getuid())
        hook_value = '~' + user.pw_name + '/' + os.path.relpath(shared, user.pw_dir)
        self.env.update(GIT_CONFIG_COUNT='4', GIT_CONFIG_KEY_2='core.hooksPath',
                        GIT_CONFIG_VALUE_2=hook_value, GIT_CONFIG_KEY_3='include.path',
                        GIT_CONFIG_VALUE_3=str(one / 'command-config'))
        included = one / 'command-config'
        included.write_text('[alias]\n x = !before\n')
        hook = shared / 'pre-commit'
        hook.write_text('original\n')
        self.assert_denied(wid, 'echo changed > ' + shlex.quote(str(hook)))
        self.assertEqual(hook.read_text(), 'original\n')
        p = self.exec_sh(wid, "printf '[alias]\\n x = !after\\n' > command-config", '--no-sandbox')
        self.assertIn(b'command alias.x: !before -> !after', p.stderr)
        p = self.exec_sh(wid, 'echo changed > ' + shlex.quote(str(hook)), '--no-sandbox')
        self.assertIn(b'exec changed a Git hook:', p.stderr)
        # Global/local config and inherited command settings remain observable without
        # executing any hook or fsmonitor during either read-only capture.
        marker = one / 'config-command-ran'
        executable = shared / 'fsmonitor'
        executable.write_text('#!/bin/sh\ntouch ' + shlex.quote(str(marker)) + '\n')
        executable.chmod(0o755)
        hook.write_text(executable.read_text())
        hook.chmod(0o755)
        self.git(one, 'config', 'core.fsmonitor', str(executable))
        self.exec_sh(wid, ':', '--no-sandbox')
        self.assertFalse(marker.exists())

    def test_exec_in_a_plain_world_says_nothing_about_git(self):
        plain = self.root / 'plain'
        plain.mkdir()
        (plain / 'file').write_text('plain\n')
        self.world('init', str(plain))
        _, wid = self.fork('plain-world', 'S1')
        for opts in (('--no-sandbox',), ('--require-sandbox',)):
            p = self.exec_sh(wid, 'mkdir -p x/hooks && echo x > x/hooks/pre-commit', *opts)
            self.assertEqual(p.stderr, b'', opts)

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

    # ---- linked worktrees of AI coding agents (Claude Code, Codex) ----

    @staticmethod
    def tree_state(root):
        """Every entry below `root`, with its type, mode and bytes (or link target)."""
        state = {}
        for dirpath, dirs, files in os.walk(root):
            for name in dirs + files:
                path = Path(dirpath) / name
                st = path.lstat()
                if path.is_symlink():
                    state[str(path.relative_to(root))] = ('l', os.readlink(path))
                elif path.is_dir():
                    state[str(path.relative_to(root))] = ('d', st.st_mode)
                else:
                    state[str(path.relative_to(root))] = ('f', st.st_mode, path.read_bytes())
        return state

    def git_path(self, repo, name):
        return Path(self.git(repo, 'rev-parse', '--path-format=absolute', '--git-path', name).stdout.decode().strip())

    def checkouts(self, repo):
        """The non-bare checkouts `git worktree list` reports for `repo`."""
        out, current, bare = [], None, False
        for line in self.git(repo, 'worktree', 'list', '--porcelain').stdout.decode().splitlines() + ['']:
            if line.startswith('worktree '):
                current, bare = line[len('worktree '):], False
            elif line == 'bare':
                bare = True
            elif not line and current is not None:
                if not bare:
                    out.append(current)
                current = None
        return out

    def claude_worktree(self, repo, name):
        """What Claude Code does: `.claude/worktrees/<name>` on branch `worktree-<name>`, a commit
        and an uncommitted edit in it, CLAUDE_BASE in its administration and a `git worktree
        lock` held until its cleanup sweep."""
        wt = repo / '.claude' / 'worktrees' / name
        self.git(repo, 'worktree', 'add', '-q', str(wt), '-b', 'worktree-' + name)
        (wt / 'agent.txt').write_text('agent work\n')
        self.git(wt, 'add', 'agent.txt')
        self.git(wt, 'commit', '-qm', 'agent commit')
        (wt / 'file').write_text('uncommitted agent edit\n')
        admin = Path(self.git(wt, 'rev-parse', '--absolute-git-dir').stdout.decode().strip())
        (admin / 'CLAUDE_BASE').write_text('main\n')
        self.git(repo, 'worktree', 'lock', '--reason', 'claude agent', str(wt))
        return wt, self.git(wt, 'rev-parse', 'HEAD').stdout.strip(), admin

    def assert_claude_source_import(self, ignored):
        if ignored:
            with open(self.source / '.git/info/exclude', 'a') as f:
                f.write('**/.claude/worktrees/\n')
        wt, commit, admin = self.claude_worktree(self.source, 'agent-x')
        # Claude Code worktrees nest: an agent inside a worktree adds another worktree of the same
        # repository below its own `.claude/worktrees`.
        nested = wt / '.claude' / 'worktrees' / 'inner'
        self.git(wt, 'worktree', 'add', '-q', str(nested), '-b', 'worktree-inner')
        before = self.tree_state(self.source)
        init = self.world('init', str(self.source))
        self.assertIn(b'note: linked worktree .claude/worktrees/agent-x is a separate checkout', init.stderr)
        self.assertIn(b'note: linked worktree .claude/worktrees/agent-x/.claude/worktrees/inner', init.stderr)
        self.assertNotIn(b'repair:', init.stderr)
        one, wid = self.fork()
        self.assertEqual(self.tree_state(self.source), before)
        self.assertFalse((one / '.claude/worktrees/agent-x').exists())
        self.assertEqual(self.checkouts(one), [str(one)])
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'rev-parse', 'worktree-agent-x').stdout.strip(), commit)
        self.git(one, 'checkout', '-q', 'worktree-agent-x')
        self.assertEqual((one / 'agent.txt').read_text(), 'agent work\n')
        self.assertEqual((one / 'file').read_text(), 'original\n')
        # The source's worktrees are untouched and still work, lock and uncommitted edit included.
        self.assertEqual(self.git(wt, 'status', '--porcelain', '--untracked-files=no').stdout, b' M file\n')
        self.assertIn(b'locked', self.git(self.source, 'worktree', 'list', '--porcelain').stdout)
        self.git(nested, 'status', '--porcelain')
        self.world('verify', 'S1')

    def test_claude_worktree_in_source_is_left_out_when_ignored(self):
        self.assert_claude_source_import(True)

    def test_claude_worktree_in_source_is_left_out_when_not_ignored(self):
        self.assert_claude_source_import(False)

    def test_claude_worktree_hardlinked_into_source_keeps_snapshot_records_consistent(self):
        wt, _, _ = self.claude_worktree(self.source, 'agent-x')
        (self.source / 'build').mkdir()
        os.link(wt / 'agent.txt', self.source / 'build' / 'linked.txt')
        # Two names in the tree and a third in the agent's checkout: the checkout's name leaves
        # the tree, so the two that stay are one group of their own, not one reaching outside.
        os.link(wt / 'file', self.source / 'build' / 'pair-a.txt')
        os.link(wt / 'file', self.source / 'build' / 'pair-b.txt')
        init = self.world('init', str(self.source))
        self.assertNotIn(b'outside this tree', init.stderr)
        snap = json.loads(self.world('inspect', 'S1', '--json').stdout)
        self.assertEqual((snap['hardlinks'], snap['hl_groups'], snap['hl_external']), (2, 1, 0))
        self.world('verify', 'S1')
        one, _ = self.fork()
        self.assertEqual((one / 'build/linked.txt').read_text(), 'agent work\n')
        self.assertEqual((one / 'build/linked.txt').stat().st_nlink, 1)
        a, b = (one / 'build/pair-a.txt').stat(), (one / 'build/pair-b.txt').stat()
        self.assertEqual(((a.st_dev, a.st_ino), a.st_nlink), ((b.st_dev, b.st_ino), 2))
        self.assertEqual((one / 'build/pair-a.txt').read_text(), 'uncommitted agent edit\n')
        self.assertFalse((one / '.claude/worktrees/agent-x').exists())

    def test_unreadable_agent_worktree_is_never_entered_by_walked_copies(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        wt, _, _ = self.claude_worktree(one, 'agent-z')
        locked = wt / 'node_modules' / 'locked'
        locked.mkdir(parents=True)
        (locked / 'secret').write_text('x\n')
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o700)
        # A copy that walks the tree -- --copy across volumes on macOS, every clone on Linux --
        # never enters the left-out checkout, so what is unreadable in there cannot fail it.
        if sys.platform == 'darwin':
            # (The 64 MB image is below the space check's reserve; the copy itself fits.)
            two = self.mount_private_volume() / 'two'
            p = self.world('fork', '--from', wid, '--to', str(two), '--copy', '--skip-space-check')
        else:
            two = self.root / 'two'
            p = self.world('fork', '--from', wid, '--to', str(two))
        self.assertIn(b'not part of W2', p.stderr)
        self.assertFalse((two / '.claude/worktrees/agent-z').exists())
        self.assertEqual(sorted(os.listdir(two / '.world-git/repo.git/worktrees')), ['active'])
        self.assertEqual(self.checkouts(two), [str(two)])
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.git(two, 'rev-parse', '--verify', 'worktree-agent-z')
        if sys.platform != 'darwin':
            # The pre-clone scan does not enter it either. (APFS clones the whole root in one
            # clonefile(2), which fails with EACCES on an unreadable directory anywhere below
            # it, an agent's included -- the same as for an unreadable file of the tree.)
            self.world('checkpoint', wid)
            self.world('verify', 'S2')
            self.assertFalse((self.fork('three', 'S2')[0] / '.claude/worktrees/agent-z').exists())

    def test_foreign_or_unlinked_git_files_in_source_are_refused(self):
        # A `.git` file leading into another repository's worktree administration.
        other = self.root / 'other'
        self.git(self.root, 'init', '-q', '-b', 'main', str(other))
        (other / 'x').write_text('x\n')
        self.git(other, 'add', 'x')
        self.git(other, '-c', 'user.name=o', '-c', 'user.email=o@o', 'commit', '-qm', 'x')
        foreign = self.source / '.claude' / 'worktrees' / 'foreign'
        self.git(other, 'worktree', 'add', '-q', '--detach', str(foreign))
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'nested Git repository', refused.stderr)
        self.git(other, 'worktree', 'remove', str(foreign))
        # The source's own worktrees, one whose `.git` names the other's registration: that
        # registration's backlink is not this checkout.
        a = self.source / '.claude' / 'worktrees' / 'a'
        b = self.source / '.claude' / 'worktrees' / 'b'
        self.git(self.source, 'worktree', 'add', '-q', '--detach', str(a))
        self.git(self.source, 'worktree', 'add', '-q', '--detach', str(b))
        self.world('init', str(self.source))
        self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 1)
        (a / '.git').write_bytes((b / '.git').read_bytes())
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'nested Git repository', refused.stderr)
        self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 1)

    def test_claude_worktree_inside_world_is_left_out_of_forks_and_checkpoints(self):
        init = self.world('init', str(self.source))
        self.assertNotIn(b'repair:', init.stderr)
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'config', '--type=bool', 'worktree.useRelativePaths').stdout.strip(), b'true')
        wt, commit, admin = self.claude_worktree(one, 'agent-y')
        # worktree.useRelativePaths: both links are relative, so they survive moving the World.
        self.assertTrue((wt / '.git').read_text().startswith('gitdir: ../'))
        self.assertFalse((admin / 'gitdir').read_text().startswith('/'))
        parent_admin = self.tree_state(one / '.world-git')
        checkpoint = self.world('checkpoint', wid)
        self.assertIn(b'note: linked worktree .claude/worktrees/agent-y is a separate checkout, not part of S2', checkpoint.stderr)
        p = self.world('fork', '--from', wid, '--to', str(self.root / 'two'))
        self.assertIn(b'not part of W2', p.stderr)
        two = self.root / 'two'
        three, _ = self.fork('three', 'S2')
        self.world('pool', 'fill', 'S2', '--count', '1')
        pooled = self.root / 'pooled'
        self.assertIn(b'(pool)', self.world('fork', '--from', 'S2', '--to', str(pooled)).stdout)
        for child in (two, three, pooled):
            self.assertFalse((child / '.claude/worktrees/agent-y').exists())
            self.assertEqual(sorted(os.listdir(child / '.world-git/repo.git/worktrees')), ['active'])
            self.assertEqual(self.checkouts(child), [str(child)])
            self.assertEqual(self.git(child, 'status', '--porcelain').stdout, b'')
            # The branch the agent had checked out is an ordinary free branch in the child.
            self.git(child, 'checkout', '-q', 'worktree-agent-y')
            self.assertEqual(self.git(child, 'rev-parse', 'HEAD').stdout.strip(), commit)
        # The parent World and its worktree are untouched.
        self.assertEqual(self.tree_state(one / '.world-git'), parent_admin)
        self.assertEqual(self.git(wt, 'status', '--porcelain').stdout, b' M file\n')
        # Moving the World, and trashing and restoring it, keep its in-tree worktree working;
        # an in-tree worktree does not block discard.
        moved = self.root / 'moved'
        one.rename(moved)
        self.world('verify', str(moved))
        moved_wt = moved / '.claude/worktrees/agent-y'
        self.assertEqual(self.git(moved_wt, 'status', '--porcelain').stdout, b' M file\n')
        self.world('discard', wid)
        self.world('restore', wid)
        self.assertEqual(self.git(moved_wt, 'status', '--porcelain').stdout, b' M file\n')
        self.assertIn(str(moved_wt).encode(), self.git(moved, 'worktree', 'list', '--porcelain').stdout)
        self.world('checkpoint', wid)
        self.world('discard', wid, '--now')
        self.assertFalse(moved.exists())
        self.world('verify', 'S2')
        self.world('verify', 'S3')

    def test_in_tree_worktree_does_not_block_discard(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'worktree', 'add', '-q', str(one / '.claude/worktrees/agent-z'), '-b', 'worktree-agent-z')
        self.world('discard', wid, '--now')
        self.assertFalse(one.exists())

    def test_codex_worktree_outside_world_is_left_out_and_blocks_discard(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        # What `codex --worktree` does: a detached worktree outside the repository, created
        # without a checkout, core.worktree in its config.worktree, then a hard reset. Codex
        # writes absolute links (the default outside a World); its thread file sits in the
        # registration.
        codex = self.root / 'codex-home' / 'worktrees' / 'ab12' / 'one'
        head = self.git(one, 'rev-parse', 'HEAD').stdout.decode().strip()
        self.git(one, 'worktree', 'add', '-q', '--no-relative-paths', '--detach', '--no-checkout', str(codex), head)
        self.git(codex, 'config', '--file', str(self.git_path(codex, 'config.worktree')), 'core.worktree', str(codex))
        self.git(codex, 'reset', '-q', '--hard')
        admin = Path(self.git(codex, 'rev-parse', '--absolute-git-dir').stdout.decode().strip())
        self.assertTrue((admin / 'gitdir').read_text().startswith('/'))
        (admin / 'codex-thread.json').write_text('{"thread": "t"}\n')
        (codex / 'file').write_text('codex edit\n')
        p = self.world('fork', '--from', wid, '--to', str(self.root / 'two'))
        self.assertIn(b'note: linked worktree ' + str(codex).encode() + b' is a separate checkout', p.stderr)
        self.world('checkpoint', wid)
        three, _ = self.fork('three', 'S2')
        for child in (self.root / 'two', three):
            self.assertEqual(sorted(os.listdir(child / '.world-git/repo.git/worktrees')), ['active'])
            self.assertEqual(self.checkouts(child), [str(child)])
            self.assertEqual(self.git(child, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(codex, 'status', '--porcelain').stdout, b' M file\n')
        self.assertIn(str(codex).encode(), self.git(one, 'worktree', 'list', '--porcelain').stdout)
        refused = self.world('discard', wid, '--now', '--force', code=3)
        self.assertIn(b'reason: linked worktrees outside the World depend on it: ' + str(codex).encode(), refused.stderr)
        self.assertIn(b'git -C ' + str(one).encode() + b' worktree remove <path>', refused.stderr)
        self.assertTrue(one.exists())
        self.git(one, 'worktree', 'remove', '--force', str(codex))
        self.world('discard', wid, '--now')
        self.assertFalse(one.exists())

    def test_world_without_relative_worktree_setting_still_forks(self):
        # Worlds imported before worktree.useRelativePaths was set: an agent's in-tree worktree
        # gets absolute links, which name the parent's own path; the fork is unchanged otherwise.
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one, 'config', '--unset', 'worktree.useRelativePaths')
        wt, commit, admin = self.claude_worktree(one, 'agent-y')
        self.assertTrue((admin / 'gitdir').read_text().startswith('/'))
        two, _ = self.fork('two', wid)
        self.assertFalse((two / '.claude/worktrees/agent-y').exists())
        self.assertEqual(self.checkouts(two), [str(two)])
        self.assertEqual(self.git(two, 'rev-parse', 'worktree-agent-y').stdout.strip(), commit)
        self.git(two, 'config', '--get', 'worktree.useRelativePaths', code=1)
        self.assertEqual(self.git(wt, 'status', '--porcelain').stdout, b' M file\n')

    def test_malformed_world_worktree_registration_is_refused(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        wt = one / '.claude' / 'worktrees' / 'agent-y'
        self.git(one, 'worktree', 'add', '-q', str(wt), '-b', 'worktree-agent-y')
        admin = one / '.world-git/repo.git/worktrees/agent-y'
        gitdir = (admin / 'gitdir').read_bytes()
        (admin / 'gitdir').write_bytes(b'garbled\n')
        refused = self.world('checkpoint', wid, code=3)
        self.assertIn(b'agent-y has no readable gitdir link', refused.stderr)
        (admin / 'gitdir').unlink()
        (admin / 'gitdir').symlink_to(self.root / 'elsewhere')
        refused = self.world('fork', '--from', wid, '--to', str(self.root / 'two'), code=3)
        self.assertIn(b'symlink', refused.stderr)
        (admin / 'gitdir').unlink()
        (admin / 'gitdir').write_bytes(gitdir)
        # A checkout in the tree whose `.git` does not lead back to its registration.
        (wt / '.git').write_text('gitdir: ' + str(self.root / 'nowhere') + '\n')
        refused = self.world('checkpoint', wid, code=3)
        self.assertIn(b'does not link back to its registration', refused.stderr)
        self.assertEqual(len(json.loads(self.world('list', '--json').stdout)['snapshots']), 1)

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

    # ---- self-contained nested repositories -------------------------------------------------

    def nested_repo(self, path, content='nested'):
        """A self-contained repository at `path` with one commit; its HEAD."""
        path.mkdir(parents=True, exist_ok=True)
        self.git(path, 'init', '-q', '-b', 'main')
        self.identify(path)
        (path / 'pkg.txt').write_text(content + '\n')
        self.git(path, 'add', 'pkg.txt')
        self.git(path, 'commit', '-qm', content)
        return self.git(path, 'rev-parse', 'HEAD').stdout.strip()

    def snapshots(self):
        return json.loads(self.world('list', '--json').stdout)['snapshots']

    def test_ignored_nested_repository_travels_as_files(self):
        (self.source / '.gitignore').write_text('build/\n.venv/\n')
        self.git(self.source, 'commit', '-qam', 'ignore the venv')
        pkg = self.source / '.venv/src/pkg'
        head = self.nested_repo(pkg)
        # A nested repository nobody runs Git in here: WorldFS's own commands must never start
        # its fsmonitor, hooks or filters either.
        spy = self.source / '.venv/src/spy'
        self.nested_repo(spy)
        marker = self.root / 'nested-ran'
        script = self.root / 'spy.sh'
        self.write_hook(script, marker)
        self.git(spy, 'config', 'core.fsmonitor', str(script))
        self.git(spy, 'config', 'filter.spy.clean', str(script))
        self.git(spy, 'config', 'filter.spy.smudge', str(script))
        (spy / '.gitattributes').write_text('* filter=spy\n')
        for hook in ('post-checkout', 'pre-commit', 'reference-transaction', 'post-index-change'):
            self.write_hook(spy / '.git/hooks' / hook, marker)
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        nested = one / '.venv/src/pkg'
        self.assertTrue((nested / '.git').is_dir())
        # Copied, not shared: a commit in the World's copy leaves the source's alone.
        (nested / 'pkg.txt').write_text('changed in the World\n')
        self.git(nested, 'commit', '-qam', 'in the World')
        changed = self.git(nested, 'rev-parse', 'HEAD').stdout.strip()
        self.assertEqual(self.git(pkg, 'rev-parse', 'HEAD').stdout.strip(), head)
        self.assertEqual((pkg / 'pkg.txt').read_text(), 'nested\n')
        self.assertEqual(self.git(pkg, 'status', '--porcelain').stdout, b'')
        self.world('checkpoint', wid)
        self.world('checkpoint', wid, '--committed-only')
        shutil.rmtree(self.source)
        for name, snapshot, expected in (('two', 'S1', head), ('three', 'S2', changed), ('four', 'S3', changed)):
            world, _ = self.fork(name, snapshot)
            self.assertEqual(self.git(world, 'status', '--porcelain').stdout, b'')
            copy = world / '.venv/src/pkg'
            self.assertEqual(self.git(copy, 'rev-parse', 'HEAD').stdout.strip(), expected)
            self.assertEqual(self.git(copy, 'status', '--porcelain').stdout, b'')
            self.git(copy, 'fsck', '--full', '--no-progress')
            self.assertEqual(self.git(copy, 'log', '--format=%s').stdout.split(b'\n')[-2], b'nested')
        self.world('fork', '--from', wid, '--to', str(self.root / 'five'))
        self.assertEqual(self.git(self.root / 'five/.venv/src/pkg', 'rev-parse', 'HEAD').stdout.strip(), changed)
        self.assertFalse(marker.exists())

    def test_untracked_nested_repository_is_uncommitted_content(self):
        head = self.nested_repo(self.source / 'scratch/clone')
        refused = self.world('init', str(self.source), code=3)
        self.assertNotIn(b'nested', refused.stderr)
        self.assertEqual(self.snapshots(), [])
        self.world('init', str(self.source), '--include-changes')
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'?? scratch/\n')
        self.assertEqual(self.git(one / 'scratch/clone', 'rev-parse', 'HEAD').stdout.strip(), head)
        self.world('checkpoint', wid, code=3)
        self.world('checkpoint', wid, '--include-changes')
        two, _ = self.fork('two', 'S2')
        self.assertEqual(self.git(two / 'scratch/clone', 'rev-parse', 'HEAD').stdout.strip(), head)
        # publish sends the World's branch, never the nested repository, and notes the
        # uncommitted content it leaves behind.
        self.commit_in(one, 'published')
        published = self.world('publish', wid)
        self.assertIn(b'uncommitted', published.stderr + published.stdout)
        self.assertEqual(self.git(self.source, 'ls-tree', '-r', '--name-only', 'world/W1').stdout, b'.gitignore\nfile\n')

    def test_committed_only_leaves_unignored_nested_repositories_behind(self):
        (self.source / '.gitignore').write_text('build/\n.venv/\n')
        (self.source / 'vendor/foo').mkdir(parents=True)
        (self.source / 'vendor/foo/a').write_text('tracked\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'vendor')
        # One Git's status cannot see (its directory holds tracked files), one in an untracked
        # directory, and ignored ones, nested in each other.
        self.nested_repo(self.source / 'vendor/foo', 'foo')
        self.nested_repo(self.source / 'scratch/clone', 'scratch')
        (self.source / 'scratch/notes').write_text('notes\n')
        head = self.nested_repo(self.source / '.venv/src/pkg')
        inner = self.nested_repo(self.source / '.venv/src/pkg/vendored/inner', 'inner')
        before = self.tree_bytes(self.source)

        def assert_committed(world):
            self.assertEqual(self.git(world, 'status', '--porcelain', '--ignored').stdout, b'!! .venv/\n!! .world\n!! .world-git/\n')
            self.assertFalse((world / 'scratch').exists())
            self.assertEqual(sorted(os.listdir(world / 'vendor/foo')), ['a'])
            self.assertEqual(self.git(world / '.venv/src/pkg', 'rev-parse', 'HEAD').stdout.strip(), head)
            self.assertEqual(self.git(world / '.venv/src/pkg/vendored/inner', 'rev-parse', 'HEAD').stdout.strip(), inner)

        self.world('init', str(self.source), '--committed-only')
        self.assertEqual(self.tree_bytes(self.source), before)
        one, _ = self.fork()
        assert_committed(one)
        # The same from a World that carries them: checkpoint and fork with --committed-only.
        self.world('init', str(self.source), '--include-changes')
        two, wid = self.fork('two', 'S2')
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout,
                         self.git(self.source, 'status', '--porcelain').stdout)
        self.assertTrue((two / 'vendor/foo/.git').is_dir())
        self.world('checkpoint', wid, '--committed-only')
        assert_committed(self.fork('three', 'S3')[0])
        self.world('fork', '--from', wid, '--to', str(self.root / 'four'), '--committed-only')
        assert_committed(self.root / 'four')
        self.assertTrue((two / 'scratch/clone/.git').is_dir())

    def test_committed_only_decides_ignored_content_by_heads_rules(self):
        # HEAD ignores build/ and vendor/; the uncommitted .gitignore drops both rules. What
        # --committed-only keeps is decided by HEAD's rules, the ones the copy ends with: the
        # ignored build output and the ignored nested repository in a tracked directory stay.
        (self.source / '.gitignore').write_text('build/\nvendor/lib/\n')
        (self.source / 'vendor').mkdir()
        (self.source / 'vendor/README').write_text('tracked\n')
        self.git(self.source, 'add', '.')
        self.git(self.source, 'commit', '-qm', 'ignores')
        (self.source / 'build').mkdir()
        (self.source / 'build/out.bin').write_text('artifact\n')
        head = self.nested_repo(self.source / 'vendor/lib', 'lib')
        (self.source / '.gitignore').write_text('')
        (self.source / 'stray.txt').write_text('untracked\n')
        self.world('init', str(self.source), '--committed-only')
        one, _ = self.fork()
        self.assertEqual((one / 'build/out.bin').read_text(), 'artifact\n')
        self.assertEqual(self.git(one / 'vendor/lib', 'rev-parse', 'HEAD').stdout.strip(), head)
        self.assertFalse((one / 'stray.txt').exists())
        self.assertEqual((one / '.gitignore').read_text(), 'build/\nvendor/lib/\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_nested_repositories_that_reach_outside_are_refused(self):
        other = self.origin('other')
        objects = str(other / '.git/objects')

        def refused(path, why, prefix=b'nested Git repository at '):
            p = self.world('init', str(self.source), '--include-changes', code=3)
            self.assertIn(b'reason: ' + prefix + str(path).encode() + b': ' + why, p.stderr)
            if path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path)

        nested = self.source / 'nested'
        self.git(self.root, 'clone', '-q', '--shared', str(other), str(nested))
        refused(nested, b'it borrows objects from another repository (objects/info/alternates)')
        self.nested_repo(nested)
        (nested / '.git/objects/info/alternates').write_text(os.path.relpath(objects, nested / '.git/objects') + '\n')
        refused(nested, b'it borrows objects from another repository (objects/info/alternates)')
        self.nested_repo(nested)
        (nested / '.git/objects/info/http-alternates').write_text('https://example.com/objects\n')
        refused(nested, b'it borrows objects from another repository (objects/info/http-alternates)')
        self.nested_repo(nested)
        self.git(nested, 'worktree', 'add', '-q', '--detach', str(self.root / 'nested-wt'))
        refused(nested, b'it has linked worktrees registered (.git/worktrees)')
        shutil.rmtree(self.root / 'nested-wt')
        self.nested_repo(nested)
        (nested / '.git/commondir').write_text(os.path.relpath(other / '.git', nested / '.git') + '\n')
        refused(nested, b'it is the administration of a linked worktree (commondir)')
        self.nested_repo(nested)
        self.git(nested, 'config', 'core.worktree', str(self.root / 'elsewhere'))
        refused(nested, b'it sets core.worktree')
        self.nested_repo(nested)
        self.git(nested, 'config', 'extensions.worktreeConfig', 'true')
        refused(nested, b'it sets extensions.worktreeConfig')
        # A Git LFS object cache outside the nested .git: absolute, or relative with a `..`.
        for storage in (str(self.root / 'lfs-cache'), '../../lfs-cache', 'lfs/../../../lfs-cache', '..'):
            self.nested_repo(nested)
            self.git(nested, 'config', 'lfs.storage', storage)
            refused(nested, b'it keeps its Git LFS objects outside its .git (lfs.storage=' + storage.encode() + b')')
        # Any include is refused, whatever it names and whatever its condition: an included
        # file's own settings and further includes (a `gitdir:` one matching the source's
        # location, say) are not examined.
        shared = str(self.root / 'shared.cfg')
        for key, value in (('include.path', '../../shared.cfg'), ('include.path', shared),
                           ('includeIf.onbranch:main.path', shared),
                           ('includeIf.hasconfig:remote.*.url:https://example.com/**.path', shared),
                           ('includeIf.gitdir:' + str(nested) + '/.path', shared),
                           ('includeIf.gitdir/i:' + str(nested) + '/.path', shared)):
            self.nested_repo(nested)
            self.git(nested, 'config', key, value)
            # Git lowercases the section and the variable, never the subsection (the condition).
            listed = 'include.path' if key == 'include.path' else 'includeif.' + key[len('includeIf.'):]
            refused(nested, b'its configuration includes ' + value.encode() + b' (' + listed.encode() + b')')
        self.nested_repo(nested)
        (nested / '.git/modules/lib').mkdir(parents=True)
        refused(nested, b'it holds submodule repositories (.git/modules)')
        self.nested_repo(nested)
        shutil.rmtree(nested / '.git/refs/tags')
        (nested / '.git/refs/tags').symlink_to(other / '.git/refs/tags')
        refused(nested, b'its .git holds a symlink or special file (refs/tags)')
        nested.mkdir()
        (nested / '.git').symlink_to(other / '.git')
        refused(nested, b'its .git is a symlink')
        nested.mkdir()
        (nested / '.git').write_text('gitdir: ' + str(other / '.git') + '\n')
        refused(nested, b'its .git is a file leading to administration elsewhere',
                prefix=b'nested Git repository or worktree at ')
        # Each repository nested in an admitted one must pass the same check.
        self.nested_repo(nested)
        inner = nested / 'deps/inner'
        self.git(self.root, 'clone', '-q', '--shared', str(other), str(inner))
        refused(inner, b'it borrows objects from another repository (objects/info/alternates)')
        shutil.rmtree(nested)
        self.assertEqual(self.snapshots(), [])
        # Admitted: a Git LFS cache elsewhere inside the nested .git, a bare repository named
        # .git, and a symlink to a repository elsewhere, which stays a symlink and is never entered.
        self.nested_repo(nested)
        self.git(nested, 'config', 'lfs.storage', 'cache/..lfs')
        self.git(self.root, 'init', '-q', '--bare', str(self.source / 'bare/.git'))
        (self.source / 'link').symlink_to(other)
        self.world('init', str(self.source), '--include-changes')
        one, wid = self.fork()
        self.assertEqual(self.git(one / 'nested', 'log', '--format=%s').stdout, b'nested\n')
        self.assertEqual(self.git(one / 'bare/.git', 'rev-parse', '--is-bare-repository').stdout.strip(), b'true')
        self.assertTrue((one / 'link').is_symlink())
        # A World is checked the same way before it is copied.
        self.git(self.root, 'clone', '-q', '--shared', str(other), str(one / 'borrowed'))
        p = self.world('checkpoint', wid, '--include-changes', code=3)
        self.assertIn(b'reason: nested Git repository at ' + str(one / 'borrowed').encode() + b': it borrows objects', p.stderr)
        self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), '--include-changes', code=3)
        self.assertFalse((self.root / 'refused').exists())
        self.assertEqual(len(self.snapshots()), 1)

    def test_nested_repository_changed_during_the_clone_is_not_published(self):
        import shlex
        nested = self.source / 'nested'
        self.nested_repo(nested)
        other = self.origin('other')
        config = nested / '.git/config'
        alternates = nested / '.git/objects/info/alternates'
        fired = self.root / 'fired'
        real_git = shutil.which('git')
        wrapper = self.root / 'nested-race-bin'
        wrapper.mkdir()
        script = wrapper / 'git'
        # Right after the capture has read the nested repository's configuration -- its last
        # check -- it starts borrowing objects, before the tree is cloned.
        script.write_text('#!/bin/sh\nhit=0\nfor arg in "$@"; do [ "$arg" = ' + shlex.quote(str(config))
                          + ' ] && hit=1; done\n' + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$hit" = 1 ] && [ ! -e ' + shlex.quote(str(fired)) + ' ]; then\n'
                          + ': > ' + shlex.quote(str(fired)) + '\n'
                          + 'echo ' + shlex.quote(str(other / '.git/objects')) + ' > ' + shlex.quote(str(alternates)) + '\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        env = dict(self.env)
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), '--include-changes', code=1)
        self.env = env
        self.assertTrue(fired.exists())
        self.assertEqual(self.snapshots(), [])
        p = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'it borrows objects from another repository', p.stderr)
        alternates.unlink()
        # A repository that appears meanwhile, already borrowing objects, is checked too.
        late = self.source / 'late'
        fired.unlink()
        script.write_text(script.read_text().replace(
            'echo ' + shlex.quote(str(other / '.git/objects')) + ' > ' + shlex.quote(str(alternates)),
            'mkdir ' + shlex.quote(str(late)) + ' && cp -R ' + shlex.quote(str(nested / '.git')) + ' '
            + shlex.quote(str(late / '.git')) + ' && echo ' + shlex.quote(str(other / '.git/objects'))
            + ' > ' + shlex.quote(str(late / '.git/objects/info/alternates'))))
        self.env['PATH'] = str(wrapper) + os.pathsep + self.env['PATH']
        self.world('init', str(self.source), '--include-changes', code=1)
        self.env = env
        self.assertTrue((late / '.git/objects/info/alternates').exists())
        self.assertEqual(self.snapshots(), [])
        shutil.rmtree(late)
        self.world('init', str(self.source), '--include-changes')

    # ---- submodules ------------------------------------------------------------------------

    def sub(self, repo, *args, code=0):
        """A submodule command, as a user runs it: local file transport allowed explicitly."""
        return self.git(repo, '-c', 'protocol.file.allow=always', 'submodule', *args, code=code)

    def origin(self, name, content=None):
        """A local repository with one commit, to be added as a submodule."""
        origin = self.root / 'origins' / name
        origin.mkdir(parents=True)
        self.git(origin, 'init', '-q', '-b', 'main')
        self.git(origin, 'config', 'user.name', 'World Test')
        self.git(origin, 'config', 'user.email', 'world@example.com')
        (origin / 'lib.txt').write_text((content or name) + '\n')
        self.git(origin, 'add', '.')
        self.git(origin, 'commit', '-qm', name)
        return origin

    def identify(self, repo):
        self.git(repo, 'config', 'user.name', 'World Test')
        self.git(repo, 'config', 'user.email', 'world@example.com')

    def submodule_fixture(self):
        """libs/lib (absorbed, named `lib-module`), which itself has deps/inner (nested), and
        vendor/unused (uninitialized) in the source."""
        inner = self.origin('inner')
        lib = self.origin('lib')
        self.sub(lib, 'add', '-q', str(inner), 'deps/inner')
        self.git(lib, 'commit', '-qm', 'nested')
        unused = self.origin('unused')
        self.sub(self.source, 'add', '-q', '--name', 'lib-module', str(lib), 'libs/lib')
        self.sub(self.source, 'add', '-q', str(unused), 'vendor/unused')
        self.git(self.source, 'commit', '-qm', 'submodules')
        self.sub(self.source, 'update', '-q', '--init', '--recursive')
        self.sub(self.source, 'deinit', '-q', 'vendor/unused')
        for repo in ('libs/lib', 'libs/lib/deps/inner'):
            self.identify(self.source / repo)
        self.base = self.git(self.source, 'rev-parse', 'HEAD').stdout.strip()
        return self.submodule_status(self.source)

    def submodule_status(self, repo):
        """`submodule status --recursive` without its `git describe` column, which may name any
        of several refs at the same commit."""
        lines = self.sub(repo, 'status', '--recursive').stdout.splitlines()
        return [line.split(b' (')[0] for line in lines]

    def assert_owned_submodules(self, world):
        """The World's submodules live in its own administration, linked relatively."""
        admin = world / '.world-git/repo.git/worktrees/active/modules'
        self.assertEqual((world / 'libs/lib/.git').read_text(),
                         'gitdir: ../../.world-git/repo.git/worktrees/active/modules/lib-module\n')
        self.assertEqual((world / 'libs/lib/deps/inner/.git').read_text(),
                         'gitdir: ../../../../.world-git/repo.git/worktrees/active/modules/lib-module/modules/deps/inner\n')
        self.assertEqual(self.git(world / 'libs/lib', 'config', 'core.worktree').stdout.strip(), b'../../../../../../libs/lib')
        self.assertEqual(Path(self.git(world / 'libs/lib/deps/inner', 'rev-parse', '--absolute-git-dir').stdout.decode().strip()),
                         (admin / 'lib-module/modules/deps/inner').resolve())
        self.assertFalse((world / 'vendor/unused/.git').exists())
        self.assertEqual(list((world / 'vendor/unused').iterdir()), [])

    def test_exec_guards_submodule_hooks(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        admin = '.world-git/repo.git/worktrees/active/modules/'
        for repo in ('lib-module', 'lib-module/modules/deps/inner'):
            (one / admin / repo / 'hooks').mkdir(exist_ok=True)
            self.assert_denied(wid, 'echo evil > %s%s/hooks/post-checkout' % (admin, repo))
            self.assertFalse((one / admin / repo / 'hooks/post-checkout').exists())
        self.assert_denied(wid, 'mv %slib-module %slib-old' % (admin, admin))
        self.assert_denied(wid, 'mv %slib-module/modules/deps %slib-module/modules/x' % (admin, admin))
        # The submodule repositories themselves stay writable.
        self.exec_sh(wid, 'git -C libs/lib config user.name Agent && echo w > libs/lib/new && '
                          'git -C libs/lib add new && git -C libs/lib commit -qm new', '--require-sandbox')
        # A submodule initialized during the exec: its clone works (Git's *.sample templates
        # included). Seatbelt's regex covers its new hooks directory; bwrap's mounts are fixed at
        # start, so on Linux the hook is written and reported afterwards.
        self.assertFalse((one / admin / 'vendor').exists())
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c',
                            'git -c protocol.file.allow=always submodule update -q --init vendor/unused && '
                            'echo evil > %svendor/unused/hooks/post-checkout' % admin),
                           env=self.env, capture_output=True, timeout=60)
        self.assertTrue((one / 'vendor/unused/lib.txt').exists(), p.stderr)
        self.assertTrue(any((one / admin / 'vendor/unused/hooks').glob('*.sample')))
        if sys.platform == 'darwin':
            self.assertNotEqual(p.returncode, 0, p.stderr)
            self.assertFalse((one / admin / 'vendor/unused/hooks/post-checkout').exists())
            # A new repository's hooks directory may be created, but not as a symlink elsewhere.
            (one / admin / 'fake').mkdir()
            self.assert_denied(wid, 'ln -s /tmp %sfake/hooks' % admin)
        else:
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn(('world: WARNING: exec added a Git hook: %svendor/unused/hooks/post-checkout\n'
                           % admin).encode(), p.stderr)
        # Settings in a submodule's own configuration are reported under its name, and a
        # rewritten checkout `.git` (pointing Git at another repository) is reported too.
        p = self.exec_sh(wid, "git -C libs/lib/deps/inner config core.sshCommand 'ssh -o ProxyCommand=evil' && "
                              "echo 'gitdir: /tmp/elsewhere' > libs/lib/.git", '--require-sandbox')
        self.assertIn(b'world: WARNING: exec changed a Git setting that runs commands: submodule '
                      b'lib-module/modules/deps/inner local core.sshcommand: (unset) -> ssh -o ProxyCommand=evil\n',
                      p.stderr)
        self.assertIn(b'world: WARNING: exec changed where Git finds a repository: libs/lib/.git '
                      b'(a file naming its repository -> a file naming another repository)\n', p.stderr)

    def test_exec_guards_a_submodules_own_hooks_path(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        shared = self.root / 'lib-hooks'
        shared.mkdir()
        self.git(one / 'libs/lib', 'config', 'core.hooksPath', str(shared))
        self.assert_denied(wid, 'echo evil > ' + shlex.quote(str(shared / 'pre-commit')))
        self.assertFalse((shared / 'pre-commit').exists())
        p = self.exec_sh(wid, 'echo evil > ' + shlex.quote(str(shared / 'post-checkout')), '--no-sandbox')
        self.assertIn(('world: WARNING: exec added a Git hook: %s\n' % (shared / 'post-checkout')).encode(), p.stderr)
        # A relative value is resolved from the submodule's checkout: inside the tree it is the
        # submodule's project content: writable, but observed.
        self.git(one / 'libs/lib', 'config', 'core.hooksPath', 'githooks')
        p = self.exec_sh(wid, 'mkdir -p libs/lib/githooks && echo lint > libs/lib/githooks/pre-commit',
                         '--require-sandbox')
        self.assertIn(b'exec added a Git hook: libs/lib/githooks/pre-commit', p.stderr)

    def test_exec_refuses_preexisting_submodule_pointer_redirection(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        dotgit = one / 'libs/lib/.git'
        original = dotgit.read_bytes()
        dotgit.write_text('gitdir: ' + str(one / '.world-git/repo.git/worktrees/active') + '\n')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'Git repository pointer is missing, redirected or unsupported', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())
        dotgit.write_bytes(original)
        commondir = one / '.world-git/repo.git/worktrees/active/modules/lib-module/commondir'
        commondir.write_text(str(one / '.world-git/repo.git') + '\n')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'Git repository pointer is missing, redirected or unsupported', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_submodule_worktree_scope_redirects_relative_hooks(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.git(one / 'libs/lib', 'config', 'extensions.worktreeConfig', 'true')
        self.git(one / 'libs/lib', 'config', 'core.hooksPath', '.husky')
        hidden = one / '.world-git/submodule-checkout'
        hidden.mkdir()
        p = self.exec_sh(wid, 'git -C libs/lib config --worktree core.worktree ' +
                         shlex.quote(str(hidden)) + ' && mkdir -p .world-git/submodule-checkout/.husky && '
                         'echo evil > .world-git/submodule-checkout/.husky/pre-commit', '--no-sandbox')
        self.assertEqual(self.git(one / 'libs/lib', 'rev-parse', '--show-toplevel').stdout.strip(),
                         str(hidden).encode())
        self.assertIn(b'submodule lib-module worktree core.worktree:', p.stderr)
        self.assertIn(b'exec added a Git hook: .world-git/submodule-checkout/.husky/pre-commit', p.stderr)
        self.assert_denied(wid, 'echo changed > .world-git/submodule-checkout/.husky/pre-commit')

    def test_exec_reports_settings_only_a_submodule_includes(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        only = self.root / 'lib-only.gitconfig'
        only.write_text('')
        glob = self.root / 'global.gitconfig'
        glob.write_text('[includeIf "gitdir:**/modules/lib-module"]\n\tpath = %s\n' % only)
        self.env['GIT_CONFIG_GLOBAL'] = str(glob)
        p = self.exec_sh(wid, "printf '[core]\\n\\tfsmonitor = echo evil\\n' > " + shlex.quote(str(only)),
                         '--no-sandbox')
        self.assertIn(b'world: WARNING: exec changed a Git setting that runs commands: submodule lib-module '
                      b'global core.fsmonitor: (unset) -> echo evil\n', p.stderr)
        self.assertNotIn(b'commands: global core.fsmonitor', p.stderr)  # the World does not include it
        # A change every repository reads is one change, reported once under the World.
        p = self.exec_sh(wid, 'git config --global core.pager "less; evil"', '--no-sandbox')
        self.assertEqual(p.stderr.count(b'core.pager'), 1, p.stderr)
        self.assertIn(b'commands: global core.pager: (unset) -> less; evil\n', p.stderr)

    def test_exec_initializes_declared_submodule_named_hooks(self):
        tool = self.origin('new-tool')
        self.sub(self.source, 'add', '-q', '--name', 'tools/hooks', str(tool), 'tools/hooks')
        self.git(self.source, 'commit', '-qm', 'uninitialized hooks-named module')
        self.sub(self.source, 'deinit', '-q', 'tools/hooks')
        self.world('init', str(self.source))
        one, wid = self.fork()
        admin = one / '.world-git/repo.git/worktrees/active/modules'
        gitdir = admin / 'tools/hooks'
        self.assertFalse(gitdir.exists())
        if sys.platform == 'darwin':
            replacement = ('mkdir -p prepared/refs && echo ref: refs/heads/main > prepared/HEAD && ' +
                           shlex.quote(sys.executable) + ' -c ' + shlex.quote(
                               "import os; os.rename('prepared', " + repr(str(admin / 'tools')) + ")"))
            self.assert_denied(wid, replacement)
            self.assertFalse((admin / 'tools/HEAD').exists())
            self.assertFalse((admin / 'tools/refs').exists())
            self.assertFalse(gitdir.exists())
        command = ('git -c protocol.file.allow=always submodule update -q --init tools/hooks && '
                   'touch initialized && echo evil > ' + shlex.quote(str(gitdir / 'hooks/pre-commit')))
        p = subprocess.run((WORLD, 'exec', wid, '--require-sandbox', '--', '/bin/sh', '-c', command),
                           env=self.env, capture_output=True, timeout=60)
        self.assertTrue((one / 'initialized').exists(), p.stderr)
        self.assertTrue((one / 'tools/hooks/lib.txt').exists(), p.stderr)
        self.assertTrue(any((gitdir / 'hooks').glob('*.sample')), p.stderr)
        if sys.platform == 'darwin':
            self.assertNotEqual(p.returncode, 0, p.stderr)
            self.assertFalse((gitdir / 'hooks/pre-commit').exists())
            # Namespace parents cannot become standalone or synthetic common repositories.
            self.assert_denied(wid, 'echo ref: refs/heads/main > ' + shlex.quote(str(admin / 'tools/HEAD')))
            self.assert_denied(wid, 'mkdir ' + shlex.quote(str(admin / 'tools/refs')))
        else:
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn(b'exec added a Git hook:', p.stderr)

    def test_exec_initializes_ordinary_marker_named_submodules(self):
        tool = self.origin('marker-tool')
        for name in ('refs', 'HEAD'):
            self.sub(self.source, 'add', '-q', '--name', name, str(tool), 'deps/' + name)
        self.git(self.source, 'commit', '-qm', 'ordinary marker names')
        for name in ('refs', 'HEAD'):
            self.sub(self.source, 'deinit', '-q', 'deps/' + name)
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.exec_sh(wid, 'git -c protocol.file.allow=always submodule update -q --init deps/refs deps/HEAD',
                     '--require-sandbox')
        for name in ('refs', 'HEAD'):
            self.assertTrue((one / 'deps' / name / 'lib.txt').exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'Seatbelt declaration budget')
    def test_exec_refuses_excess_submodule_declaration_records(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / '.gitmodules').write_text('[metadata]\n' + ' item = value\n' * 65537)
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'Git declaration entry limit exceeded', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'Seatbelt planned submodule namespaces')
    def test_exec_refuses_submodule_plan_overlapping_hooks(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / '.gitmodules').write_text('[submodule "tool"]\n path = tool\n'
                                        '[submodule "tool/hooks/pre-commit"]\n path = other\n')
        p = self.exec_sh(wid, 'touch should-not-run', '--require-sandbox', code=3)
        self.assertIn(b'declared submodule administration overlaps Git hooks', p.stderr)
        self.assertFalse((one / 'should-not-run').exists())

    def test_exec_keeps_a_submodule_named_hooks_writable(self):
        """Seatbelt's regex cannot tell modules/tools/hooks (a repository) from a hooks directory;
        the existing repository is given back, and only its own hooks are denied."""
        tool = self.origin('tool')
        self.sub(self.source, 'add', '-q', '--name', 'tools/hooks', str(tool), 'tools/hooks')
        self.git(self.source, 'commit', '-qm', 'tool')
        self.identify(self.source / 'tools/hooks')
        self.world('init', str(self.source))
        one, wid = self.fork()
        gitdir = one / '.world-git/repo.git/worktrees/active/modules/tools/hooks'
        self.assertTrue((gitdir / 'HEAD').is_file())
        self.exec_sh(wid, 'echo w > tools/hooks/new && git -C tools/hooks add new && '
                          'git -C tools/hooks commit -qm new', '--require-sandbox')
        (gitdir / 'hooks').mkdir(exist_ok=True)
        self.assert_denied(wid, 'echo evil > .world-git/repo.git/worktrees/active/modules/tools/hooks/hooks/pre-commit')
        custom = gitdir / 'custom-hooks'
        custom.mkdir()
        hook = custom / 'pre-commit'
        hook.write_text('#!/bin/sh\nexit 0\n')
        hook.chmod(0o755)
        self.git(one / 'tools/hooks', 'config', 'core.hooksPath', str(custom))
        original = hook.read_bytes()
        self.assert_denied(wid, 'echo evil > ' + shlex.quote(str(hook)))
        self.assertEqual(hook.read_bytes(), original)
        self.assert_denied(wid, 'echo evil > ' + shlex.quote(str(custom / 'post-checkout')))
        self.assertFalse((custom / 'post-checkout').exists())
        self.exec_sh(wid, 'echo w > tools/hooks/another && git -C tools/hooks add another && '
                          'git -C tools/hooks commit -qm another', '--require-sandbox')

    def test_submodules_survive_deletion_of_the_source_and_their_origins(self):
        status = self.submodule_fixture()
        self.assertEqual(status[-1][:1], b'-')   # vendor/unused is uninitialized
        branches = [self.git(self.source / repo, 'branch', '--show-current').stdout
                    for repo in ('libs/lib', 'libs/lib/deps/inner')]
        before = self.tree_bytes(self.source)
        self.world('init', str(self.source))
        self.assertEqual(self.tree_bytes(self.source), before)
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.submodule_status(one), status)
        self.assert_owned_submodules(one)
        for repo, branch in zip(('libs/lib', 'libs/lib/deps/inner'), branches):
            self.git(one / repo, 'fsck', '--full')
            self.assertEqual(self.git(one / repo, 'status', '--porcelain').stdout, b'')
            self.assertEqual(self.git(one / repo, 'branch', '--show-current').stdout, branch)
        # The uninitialized submodule keeps its settings; the initialized ones keep theirs.
        self.assertEqual(self.git(one, 'config', 'submodule.lib-module.active').stdout.strip(), b'true')
        self.assertEqual(self.git(one, 'config', 'submodule.lib-module.url').stdout.strip(),
                         str(self.root / 'origins' / 'lib').encode())
        self.assertEqual(self.git(one / 'libs/lib', 'config', 'submodule.deps/inner.url').stdout.strip(),
                         str(self.root / 'origins' / 'inner').encode())
        self.assertEqual(self.git(one / 'libs/lib', 'remote', 'get-url', 'origin').stdout.strip(),
                         str(self.root / 'origins' / 'lib').encode())
        # Commits in a submodule and in the root, with no network and no source.
        (one / 'libs/lib/lib.txt').write_text('changed in the World\n')
        self.git(one / 'libs/lib', 'commit', '-qam', 'world lib change')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b' M libs/lib\n')
        self.git(one, 'commit', '-qam', 'bump lib')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one, 'rev-parse', 'HEAD:libs/lib').stdout,
                         self.git(one / 'libs/lib', 'rev-parse', 'HEAD').stdout)
        self.git(one, 'submodule', 'foreach', '--recursive', '-q', 'git rev-parse HEAD')
        # A fresh fork of the snapshot is untouched by that World.
        two, _ = self.fork('two')
        self.assertEqual(self.submodule_status(two), status)
        self.world('verify', 'S1')

    def test_submodule_stash_stacks_are_carried(self):
        self.submodule_fixture()
        lib, inner = self.source / 'libs/lib', self.source / 'libs/lib/deps/inner'
        self.make_stash_stack(lib, 'lib.txt')
        self.make_stash_stack(inner, 'lib.txt')
        views = [self.stash_view(repo) for repo in (lib, inner)]
        self.assertNotEqual(views[0], views[1])
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, wid = self.fork()
        self.world('checkpoint', wid)
        two, _ = self.fork('two', 'S2')
        for world in (one, two):
            for repo, view in zip(('libs/lib', 'libs/lib/deps/inner'), views):
                self.assertEqual(self.stash_view(world / repo), view)
                self.git(world / repo, 'fsck', '--full')
        # The root has no stash of its own.
        self.assertEqual(self.git(one, 'stash', 'list').stdout, b'')
        self.git(one / 'libs/lib', 'stash', 'pop', '-q')
        self.assertEqual((one / 'libs/lib/stashed-untracked').read_text(), 'untracked\n')

    def test_submodule_objects_are_cloned_and_checked_before_publication(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        self.git(lib, 'repack', '-adq')
        (lib / 'loose.txt').write_text('loose in the submodule\n')
        self.git(lib, 'add', 'loose.txt')
        self.git(lib, 'commit', '-qm', 'loose')
        self.git(self.source, 'commit', '-qam', 'bump lib')
        blob = self.git(lib, 'rev-parse', 'HEAD:loose.txt').stdout.decode().strip()
        objects = self.source / '.git/modules/lib-module/objects'
        self.assertTrue((objects / blob[:2] / blob[2:]).is_file())
        packs = sorted(p.name for p in (objects / 'pack').iterdir())
        (objects / 'pack' / 'tmp_pack_raced').write_bytes(b'partial')
        # A module repository that lost an object mid-import is not published either.
        env = dict(self.env)
        self.env['PATH'] = str(self.object_loss_wrapper(blob)) + os.pathsep + self.env['PATH']
        result = self.world('init', str(self.source), code=1)
        self.assertIn(os.strerror(errno.EBUSY).encode(), result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.env = env
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, _ = self.fork('one', snapshot)
        owned = one / '.world-git/repo.git/worktrees/active/modules/lib-module/objects'
        self.assertEqual(sorted(p.name for p in (owned / 'pack').iterdir()), packs)
        self.assertTrue((owned / blob[:2] / blob[2:]).is_file())
        for repo in ('libs/lib', 'libs/lib/deps/inner'):
            self.git(one / repo, 'fsck', '--full')
        self.assertEqual((one / 'libs/lib/loose.txt').read_text(), 'loose in the submodule\n')

    def test_old_style_submodule_is_absorbed_into_the_world(self):
        lib = self.origin('lib')
        self.git(self.source, 'clone', '-q', str(lib), 'libs/lib')
        self.sub(self.source, 'add', '-q', '--name', 'lib-module', str(lib), 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'old-style submodule')
        self.assertTrue((self.source / 'libs/lib/.git').is_dir())
        head = self.git(self.source / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip()
        self.world('init', str(self.source))
        self.assertTrue((self.source / 'libs/lib/.git').is_dir())
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, _ = self.fork()
        self.assertEqual((one / 'libs/lib/.git').read_text(),
                         'gitdir: ../../.world-git/repo.git/worktrees/active/modules/lib-module\n')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(one / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), head)
        self.git(one / 'libs/lib', 'fsck', '--full')
        self.world('verify', 'S1')

    def test_world_with_submodules_moves_checkpoints_forks_and_pools(self):
        status = self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / 'libs/lib/deps/inner/lib.txt').write_text('inner change\n')
        self.git(one / 'libs/lib/deps/inner', 'commit', '-qam', 'inner change')
        self.git(one / 'libs/lib', 'commit', '-qam', 'bump inner')
        self.git(one, 'commit', '-qam', 'bump lib')
        changed = self.submodule_status(one)
        moved = self.root / 'moved'
        one.rename(moved)
        self.assertEqual(self.git(moved, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.submodule_status(moved), changed)
        self.world('verify', str(moved))
        self.world('checkpoint', wid)
        two, wid2 = self.fork('two', 'S2')
        self.assertEqual(self.submodule_status(two), changed)
        self.assert_owned_submodules(two)
        three, _ = self.fork('three', wid)
        self.assertEqual(self.submodule_status(three), changed)
        self.world('discard', wid)
        self.world('restore', wid)
        self.assertEqual(self.submodule_status(moved), changed)
        # A registered linked worktree of a submodule blocks discard, fork and checkpoint.
        linked = self.root / 'linked'
        self.git(moved / 'libs/lib', 'worktree', 'add', '-q', '--detach', str(linked))
        self.world('discard', wid, '--now', '--force', code=3)
        refused = self.world('checkpoint', wid, code=3)
        self.assertIn(b'linked worktree', refused.stderr)
        self.git(moved / 'libs/lib', 'worktree', 'remove', str(linked))
        self.world('discard', wid, '--now')
        self.assertFalse(moved.exists())
        self.assertEqual(self.submodule_status(two), changed)
        # Pooled forks get the same per-World setup.
        self.world('pool', 'fill', 'S1', '--count', '1')
        pooled, _ = self.pool_fork('pooled', True)
        self.assertEqual(self.git(pooled, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.submodule_status(pooled), status)
        self.assert_owned_submodules(pooled)
        self.world('verify', 'S1')
        self.world('verify', 'S2')

    def test_changed_submodule_links_are_refused_by_fork(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        dot = one / 'libs/lib/.git'
        text = dot.read_text()
        dot.write_text('gitdir: ' + str((one / '.world-git/repo.git/worktrees/active/modules/lib-module').resolve()) + '\n')
        refused = self.world('checkpoint', wid, code=3)
        self.assertIn(b'reason: the .git link of submodule libs/lib was changed', refused.stderr)
        dot.write_text(text)
        self.world('checkpoint', wid)

    def test_dirty_submodule_needs_an_explicit_choice(self):
        status = self.submodule_fixture()
        lib = self.source / 'libs/lib'
        (lib / 'lib.txt').write_text('staged in the submodule\n')
        self.git(lib, 'add', 'lib.txt')
        (lib / 'lib.txt').write_text('unstaged in the submodule\n')
        (lib / 'untracked').write_text('untracked\n')
        sub_status = self.git(lib, 'status', '--porcelain').stdout
        self.world('init', str(self.source), code=3)
        self.world('init', str(self.source), '--include-changes')
        one, _ = self.fork()
        self.assertEqual(self.git(one / 'libs/lib', 'status', '--porcelain').stdout, sub_status)
        self.assertEqual(self.git(one / 'libs/lib', 'show', ':lib.txt').stdout, b'staged in the submodule\n')
        self.world('init', str(self.source), '--committed-only')
        two, _ = self.fork('two', 'S2')
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(two / 'libs/lib', 'status', '--porcelain', '--untracked-files=all').stdout, b'')
        self.assertEqual((two / 'libs/lib/lib.txt').read_text(), 'lib\n')
        self.assertEqual(self.submodule_status(two), status)
        # The source keeps its uncommitted submodule work.
        self.assertEqual(self.git(lib, 'status', '--porcelain').stdout, sub_status)

    def test_submodule_ahead_of_its_gitlink(self):
        status = self.submodule_fixture()
        lib = self.source / 'libs/lib'
        recorded = self.git(lib, 'rev-parse', 'HEAD').stdout.strip()
        (lib / 'lib.txt').write_text('ahead\n')
        self.git(lib, 'commit', '-qam', 'ahead of the gitlink')
        ahead = self.git(lib, 'rev-parse', 'HEAD').stdout.strip()
        self.world('init', str(self.source), code=3)
        self.world('init', str(self.source), '--include-changes')
        one, _ = self.fork()
        self.assertEqual(self.git(one / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), ahead)
        self.assertEqual(self.git(one / 'libs/lib', 'branch', '--show-current').stdout.strip(), b'main')
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b' M libs/lib\n')
        self.world('init', str(self.source), '--committed-only')
        two, wid = self.fork('two', 'S2')
        self.assertEqual(self.git(two / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), recorded)
        self.assertEqual(self.git(two / 'libs/lib', 'branch', '--show-current').stdout.strip(), b'')
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.submodule_status(two), status)
        # The commit it was ahead with is still in its repository.
        self.git(two / 'libs/lib', 'cat-file', '-e', ahead.decode())
        # A World checkpoint with --committed-only resets its submodules the same way.
        self.world('checkpoint', 'W1', code=3)
        self.world('checkpoint', 'W1', '--committed-only')
        three, _ = self.fork('three', 'S3')
        self.assertEqual(self.git(three / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), recorded)
        self.assertEqual(self.git(three, 'status', '--porcelain').stdout, b'')
        self.assertEqual(self.git(self.source / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), ahead)

    def test_committed_only_refuses_a_recorded_commit_the_submodule_lacks(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        (lib / 'lib.txt').write_text('recorded, then dropped\n')
        self.git(lib, 'commit', '-qam', 'to be dropped')
        self.git(self.source, 'commit', '-qam', 'record it')
        self.git(lib, 'reset', '-q', '--hard', 'HEAD~1')
        self.git(lib, 'update-ref', '-d', 'ORIG_HEAD')
        self.git(lib, 'reflog', 'expire', '--expire=now', '--all')
        self.git(lib, 'gc', '-q', '--prune=now')
        refused = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b'reason: submodule libs/lib: commit ', refused.stderr)
        self.assertIn(b'is not in its repository', refused.stderr)

    def test_committed_only_activates_lfs_in_a_submodule_target_only(self):
        import hashlib
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        old = self.git(lib, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.install_lfs(lib)
        self.git(lib, 'config', 'filter.lfs.smudge', 'git-lfs smudge --skip -- %f')
        self.git(lib, 'config', 'filter.lfs.process', 'git-lfs filter-process --skip')
        payload = b'committed target-only submodule LFS payload\n'
        oid = hashlib.sha256(payload).hexdigest()
        (lib / '.lfsconfig').write_text('[lfs]\n    url = ../relative-lfs\n')
        (lib / 'target.bin').write_bytes(payload)
        self.git(lib, 'add', '.gitattributes', '.lfsconfig', 'target.bin')
        self.git(lib, 'commit', '-qm', 'target-only LFS with relative endpoint')
        relative_target = self.git(lib, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(self.source, 'add', 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'record target-only LFS commit')
        self.git(lib, 'checkout', '-q', old)
        cache_root = self.lfs_object_path(lib, oid).parents[2]

        refused = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b'must be an absolute URL or absolute filesystem path', refused.stderr)
        self.assertEqual(self.git(lib, 'rev-parse', 'HEAD').stdout.strip(), old.encode())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

        # The same target is accepted after its committed endpoint is made location-independent.
        self.git(lib, 'checkout', '-q', relative_target)
        safe_config = '[lfs]\n    url = https://lfs.example.test/objects\n'
        (lib / '.lfsconfig').write_text(safe_config)
        self.git(lib, 'add', '.lfsconfig')
        self.git(lib, 'commit', '-qm', 'target-only LFS with absolute endpoint')
        target = self.git(lib, 'rev-parse', 'HEAD').stdout.strip()
        self.git(self.source, 'add', 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'record safe target-only LFS commit')
        self.git(lib, 'checkout', '-q', old)
        shutil.rmtree(cache_root, ignore_errors=True)

        snapshot = self.world('init', str(self.source), '--committed-only').stdout.split()[0].decode()
        self.assertEqual(self.git(lib, 'rev-parse', 'HEAD').stdout.strip(), old.encode())
        one, wid = self.fork('target-lfs', snapshot)
        owned_lib = one / 'libs/lib'
        self.assertEqual(self.git(owned_lib, 'rev-parse', 'HEAD').stdout.strip(), target)
        self.assertEqual(self.git(owned_lib, 'show', ':target.bin').stdout,
                         self.lfs_pointer(oid, len(payload)))
        self.assertEqual(self.git(owned_lib, 'config', '--get', 'filter.lfs.process').stdout.strip(),
                         b'git-lfs filter-process --skip')
        self.assertEqual(self.git(owned_lib, 'config', '--get', 'filter.lfs.smudge').stdout.strip(),
                         b'git-lfs smudge --skip -- %f')
        self.assertEqual(self.git(owned_lib, 'config', '--get', 'lfs.storage').stdout.strip(), b'lfs')
        hooks_path = self.git(owned_lib, 'config', '--get', 'core.hooksPath').stdout.decode().strip()
        hook = Path(hooks_path)
        if not hook.is_absolute():
            hook = owned_lib / hook
        self.assertTrue((hook / 'pre-push').is_file())
        self.assertEqual(self.git(owned_lib, 'status', '--porcelain').stdout, b'')

        # A managed submodule can become LFS-active only in the committed target. Verify the
        # managed-copy path provisions the owned filter and hook after checking the source.
        self.git(owned_lib, 'checkout', '-q', old)
        shutil.rmtree(self.lfs_object_path(owned_lib, oid).parents[2], ignore_errors=True)
        (hook / 'pre-push').unlink()
        self.assertEqual(self.git(owned_lib, 'rev-parse', 'HEAD').stdout.strip(), old.encode())
        self.assertFalse((hook / 'pre-push').exists())
        managed_snapshot = self.world('checkpoint', wid, '--committed-only').stdout.split()[0].decode()
        self.assertEqual(self.git(owned_lib, 'rev-parse', 'HEAD').stdout.strip(), old.encode())
        self.assertFalse((hook / 'pre-push').exists())
        managed_copy, _ = self.fork('managed-target-lfs', managed_snapshot)
        managed_lib = managed_copy / 'libs/lib'
        self.assertEqual(self.git(managed_lib, 'rev-parse', 'HEAD').stdout.strip(), target)
        self.assertEqual(self.git(managed_lib, 'config', '--get', 'filter.lfs.process').stdout.strip(),
                         b'git-lfs filter-process --skip')
        self.assertEqual(self.git(managed_lib, 'config', '--get', 'filter.lfs.smudge').stdout.strip(),
                         b'git-lfs smudge --skip -- %f')
        self.assertEqual(self.git(managed_lib, 'config', '--get', 'lfs.storage').stdout.strip(), b'lfs')
        managed_hooks = self.git(managed_lib, 'config', '--get', 'core.hooksPath').stdout.decode().strip()
        managed_hook = Path(managed_hooks)
        if not managed_hook.is_absolute():
            managed_hook = managed_lib / managed_hook
        self.assertTrue((managed_hook / 'pre-push').is_file())

    def test_submodule_ignore_setting_does_not_hide_a_dirty_submodule(self):
        self.submodule_fixture()
        self.git(self.source, 'config', 'submodule.lib-module.ignore', 'all')
        self.git(self.source, 'config', 'diff.ignoreSubmodules', 'all')
        (self.source / 'libs/lib/lib.txt').write_text('hidden by ignore=all\n')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source), code=3)
        (self.source / 'libs/lib/lib.txt').write_text('lib\n')
        (self.source / 'libs/lib/lib.txt').write_text('lib\n')
        self.git(self.source / 'libs/lib', 'commit', '-q', '--allow-empty', '-m', 'moved on')
        self.assertEqual(self.git(self.source, 'status', '--porcelain').stdout, b'')
        self.world('init', str(self.source), code=3)
        self.git(self.source / 'libs/lib', 'reset', '-q', '--hard', 'HEAD~1')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', 'submodule.lib-module.ignore').stdout.strip(), b'all')

    def test_submodule_update_command_is_refused_and_never_run(self):
        self.submodule_fixture()
        marker = self.root / 'update-ran'
        self.git(self.source, 'config', 'submodule.vendor/unused.url', str(self.root / 'origins' / 'unused'))
        self.git(self.source, 'config', 'submodule.vendor/unused.update', '!touch ' + str(marker))
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule.vendor/unused.update = !touch', refused.stderr)
        self.git(self.source / 'libs/lib', 'config', 'submodule.deps/inner.update', '!touch ' + str(marker))
        self.git(self.source, 'config', 'submodule.vendor/unused.update', 'rebase')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule libs/lib: submodule.deps/inner.update = !touch', refused.stderr)
        self.git(self.source / 'libs/lib', 'config', '--unset', 'submodule.deps/inner.update')
        self.world('init', str(self.source))
        self.assertFalse(marker.exists())
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', 'submodule.vendor/unused.update').stdout.strip(), b'rebase')
        self.assertEqual(self.git(one, 'config', 'submodule.vendor/unused.url').stdout.strip(),
                         str(self.root / 'origins' / 'unused').encode())
        # The uninitialized submodule can be initialized in the World from its carried URL.
        self.git(one, 'config', 'submodule.vendor/unused.update', 'checkout')
        self.sub(one, 'update', '-q', '--init', 'vendor/unused')
        self.assertEqual((one / 'vendor/unused/lib.txt').read_text(), 'unused\n')
        self.assertTrue((one / '.world-git/repo.git/worktrees/active/modules/vendor/unused').is_dir())
        self.assertFalse(marker.exists())

    def test_relative_configured_submodule_url_is_made_absolute(self):
        self.submodule_fixture()
        self.git(self.source, 'config', 'submodule.vendor/unused.url', '../origins/unused')
        # Git clones a configured URL from the worktree top even when there is a remote.
        self.git(self.source, 'remote', 'add', 'origin', 'https://example.com/project.git')
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        one, _ = self.fork()
        self.assertEqual(self.git(one, 'config', 'submodule.vendor/unused.url').stdout.strip(),
                         str(self.root / 'origins' / 'unused').encode())
        self.sub(one, 'update', '-q', '--init', 'vendor/unused')
        self.assertEqual((one / 'vendor/unused/lib.txt').read_text(), 'unused\n')

    def test_world_git_inside_a_submodule_is_ordinary_content(self):
        self.submodule_fixture()
        child = self.source / 'libs/lib/.world-git/child'
        head = self.nested_repo(child)
        # Only the World's root owns `.world-git`; in a submodule it is a directory like any
        # other, and the repository in it is checked like any nested repository.
        (child / '.git/objects/info/alternates').write_text('/elsewhere/objects\n')
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: submodule libs/lib: nested Git repository at ' + str(child).encode()
                      + b': it borrows objects from another repository', refused.stderr)
        (child / '.git/objects/info/alternates').unlink()
        self.world('init', str(self.source), '--include-changes')
        one, _ = self.fork()
        self.assertEqual(self.git(one / 'libs/lib/.world-git/child', 'rev-parse', 'HEAD').stdout.strip(), head)
        self.assertEqual(self.git(one / 'libs/lib', 'status', '--porcelain').stdout, b'?? .world-git/\n')

    def test_submodule_filters_and_hooks_never_run(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        filter_marker = self.root / 'filter-ran'
        hook_marker = self.root / 'hook-ran'
        self.write_hook(Path(self.git(lib, 'rev-parse', '--path-format=absolute', '--git-path', 'hooks').stdout.decode().strip()) / 'post-checkout', hook_marker)
        (lib / '.gitattributes').write_text('*.txt filter=example\n')
        self.git(lib, 'add', '.gitattributes')
        self.git(lib, 'commit', '-qm', 'attributes')
        self.git(self.source, 'commit', '-qam', 'bump lib')
        self.git(lib, 'config', 'filter.example.clean', 'touch ' + str(filter_marker) + '; cat')
        self.git(lib, 'config', 'filter.example.smudge', 'touch ' + str(filter_marker) + '; cat')
        for flag in ((), ('--include-changes',), ('--committed-only',)):
            refused = self.world('init', str(self.source), *flag, code=3)
            self.assertIn(b"reason: submodule libs/lib: tracked file lib.txt uses the 'example' filter", refused.stderr)
        self.assertFalse(filter_marker.exists())
        self.git(lib, 'config', '--remove-section', 'filter.example')
        self.world('init', str(self.source), '--with-hooks')
        one, _ = self.fork()
        self.world('checkpoint', 'W1', '--committed-only')
        self.assertFalse(filter_marker.exists())
        self.assertEqual(self.hook_runs(hook_marker), [])
        # --with-hooks carries the submodule's hooks into its owned repository, never run.
        hooks = Path(self.git(one / 'libs/lib', 'rev-parse', '--path-format=absolute', '--git-path', 'hooks').stdout.decode().strip())
        self.assertTrue((hooks / 'post-checkout').exists())

    def test_nested_repositories_that_are_not_submodules_are_refused(self):
        self.submodule_fixture()
        # A plain repository below the uninitialized submodule's directory.
        (self.source / 'vendor/unused/deeper').mkdir()
        self.git(self.source / 'vendor/unused/deeper', 'init', '-q')
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: nested Git repository at ' + str(self.source / 'vendor/unused/deeper').encode()
                      + b': it is inside the directory of uninitialized submodule vendor/unused', refused.stderr)
        shutil.rmtree(self.source / 'vendor/unused/deeper')
        # One inside an initialized submodule that reaches outside is refused there too (see
        # test_nested_repository_in_a_submodule_is_its_untracked_content).
        extra = self.source / 'libs/lib/extra'
        self.nested_repo(extra)
        (extra / '.git/commondir').write_text('../../elsewhere\n')
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: submodule libs/lib: nested Git repository at ' + str(extra).encode()
                      + b': it is the administration of a linked worktree (commondir)', refused.stderr)
        shutil.rmtree(extra)
        # A submodule whose .git points at some other repository.
        stranger = self.origin('stranger')
        dot = self.source / 'libs/lib/.git'
        text = dot.read_text()
        dot.write_text('gitdir: ' + str(stranger / '.git') + '\n')
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b"reason: the .git of submodule libs/lib points outside its superproject's modules/lib-module", refused.stderr)
        dot.write_text(text)
        # A symlink in place of a submodule directory.
        self.sub(self.source, 'deinit', '-q', '--force', 'libs/lib')
        (self.source / 'libs/lib').rmdir()
        (self.source / 'libs/lib').symlink_to(stranger)
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: submodule path libs/lib is not a directory', refused.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])

    def test_nested_repository_in_a_submodule_is_its_untracked_content(self):
        self.submodule_fixture()
        # A self-contained repository inside an initialized submodule is that submodule's
        # untracked content, carried as files like one in the root.
        extra = self.source / 'libs/lib/extra'
        head = self.nested_repo(extra)
        refused = self.world('init', str(self.source), code=3)
        self.assertNotIn(b'nested', refused.stderr)
        self.world('init', str(self.source), '--include-changes')
        shutil.rmtree(self.source)
        one, wid = self.fork()
        self.assertEqual(self.git(one / 'libs/lib', 'status', '--porcelain').stdout, b'?? extra/\n')
        self.assertEqual(self.git(one / 'libs/lib/extra', 'rev-parse', 'HEAD').stdout.strip(), head)
        self.git(one / 'libs/lib/extra', 'commit', '-q', '--allow-empty', '-m', 'in the World')
        self.world('checkpoint', wid, '--include-changes')
        two, _ = self.fork('two', 'S2')
        self.assertEqual(self.git(two / 'libs/lib/extra', 'log', '--format=%s').stdout, b'in the World\nnested\n')
        # --committed-only leaves it behind with the rest of the submodule's untracked content.
        self.world('checkpoint', wid, '--committed-only')
        three, _ = self.fork('three', 'S3')
        self.assertFalse((three / 'libs/lib/extra').exists())
        self.assertEqual(self.git(three / 'libs/lib', 'status', '--porcelain').stdout, b'')

    def after_first_status(self, repo, name, action, command='status'):
        """A `git` on PATH that runs the shell `action` once, right after the first successful
        `command` (by default `status`, an admission check) whose -C is `repo`, before the tree
        is copied."""
        import shlex
        real_git = shutil.which('git')
        wrapper = self.root / ('race-bin-' + name)
        wrapper.mkdir()
        done = self.root / ('race-done-' + name)
        script = wrapper / 'git'
        script.write_text('#!/bin/sh\ncwd=\nprev=\nstatus=0\nfor arg in "$@"; do\n'
                          + '  [ "$prev" = -C ] && cwd=$arg\n  [ "$arg" = ' + command + ' ] && status=1\n  prev=$arg\ndone\n'
                          + shlex.quote(real_git) + ' "$@"\nresult=$?\n'
                          + 'if [ "$result" = 0 ] && [ "$status" = 1 ] && [ "$cwd" = ' + shlex.quote(str(repo))
                          + ' ] && [ ! -e ' + shlex.quote(str(done)) + ' ]; then\n'
                          + '  : > ' + shlex.quote(str(done)) + ' || exit $?\n  ' + action + ' || exit $?\n'
                          + 'fi\nexit "$result"\n')
        script.chmod(0o700)
        return wrapper, done

    def test_submodule_changes_after_the_clean_check_are_not_published(self):
        import shlex
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        path = self.env['PATH']
        # A tracked file of a submodule edited after its own clean check.
        wrapper, done = self.after_first_status(lib, 'edit', 'printf "edited\\n" > ' + shlex.quote(str(lib / 'lib.txt')))
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        result = self.world('init', str(self.source), code=3)
        self.env['PATH'] = path
        self.assertTrue(done.exists())
        self.assertIn(b'uncommitted', result.stderr.lower())
        self.git(lib, 'checkout', '-q', '--', 'lib.txt')
        # The uninitialized submodule gains a `.git` into the source's kept module repository
        # after the check: the copy would hold a link back into the source.
        unused = self.source / 'vendor/unused'
        self.assertTrue((self.source / '.git/modules/vendor/unused').is_dir())
        wrapper, done = self.after_first_status(self.source, 'init', 'printf "gitdir: ../../.git/modules/vendor/unused\\n" > '
                                                + shlex.quote(str(unused / '.git')))
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        self.world('init', str(self.source), code=1)
        self.env['PATH'] = path
        self.assertTrue(done.exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        (unused / '.git').unlink()
        self.world('init', str(self.source))

    def test_gitmodules_renamed_after_capture_is_not_published(self):
        import shlex
        self.submodule_fixture()
        # The deepest submodule is captured last; right after its history scan, the root's
        # .gitmodules renames the initialized submodule before the tree is copied.
        rename = (shlex.quote(shutil.which('git')) + ' config -f ' + shlex.quote(str(self.source / '.gitmodules'))
                  + ' --rename-section submodule.lib-module submodule.renamed')
        wrapper, done = self.after_first_status(self.source / 'libs/lib/deps/inner', 'rename', rename, 'rev-list')
        path = self.env['PATH']
        self.env['PATH'] = str(wrapper) + os.pathsep + path
        self.world('init', str(self.source), '--include-changes', code=1)
        self.env['PATH'] = path
        self.assertTrue(done.exists())
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        # Seen at capture time, the new name no longer matches the source's module repository.
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b"points outside its superproject's modules/renamed", refused.stderr)
        self.git(self.source, 'config', '-f', '.gitmodules', '--rename-section', 'submodule.renamed', 'submodule.lib-module')
        snapshot = self.world('init', str(self.source), '--include-changes').stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')

    def test_relative_gitmodules_url_needs_a_remote_or_a_configured_url(self):
        self.submodule_fixture()
        self.git(self.source, 'config', '-f', '.gitmodules', 'submodule.vendor/unused.url', '../origins/unused')
        self.git(self.source, 'commit', '-qam', 'relative url')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule vendor/unused: its .gitmodules url ../origins/unused is relative', refused.stderr)
        refused = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b'its .gitmodules url ../origins/unused is relative', refused.stderr)
        # A configured URL decides instead of .gitmodules.
        self.git(self.source, 'config', 'submodule.vendor/unused.url', str(self.root / 'origins' / 'unused'))
        self.world('init', str(self.source))
        self.git(self.source, 'config', '--unset', 'submodule.vendor/unused.url')
        # With a remote origin, Git resolves it against the carried remote URL, the same everywhere.
        self.git(self.source, 'remote', 'add', 'origin', str(self.root / 'origins' / 'project.git'))
        self.world('init', str(self.source))

    def test_relative_gitmodules_url_resolves_against_the_branch_remote(self):
        # libs/lib is on main, whose remote is `upstream` (no origin), and records an
        # uninitialized child at ../x.git: Git resolves that against upstream's URL, which the
        # World carries, so it resolves the same there.
        up = self.root / 'origins' / 'up'
        up.mkdir(parents=True)
        x = self.origin('x')
        self.git(self.root, 'clone', '-q', '--bare', str(x), str(up / 'x.git'))
        x_head = self.git(x, 'rev-parse', 'HEAD').stdout.strip().decode()
        lib = self.origin('lib')
        self.git(lib, 'update-index', '--add', '--cacheinfo', '160000,' + x_head + ',child')
        (lib / '.gitmodules').write_text('[submodule "child"]\n\tpath = child\n\turl = ../x.git\n')
        self.git(lib, 'add', '.gitmodules')
        self.git(lib, 'commit', '-qm', 'child')
        self.sub(self.source, 'add', '-q', str(lib), 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'lib')
        sub = self.source / 'libs/lib'
        self.git(sub, 'remote', 'rename', 'origin', 'upstream')
        self.git(sub, 'remote', 'set-url', 'upstream', str(up / 'lib.git'))
        self.git(sub, 'remote', 'add', 'other', str(self.root / 'origins' / 'other.git'))
        self.assertEqual(self.git(sub, 'config', 'branch.main.remote').stdout.strip(), b'upstream')
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.sub(one / 'libs/lib', 'update', '-q', '--init', 'child')
        self.assertEqual(self.git(one / 'libs/lib/child', 'rev-parse', 'HEAD').stdout.strip().decode(), x_head)
        self.assertEqual(self.git(one / 'libs/lib', 'config', 'submodule.child.url').stdout.strip(),
                         str(up / 'x.git').encode())
        # An explicitly empty branch remote is still the selected one: Git finds no URL for it.
        self.git(sub, 'config', 'branch.main.remote', '')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b"reason: submodule libs/lib/child: its .gitmodules url ../x.git is relative and the repository's default remote (an empty branch.<name>.remote)",
                      refused.stderr)
        self.git(sub, 'config', 'branch.main.remote', 'upstream')
        # --committed-only detaches a submodule that is ahead of its gitlink; detached, with two
        # remotes and no origin, Git would resolve against the World's own directory.
        self.identify(sub)
        self.git(sub, 'commit', '-q', '--allow-empty', '-m', 'ahead')
        refused = self.world('init', str(self.source), '--committed-only', code=3)
        self.assertIn(b"reason: submodule libs/lib/child: its .gitmodules url ../x.git is relative and the repository's default remote (origin)",
                      refused.stderr)

    def test_publish_refuses_a_repointed_submodule_gitfile(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.commit_in(one, 'root change')
        stranger = self.root / 'stranger'
        self.git(self.root, 'clone', '-q', str(one / 'libs/lib'), str(stranger))
        (one / 'libs/lib/.git').write_text('gitdir: ' + str(stranger / '.git') + '\n')
        self.assertEqual(self.git(stranger, 'status', '--porcelain').stdout, b'')
        refused = self.world('publish', wid, code=3)
        self.assertIn(b'reason: the .git link of submodule libs/lib was changed', refused.stderr)
        self.git(self.source, 'rev-parse', '--verify', '-q', 'refs/heads/world/W1', code=1)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs/').stdout, b'')

    def test_relative_gitmodules_url_is_rechecked_when_forking_a_world(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        x = self.origin('x')
        x_head = self.git(x, 'rev-parse', 'HEAD').stdout.strip().decode()
        # In the World, the branch tracks up2 of two non-origin remotes; a fork's own branch has
        # no upstream, so there Git would resolve ../x.git against origin, which has no URL.
        self.git(one, 'remote', 'add', 'up1', str(self.root / 'origins' / 'up1' / 'project.git'))
        self.git(one, 'remote', 'add', 'up2', str(self.root / 'origins' / 'up2' / 'project.git'))
        self.git(one, 'config', 'branch.' + 'world/' + wid + '.remote', 'up2')
        self.git(one, 'update-index', '--add', '--cacheinfo', '160000,' + x_head + ',child')
        (one / '.gitmodules').write_text('[submodule "child"]\n\tpath = child\n\turl = ../x.git\n')
        self.git(one, 'add', '.gitmodules')
        self.git(one, 'commit', '-qm', 'child')
        (one / 'child').mkdir()
        refused = self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
        self.assertIn(b"reason: submodule child: its .gitmodules url ../x.git is relative and the repository's default remote (origin)",
                      refused.stderr)
        self.assertFalse((self.root / 'refused').exists())
        self.world('checkpoint', wid, code=3)
        self.git(one, 'remote', 'add', 'origin', str(self.root / 'origins' / 'project.git'))
        two, _ = self.fork('two', wid)
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')

    def test_every_gitlink_name_is_validated(self):
        self.submodule_fixture()
        unused = self.git(self.source, 'rev-parse', 'HEAD:vendor/unused').stdout.strip().decode()
        def gitlink(path, name):
            self.git(self.source, 'update-index', '--add', '--cacheinfo', '160000,' + unused + ',' + path)
            if name:
                self.git(self.source, 'config', '-f', '.gitmodules', 'submodule.' + name + '.path', path)
                self.git(self.source, 'config', '-f', '.gitmodules', 'submodule.' + name + '.url', str(self.root / 'origins' / 'unused'))
                self.git(self.source, 'add', '.gitmodules')
            self.git(self.source, 'commit', '-qm', 'gitlink ' + path)
        def undo():
            self.git(self.source, 'reset', '-q', '--hard', 'HEAD~1')
        gitlink('escape', '../escape')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule escape has an unsafe name (../escape)', refused.stderr)
        undo()
        gitlink('inside', 'lib-module/inside')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule names lib-module/inside and lib-module share a repository directory', refused.stderr)
        undo()
        # Names that differ only in ASCII case share a directory on a case-insensitive volume.
        for first, second in (('Lib', 'lib'), ('Lib', 'lib/x')):
            with self.subTest(names=(first, second)):
                gitlink('case-a', first)
                gitlink('case-b', second)
                refused = self.world('init', str(self.source), code=3)
                self.assertIn(b'reason: submodule names ' + first.encode() + b' and ' + second.encode()
                              + b' share a repository directory', refused.stderr)
                self.git(self.source, 'reset', '-q', '--hard', 'HEAD~2')
        # An embedded gitlink without any .gitmodules entry is left exactly as it is.
        gitlink('embedded', None)
        (self.source / 'embedded').mkdir()
        status = self.git(self.source, 'status', '--porcelain').stdout
        snapshot = self.world('init', str(self.source)).stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, status)
        self.assertEqual(self.git(one, 'ls-files', '--stage', 'embedded').stdout,
                         self.git(self.source, 'ls-files', '--stage', 'embedded').stdout)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'][0]['name'], 'source')

    def test_committed_only_validates_the_committed_gitmodules(self):
        self.submodule_fixture()
        unused = self.git(self.source, 'rev-parse', 'HEAD:vendor/unused').stdout.strip().decode()
        for name, reason in (('../escape', b'reason: submodule escape has an unsafe name (../escape)'),
                             ('lib-module/inside', b'reason: submodule names lib-module/inside and lib-module share a repository directory')):
            with self.subTest(name=name):
                self.git(self.source, 'update-index', '--add', '--cacheinfo', '160000,' + unused + ',escape')
                self.git(self.source, 'config', '-f', '.gitmodules', 'submodule.' + name + '.path', 'escape')
                self.git(self.source, 'add', '.gitmodules')
                self.git(self.source, 'commit', '-qm', 'committed ' + name)
                # A safe name in the worktree only; the reset publishes the committed one.
                self.git(self.source, 'config', '-f', '.gitmodules', '--rename-section', 'submodule.' + name, 'submodule.safe')
                self.world('init', str(self.source), '--include-changes')
                refused = self.world('init', str(self.source), '--committed-only', code=3)
                self.assertIn(reason, refused.stderr)
                self.git(self.source, 'reset', '-q', '--hard', 'HEAD~1')

    def test_deleted_gitmodules_is_an_empty_mapping(self):
        # Only an uninitialized submodule: nothing to import, and the World has no .gitmodules.
        unused = self.origin('unused')
        self.sub(self.source, 'add', '-q', str(unused), 'vendor/unused')
        self.git(self.source, 'commit', '-qm', 'submodule')
        self.sub(self.source, 'deinit', '-q', 'vendor/unused')
        (self.source / '.gitmodules').unlink()
        status = self.git(self.source, 'status', '--porcelain').stdout
        snapshot = self.world('init', str(self.source), '--include-changes').stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertFalse((one / '.gitmodules').exists())
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, status)
        # An initialized submodule needs the entry the published tree no longer has.
        self.git(self.source, 'checkout', '--', '.gitmodules')
        lib = self.origin('lib')
        self.sub(self.source, 'add', '-q', '--name', 'lib-module', str(lib), 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'lib')
        (self.source / '.gitmodules').unlink()
        refused = self.world('init', str(self.source), '--include-changes', code=3)
        self.assertIn(b'reason: submodule libs/lib has no entry in the .gitmodules that would be published', refused.stderr)

    def test_gitmodules_url_is_checked_like_a_carried_url(self):
        self.submodule_fixture()
        unused = str(self.root / 'origins' / 'unused')
        rules = self.root / 'rewrite-rules'
        rules.write_text('[url "/nowhere/"]\n insteadOf = ' + unused + '\n')
        global_config = self.root / 'rewrite-global'
        global_config.write_text('[includeIf "gitdir:' + str(self.root / 'elsewhere') + '/"]\n path = ' + str(rules) + '\n')
        def gitmodules_url(url):
            self.git(self.source, 'config', '-f', '.gitmodules', 'submodule.vendor/unused.url', url)
            self.git(self.source, 'commit', '-qam', 'url ' + url)
        def init(code):
            self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
            result = self.world('init', str(self.source), code=code)
            self.env['GIT_CONFIG_GLOBAL'] = '/dev/null'
            return result
        # The absolute URL only .gitmodules gives, and the same URL resolved from a relative one.
        refused = init(3)
        self.assertIn(b'reason: submodule vendor/unused: its url resolves to ' + unused.encode()
                      + b', which url./nowhere/.insteadOf from a conditional include', refused.stderr)
        self.git(self.source, 'remote', 'add', 'origin', str(self.root / 'origins' / 'project.git'))
        gitmodules_url('../unused')
        refused = init(3)
        self.assertIn(b'its url resolves to ' + unused.encode(), refused.stderr)
        rules.write_text('')
        init(0)
        # A plain relative path is cloned from the worktree top, a different place in the World.
        gitmodules_url('unused.git')
        refused = init(3)
        self.assertIn(b'its .gitmodules url unused.git is a path Git takes relative to the worktree', refused.stderr)

    def test_relative_paths_set_inside_a_world_are_refused_by_fork(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        cases = (
            (one, 'submodule.vendor/unused.url', '../origins/unused', b"the World's submodule.vendor/unused.url is the relative path ../origins/unused"),
            (one, 'remote.up.url', '../origins/up.git', b"the World's remote.up.url is the relative path ../origins/up.git"),
            (one / 'libs/lib', 'remote.origin.url', '../lib.git', b"reason: submodule libs/lib: the World's remote.origin.url is the relative path ../lib.git"),
            (one, 'core.hooksPath', '../shared-hooks', b"the World's core.hooksPath ../shared-hooks leaves its tree"),
        )
        for repo, key, value, reason in cases:
            with self.subTest(key=key):
                previous = subprocess.run(['git', '-C', str(repo), 'config', key], env=self.env, capture_output=True).stdout
                self.git(repo, 'config', key, value)
                refused = self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
                self.assertIn(reason, refused.stderr)
                self.world('checkpoint', wid, code=3)
                if previous:
                    self.git(repo, 'config', key, previous.decode().strip())
                else:
                    self.git(repo, 'config', '--unset', key)
        self.fork('two', wid)

    def test_managed_configuration_is_read_with_its_includes(self):
        self.world('init', str(self.source))
        one, wid = self.fork()
        x = self.origin('x')
        x_head = self.git(x, 'rev-parse', 'HEAD').stdout.strip().decode()
        self.git(one, 'remote', 'add', 'up1', str(self.root / 'origins' / 'up1' / 'project.git'))
        self.git(one, 'remote', 'add', 'up2', str(self.root / 'origins' / 'up2' / 'project.git'))
        self.git(one, 'update-index', '--add', '--cacheinfo', '160000,' + x_head + ',child')
        (one / '.gitmodules').write_text('[submodule "child"]\n\tpath = child\n\turl = ../x.git\n')
        self.git(one, 'add', '.gitmodules')
        self.git(one, 'commit', '-qm', 'child')
        (one / 'child').mkdir()
        self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
        # origin comes only from an included file; the copy keeps that include.
        included = self.root / 'origin-include'
        included.write_text('[remote "origin"]\n\turl = ' + str(self.root / 'origins' / 'project.git') + '\n')
        self.git(one, 'config', 'include.path', str(included))
        two, _ = self.fork('two', wid)
        self.assertEqual(self.git(two, 'remote', 'get-url', 'origin').stdout.strip(),
                         str(self.root / 'origins' / 'project.git').encode())
        # A branch upstream from an included file cannot be removed for the new World's branch.
        included.write_text(included.read_text() + '[branch "world/W3"]\n\tremote = origin\n')
        refused = self.world('fork', '--from', wid, '--to', str(self.root / 'three'), code=3)
        self.assertIn(b'reason: branch.world/W3 settings come from a file the World', refused.stderr)

    def test_relative_gitmodules_url_uses_the_shared_branch_remote(self):
        # libs/lib is on main with a local origin but no local branch remote; the global
        # configuration, which the World shares, makes `amb` main's remote.
        amb = self.root / 'origins' / 'amb'
        amb.mkdir(parents=True)
        x = self.origin('x')
        self.git(self.root, 'clone', '-q', '--bare', str(x), str(amb / 'x.git'))
        x_head = self.git(x, 'rev-parse', 'HEAD').stdout.strip().decode()
        lib = self.origin('lib')
        self.git(lib, 'update-index', '--add', '--cacheinfo', '160000,' + x_head + ',child')
        (lib / '.gitmodules').write_text('[submodule "child"]\n\tpath = child\n\turl = ../x.git\n')
        self.git(lib, 'add', '.gitmodules')
        self.git(lib, 'commit', '-qm', 'child')
        self.sub(self.source, 'add', '-q', str(lib), 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'lib')
        sub = self.source / 'libs/lib'
        self.git(sub, 'config', '--unset', 'branch.main.remote')
        global_config = self.root / 'shared-remote-global'
        def shared(url):
            global_config.write_text('[branch "main"]\n\tremote = amb\n[remote "amb"]\n\turl = ' + url + '\n')
        shared('../amb/lib.git')
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule libs/lib/child: its .gitmodules url ../x.git resolves against remote.amb.url in global or system configuration',
                      refused.stderr)
        shared(str(amb / 'lib.git'))
        self.world('init', str(self.source))
        one, _ = self.fork()
        self.sub(one / 'libs/lib', 'update', '-q', '--init', 'child')
        self.assertEqual(self.git(one / 'libs/lib', 'config', 'submodule.child.url').stdout.strip(),
                         str(amb / 'x.git').encode())
        self.assertEqual(self.git(one / 'libs/lib/child', 'rev-parse', 'HEAD').stdout.strip().decode(), x_head)

    def test_shared_submodule_url_is_classified_like_a_carried_one(self):
        self.submodule_fixture()
        # vendor/unused is uninitialized with no configured URL; a global one then wins.
        global_config = self.root / 'shared-submodule-global'
        self.env['GIT_CONFIG_GLOBAL'] = str(global_config)
        global_config.write_text('[submodule "vendor/unused"]\n\turl = ../origins/unused\n')
        refused = self.world('init', str(self.source), code=3)
        self.assertIn(b'reason: submodule.vendor/unused.url is the relative path ../origins/unused in global or system configuration',
                      refused.stderr)
        global_config.write_text('[submodule "vendor/unused"]\n\turl = ' + str(self.root / 'origins' / 'unused') + '\n')
        self.world('init', str(self.source))
        # A repository-local value wins over the shared one, and is made absolute. (A relative
        # shared value would still be refused: every repository, libs/lib too, reads it.)
        global_config.write_text('[submodule "vendor/unused"]\n\turl = /elsewhere\n')
        self.git(self.source, 'config', 'submodule.vendor/unused.url', '../origins/unused')
        self.world('init', str(self.source))

    def test_dormant_submodule_urls_are_classified_too(self):
        # A submodule.<name>.url for no current gitlink (another branch's submodule).
        self.git(self.source, 'config', 'submodule.other.url', '../origins/other')
        self.world('init', str(self.source))
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'config', 'submodule.other.url').stdout.strip(),
                         str(self.root / 'origins' / 'other').encode())
        self.git(one, 'config', 'submodule.other.url', '../elsewhere')
        refused = self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), code=3)
        self.assertIn(b"reason: the World's submodule.other.url is the relative path ../elsewhere", refused.stderr)
        self.world('checkpoint', wid, code=3)

    def test_publish_refuses_a_gitlink_commit_the_target_lacks(self):
        self.submodule_fixture()
        self.world('init', str(self.source))
        one, wid = self.fork()
        (one / 'file').write_text('root only\n')
        self.git(one, 'commit', '-qam', 'root only')
        self.world('publish', wid)
        other = self.root / 'other'
        self.git(self.root, 'clone', '-q', str(self.source), str(other))
        (one / 'libs/lib/lib.txt').write_text('only in the World\n')
        self.git(one / 'libs/lib', 'commit', '-qam', 'world lib change')
        missing = self.git(one / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip()
        self.git(one, 'commit', '-qam', 'bump lib')
        before = self.git(self.source, 'rev-parse', 'world/W1').stdout.strip()
        refused = self.world('publish', wid, code=3)
        self.assertIn(b'record submodule libs/lib at ' + missing, refused.stderr)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1').stdout.strip(), before)
        self.assertEqual(self.git(self.source, 'for-each-ref', 'refs/worldfs/').stdout, b'')
        # Once the target's submodule has that commit, the same publish goes through.
        self.git(self.source / 'libs/lib', 'fetch', '-q', str(one / 'libs/lib'), missing.decode())
        self.world('publish', wid)
        self.assertEqual(self.git(self.source, 'rev-parse', 'world/W1:libs/lib').stdout.strip(), missing)
        # A target with the submodule uninitialized cannot check it out either.
        refused = self.world('publish', wid, '--repo', str(other), code=3)
        self.assertIn(b'does not have that submodule initialized', refused.stderr)

    # ---- shallow clones --------------------------------------------------------------------

    def shallow_clone(self, name='shallow', depth=2):
        """A `--depth` clone of the source after three more commits: base, c1, c2, c3 there,
        only the last `depth` of them here. Its origin is the source, by file:// URL."""
        for n in (1, 2, 3):
            (self.source / 'file').write_text('c%d\n' % n)
            self.git(self.source, 'commit', '-qam', 'c%d' % n)
        clone = self.root / name
        self.git(self.root, 'clone', '-q', '--depth', str(depth), 'file://' + str(self.source), str(clone))
        self.identify(clone)
        self.assertEqual(self.git(clone, 'rev-parse', '--is-shallow-repository').stdout.strip(), b'true')
        return clone

    def history(self, repo, rev='HEAD'):
        return self.git(repo, 'log', '--format=%s', rev).stdout.decode().splitlines()

    def test_shallow_clone_is_imported_with_its_boundary(self):
        shallow = self.shallow_clone()
        boundary = (shallow / '.git/shallow').read_bytes()
        # Everything the import walks, on both sides of the boundary: a stash entry, ORIG_HEAD
        # and FETCH_HEAD next to the refs.
        (shallow / 'file').write_text('stashed\n')
        self.git(shallow, 'stash', 'push', '-q', '-m', 'on a shallow clone')
        self.git(shallow, 'reset', '-q', 'HEAD')
        self.git(shallow, 'fetch', '-q', 'origin')
        stash = self.stash_view(shallow)
        snapshot = self.world('init', str(shallow)).stdout.split()[0].decode()
        self.assertEqual((shallow / '.git/shallow').read_bytes(), boundary)
        # --committed-only from a dirty shallow clone: the copy is reset to HEAD.
        (shallow / 'file').write_text('uncommitted\n')
        (shallow / 'untracked').write_text('left behind\n')
        committed = self.world('init', str(shallow), '--committed-only').stdout.split()[0].decode()
        shutil.rmtree(shallow)
        one, wid = self.fork('one', snapshot)
        owned = one / '.world-git/repo.git'
        self.assertEqual((owned / 'shallow').read_bytes(), boundary)
        self.assertEqual(self.git(one, 'rev-parse', '--is-shallow-repository').stdout.strip(), b'true')
        self.assertEqual(self.history(one), ['c3', 'c2'])
        self.assertEqual(self.stash_view(one), stash)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.git(one, 'fsck', '--full')
        three, _ = self.fork('three', committed)
        self.assertEqual((three / '.world-git/repo.git/shallow').read_bytes(), boundary)
        self.assertEqual((three / 'file').read_text(), 'c3\n')
        self.assertFalse((three / 'untracked').exists())
        self.assertEqual(self.git(three, 'status', '--porcelain').stdout, b'')
        # Git's own shallow semantics in the World: the carried origin deepens it.
        self.git(one, 'commit', '-q', '--allow-empty', '-m', 'in the World')
        self.git(one, 'fetch', '-q', '--deepen=1', 'origin')
        self.assertEqual(self.history(one), ['in the World', 'c3', 'c2', 'c1'])
        deepened = (owned / 'shallow').read_bytes()
        self.assertNotEqual(deepened, boundary)
        self.git(one, 'fsck', '--full')
        # A fork, a checkpoint and a pooled fork of the shallow World carry its boundary.
        two, _ = self.fork('two', wid)
        self.assertEqual((two / '.world-git/repo.git/shallow').read_bytes(), deepened)
        self.assertEqual(self.history(two), ['in the World', 'c3', 'c2', 'c1'])
        self.git(two, 'fsck', '--full')
        checkpoint = self.world('checkpoint', wid).stdout.split()[0].decode()
        self.world('pool', 'fill', checkpoint, '--count', '1')
        four = self.root / 'four'
        out = self.world('fork', '--from', checkpoint, '--to', str(four)).stdout
        self.assertIn(b'(pool)', out)
        self.assertEqual((four / '.world-git/repo.git/shallow').read_bytes(), deepened)
        self.assertEqual(self.history(four), ['in the World', 'c3', 'c2', 'c1'])
        self.assertEqual(self.git(four, 'status', '--porcelain').stdout, b'')
        self.git(four, 'fsck', '--full')

    def test_shallow_boundary_changed_during_import_is_not_published(self):
        import shlex
        shallow = self.shallow_clone()
        real_git = shlex.quote(shutil.which('git'))
        head = self.git(shallow, 'rev-parse', 'HEAD').stdout.decode().strip()
        # A `git fetch --deepen` in the source, and the boundary alone moving (the tip becomes
        # a shallow commit: no object or ref changes, only the shallow file).
        actions = {
            'deepen': real_git + ' -C ' + shlex.quote(str(shallow)) + ' fetch -q --deepen=1 origin',
            'boundary': 'echo ' + head + ' >> ' + shlex.quote(str(shallow / '.git/shallow')),
        }
        for name, action in actions.items():
            with self.subTest(name):
                before = (shallow / '.git/shallow').read_bytes()
                wrapper, done = self.pack_refs_wrapper(name, action)
                path = self.env['PATH']
                self.env['PATH'] = str(wrapper) + os.pathsep + path
                self.world('init', str(shallow), code=1)
                self.env['PATH'] = path
                self.assertTrue(done.exists())
                self.assertNotEqual((shallow / '.git/shallow').read_bytes(), before)
                self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        snapshot = self.world('init', str(shallow)).stdout.split()[0].decode()
        one, _ = self.fork('one', snapshot)
        self.assertEqual((one / '.world-git/repo.git/shallow').read_bytes(), (shallow / '.git/shallow').read_bytes())
        self.git(one, 'fsck', '--full')

    def test_publish_from_a_shallow_world(self):
        shallow = self.shallow_clone()
        boundary = (shallow / '.git/shallow').read_bytes()
        self.world('init', str(shallow))
        one, wid = self.fork()
        (one / 'file').write_text('world change\n')
        self.git(one, 'commit', '-qam', 'world change')
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
        # (a) The shallow clone it came from has the history down to the same boundary.
        self.world('publish', wid)
        self.assertEqual(self.git(shallow, 'rev-parse', 'world/W1').stdout.strip(), head)
        self.assertEqual((shallow / '.git/shallow').read_bytes(), boundary)
        self.git(shallow, 'fsck', '--full')
        # (b) A full clone of the project has all of it and stays complete.
        full = self.root / 'full'
        self.git(self.root, 'clone', '-q', str(self.source), str(full))
        self.world('publish', wid, '--repo', str(full))
        self.assertEqual(self.git(full, 'rev-parse', 'world/W1').stdout.strip(), head)
        self.assertEqual(self.git(full, 'rev-parse', '--is-shallow-repository').stdout.strip(), b'false')
        self.assertFalse((full / '.git/shallow').exists())
        self.git(full, 'fsck', '--full')
        # A clone shallower than the World that has the commit the World built on takes only
        # the new commit, its own boundary unchanged.
        depth1 = self.root / 'depth1'
        self.git(self.root, 'clone', '-q', '--depth', '1', 'file://' + str(self.source), str(depth1))
        own = (depth1 / '.git/shallow').read_bytes()
        self.world('publish', wid, '--repo', str(depth1))
        self.assertEqual(self.git(depth1, 'rev-parse', 'world/W1').stdout.strip(), head)
        self.assertEqual((depth1 / '.git/shallow').read_bytes(), own)
        self.git(depth1, 'fsck', '--full')
        # (c) A repository without the history below the boundary would have to become shallow
        # to take the commits; it is refused, --force or not, and never made shallow.
        empty = self.root / 'empty'
        self.git(self.root, 'init', '-q', '-b', 'main', str(empty))
        unrelated = self.root / 'unrelated'
        self.git(self.root, 'init', '-q', '-b', 'main', str(unrelated))
        self.git(unrelated, '-c', 'user.name=Other', '-c', 'user.email=other@example.com',
                 'commit', '-q', '--allow-empty', '-m', 'unrelated')
        for target in (empty, unrelated):
            for extra in ((), ('--force',)):
                with self.subTest(target=target.name, extra=extra):
                    result = self.world('publish', wid, '--repo', str(target), *extra, code=3)
                    self.assertIn(b'reason: the World is a shallow clone', result.stderr)
                    self.assertIn(b'does not have the history below its shallow boundary', result.stderr)
                    self.assertEqual(self.git(target, 'rev-parse', '--is-shallow-repository').stdout.strip(), b'false')
                    self.assertFalse((target / '.git/shallow').exists())
                    self.assertEqual(self.git(target, 'for-each-ref', 'refs/worldfs/', 'refs/heads/world/').stdout, b'')

    def test_shallow_submodule_is_imported_with_its_boundary(self):
        lib = self.origin('lib')
        for n in (1, 2):
            (lib / 'lib.txt').write_text('lib %d\n' % n)
            self.git(lib, 'commit', '-qam', 'lib %d' % n)
        self.sub(self.source, 'add', '-q', 'file://' + str(lib), 'libs/lib')
        self.git(self.source, 'commit', '-qm', 'submodule')
        # Re-clone it the way `submodule.<name>.shallow` or `update --depth` does.
        self.sub(self.source, 'deinit', '-q', '-f', 'libs/lib')
        shutil.rmtree(self.source / '.git/modules/libs/lib')
        self.sub(self.source, 'update', '-q', '--init', '--depth', '1', 'libs/lib')
        module = self.source / '.git/modules/libs/lib'
        boundary = (module / 'shallow').read_bytes()
        self.assertEqual(self.history(self.source / 'libs/lib'), ['lib 2'])
        self.world('init', str(self.source))
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / 'origins')
        one, _ = self.fork()
        owned = one / '.world-git/repo.git/worktrees/active/modules/libs/lib'
        self.assertEqual((owned / 'shallow').read_bytes(), boundary)
        self.assertEqual(self.history(one / 'libs/lib'), ['lib 2'])
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, b'')
        self.git(one / 'libs/lib', 'fsck', '--full')

    # ---- unborn repositories ---------------------------------------------------------------

    def unborn_source(self, name='unborn'):
        repo = self.root / name
        self.git(self.root, 'init', '-q', '-b', 'trunk', str(repo))
        self.identify(repo)
        return repo

    def git_info(self, wid):
        return json.loads(self.world('inspect', wid, '--json').stdout)['git']

    def test_unborn_repository_is_imported_forked_and_committed(self):
        unborn = self.unborn_source()
        (unborn / '.gitignore').write_text('build/\n')
        (unborn / 'staged').write_text('staged before the first commit\n')
        self.git(unborn, 'add', '.')
        (unborn / 'untracked').write_text('untracked\n')
        (unborn / 'build').mkdir()
        (unborn / 'build/artifact').write_text('ignored\n')
        status = self.git(unborn, 'status', '--porcelain').stdout
        index = (unborn / '.git/index').read_bytes()
        # Staged files are uncommitted changes: an explicit choice, and nothing is committed yet
        # to start from with --committed-only.
        self.world('init', str(unborn), code=3)
        result = self.world('init', str(unborn), '--committed-only', code=3)
        self.assertIn(b'reason: HEAD is on refs/heads/trunk, which has no commit yet', result.stderr)
        self.assertEqual(json.loads(self.world('list', '--json').stdout)['snapshots'], [])
        self.world('init', str(unborn), '--include-changes')
        self.assertEqual((unborn / '.git/index').read_bytes(), index)
        self.git(unborn, 'rev-parse', '--verify', '-q', 'HEAD', code=1)
        shutil.rmtree(unborn)
        owned = Path('.world-git/repo.git')
        one, wid = self.fork()
        self.assertEqual((one / owned / 'HEAD').read_text(), 'ref: refs/heads/trunk\n')
        self.assertEqual(self.git(one, 'for-each-ref').stdout, b'')
        self.assertEqual(self.git(one, 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/world/W1')
        self.git(one, 'rev-parse', '--verify', '-q', 'HEAD', code=1)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout, status)
        self.assertEqual((one / 'build/artifact').read_text(), 'ignored\n')
        self.assertEqual(self.git_info(wid), {'branch': 'world/W1', 'head': '', 'baseline': '',
                                              'git_dir': str(one / owned)})
        self.assertIn(b'(no commit yet)', self.world('inspect', wid).stdout)
        # A World with no commit publishes nothing, and --committed-only has nothing to start from.
        result = self.world('publish', wid, '--repo', str(self.source), code=3)
        self.assertIn(b'reason: the World\'s branch world/W1 has no commit yet; there is nothing to publish', result.stderr)
        result = self.world('fork', '--from', wid, '--to', str(self.root / 'refused'), '--committed-only', code=3)
        self.assertIn(b'has no commit yet', result.stderr)
        # Checkpoints, forks and pooled forks of the unborn World.
        checkpoint = self.world('checkpoint', wid, '--include-changes').stdout.split()[0].decode()
        two, wid2 = self.fork('two', wid, '--include-changes')
        self.world('pool', 'fill', checkpoint, '--count', '1')
        three = self.root / 'three'
        out = self.world('fork', '--from', checkpoint, '--to', str(three)).stdout
        self.assertIn(b'(pool)', out)
        wid3 = out.decode().split()[0]
        for world, w in ((two, wid2), (three, wid3)):
            branch = 'world/' + w
            self.assertEqual(self.git(world, 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/' + branch.encode())
            self.git(world, 'rev-parse', '--verify', '-q', 'HEAD', code=1)
            self.assertEqual(self.git(world, 'status', '--porcelain').stdout, status)
            self.assertEqual(self.git_info(w)['branch'], branch)
            self.assertEqual(self.git_info(w)['head'], '')
        # The first commit creates the World's branch as a root commit.
        (one / 'untracked').unlink()
        self.git(one, 'commit', '-qm', 'first')
        first = self.git(one, 'rev-parse', 'HEAD').stdout.decode().strip()
        self.assertEqual(self.git(one, 'rev-list', '--parents', 'HEAD').stdout.decode().split(), [first])
        self.assertEqual(self.git(one, 'rev-parse', 'refs/heads/world/W1').stdout.decode().strip(), first)
        self.assertEqual(self.git_info(wid)['head'], first)
        self.assertEqual(self.git_info(wid)['baseline'], '')
        self.assertEqual(self.git(two, 'for-each-ref').stdout, b'')
        # A checkpoint after it forks with that commit as the baseline.
        later = self.world('checkpoint', wid).stdout.split()[0].decode()
        four, wid4 = self.fork('four', later)
        self.assertEqual(self.git(four, 'rev-parse', 'HEAD').stdout.decode().strip(), first)
        self.assertEqual(self.git(four, 'branch', '--show-current').stdout.decode().strip(), 'world/' + wid4)
        self.assertEqual(self.git_info(wid4)['baseline'], first)
        self.assertEqual(self.git(four, 'status', '--porcelain').stdout, b'')
        self.git(four, 'fsck', '--full')

    def test_orphan_branch_is_imported_unborn(self):
        self.git(self.source, 'checkout', '-q', '--orphan', 'fresh')
        self.world('init', str(self.source), code=3)
        self.world('init', str(self.source), '--include-changes')
        one, wid = self.fork()
        self.assertEqual(self.git(one, 'rev-parse', 'main').stdout.strip(), self.base)
        self.git(one, 'rev-parse', '--verify', '-q', 'HEAD', code=1)
        self.assertEqual(self.git(one, 'status', '--porcelain').stdout,
                         self.git(self.source, 'status', '--porcelain').stdout)
        self.assertEqual(self.git_info(wid)['branch'], 'world/W1')
        self.git(one, 'commit', '-qm', 'orphan root')
        self.assertEqual(self.git(one, 'rev-list', '--count', 'HEAD').stdout.strip(), b'1')
        # Unrelated to the source's history: publishing it needs --force.
        result = self.world('publish', wid, code=3)
        self.assertIn(b'shares no history with the World', result.stderr)
        self.world('publish', wid, '--force')

    def test_submodule_on_an_unborn_branch(self):
        self.submodule_fixture()
        lib = self.source / 'libs/lib'
        recorded = self.git(lib, 'rev-parse', 'HEAD').stdout.strip()
        self.git(lib, 'checkout', '-q', '--orphan', 'fresh')
        # Its HEAD is no longer the recorded commit: an uncommitted change of the superproject.
        self.world('init', str(self.source), code=3)
        self.world('init', str(self.source), '--include-changes')
        committed = self.world('init', str(self.source), '--committed-only').stdout.split()[0].decode()
        one, _ = self.fork('one', 'S1')
        self.assertEqual(self.git(one / 'libs/lib', 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/fresh')
        self.git(one / 'libs/lib', 'rev-parse', '--verify', '-q', 'HEAD', code=1)
        self.assertEqual(self.git(one / 'libs/lib', 'status', '--porcelain').stdout,
                         self.git(lib, 'status', '--porcelain').stdout)
        # --committed-only detaches it at the commit the superproject records.
        two, _ = self.fork('two', committed)
        self.assertEqual(self.git(two / 'libs/lib', 'rev-parse', 'HEAD').stdout.strip(), recorded)
        self.git(two / 'libs/lib', 'symbolic-ref', '-q', 'HEAD', code=1)
        self.assertEqual(self.git(two, 'status', '--porcelain').stdout, b'')

    def test_publish_into_a_repository_with_no_commit(self):
        unborn = self.unborn_source()
        self.world('init', str(unborn))
        one, wid = self.fork()
        (one / 'file').write_text('first in the World\n')
        self.git(one, 'add', 'file')
        self.git(one, 'commit', '-qm', 'first')
        head = self.git(one, 'rev-parse', 'HEAD').stdout.strip()
        # The source it came from has no history to share or lose: no --force needed.
        self.world('publish', wid)
        self.assertEqual(self.git(unborn, 'rev-parse', 'refs/heads/world/W1').stdout.strip(), head)
        self.assertEqual(self.git(unborn, 'symbolic-ref', 'HEAD').stdout.strip(), b'refs/heads/trunk')
        self.git(unborn, 'rev-parse', '--verify', '-q', 'HEAD', code=1)
        self.assertEqual(self.git(unborn, 'for-each-ref', 'refs/worldfs/').stdout, b'')
        # A World of a repository with history publishes into an empty one too; a repository
        # with unrelated commits still needs --force.
        self.world('init', str(self.source))
        two, wid2 = self.fork('two', 'S2')
        self.git(two, 'commit', '-q', '--allow-empty', '-m', 'world change')
        empty = self.unborn_source('empty')
        self.world('publish', wid2, '--repo', str(empty))
        self.assertEqual(self.git(empty, 'rev-parse', 'world/W2').stdout.strip(),
                         self.git(two, 'rev-parse', 'HEAD').stdout.strip())
        self.assertEqual(self.git(empty, 'rev-list', '--count', 'world/W2').stdout.strip(), b'2')
        result = self.world('publish', wid, '--repo', str(self.source), code=3)
        self.assertIn(b'shares no history with the World', result.stderr)

if __name__ == '__main__':
    unittest.main()
