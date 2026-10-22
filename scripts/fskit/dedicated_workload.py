"""Launch a workload AFTER a trusted supervisor has switched to its dedicated UID.

This program never runs workloads as root and never installs identities/services.
The CLI requires isolated Python and root-owned installed code and policy.
"""
import argparse
import ctypes
import errno
import importlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tempfile

ENTRY_ENV = dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin', HOME='/var/empty', LANG='C', LC_ALL='C')


def admit_entry_environment():
    expected = dict(ENTRY_ENV)
    # CoreFoundation adds this during macOS Python startup even for env -i.
    # Accept only the current UID's C-locale value, never arbitrary text.
    if '__CF_USER_TEXT_ENCODING' in os.environ:
        encoding = os.environ['__CF_USER_TEXT_ENCODING']
        if encoding not in (f'0x{os.getuid():X}:0x0:0x0', f'0x{os.getuid():X}:0:0'):
            raise ValueError('supervisor must exec the launcher with the exact fresh entry environment')
        expected['__CF_USER_TEXT_ENCODING'] = encoding
    if dict(os.environ) != expected:
        raise ValueError('supervisor must exec the launcher with the exact fresh entry environment')


def no_acl(path):
    """Reject extended ACLs conservatively, including deny-only/inherited ACLs."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.acl_get_link_np.restype = ctypes.c_void_p
    libc.acl_free.argtypes = [ctypes.c_void_p]
    ctypes.set_errno(0)
    acl = libc.acl_get_link_np(os.fsencode(path), 0x100)  # ACL_TYPE_EXTENDED
    if acl:
        libc.acl_free(acl)
        raise ValueError('extended ACL is not admitted: ' + str(path))
    if ctypes.get_errno() != errno.ENOENT:
        raise OSError(ctypes.get_errno(), 'cannot verify absence of ACL', str(path))


def trusted_path(path, *, directory=False):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('trusted paths must be absolute without parent traversal')
    for index, item in enumerate([path, *path.parents]):
        info = os.lstat(item)
        expected = stat.S_ISDIR if index or directory else stat.S_ISREG
        if (not expected(info.st_mode) or info.st_uid != 0
                or info.st_mode & 0o7022 or (not index and not directory and info.st_nlink != 1)):
            raise ValueError('path must be root-owned, without special modes or group/world writes: ' + str(item))
        no_acl(item)
    return path


def parse_policy(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:raise ValueError('duplicate policy key: ' + key)
            result[key] = value
        return result
    policy = json.loads(data, object_pairs_hook=unique)
    fields = {'version', 'host_uid', 'service_uid', 'frontend_uid', 'worker_uid',
              'worker_gid', 'mount', 'mount_fsid', 'backing', 'read_only'}
    if not isinstance(policy, dict) or set(policy) != fields or type(policy['version']) is not int or policy['version'] != 1:
        raise ValueError('unsupported identity policy schema')
    ids = [policy[key] for key in ('host_uid', 'service_uid', 'frontend_uid', 'worker_uid')]
    if any(type(uid) is not int or not 0 < uid < 2**32 - 1 for uid in ids) or len(set(ids)) != 4:
        raise ValueError('host, service, frontend and worker require distinct non-root UIDs')
    if type(policy['worker_gid']) is not int or not 0 < policy['worker_gid'] < 2**32 - 1:
        raise ValueError('invalid worker GID')
    for key in ('mount', 'backing'):
        value = policy[key]
        if not isinstance(value, str) or not Path(value).is_absolute() or '..' in Path(value).parts:
            raise ValueError('invalid ' + key + ' path')
    fsid = policy['mount_fsid']
    if not isinstance(fsid, list) or len(fsid) != 2 or any(type(i) is not int or not -(2**31) <= i < 2**31 for i in fsid):
        raise ValueError('invalid mount fsid')
    grants = policy['read_only']
    if not isinstance(grants, list) or any(not isinstance(p, str) or not Path(p).is_absolute() or '..' in Path(p).parts for p in grants):
        raise ValueError('invalid toolchain grants')
    return policy


def read_policy(path):
    path = trusted_path(path)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        current = os.lstat(path)
        if (info.st_dev, info.st_ino) != (current.st_dev, current.st_ino):
            raise ValueError('policy changed during admission')
        if info.st_size > 65536:raise ValueError('identity policy too large')
        with os.fdopen(fd, 'rb', closefd=False) as stream:data = stream.read(65537)
        if len(data) > 65536:raise ValueError('identity policy too large')
        return parse_policy(data)
    finally:os.close(fd)


def kernel_groups():
    # Use the BSD libc symbol, not Python's macOS directory-service group list.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.getgroups.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint)]
    libc.getgroups.restype = ctypes.c_int
    count = libc.getgroups(0, None)
    if not 0 <= count <= 1024:raise RuntimeError('cannot inspect supplementary groups')
    groups = (ctypes.c_uint * count)()
    if libc.getgroups(count, groups) != count:raise RuntimeError('supplementary groups changed')
    return set(groups)


def admit_identity(policy):
    if (os.getuid() != policy['worker_uid'] or os.geteuid() != policy['worker_uid']
            or os.getgid() != policy['worker_gid'] or os.getegid() != policy['worker_gid']
            or kernel_groups() - {policy['worker_gid']}):
        raise ValueError('supervisor must drop UID, GID and supplementary groups before launch')
    # setuid/setgid to the same unprivileged ID is valid even without privilege.
    # Check real saved-root recovery rather than relying on effective IDs alone.
    for recover in (lambda: os.setuid(0), lambda: os.setgid(0), lambda: os.setgroups([0])):
        try:recover()
        except OSError as error:
            if error.errno != errno.EPERM:raise
        else:raise RuntimeError('retained ability to regain root; refusing workload')


def installed_runtime():
    directory = Path(__file__).absolute().parent
    for name in ('dedicated_workload.py', 'restricted_workload.py', 'mount_probe.py'):
        trusted_path(directory / name)
    sys.path.insert(0, str(directory))
    return importlib.import_module('restricted_workload')


def dedicated_profile(runtime, mount, backing, scratch, grants):
    policy = runtime.profile(mount, backing, scratch, grants)
    denial = '(deny process-info*)'
    if policy.count(denial) != 1:raise RuntimeError('sandbox policy changed; review dedicated policy')
    return policy.replace(denial, '(allow process-info*)\n(allow signal (target same-sandbox))')


def safe_output():
    # Do not expose an inherited backing file/socket/terminal as workload stdio.
    # The host supervisor may receive output through fresh pipes.
    null = os.stat('/dev/null')
    for descriptor in (1, 2):
        info = os.fstat(descriptor)
        if stat.S_ISFIFO(info.st_mode):continue
        if stat.S_ISCHR(info.st_mode) and info.st_rdev == null.st_rdev:continue
        raise ValueError('dedicated workload output must be a pipe or /dev/null')


def run(policy_path, command):
    if platform.system() != 'Darwin':raise RuntimeError('dedicated launcher requires macOS')
    if os.getuid() == 0 or os.geteuid() == 0:raise RuntimeError('launcher must already run as the dedicated non-root UID')
    # Scrubbing only the workload's environment leaves this same-UID supervisor's
    # original exec-stack environment queryable. Admission requires a clean exec
    # from the trusted identity broker, before any untrusted child can exist.
    admit_entry_environment()
    if not command:raise ValueError('workload command is required')
    policy = read_policy(policy_path)
    admit_identity(policy)
    runtime = installed_runtime()
    mount = Path(policy['mount'])
    # No CLI option or native-directory fallback may bypass this gate.
    attestation = runtime.require_mount(mount)
    if attestation['fsid'] != policy['mount_fsid']:raise ValueError('mount identity differs from provisioned fsid')
    backing = Path(policy['backing'])
    trusted_path(backing.parent, directory=True)
    info = os.lstat(backing)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != policy['service_uid'] or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('backing must be a private directory owned by the service UID')
    no_acl(backing)
    # The worker must not inventory/open the service's private store.
    safe_output()
    with tempfile.TemporaryDirectory(prefix='forkfs-dedicated-', dir='/private/tmp') as name:
        scratch = Path(name).resolve()
        environment = runtime.workload_environment(scratch)
        requested = command[0]
        if not Path(requested).is_absolute() and os.path.sep in requested:
            requested = str((mount / requested).resolve())
        executable = shutil.which(requested, path=environment['PATH'])
        if not executable:raise ValueError('workload executable was not found')
        sandbox = dedicated_profile(runtime, mount, backing, scratch, policy['read_only'])
        # Recheck after admission and command selection, immediately before exec.
        if runtime.require_mount(mount)['fsid'] != policy['mount_fsid']:
            raise ValueError('mount changed during admission')
        return subprocess.run(['/usr/bin/sandbox-exec', '-p', sandbox, executable, *command[1:]],
                              cwd=mount, env=environment, stdin=subprocess.DEVNULL,
                              close_fds=True, umask=0o077, start_new_session=True).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', required=True, type=Path)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if not sys.flags.isolated or not sys.flags.no_site:
            raise RuntimeError('invoke installed launcher with trusted Python -I -S')
        command = args.command[1:] if args.command[:1] == ['--'] else args.command
        return run(args.policy, command)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(status='blocked', error=str(error))), file=sys.stderr)
        return 2


if __name__ == '__main__':raise SystemExit(main())
