"""Opt-in macOS cross-UID experiment; never installs users or changes production policy.

Run as root on a disposable machine/CI runner. Fixtures are ordinary directories,
not a forkfs mount: a pass establishes only the UID/toolchain prerequisite.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import secrets
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fskit'))
from restricted_workload import profile, workload_environment


PROBE = r'''
import ctypes, errno, json, os, sys
for change in (lambda: os.setuid(0), lambda: os.setgid(0), lambda: os.setgroups([0])):
    try:change()
    except OSError as error:assert error.errno == errno.EPERM, error
    else:raise AssertionError('child can regain privileged credentials')
libc = ctypes.CDLL(None, use_errno=True)
# Python's modern macOS os.getgroups() reports directory-service memberships,
# not necessarily the kernel supplementary groups installed by setgroups().
libc.getgroups.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint)]
libc.getgroups.restype = ctypes.c_int
count = libc.getgroups(0, None)
assert count >= 0, ctypes.get_errno()
groups = (ctypes.c_uint * count)()
assert libc.getgroups(count, groups) == count, ctypes.get_errno()
libc.sysctl.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
                       ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                       ctypes.c_void_p, ctypes.c_size_t]
libc.sysctl.restype = ctypes.c_int
def query(pid):
    mib = (ctypes.c_int * 3)(1, 49, pid)  # CTL_KERN, KERN_PROCARGS2
    data = ctypes.create_string_buffer(os.sysconf('SC_ARG_MAX'))
    size = ctypes.c_size_t(len(data))
    ctypes.set_errno(0)
    result = libc.sysctl(mib, 3, data, ctypes.byref(size), None, 0)
    return result, ctypes.get_errno(), data.raw[:size.value] if result == 0 else b''
mode, pid, marker = sys.argv[1:]
result, error, data = query(int(pid))
if mode == 'allow':
    assert result == 0 and marker.encode() in data, (result, error)
else:
    # XNU sysctl_procargsx returns EINVAL specifically for a UID mismatch.
    # Identical positive queries and live-marker checks guard malformed requests.
    assert result == -1 and error in (errno.EPERM, errno.EACCES, errno.EINVAL), (result, error)
    assert marker.encode() not in data
print(json.dumps(dict(mode=mode, result=result, errno=error, uid=os.getuid(),
                     euid=os.geteuid(), gid=os.getgid(), egid=os.getegid(),
                     groups=list(groups))))
'''


def identities(host_user, worker_user):
    host, worker = pwd.getpwnam(host_user), pwd.getpwnam(worker_user)
    if host.pw_uid <= 0 or worker.pw_uid <= 0 or host.pw_uid == worker.pw_uid:
        raise ValueError('host and worker must have distinct non-root positive UIDs')
    if worker.pw_gid <= 0 or worker.pw_gid == host.pw_gid:
        raise ValueError('worker needs a separate non-root primary group')
    return host, worker


def experimental_profile(mount, backing, scratch, read_only=()):
    # Deliberately private to this experiment, never a production launcher option.
    policy = profile(mount, backing, scratch, read_only)
    denial = '(deny process-info*)'
    if policy.count(denial) != 1:
        raise ValueError('production policy changed; review experiment before running')
    return policy.replace(denial, '(allow process-info*)\n(allow signal (target same-sandbox))')


def credentials(account):
    # Popen drops supplementary groups, GID and UID before exec. No saved root
    # identity, shell, ambient cwd, environment or extra descriptors reach children.
    return dict(user=account.pw_uid, group=account.pw_gid, extra_groups=(),
                umask=0o077, close_fds=True, start_new_session=True)


def require_success(result, label):
    if result.returncode:
        raise RuntimeError(f'{label}: exit {result.returncode}; '
                           f'{result.stderr[-4000:]}')
    return result


def experiment(host_user, worker_user):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('requires real/effective root on a disposable macOS host')
    host, worker = identities(host_user, worker_user)
    clean = dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin', HOME='/var/empty',
                 LANG='C', LC_ALL='C')
    def tool(*args):
        return subprocess.check_output(args, env=clean, cwd='/', text=True,
                                       stdin=subprocess.DEVNULL, timeout=30).strip()
    developer = Path(tool('/usr/bin/xcode-select', '-p')).resolve(strict=True)
    python = developer / 'usr/bin/python3'
    if not python.is_file():
        raise RuntimeError('selected developer directory must contain a real python3')
    compiler = tool('/usr/bin/xcrun', '--find', 'clang')
    cxx = tool('/usr/bin/xcrun', '--find', 'clang++')
    sdk = tool('/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-path')
    records = []
    with tempfile.TemporaryDirectory(prefix='forkfs-uid-', dir='/private/tmp') as name:
        root = Path(name).resolve(); root.chmod(0o755)
        mount, backing, scratch, host_home = [root / p for p in ('work', 'backing', 'scratch', 'host')]
        for path, account in ((mount, worker), (backing, host), (scratch, worker), (host_home, host)):
            path.mkdir(mode=0o700); os.chown(path, account.pw_uid, account.pw_gid)
        secret = backing / 'secret'; secret.write_text(secrets.token_hex(32))
        os.chown(secret, host.pw_uid, host.pw_gid); secret.chmod(0o600)
        environment = workload_environment(scratch)
        environment.update(DEVELOPER_DIR=str(developer), SDKROOT=sdk,
                           PATH=str(developer / 'usr/bin') + ':' + clean['PATH'])
        for directory in (scratch / 'home', scratch / 'tmp'):
            os.chown(directory, worker.pw_uid, worker.pw_gid)
        policy = experimental_profile(mount, backing, scratch, [developer, Path(sdk)])
        def run(account, argv, *, sandbox=True, cwd=mount):
            command = ['/usr/bin/sandbox-exec', '-p', policy, *argv] if sandbox else argv
            return subprocess.run(command, cwd=cwd, env=environment,
                                  stdin=subprocess.DEVNULL, capture_output=True,
                                  text=True, timeout=60, **credentials(account))
        markers = []
        try:
            for account, directory in ((host, host_home), (worker, mount)):
                token = secrets.token_hex(32)
                marker = subprocess.Popen([str(python), '-c',
                    'import time; print("ready", flush=True); time.sleep(180)', token],
                    cwd=directory, env=clean, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    **credentials(account))
                markers.append((marker, token))
                # A bounded child avoids an indefinitely blocked readiness read.
                import select
                ready, _, _ = select.select([marker.stdout], [], [], 15)
                if not ready or marker.stdout.readline().strip() != 'ready' or marker.poll() is not None:
                    raise RuntimeError('marker process did not start')
            host_process, host_token = markers[0]
            worker_process, worker_token = markers[1]
            cases = [
                ('host-positive-control', host, 'allow', host_process, host_token, False, host_home),
                ('same-uid-kernel-positive', worker, 'allow', worker_process, worker_token, False, mount),
                ('cross-uid-kernel-denial', worker, 'deny', host_process, host_token, False, mount),
                ('same-uid-sandbox-positive', worker, 'allow', worker_process, worker_token, True, mount),
                ('cross-uid-sandbox-denial', worker, 'deny', host_process, host_token, True, mount),
            ]
            for label, account, mode, process, token, sandbox, directory in cases:
                if process.poll() is not None:raise RuntimeError('marker exited during test')
                result = require_success(run(account, [str(python), '-c', PROBE, mode,
                    str(process.pid), token], sandbox=sandbox, cwd=directory), label)
                if process.poll() is not None:raise RuntimeError('marker exited during query')
                record = json.loads(result.stdout)
                if (record['uid'] != account.pw_uid or record['euid'] != account.pw_uid
                        or record['gid'] != account.pw_gid or record['egid'] != account.pw_gid
                        or set(record['groups']) - {account.pw_gid}):
                    raise RuntimeError('child retained unexpected credentials')
                records.append(dict(name=label, status='passed', **record))
            source = mount / 'hello.c'
            source.write_text('#include <stdio.h>\nint main(void){puts("forkfs-uid-build-ok");return 0;}\n')
            os.chown(source, worker.pw_uid, worker.pw_gid)
            require_success(run(worker, [compiler, '-isysroot', sdk, str(source), '-o', str(mount / 'hello')]), 'compile-link')
            binary = require_success(run(worker, [str(mount / 'hello')]), 'execute-linked-program')
            if binary.stdout.strip() != 'forkfs-uid-build-ok':raise RuntimeError('unexpected binary output')
            records.append(dict(name='compile-link-execute', status='passed'))
            source = mount / 'hello.cc'
            source.write_text('#include <fstream>\n#include <string>\n#include <vector>\n'
                              'int main(){std::vector<std::string> v{"forkfs","uid"};'
                              'std::ofstream out("generated");out<<v.at(0)<<"-"<<v.at(1);'
                              'out.close();return out.fail();}\n')
            os.chown(source, worker.pw_uid, worker.pw_gid)
            require_success(run(worker, [cxx, '-std=c++20', '-isysroot', sdk, str(source),
                                         '-o', str(mount / 'hello-cxx')]), 'compile-link-cxx')
            require_success(run(worker, [str(mount / 'hello-cxx')]), 'execute-linked-cxx')
            if (mount / 'generated').read_text() != 'forkfs-uid':
                raise RuntimeError('C++ program did not write the expected workspace content')
            records.append(dict(name='cxx-compile-link-execute-write', status='passed'))
            for sandbox in (False, True):
                denied = run(worker, ['/bin/cat', str(secret)], sandbox=sandbox)
                if denied.returncode != 1 or denied.stdout:
                    raise RuntimeError('worker could read host-owned backing')
                records.append(dict(name='backing-denial-' + ('sandbox' if sandbox else 'posix'), status='passed'))
        finally:
            for process, _ in markers:
                if process.poll() is None:process.terminate()
                try:process.communicate(timeout=10)
                except subprocess.TimeoutExpired:process.kill();process.communicate(timeout=10)
    return dict(status='passed', mounted_acceptance=False, production_policy_changed=False,
                host_uid=host.pw_uid, worker_uid=worker.pw_uid,
                macos=platform.mac_ver()[0], compiler=compiler, checks=records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-user', required=True)
    parser.add_argument('--worker-user', required=True)
    args = parser.parse_args()
    try:result = experiment(args.host_user, args.worker_user)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='blocked', mounted_acceptance=False, error=str(error))))
        return 2
    print(json.dumps(result, indent=2)); return 0


if __name__ == '__main__':raise SystemExit(main())
