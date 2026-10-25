"""Administrator-only supervisor for a provisioned macOS agent identity.

Run trusted, root-owned installed code with isolated Python and a fresh environment.
This CLI has no agent-facing socket, creates no accounts, and never returns an
identity to a general pool. Persistent binding/active markers prevent unsafe reuse.
"""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import pwd
import selectors
import shutil
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
import platform

MAX_OUTPUT = 1024 * 1024
REAP_SECONDS = 10


def unique_json_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value: raise ValueError('duplicate JSON key')
        value[key] = item
    return value


def installed_launcher():
    # Bootstrap without importing any sibling until its complete installation
    # chain has passed ownership/mode/ACL checks. The interpreter and this entry
    # file must already be trusted by the administrator invoking root.
    directory = Path(__file__).absolute().parent
    libc = ctypes.CDLL(None, use_errno=True)
    libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.acl_get_link_np.restype = ctypes.c_void_p
    libc.acl_free.argtypes = [ctypes.c_void_p]
    for filename in ('agent_supervisor.py', 'dedicated_workload.py', 'restricted_workload.py', 'mount_probe.py',
                     'mount_lifecycle.py', 'mount_session.py'):
        path = directory / filename
        for index, item in enumerate([path, *path.parents]):
            info = os.lstat(item)
            expected = stat.S_ISDIR if index else stat.S_ISREG
            if (not expected(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o7022
                    or (not index and info.st_nlink != 1)):
                raise ValueError('supervisor installation is not root-owned and private to root writes')
            ctypes.set_errno(0)
            acl = libc.acl_get_link_np(os.fsencode(item), 0x100)
            if acl:
                libc.acl_free(acl);raise ValueError('supervisor installation has an extended ACL')
            if ctypes.get_errno() != errno.ENOENT:
                raise RuntimeError('cannot inspect supervisor installation ACL')
    sys.path.insert(0, str(directory))
    return importlib.import_module('dedicated_workload')


def trusted_interpreter(runtime):
    expected = Path('/Library/Developer/CommandLineTools/usr/bin/python3').resolve(strict=True)
    actual = Path(sys.executable).resolve(strict=True)
    if actual != expected: raise RuntimeError('supervisor must run under the installed trusted Python interpreter')
    return runtime.trusted_path(expected)


def uid_processes(uid):
    """Observe effective OR real UID; never use enumerated PIDs for root signals."""
    library = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    query = library.proc_listpids
    query.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    query.restype = ctypes.c_int
    def collect(kind, identity):
        buffer = (ctypes.c_int * 65536)()
        ctypes.set_errno(0)
        size = query(kind, identity, buffer, ctypes.sizeof(buffer))
        if size < 0 or size >= ctypes.sizeof(buffer) or size % ctypes.sizeof(ctypes.c_int):
            raise RuntimeError('process enumeration failed or exceeded capacity')
        if size == 0 and ctypes.get_errno():raise OSError(ctypes.get_errno(), 'process enumeration')
        return {pid for pid in buffer[:size // ctypes.sizeof(ctypes.c_int)] if pid > 0}
    # A successful positive control prevents a silent API failure looking idle.
    if os.getpid() not in collect(4, 0):  # PROC_UID_ONLY
        raise RuntimeError('cannot verify root process enumeration')
    return collect(4, uid) | collect(5, uid)  # PROC_RUID_ONLY


REAPER = r'''
import json, os, signal, sys
uid, gid = map(int, sys.argv[1:])
if uid <= 0 or gid <= 0 or (os.getuid(),os.geteuid(),os.getgid(),os.getegid()) != (uid,uid,gid,gid):
    raise SystemExit('reaper refused unexpected credentials')
data = sys.stdin.buffer.read(2 * 1024 * 1024 + 1)
if len(data) > 2 * 1024 * 1024:raise SystemExit('reaper input exceeds bound')
pids = json.loads(data)
if not isinstance(pids,list) or len(pids)>131072 or any(type(pid) is not int or pid<=1 for pid in pids):
    raise SystemExit('reaper requires bounded positive PIDs')
# A PID reused by another identity remains protected by the kernel's UID check.
# Never broadcast kill(-1): observed Darwin runners also killed the sender.
for pid in pids:
    if pid == os.getpid():continue
    try:os.kill(pid,signal.SIGKILL)
    except (ProcessLookupError,PermissionError):pass

'''


def credentials(policy):
    return dict(user=policy['worker_uid'], group=policy['worker_gid'], extra_groups=(),
                umask=0o077, close_fds=True, start_new_session=True)


def reap_identity(policy, launcher, interpreter, environment):
    deadline = time.monotonic() + REAP_SECONDS
    while True:
        if launcher is not None:launcher.poll()  # reap our immediate child
        targets = uid_processes(policy['worker_uid'])
        if not targets:return
        if time.monotonic() >= deadline:raise RuntimeError('worker UID did not quiesce; active marker retained')
        result = subprocess.run([str(interpreter), '-I', '-S', '-c', REAPER,
                                 str(policy['worker_uid']), str(policy['worker_gid'])],
                                cwd='/', env=environment, input=json.dumps(sorted(targets)).encode(),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                timeout=3, **credentials(policy))
        if result.returncode:raise RuntimeError(f'unprivileged UID cleanup failed ({result.returncode}): '
                                             f'{result.stderr[:512]!r}; active marker retained')
        time.sleep(0.05)


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:os.fsync(descriptor)
    finally:os.close(descriptor)


def create_record(path, payload):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, 'wb', closefd=False) as stream:
            stream.write(payload);stream.flush();os.fsync(descriptor)
    finally:os.close(descriptor)
    sync_directory(path.parent)


class Reservation:
    def __init__(self, state, policy, runtime, mount_lifecycle=None):
        self.state, self.policy, self.runtime = Path(state), policy, runtime
        self.mount_lifecycle = Path(mount_lifecycle).absolute() if mount_lifecycle is not None else None
        self.descriptor = None
        self.active = self.state / f'{policy["worker_uid"]}.active'
        self.session = None

    def __enter__(self):
        self.runtime.trusted_path(self.state, directory=True)
        if stat.S_IMODE(os.lstat(self.state).st_mode) != 0o711:
            raise ValueError('identity state directory must be root-owned mode 0711')
        lock = self.state / f'{self.policy["worker_uid"]}.lock'
        self.descriptor = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            info = os.fstat(self.descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
                raise ValueError('unsafe UID lease file')
            os.fchmod(self.descriptor, 0o600)
            if stat.S_IMODE(os.fstat(self.descriptor).st_mode) != 0o600:raise ValueError('unsafe UID lease file mode')
            self.runtime.no_acl(lock)
            fcntl.flock(self.descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if os.path.lexists(self.active):raise ValueError('UID has an active/crashed session; administrator recovery required')
            if uid_processes(self.policy['worker_uid']):raise ValueError('worker UID is already in use')
            fingerprint = hashlib.sha256(json.dumps(self.policy, sort_keys=True, separators=(',', ':')).encode()).hexdigest().encode()
            binding = self.state / f'{self.policy["worker_uid"]}.binding'
            if os.path.lexists(binding):
                self.runtime.trusted_path(binding)
                with binding.open('rb') as stream:bound = stream.read(65)
                if bound != fingerprint:
                    raise ValueError('UID is reserved for another policy; explicit retirement required')
            else:create_record(binding, fingerprint)
            self.session = Path(tempfile.mkdtemp(prefix=f'{self.policy["worker_uid"]}-', dir=self.state))
            self.session.chmod(0o711)
            if self.mount_lifecycle is None:
                create_record(self.active, b'pending administrator-supervised session\n')
            else:
                create_record(self.active, json.dumps(dict(status='pending', mount_record=str(self.mount_lifecycle),
                    session_dir=str(self.session)), sort_keys=True, separators=(',', ':')).encode())
                sync_directory(self.state)
            return self
        except BaseException:
            os.close(self.descriptor);self.descriptor = None
            raise

    def release_after_quiescence(self):
        # Keep active marker on any failure. Never delete a suspect path by name
        # after the UID lease has been released.
        if uid_processes(self.policy['worker_uid']):raise RuntimeError('cannot release a live UID')
        if not shutil.rmtree.avoids_symlink_attacks:raise RuntimeError('safe descriptor-based scratch removal unavailable')
        shutil.rmtree(self.session)
        self.active.unlink()
        sync_directory(self.state)

    def write_active(self, payload):
        current = os.lstat(self.active)
        if not stat.S_ISREG(current.st_mode) or current.st_uid != 0 or current.st_nlink != 1:
            raise ValueError('unsafe active worker marker')
        runtime = self.runtime
        runtime.no_acl(self.active)
        encoded = json.dumps(payload, sort_keys=True, separators=(',', ':')).encode()
        fd, temp = tempfile.mkstemp(prefix=f'.{self.active.name}.', suffix='.tmp', dir=self.state)
        try:
            os.fchmod(fd, 0o600)
            offset = 0
            while offset < len(encoded): offset += os.write(fd, encoded[offset:])
            os.fsync(fd)
            os.replace(temp, self.active)
            sync_directory(self.state)
        except BaseException:
            try: os.unlink(temp)
            except OSError: pass
            raise
        finally: os.close(fd)

    def __exit__(self, *unused):
        if self.descriptor is not None:os.close(self.descriptor)


def monitor(child, timeout, interrupted, health=None):
    streams = selectors.DefaultSelector()
    output = {'stdout': bytearray(), 'stderr': bytearray()}
    started = time.monotonic()
    next_health = started
    reason = 'exited'
    try:
        for name in output:
            descriptor = getattr(child, name)
            os.set_blocking(descriptor.fileno(), False)
            streams.register(descriptor, selectors.EVENT_READ, name)
        while True:
            if interrupted():reason = 'interrupted';break
            if time.monotonic() - started >= timeout:reason = 'timeout';break
            if health is not None and time.monotonic() >= next_health:
                try: health()
                except Exception: reason = 'mount_lost'; break
                next_health = time.monotonic() + 0.5
            # Drain buffered output after exit, without waiting for descendants
            # that inherited pipes to close them. An output flood remains capped.
            exited = child.poll() is not None
            events = streams.select(0 if exited else 0.05)
            if exited and not events:break
            for item, _ in events:
                data = os.read(item.fd, 65536)
                if not data:streams.unregister(item.fileobj);continue
                remaining = MAX_OUTPUT - sum(map(len, output.values()))
                output[item.data].extend(data[:remaining])
                if len(data) > remaining:
                    reason = 'output_limit';break
            if reason == 'output_limit':break
        return reason, output
    finally:streams.close()


def launcher_command(interpreter, session_path, command):
    return [str(interpreter), '-I', '-S',
            str(Path(__file__).absolute().parent / 'dedicated_workload.py'),
            '--policy', str(session_path), '--', *command]


def start_deathwatch(interpreter, policy_path, state, mount_lifecycle, environment):
    """Start an independent root monitor that recovers after parent EOF."""
    death_read, death_write = os.pipe()
    ready_read, ready_write = os.pipe()
    for descriptor in (death_read, death_write, ready_read, ready_write):
        os.set_inheritable(descriptor, False)
    script = Path(__file__).absolute()
    command = [str(interpreter), '-I', '-S', str(script), '--watch', '--policy', str(policy_path),
               '--state', str(state), '--mount-lifecycle', str(mount_lifecycle),
               '--watch-fd', str(death_read), '--ready-fd', str(ready_write)]
    try:
        watcher = subprocess.Popen(command, cwd='/', env=environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   close_fds=True, pass_fds=(death_read, ready_write), start_new_session=True)
    except BaseException:
        for fd in (death_read, death_write, ready_read, ready_write): os.close(fd)
        raise
    os.close(death_read); death_read = None
    os.close(ready_write); ready_write = None
    try:
        ready, _, _ = select.select([ready_read], [], [], 5)
        signal_byte = os.read(ready_read, 1) if ready else b''
    except BaseException:
        for fd in (ready_read, death_write):
            if fd is not None:
                try: os.close(fd)
                except OSError: pass
        try:
            if watcher.poll() is None: watcher.kill()
            watcher.wait(timeout=5)
        except Exception: pass
        raise
    else: os.close(ready_read)
    if signal_byte != b'R':
        os.close(death_write)
        try: watcher.wait(timeout=5)
        except subprocess.TimeoutExpired: watcher.kill(); watcher.wait(timeout=5)
        raise RuntimeError('root mount deathwatch did not become ready')
    return watcher, death_write


def await_parent_and_recover(policy_path, state, mount_lifecycle, read_fd, ready_fd):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('mount deathwatch requires root on macOS')
    if not sys.flags.isolated or not sys.flags.no_site: raise RuntimeError('deathwatch requires Python -I -S')
    runtime = installed_launcher(); runtime.admit_entry_environment()
    trusted_interpreter(runtime)
    for fd in (read_fd, ready_fd):
        if not stat.S_ISFIFO(os.fstat(fd).st_mode): raise ValueError('deathwatch descriptors must be pipes')
    os.write(ready_fd, b'R'); os.close(ready_fd)
    while True:
        data = os.read(read_fd, 4096)
        if not data: break
        raise ValueError('unexpected parent deathwatch protocol data')
    os.close(read_fd)
    lifecycle = importlib.import_module('mount_lifecycle')
    policy = runtime.read_policy(Path(policy_path))
    active = Path(state) / f'{policy["worker_uid"]}.active'
    if not os.path.lexists(active):
        record = lifecycle.load_record(mount_lifecycle, runtime)
        if record['phase'] == 'retired': return dict(status='already_retired')
    last_error = None
    for _ in range(300):
        try: return recover_mount(policy_path, state, mount_lifecycle)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, KeyError) as error:
            last_error = error
            time.sleep(1)
    raise RuntimeError(f'mount recovery remains quarantined: {last_error}')


def supervise(policy_path, state, command, timeout=600, mount_lifecycle=None):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('supervisor requires real/effective root on macOS')
    if not sys.flags.isolated or not sys.flags.no_site:
        raise RuntimeError('invoke trusted supervisor with Python -I -S')
    if not command or not 0 < timeout <= 86400:raise ValueError('invalid workload or deadline')
    runtime = installed_launcher()
    runtime.admit_entry_environment()
    policy_path = Path(policy_path)
    policy = runtime.read_policy(policy_path)
    if policy['version'] != 1:raise ValueError('supervisor requires a provisioned version 1 identity policy')
    if mount_lifecycle is not None:
        lifecycle_module = importlib.import_module('mount_lifecycle')
        lifecycle_record = lifecycle_module.load_record(mount_lifecycle, runtime)
        if lifecycle_record['phase'] != 'bound': raise ValueError('mount lifecycle record is not available for a new worker')
        if (lifecycle_record['policy_sha256'] != hashlib.sha256(
                json.dumps(policy, sort_keys=True, separators=(',', ':')).encode()).hexdigest()):
            raise ValueError('mount lifecycle policy fingerprint changed')
    try:
        account = pwd.getpwuid(policy['worker_uid'])
    except KeyError as error:
        raise ValueError(f'provisioned worker account for UID {policy["worker_uid"]} is missing') from error
    if (account.pw_gid != policy['worker_gid'] or
            account.pw_shell not in ('/usr/bin/false', '/bin/false', '/usr/sbin/nologin', '/sbin/nologin')):
        raise ValueError('worker must be a dedicated account with matching GID and non-login shell')
    interpreter = trusted_interpreter(runtime)
    if not shutil.rmtree.avoids_symlink_attacks:raise RuntimeError('descriptor-based cleanup required')
    stopped = [False]
    previous = {}
    watcher = None; death_write = None
    for sig in (signal.SIGTERM, signal.SIGINT):
        previous[sig] = signal.signal(sig, lambda *unused: stopped.__setitem__(0, True))
    try:
        with Reservation(state, policy, runtime, mount_lifecycle) as lease:
            child = None
            guard = None
            try:
                if mount_lifecycle is not None:
                    watcher, death_write = start_deathwatch(interpreter, policy_path, state,
                                                             mount_lifecycle, runtime.ENTRY_ENV)
                scratch = lease.session / 'scratch';scratch.mkdir(mode=0o700)
                os.chown(scratch, policy['worker_uid'], policy['worker_gid'])
                session_policy = dict(policy, version=2, scratch_parent=str(scratch))
                session_path = lease.session / 'policy.json'
                create_record(session_path, json.dumps(session_policy).encode())
                session_path.chmod(0o644)
                if mount_lifecycle is not None:
                    lifecycle_module = importlib.import_module('mount_lifecycle')
                    guard = lifecycle_module.Guard(mount_lifecycle, policy, runtime)
                    guard_health = guard.start(lease.active, lease.session)
                    def health():
                        if watcher is None or watcher.poll() is not None:
                            raise RuntimeError('root mount deathwatch exited')
                        guard_health()
                else: health = None
                child = subprocess.Popen(launcher_command(interpreter, session_path, command),
                    cwd='/', env=runtime.ENTRY_ENV, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, **credentials(policy))
                reason, output = monitor(child, timeout, lambda: stopped[0], health)
            finally:
                try:
                    stop_error = None
                    if guard is not None and guard.record is not None and guard.record.get('phase') == 'active':
                        try: guard.stopping()
                        except Exception as error: stop_error = error
                    # UID cleanup is unconditional, even after a mount health loss.
                    try: reap_identity(policy, child, interpreter, runtime.ENTRY_ENV)
                    finally:
                        if child is not None:
                            try: child.wait(timeout=3)
                            except Exception: pass
                            for stream in (child.stdout, child.stderr):
                                try: stream.close()
                                except OSError: pass
                    if stop_error is not None: raise stop_error
                    if mount_lifecycle is not None and guard is None:
                        raise RuntimeError('mount guard did not initialize; worker identity quarantined')
                    if guard is not None:
                        if guard.record is None or guard.record.get('phase') == 'bound':
                            raise RuntimeError('mount admission did not complete; worker identity quarantined')
                        guard.finish()
                        if guard.record.get('phase') != 'retired':
                            raise RuntimeError('mount retirement did not complete; worker identity quarantined')
                    lease.release_after_quiescence()
                finally:
                    if guard is not None: guard.close()
            if death_write is not None:
                os.close(death_write); death_write = None
                watcher.wait(timeout=10)
            return dict(status='completed', reason=reason, returncode=child.returncode,
                        worker_uid=policy['worker_uid'], identity_idle=True,
                        stdout=bytes(output['stdout']).decode('utf-8', 'replace'),
                        stderr=bytes(output['stderr']).decode('utf-8', 'replace'))
    finally:
        if death_write is not None:
            os.close(death_write)
        for sig, handler in previous.items():signal.signal(sig, handler)


def recover_mount(policy_path, state, mount_lifecycle):
    """Explicitly retire a quarantined worker only after reap and mount proof."""
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('supervisor recovery requires real/effective root on macOS')
    if not sys.flags.isolated or not sys.flags.no_site:
        raise RuntimeError('invoke trusted supervisor with Python -I -S')
    runtime = installed_launcher(); runtime.admit_entry_environment()
    policy = runtime.read_policy(Path(policy_path))
    if policy['version'] != 1: raise ValueError('recovery requires a provisioned version 1 identity policy')
    account = pwd.getpwuid(policy['worker_uid'])
    if (account.pw_gid != policy['worker_gid'] or
            account.pw_shell not in ('/usr/bin/false','/bin/false','/usr/sbin/nologin','/sbin/nologin')):
        raise ValueError('worker must be a dedicated non-login account')
    state = Path(state); runtime.trusted_path(state, directory=True)
    if stat.S_IMODE(os.lstat(state).st_mode) != 0o711: raise ValueError('identity state directory must be mode 0711')
    uid = policy['worker_uid']; lock_path = state / f'{uid}.lock'; active = state / f'{uid}.active'
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    guard = None
    try:
        info = os.fstat(lock_fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise ValueError('unsafe UID lease file')
        os.fchmod(lock_fd, 0o600)
        if stat.S_IMODE(os.fstat(lock_fd).st_mode) != 0o600:raise ValueError('unsafe UID lease file mode')
        runtime.no_acl(lock_path); fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runtime.trusted_path(active)
        runtime.no_acl(active)
        if stat.S_IMODE(os.lstat(active).st_mode) != 0o600: raise ValueError('unsafe active worker marker')
        marker_fd = os.open(active, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            if (os.fstat(marker_fd).st_uid != 0 or os.fstat(marker_fd).st_nlink != 1
                    or os.fstat(marker_fd).st_size > 65536): raise ValueError('invalid active worker marker')
            raw = os.read(marker_fd, 65537)
        finally: os.close(marker_fd)
        if len(raw) > 65536: raise ValueError('active worker marker too large')
        marker = json.loads(raw, object_pairs_hook=unique_json_object)
        if (not isinstance(marker, dict) or marker.get('status') not in ('pending','active')
                or marker.get('mount_record') != str(Path(mount_lifecycle).absolute())):
            raise ValueError('active marker does not bind the requested lifecycle record')
        binding = state / f'{uid}.binding'; runtime.trusted_path(binding); runtime.no_acl(binding)
        fingerprint = hashlib.sha256(json.dumps(policy, sort_keys=True, separators=(',', ':')).encode()).hexdigest().encode()
        bind_fd = os.open(binding, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            if os.fstat(bind_fd).st_size != len(fingerprint) or os.read(bind_fd, len(fingerprint) + 1) != fingerprint:
                raise ValueError('UID policy binding mismatch')
        finally: os.close(bind_fd)
        interpreter = trusted_interpreter(runtime)
        # Once the root active marker and permanent policy binding authorize
        # this identity, reap it before any mount lookup or FD operation.
        reap_identity(policy, None, interpreter, runtime.ENTRY_ENV)
        if not shutil.rmtree.avoids_symlink_attacks: raise RuntimeError('descriptor-based cleanup required')
        session = marker.get('session_dir')
        if (not isinstance(session, str) or Path(session).parent != state
                or not Path(session).name.startswith(f'{uid}-')):
            raise ValueError('active marker has no safe session directory')
        session_path = Path(session)
        lifecycle_module = importlib.import_module('mount_lifecycle')
        lifecycle_record = lifecycle_module.load_record(mount_lifecycle, runtime)
        session_exists = os.path.lexists(session_path)
        if session_exists:
            session_info = os.lstat(session_path)
            if not stat.S_ISDIR(session_info.st_mode) or session_info.st_uid != 0 or stat.S_IMODE(session_info.st_mode) != 0o711:
                raise ValueError('unsafe worker session directory')
        elif lifecycle_record['phase'] != 'retired':
            raise ValueError('worker session is missing before verified mount retirement')
        guard = lifecycle_module.Guard(mount_lifecycle, policy, runtime)
        guard.recover()
        guard.finish()
        if uid_processes(uid): raise RuntimeError('worker UID became active during recovery')
        if os.path.lexists(session_path): shutil.rmtree(session_path)
        active.unlink(); sync_directory(state)
        return dict(status='recovered', worker_uid=uid, identity_idle=True)
    finally:
        if guard is not None: guard.close()
        os.close(lock_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=Path, required=True)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--mount-lifecycle', type=Path, help='root-owned bound mount lifecycle record')
    parser.add_argument('--recover', action='store_true', help='recover a quarantined mount lifecycle and UID lease')
    parser.add_argument('--watch', action='store_true', help='wait for parent death and recover its mount lease')
    parser.add_argument('--watch-fd', type=int)
    parser.add_argument('--ready-fd', type=int)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if args.watch:
            if args.mount_lifecycle is None or args.watch_fd is None or args.ready_fd is None or args.command:
                parser.error('--watch requires --mount-lifecycle and its inherited pipe descriptors')
            report = await_parent_and_recover(args.policy, args.state, args.mount_lifecycle, args.watch_fd, args.ready_fd)
        elif args.recover:
            if args.mount_lifecycle is None or args.command: parser.error('--recover requires --mount-lifecycle and no command')
            report = recover_mount(args.policy, args.state, args.mount_lifecycle)
        else:
            command = args.command[1:] if args.command[:1] == ['--'] else args.command
            report = supervise(args.policy, args.state, command, args.timeout, args.mount_lifecycle)
        print(json.dumps(report))
        if args.watch or args.recover: return 0
        return report['returncode'] if report['reason'] == 'exited' else 124
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='blocked', error=str(error))))
        return 2


if __name__ == '__main__':raise SystemExit(main())
