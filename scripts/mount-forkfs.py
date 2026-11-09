#!/usr/bin/env python3
"""Mount a revision-backed ForkFS repository and supervise its local services.

Requires an installed, enabled FSKit module. Runs in the foreground; Ctrl-C
unmounts before stopping the bridge and daemon. Existing repositories are never
initialized implicitly. Use --initialize only when creating a new repository.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import threading
import time


class UnmountBusy(RuntimeError):
    """Keep services running while the caller closes files and retries."""


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise RuntimeError('directory must be private and owned: ' + str(path))


def private_file(path):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        os.close(fd)
        raise RuntimeError('invalid private file: ' + str(path))
    return os.fdopen(fd, 'ab', buffering=0)


class MountSession:
    def __init__(self, daemon, repository, runtime, mountpoint, *, world='main', revision=None,
                 initialize=False, stopping=None):
        self.binary = Path(daemon).resolve(strict=True)
        self.repository = Path(repository).absolute()
        self.runtime = Path(runtime).absolute()
        self.mountpoint = Path(mountpoint).absolute()
        self.view = ['--revision', revision] if revision is not None else ['--world', world]
        self.initialize = initialize
        self.stopping = stopping if stopping is not None else threading.Event()
        self.daemon = self.bridge = None
        self.logs = []
        self.lock = None
        self.mount_lock = None
        self.socket_identity = None
        self.mounted = self.mount_attempted = False

    def mounted_on_host(self):
        return os.path.ismount(self.mountpoint)

    def mount_volume(self):
        subprocess.run(['/sbin/mount', '-F', '-t', 'forkfs', str(self.runtime), str(self.mountpoint)],
                       check=True, timeout=30)

    def unmount_volume(self, *, force=False):
        subprocess.run(['/sbin/umount', *(['-f'] if force else []), str(self.mountpoint)], check=True, timeout=15)

    def spawn(self, command, name):
        log = private_file(self.runtime / (name + '.log'))
        self.logs.append(log)
        return subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                start_new_session=True)

    def wait_ready(self, process, ready, name):
        deadline = time.monotonic() + 20
        while True:
            if process.poll() is not None:
                raise RuntimeError(name + ' exited; see ' + str(self.runtime / (name + '.log')))
            if self.stopping.is_set():
                raise RuntimeError('mount startup cancelled')
            if ready():
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(name + ' startup timed out; see ' + str(self.runtime / (name + '.log')))
            self.stopping.wait(0.05)

    def start(self):
        if self.mounted_on_host():
            raise RuntimeError('mount point already contains a mounted filesystem')
        paths = [self.repository.resolve(), self.runtime.resolve(), self.mountpoint.resolve()]
        for index, path in enumerate(paths):
            for other in paths[index + 1:]:
                if path == other or path in other.parents or other in path.parents:
                    raise RuntimeError('repository, runtime and mountpoint must be separate directories')
        if len(os.fsencode(str(self.runtime.resolve() / 'control.sock'))) >= 104:
            raise RuntimeError('runtime socket path is too long; choose a shorter runtime directory')
        private_directory(self.runtime)
        self.runtime = self.runtime.resolve()
        self.lock = private_file(self.runtime / '.forkfs-launcher.lock')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another launcher owns this runtime') from None
        # Never remove a pre-existing socket: it can belong to a live daemon.
        endpoint = self.runtime / 'control.sock'
        if endpoint.exists() or endpoint.is_symlink():
            raise RuntimeError('runtime socket already exists; use a fresh runtime directory')
        private_directory(self.mountpoint)
        self.mountpoint = self.mountpoint.resolve()
        self.mount_lock = os.open(self.mountpoint, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            fcntl.flock(self.mount_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another launcher owns this mount point') from None
        if any(self.mountpoint.iterdir()):
            raise RuntimeError('mount point must be empty')
        if self.initialize:
            if self.repository.exists():
                raise RuntimeError('--initialize requires a new repository path')
            subprocess.run([str(self.binary), 'init', str(self.repository)], check=True,
                           stdout=subprocess.DEVNULL, timeout=30)
            subprocess.run([str(self.binary), 'fs-init', str(self.repository)], check=True,
                           stdout=subprocess.DEVNULL, timeout=30)
        elif not self.repository.is_dir():
            raise RuntimeError('repository does not exist; use --initialize to create it')
        self.daemon = self.spawn([str(self.binary), 'serve', str(self.repository),
                                  '--socket', str(endpoint), '--rpc'], 'daemon')
        def daemon_ready():
            if not endpoint.exists():
                return False
            try:
                subprocess.run([str(self.binary), 'request', str(endpoint), 'status'],
                               check=True, timeout=1, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
                return self.daemon.poll() is None
            except (OSError, subprocess.SubprocessError):
                return False
        self.wait_ready(self.daemon, daemon_ready, 'daemon')
        info = endpoint.lstat()
        self.socket_identity = (info.st_dev, info.st_ino, info.st_ctime_ns)
        descriptor = self.runtime / 'forkfs.json'
        def bridge_key():
            try:
                return json.loads(descriptor.read_text()).get('rpc', {}).get('key')
            except (FileNotFoundError, ValueError, AttributeError):
                return None
        old_key = bridge_key()
        self.bridge = self.spawn([sys.executable, str(Path(__file__).with_name('fskit-bridge.py')),
                                  str(self.binary), str(self.runtime), *self.view], 'bridge')
        def bridge_ready():
            key = bridge_key()
            return isinstance(key, str) and len(key) == 64 and key != old_key
        self.wait_ready(self.bridge, bridge_ready, 'bridge')
        if self.stopping.is_set():
            raise RuntimeError('mount startup cancelled')
        self.mount_attempted = True
        self.mount_volume()
        if not self.mounted_on_host():
            raise RuntimeError('mount command returned without creating a mounted filesystem')
        self.mounted = True

    def failed_child(self):
        for name, process in [('daemon', self.daemon), ('bridge', self.bridge)]:
            if process is not None and process.poll() is not None:
                return name
        return None

    def stop(self):
        if self.mounted or (self.mount_attempted and self.mounted_on_host()):
            if self.mounted_on_host():
                try:
                    self.unmount_volume()
                except (subprocess.SubprocessError, OSError) as error:
                    # FSKit can reject normal unmount with ENOTCONN after a
                    # backend dies. Detach that failed mount without restarting
                    # services or retargeting cached inodes to another session.
                    if self.failed_child() and self.mounted_on_host():
                        print('forkfs: backend exited; detaching the failed mount.', file=sys.stderr, flush=True)
                        try:
                            self.unmount_volume(force=True)
                        except (subprocess.SubprocessError, OSError):
                            pass
                    if self.mounted_on_host():
                        raise UnmountBusy('unmount failed; services remain running. Close open files and press Ctrl-C to retry.') from error
            self.mounted = False
        for process in [self.bridge, self.daemon]:
            if process is not None and process.poll() is None:
                try:
                    process.terminate()
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=5)
        if self.socket_identity is not None:
            endpoint = self.runtime / 'control.sock'
            try:
                info = endpoint.lstat()
                if stat.S_ISSOCK(info.st_mode) and (info.st_dev, info.st_ino, info.st_ctime_ns) == self.socket_identity:
                    endpoint.unlink()
            except FileNotFoundError:
                pass
            self.socket_identity = None
        for log in self.logs:
            log.close()
        self.logs.clear()
        if self.lock is not None:
            self.lock.close()
            self.lock = None
        if self.mount_lock is not None:
            os.close(self.mount_lock)
            self.mount_lock = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('daemon', type=Path)
    parser.add_argument('repository', type=Path)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--runtime', type=Path, required=True)
    selector = parser.add_mutually_exclusive_group()
    selector.add_argument('--world', default='main')
    selector.add_argument('--revision')
    parser.add_argument('--initialize', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('OS mounts require macOS and an enabled ForkFS FSKit module')
    stopping = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: stopping.set())
    try:
        session = MountSession(args.daemon, args.repository, args.runtime, args.mountpoint,
                               world=args.world, revision=args.revision,
                               initialize=args.initialize, stopping=stopping)
    except OSError as error:
        parser.error(str(error))
    result = 0
    try:
        session.start()
        print('ForkFS mounted at ' + str(session.mountpoint) + '; press Ctrl-C to unmount.', flush=True)
        print('Service logs: ' + str(session.runtime), flush=True)
        while not stopping.wait(0.2):
            child = session.failed_child()
            if child:
                raise RuntimeError(child + ' exited; unmounting the failed session')
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print('forkfs: ' + str(error), file=sys.stderr)
        result = 1
    while True:
        try:
            session.stop()
            break
        except UnmountBusy as error:
            print('forkfs: ' + str(error), file=sys.stderr, flush=True)
            result = 1
            stopping.clear()
            stopping.wait()
    return result


if __name__ == '__main__':
    sys.exit(main())
