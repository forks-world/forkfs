"""Root-owned binding and retirement guard for one FSKit worker mount.

This module is intentionally called only by the installed root supervisor.
The explicit policy-to-receipt binding is an administrator assertion about
which service mount belongs to the worker; the kernel checks below establish
the mounted filesystem type, flags, and filesystem ID at use time.
"""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import sys
import struct
import tempfile
import time
import uuid


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def write_all(fd, payload):
    offset = 0
    while offset < len(payload):
        count = os.write(fd, payload[offset:])
        if count <= 0: raise OSError(errno.EIO, 'short write while publishing root lifecycle data')
        offset += count


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError('duplicate lifecycle record key')
        result[key] = value
    return result


def expected_session_uuid(generation, session):
    from mount_session import token
    if not token(generation) or not token(session): raise ValueError('invalid mount session identity')
    identity = b'forkfs.fskit.volume.v1' + generation.encode('ascii') + session.encode('ascii')
    digest = bytearray(hashlib.sha256(b'forkfs.object.v0' +
        struct.pack('<HHQ', 4, 0, len(identity)) + identity).digest()[:16])
    digest[6] = (digest[6] & 0x0f) | 0x80
    digest[8] = (digest[8] & 0x3f) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def read_root_file(path, mode=0o600, maximum=65536):
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts: raise ValueError('root record path must be absolute')
    # Every ancestor must be root-owned and not writable by group/other. The
    # installed runtime applies the stronger ACL checks before this helper.
    for item in [path.parent, *path.parent.parents]:
        info = os.lstat(item)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o7022:
            raise ValueError('root lifecycle directory chain is not trusted')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != mode:
            raise ValueError('unsafe root lifecycle file')
        data = os.read(fd, maximum + 1)
        if len(data) > maximum: raise ValueError('root lifecycle file too large')
        return data
    finally: os.close(fd)


def load_record(path, runtime):
    runtime.trusted_path(Path(path).parent, directory=True)
    runtime.no_acl(Path(path))
    data = read_root_file(path)
    value = json.loads(data, object_pairs_hook=unique_object)
    required = {'version', 'phase', 'policy_sha256', 'mount', 'fsid', 'volume_uuid', 'service_uid', 'socket',
                'generation', 'session', 'name', 'read_only'}
    if (not isinstance(value, dict) or set(value) != required or type(value.get('version')) is not int
            or value.get('version') != 1):
        raise ValueError('unsupported mount lifecycle record')
    if value['phase'] not in ('bound', 'active', 'stopping', 'retired'):
        raise ValueError('invalid mount lifecycle phase')
    from mount_session import token
    if (type(value['read_only']) is not bool or type(value['service_uid']) is not int or value['service_uid'] <= 0
            or not isinstance(value['policy_sha256'], str) or len(value['policy_sha256']) != 64
            or any(c not in '0123456789abcdef' for c in value['policy_sha256'])
            or not token(value['generation']) or not token(value['session'])
            or not isinstance(value['fsid'], list) or len(value['fsid']) != 2
            or any(type(part) is not int or not -(2**31) <= part < 2**31 for part in value['fsid'])
            or not isinstance(value['mount'], str) or not Path(value['mount']).is_absolute()
            or '..' in Path(value['mount']).parts or not isinstance(value['socket'], str)
            or not Path(value['socket']).is_absolute() or '..' in Path(value['socket']).parts
            or not isinstance(value['name'], str) or not value['name'] or len(value['name']) > 64
            or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in value['name'])):
        raise ValueError('invalid mount lifecycle record fields')
    if value['volume_uuid'] != expected_session_uuid(value['generation'], value['session']):
        raise ValueError('mount lifecycle UUID does not match its daemon generation and session')
    return value


def bind(receipt_path, policy_path, output, runtime, probe=None):
    if platform.system() != 'Darwin' or os.getuid() != 0 or os.geteuid() != 0:
        raise RuntimeError('mount binding requires root on macOS')
    runtime.admit_entry_environment()
    policy = runtime.read_policy(Path(policy_path))
    if policy['version'] != 1: raise ValueError('mount lifecycle binding requires worker policy version 1')
    runtime.trusted_path(Path(receipt_path).parent, directory=True)
    runtime.no_acl(Path(receipt_path))
    receipt_data = read_root_file(receipt_path)
    receipt = json.loads(receipt_data, object_pairs_hook=unique_object)
    required = {'version','socket','service_uid','frontend_uid','generation','session','name','read_only','lease_ms'}
    if (not isinstance(receipt, dict) or set(receipt) != required or type(receipt['version']) is not int
            or receipt['version'] != 1):
        raise ValueError('unsupported mount receipt')
    from mount_session import token
    if (type(receipt['service_uid']) is not int or receipt['service_uid'] <= 0
            or type(receipt['frontend_uid']) is not int or receipt['frontend_uid'] <= 0
            or receipt['service_uid'] != policy['service_uid'] or receipt['frontend_uid'] != policy['frontend_uid']
            or not token(receipt['generation']) or not token(receipt['session'])
            or type(receipt['read_only']) is not bool or type(receipt['lease_ms']) is not int
            or not 3000 <= receipt['lease_ms'] <= 300000
            or not isinstance(receipt['name'], str) or not 1 <= len(receipt['name']) <= 64
            or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in receipt['name'])):
        raise ValueError('mount receipt does not match worker policy')
    if (not isinstance(receipt['socket'], str) or not Path(receipt['socket']).is_absolute()
            or '..' in Path(receipt['socket']).parts): raise ValueError('invalid service socket path')
    resource = Path(receipt['socket']).parent
    resource_info = os.lstat(resource)
    if (not stat.S_ISDIR(resource_info.st_mode) or resource_info.st_uid != receipt['service_uid']
            or stat.S_IMODE(resource_info.st_mode) != 0o711):
        raise ValueError('receipt does not identify a service-owned 0711 resource directory')
    socket_info = os.lstat(receipt['socket'])
    if not stat.S_ISSOCK(socket_info.st_mode) or socket_info.st_uid != receipt['service_uid']:
        raise ValueError('receipt service socket ownership mismatch')
    from mount_session import request
    checked = request(receipt['socket'], receipt['service_uid'],
                      ['mount-check', receipt['session'], '--generation', receipt['generation']], timeout=1)
    if (checked.get('generation') != receipt['generation'] or checked.get('name') != receipt['name']
            or checked.get('read_only') is not receipt['read_only']):
        raise ValueError('mount receipt session is not live and matching')
    if probe is None:
        import mount_probe
        probe = mount_probe
    expected_uuid = expected_session_uuid(receipt['generation'], receipt['session'])
    root_fd = os.open(policy['mount'], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = probe.descriptor_mount_info(root_fd)
        actual_uuid = probe.descriptor_volume_uuid(root_fd)
        mount_path = Path(policy['mount']).resolve(strict=True)
        if (info['fstype'] != 'forkrevision' or not info['fskit']
                or info['read_only'] != receipt['read_only']
                or Path(info['mountpoint']).resolve() != mount_path):
            raise RuntimeError('not an attested forkfs FSKit mount root')
        if actual_uuid != expected_uuid:
            raise ValueError('FSKit volume UUID does not match daemon generation and session')
    finally: os.close(root_fd)
    if info['fsid'] != policy['mount_fsid']:
        raise ValueError('mounted filesystem ID differs from worker policy')
    # The receipt is intentionally a root-only bearer credential. It records
    # the daemon session, not a frontend-controlled marker or claim.
    record = dict(version=1, phase='bound', policy_sha256=hashlib.sha256(canonical(policy)).hexdigest(),
                  mount=policy['mount'], fsid=info['fsid'], volume_uuid=actual_uuid,
                  service_uid=receipt['service_uid'], socket=receipt['socket'],
                  generation=receipt['generation'], session=receipt['session'], name=receipt['name'],
                  read_only=receipt['read_only'])
    output = Path(output)
    runtime.trusted_path(output.parent, directory=True)
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        write_all(fd, canonical(record)); os.fchmod(fd, 0o600); os.fsync(fd)
    except BaseException:
        os.close(fd)
        try: os.unlink(output)
        except OSError: pass
        raise
    else: os.close(fd)
    dfd = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try: os.fsync(dfd)
    finally: os.close(dfd)
    return record


def atomic_root_file(path, payload, runtime):
    path = Path(path)
    runtime.trusted_path(path.parent, directory=True)
    runtime.trusted_path(path)
    runtime.no_acl(path)
    temp_fd, temp_name = tempfile.mkstemp(prefix='.' + path.name + '.', suffix='.tmp', dir=path.parent)
    try:
        os.fchmod(temp_fd, 0o600)
        write_all(temp_fd, payload)
        os.fsync(temp_fd)
        os.replace(temp_name, path)
        parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try: os.fsync(parent_fd)
        finally: os.close(parent_fd)
        runtime.no_acl(path)
    except BaseException:
        try: os.unlink(temp_name)
        except OSError: pass
        raise
    finally: os.close(temp_fd)


class Guard:
    """Held by supervise() from pre-launch verification through retirement."""
    def __init__(self, record_path, policy, runtime, probe=None, rpc=None, sleep=time.sleep):
        self.path = Path(record_path); self.policy = policy; self.runtime = runtime
        self.probe = probe; self.rpc = rpc; self.sleep = sleep
        self.fd = self.record_fd = self.lock_fd = None; self.record = None

    def _modules(self):
        if self.probe is None:
            import mount_probe
            self.probe = mount_probe
        if self.rpc is None:
            import mount_session
            self.rpc = mount_session.request

    def _rpc_status(self):
        self._modules()
        value = self.rpc(self.record['socket'], self.record['service_uid'],
                         ['mount-check', self.record['session'], '--generation', self.record['generation']], timeout=1)
        if (value.get('generation') != self.record['generation'] or value.get('name') != self.record['name']
                or value.get('read_only') is not self.record['read_only']):
            raise RuntimeError('daemon mount session identity changed')

    def _persist(self):
        payload = canonical(self.record)
        current = os.fstat(self.record_fd)
        path_info = os.lstat(self.path)
        if (current.st_dev, current.st_ino) != (path_info.st_dev, path_info.st_ino):
            raise ValueError('mount lifecycle record changed under controller')
        temp_fd, temp_name = tempfile.mkstemp(prefix='.' + self.path.name + '.', suffix='.tmp', dir=self.path.parent)
        try:
            os.fchmod(temp_fd, 0o600)
            write_all(temp_fd, payload)
            os.fsync(temp_fd)
            os.replace(temp_name, self.path)
            parent_fd = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            try: os.fsync(parent_fd)
            finally: os.close(parent_fd)
        except BaseException:
            try: os.unlink(temp_name)
            except OSError: pass
            raise
        finally: os.close(temp_fd)
        os.close(self.record_fd)
        self.record_fd = os.open(self.path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        self.runtime.no_acl(self.path)

    def start(self, active_path, session_dir=None):
        runtime = self.runtime
        runtime.trusted_path(self.path.parent, directory=True)
        lock_path = self.path.with_name(self.path.name + '.lock')
        self.lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        lock_info = os.fstat(self.lock_fd)
        if not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != 0 or lock_info.st_nlink != 1:
            raise ValueError('unsafe lifecycle controller lock')
        os.fchmod(self.lock_fd, 0o600)
        if stat.S_IMODE(os.fstat(self.lock_fd).st_mode) != 0o600: raise ValueError('unsafe lifecycle controller lock mode')
        runtime.no_acl(lock_path)
        fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.record_fd = os.open(self.path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        meta = os.fstat(self.record_fd)
        if not stat.S_ISREG(meta.st_mode) or meta.st_uid != 0 or meta.st_nlink != 1 or stat.S_IMODE(meta.st_mode) != 0o600:
            raise ValueError('unsafe mount lifecycle record')
        self.record = load_record(self.path, runtime)
        current = os.fstat(self.record_fd); named = os.stat(self.path, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (named.st_dev, named.st_ino):
            raise ValueError('mount lifecycle record changed during admission')
        if (not isinstance(self.record, dict) or set(self.record) !=
                {'version','phase','policy_sha256','mount','fsid','volume_uuid','service_uid','socket','generation','session','name','read_only'}
                or self.record.get('version') != 1): raise ValueError('unsupported mount lifecycle record')
        runtime.no_acl(self.path)
        if self.record.get('phase') != 'bound': raise ValueError('mount lifecycle record is not bound')
        if hashlib.sha256(canonical(self.policy)).hexdigest() != self.record['policy_sha256']:
            raise ValueError('mount lifecycle policy fingerprint changed')
        if (self.record['mount'] != self.policy['mount'] or self.record['fsid'] != self.policy['mount_fsid']
                or self.record['service_uid'] != self.policy['service_uid']
                or self.record['volume_uuid'] != expected_session_uuid(self.record['generation'], self.record['session'])):
            raise ValueError('mount lifecycle record does not match worker policy')
        self._modules()
        self.fd = os.open(self.record['mount'], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        info = self.probe.descriptor_mount_info(self.fd)
        if (info['fstype'] != 'forkrevision' or not info['fskit'] or info['fsid'] != self.record['fsid']
                or info['read_only'] != self.record['read_only']
                or self.probe.descriptor_volume_uuid(self.fd) != self.record['volume_uuid']
                or Path(info['mountpoint']).resolve() != Path(self.record['mount']).resolve()):
            raise RuntimeError('pinned directory is not the recorded FSKit mount')
        self._rpc_status()
        self.record['phase'] = 'active'; self._persist()
        # The caller already owns the UID lease; this durable marker completes
        # admission before any worker child is started.
        atomic_root_file(active_path, canonical(dict(status='active', mount_record=str(self.path),
                                                     session_dir=str(session_dir) if session_dir else None)), runtime)
        return self.check

    def recover(self):
        """Adopt an interrupted bound/active session under its controller lock."""
        self.runtime.trusted_path(self.path.parent, directory=True)
        lock_path = self.path.with_name(self.path.name + '.lock')
        self.lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        info = os.fstat(self.lock_fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise ValueError('unsafe lifecycle controller lock')
        os.fchmod(self.lock_fd, 0o600)
        if stat.S_IMODE(os.fstat(self.lock_fd).st_mode) != 0o600: raise ValueError('unsafe lifecycle controller lock mode')
        self.runtime.no_acl(lock_path)
        fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.record_fd = os.open(self.path, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
        info = os.fstat(self.record_fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1
                or stat.S_IMODE(info.st_mode) != 0o600): raise ValueError('unsafe mount lifecycle record')
        self.runtime.no_acl(self.path)
        self.record = load_record(self.path, self.runtime)
        current = os.fstat(self.record_fd); named = os.stat(self.path, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (named.st_dev, named.st_ino):
            raise ValueError('mount lifecycle record changed during recovery')
        if (not isinstance(self.record, dict) or set(self.record) !=
                {'version','phase','policy_sha256','mount','fsid','volume_uuid','service_uid','socket','generation','session','name','read_only'}
                or self.record.get('version') != 1 or self.record.get('phase') not in ('bound','active','stopping','retired')):
            raise ValueError('record is not recoverable')
        if (hashlib.sha256(canonical(self.policy)).hexdigest() != self.record['policy_sha256']
                or self.record['mount'] != self.policy['mount'] or self.record['fsid'] != self.policy['mount_fsid']
                or self.record['service_uid'] != self.policy['service_uid']
                or self.record['volume_uuid'] != expected_session_uuid(self.record['generation'], self.record['session'])):
            raise ValueError('recovery policy does not match lifecycle record')
        self._modules()
        old_fsid = tuple(self.record['fsid'])
        if self.record['phase'] == 'retired':
            if old_fsid in self.probe.mounted_fsids():
                raise RuntimeError('retired filesystem ID unexpectedly remains mounted')
            return
        if old_fsid in self.probe.mounted_fsids():
            self.fd = os.open(self.record['mount'], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            mount = self.probe.descriptor_mount_info(self.fd)
            if (mount['fstype'] != 'forkrevision' or not mount['fskit'] or mount['fsid'] != self.record['fsid']
                    or mount['read_only'] != self.record['read_only']
                    or self.probe.descriptor_volume_uuid(self.fd) != self.record['volume_uuid']
                    or Path(mount['mountpoint']).resolve() != Path(self.record['mount']).resolve()):
                raise RuntimeError('recorded mount path no longer names the pinned FSKit mount')
        self.record['phase'] = 'stopping'; self._persist()

    def check(self):
        if self.record is None or self.record['phase'] != 'active': raise RuntimeError('mount guard is not active')
        if tuple(self.record['fsid']) not in self.probe.mounted_fsids():
            raise RuntimeError('recorded mount filesystem ID disappeared')
        self._rpc_status()

    def stopping(self):
        if self.record is None: return
        self.record['phase'] = 'stopping'; self._persist()

    def finish(self):
        if self.record is None: return
        if self.record['phase'] == 'retired': return
        self._modules()
        try:
            # Keep the backend session live while the kernel runs its unmount
            # callback, so dirty filesystem state can flush before retirement.
            old_fsid = tuple(self.record['fsid'])
            if old_fsid in self.probe.mounted_fsids() and self.fd is not None:
                self.probe.funmount_fd(self.fd)
            if self.fd is not None:
                os.close(self.fd); self.fd = None
            deadline = time.monotonic() + 5
            while old_fsid in self.probe.mounted_fsids():
                if time.monotonic() >= deadline: raise RuntimeError('recorded mount filesystem ID remains mounted')
                self.sleep(0.05)
            hello = self.rpc(self.record['socket'], self.record['service_uid'], ['service-hello'], timeout=1)
            current_generation = hello.get('generation')
            from mount_session import token
            if hello.get('protocol') != 1 or not token(current_generation):
                raise RuntimeError('cannot verify current daemon generation for retirement')
            self.rpc(self.record['socket'], self.record['service_uid'],
                     ['mount-retire', self.record['generation'], self.record['session'],
                      '--generation', current_generation], timeout=1)
            self.record['phase'] = 'retired'; self._persist()
        finally:
            if self.fd is not None: os.close(self.fd); self.fd = None

    def close(self):
        if self.fd is not None: os.close(self.fd); self.fd = None
        if self.record_fd is not None: os.close(self.record_fd); self.record_fd = None
        if self.lock_fd is not None: os.close(self.lock_fd); self.lock_fd = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    command = sub.add_parser('bind'); command.add_argument('--receipt', required=True); command.add_argument('--policy', required=True); command.add_argument('--output', required=True)
    args = parser.parse_args()
    try:
        if not sys.flags.isolated or not sys.flags.no_site: raise RuntimeError('invoke trusted lifecycle controller with Python -I -S')
        directory = Path(__file__).absolute().parent
        libc = ctypes.CDLL(None, use_errno=True)
        libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
        libc.acl_get_link_np.restype = ctypes.c_void_p
        libc.acl_free.argtypes = [ctypes.c_void_p]
        for filename in ('mount_lifecycle.py','agent_supervisor.py','mount_session.py','mount_probe.py',
                         'dedicated_workload.py','restricted_workload.py'):
            entry = directory / filename
            for index, item in enumerate([entry, *entry.parents]):
                info = os.lstat(item)
                expected = stat.S_ISDIR if index else stat.S_ISREG
                if (not expected(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o7022
                        or (not index and info.st_nlink != 1)):
                    raise ValueError('lifecycle installation is not root-owned and private to root writes')
                ctypes.set_errno(0); acl = libc.acl_get_link_np(os.fsencode(item), 0x100)
                if acl:
                    libc.acl_free(acl); raise ValueError('lifecycle installation has an extended ACL')
                if ctypes.get_errno() != errno.ENOENT: raise RuntimeError('cannot inspect lifecycle installation ACL')
        import agent_supervisor
        runtime = agent_supervisor.installed_launcher()
        agent_supervisor.trusted_interpreter(runtime)
        result = bind(args.receipt, args.policy, args.output, runtime)
        print(json.dumps(dict(status='bound', mount= result['mount'], fsid=result['fsid'])))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps(dict(status='blocked', error=str(error))))
        return 2


if __name__ == '__main__': raise SystemExit(main())
