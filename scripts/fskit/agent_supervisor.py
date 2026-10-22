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
import signal
import stat
import subprocess
import sys
import tempfile
import time
import platform

MAX_OUTPUT = 1024 * 1024
REAP_SECONDS = 10


def installed_launcher():
    # Bootstrap without importing any sibling until its complete installation
    # chain has passed ownership/mode/ACL checks. The interpreter and this entry
    # file must already be trusted by the administrator invoking root.
    directory = Path(__file__).absolute().parent
    libc = ctypes.CDLL(None, use_errno=True)
    libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.acl_get_link_np.restype = ctypes.c_void_p
    libc.acl_free.argtypes = [ctypes.c_void_p]
    for filename in ('agent_supervisor.py', 'dedicated_workload.py', 'restricted_workload.py', 'mount_probe.py'):
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
        with os.fdopen(descriptor, 'wb', closefd=False) as stream:
            stream.write(payload);stream.flush();os.fsync(descriptor)
    finally:os.close(descriptor)
    sync_directory(path.parent)


class Reservation:
    def __init__(self, state, policy, runtime):
        self.state, self.policy, self.runtime = Path(state), policy, runtime
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
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1
                    or stat.S_IMODE(info.st_mode) != 0o600):
                raise ValueError('unsafe UID lease file')
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
            create_record(self.active, b'pending administrator-supervised session\n')
            self.session = Path(tempfile.mkdtemp(prefix=f'{self.policy["worker_uid"]}-', dir=self.state))
            self.session.chmod(0o711)
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

    def __exit__(self, *unused):
        if self.descriptor is not None:os.close(self.descriptor)


def monitor(child, timeout, interrupted):
    streams = selectors.DefaultSelector()
    output = {'stdout': bytearray(), 'stderr': bytearray()}
    started = time.monotonic()
    reason = 'exited'
    try:
        for name in output:
            descriptor = getattr(child, name)
            os.set_blocking(descriptor.fileno(), False)
            streams.register(descriptor, selectors.EVENT_READ, name)
        while True:
            if interrupted():reason = 'interrupted';break
            if time.monotonic() - started >= timeout:reason = 'timeout';break
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


def supervise(policy_path, state, command, timeout=600):
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
    account = pwd.getpwuid(policy['worker_uid'])
    if (account.pw_gid != policy['worker_gid'] or
            account.pw_shell not in ('/usr/bin/false', '/bin/false', '/usr/sbin/nologin', '/sbin/nologin')):
        raise ValueError('worker must be a dedicated account with matching GID and non-login shell')
    interpreter = runtime.trusted_path(Path('/Library/Developer/CommandLineTools/usr/bin/python3').resolve(strict=True))
    if not shutil.rmtree.avoids_symlink_attacks:raise RuntimeError('descriptor-based cleanup required')
    stopped = [False]
    previous = {}
    for sig in (signal.SIGTERM, signal.SIGINT):
        previous[sig] = signal.signal(sig, lambda *unused: stopped.__setitem__(0, True))
    try:
        with Reservation(state, policy, runtime) as lease:
            child = None
            try:
                scratch = lease.session / 'scratch';scratch.mkdir(mode=0o700)
                os.chown(scratch, policy['worker_uid'], policy['worker_gid'])
                session_policy = dict(policy, version=2, scratch_parent=str(scratch))
                session_path = lease.session / 'policy.json'
                create_record(session_path, json.dumps(session_policy).encode())
                session_path.chmod(0o644)
                child = subprocess.Popen(launcher_command(interpreter, session_path, command),
                    cwd='/', env=runtime.ENTRY_ENV, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, **credentials(policy))
                reason, output = monitor(child, timeout, lambda: stopped[0])
            finally:
                reap_identity(policy, child, interpreter, runtime.ENTRY_ENV)
                if child is not None:
                    child.wait(timeout=3)
                    child.stdout.close();child.stderr.close()
                lease.release_after_quiescence()
            return dict(status='completed', reason=reason, returncode=child.returncode,
                        worker_uid=policy['worker_uid'], identity_idle=True,
                        stdout=bytes(output['stdout']).decode('utf-8', 'replace'),
                        stderr=bytes(output['stderr']).decode('utf-8', 'replace'))
    finally:
        for sig, handler in previous.items():signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=Path, required=True)
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        command = args.command[1:] if args.command[:1] == ['--'] else args.command
        report = supervise(args.policy, args.state, command, args.timeout)
        print(json.dumps(report))
        return report['returncode'] if report['reason'] == 'exited' else 124
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='blocked', error=str(error))))
        return 2


if __name__ == '__main__':raise SystemExit(main())
