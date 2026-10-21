"""Run a workload under an explicit macOS sandbox; require a real forkfs mount.
This constrains this process tree, not other processes owned by the host user.
"""
import argparse
import errno
import fcntl
import stat
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from mount_probe import require_mount

F_GETPATH = 50  # Darwin fcntl command
OWNER_LEASE = 'OWNERSHIP'

def inside(path,parent):
    try:path.relative_to(parent);return True
    except ValueError:return False

def inventory_backing(backing):
    """Validate a stable-looking backing tree and return its regular-file IDs.

    This is startup admission under the trusted-host assumption. It cannot
    prevent another same-UID process from changing the tree after this walk.
    """
    backing=Path(backing).absolute()
    try:root=os.lstat(backing)
    except OSError as error:raise ValueError('cannot inspect backing root') from error
    if not stat.S_ISDIR(root.st_mode):raise ValueError('backing root must be a real directory')
    files=set()
    def visit(directory):
        try:entries=list(os.scandir(directory))
        except OSError as error:raise ValueError('cannot inventory backing') from error
        for entry in entries:
            try:info=entry.stat(follow_symlinks=False)
            except OSError as error:raise ValueError('cannot inventory backing entry') from error
            if stat.S_ISDIR(info.st_mode):
                visit(Path(entry.path))
            elif stat.S_ISREG(info.st_mode):
                if info.st_nlink!=1:raise ValueError('backing file has an external hardlink alias')
                files.add((info.st_dev,info.st_ino))
            else:
                raise ValueError('backing contains a symlink or unsupported file type')
    visit(backing)
    return backing,files

def validate_owner_lease(backing):
    """Require contention on an exclusive lease; this does not identify its holder."""
    path=Path(backing)/OWNER_LEASE
    try:
        before=os.lstat(path)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink!=1:
            raise ValueError('owner lease must be a single-link regular file')
        flags=os.O_RDONLY|getattr(os,'O_CLOEXEC',0)|getattr(os,'O_NOFOLLOW',0)|getattr(os,'O_NONBLOCK',0)
        descriptor=os.open(path,flags)
    except ValueError:raise
    except OSError as error:raise ValueError('cannot open owner lease') from error
    try:
        after=os.fstat(descriptor)
        if (not stat.S_ISREG(after.st_mode) or after.st_nlink!=1 or
            (before.st_dev,before.st_ino)!=(after.st_dev,after.st_ino)):
            raise ValueError('owner lease changed during admission')
        try:fcntl.flock(descriptor,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EWOULDBLOCK,errno.EAGAIN):
                return
            raise ValueError('cannot check owner lease') from error
        fcntl.flock(descriptor,fcntl.LOCK_UN)
        raise ValueError('backing has no live exclusive owner lease')
    finally:os.close(descriptor)

def validate_stdio(backing,descriptors=(0,1,2),backing_files=None):
    """Allow only safe inherited streams; `descriptors` makes this testable."""
    backing=Path(backing).resolve(strict=True)
    if backing_files is None:
        backing,backing_files=inventory_backing(backing)
    else:backing=Path(backing).resolve(strict=True)
    for descriptor in descriptors:
        try:info=os.fstat(descriptor)
        except OSError as error:raise RuntimeError('cannot validate inherited stdio') from error
        mode=info.st_mode
        if stat.S_ISSOCK(mode):raise ValueError('socket stdio is not allowed')
        if stat.S_ISFIFO(mode):continue
        if stat.S_ISCHR(mode):
            try:raw=fcntl.fcntl(descriptor,F_GETPATH,bytes(1024))
            except OSError as error:raise RuntimeError('cannot identify character stdio') from error
            path=Path(os.fsdecode(raw.split(b'\0',1)[0]))
            if path==Path('/dev/null') or os.isatty(descriptor):continue
            raise ValueError('unsupported character-device stdio')
        if stat.S_ISREG(mode):
            try:raw=fcntl.fcntl(descriptor,F_GETPATH,bytes(1024))
            except OSError as error:raise RuntimeError('cannot identify regular stdio') from error
            inherited=Path(os.fsdecode(raw.split(b'\0',1)[0])).resolve(strict=True)
            if inside(inherited,backing) or (info.st_dev,info.st_ino) in backing_files:
                raise ValueError('stdio refers to backing storage')
            continue
        raise ValueError('unsupported inherited stdio type')

def validate_backing(backing,read_only=()):
    """Admit a backing tree only while another process holds OWNERSHIP exclusively."""
    backing,files=inventory_backing(backing)
    if not (backing/'CURRENT').is_file():raise ValueError('backing is not a repository')
    validate_owner_lease(backing)
    # nlink==1 above rejects every pre-existing hardlink alias, including one
    # hidden under an otherwise allowed read-only toolchain.
    for grant in read_only:
        path=Path(grant).resolve(strict=True)
        if inside(path,backing) or inside(backing,path):raise ValueError('read-only grants overlap backing')
    return backing,files

def profile(mount,backing,scratch,read_only=()):
    mount,backing,scratch=(Path(p).resolve() for p in (mount,backing,scratch))
    if any(inside(a,b) or inside(b,a) for a,b in ((mount,backing),(scratch,backing),(mount,scratch))):
        raise ValueError('mount, backing and scratch must be separate directories')
    readonly=[Path(p).resolve() for p in ('/System','/usr','/bin','/sbin','/Library/Apple','/Library/Developer') if Path(p).exists()]
    readonly += [Path(p).resolve(strict=True) for p in read_only]
    if any(inside(backing,p) or inside(p,backing) for p in readonly):raise ValueError('read-only grants overlap backing')
    quote=lambda p:json.dumps(str(p),ensure_ascii=False)
    terms=lambda paths:' '.join('(subpath '+quote(p)+')' for p in paths)
    # realpath/toolchain startup needs ancestor metadata, not directory contents.
    ancestors=sorted({parent for path in readonly+[mount,scratch,Path('/dev/null')]
                      for parent in path.parents},key=str)
    metadata=' '.join('(literal '+quote(p)+')' for p in ancestors)
    return '\n'.join([
        '(version 1)','(deny default)',
        '(allow process-exec process-fork)',
        '(allow sysctl-read (sysctl-name-prefix "hw."))',
        '(allow sysctl-read (sysctl-name-prefix "machdep."))',
        '(allow sysctl-read (sysctl-name-prefix "kern.os"))',
        '(allow sysctl-read (sysctl-name "kern.version") (sysctl-name "kern.hostname") '
            '(sysctl-name "kern.boottime") (sysctl-name "kern.argmax") (sysctl-name "vm.pagesize"))',
        '(deny process-info*)',
        '(allow file-read* (literal "/"))',
        '(allow file-read-metadata '+metadata+')',
        '(allow file-read* '+terms(readonly+[mount,scratch])+')',
        '(allow file-write* '+terms([mount,scratch])+')',
        '(allow file-read* file-write* (literal "/dev/null") (literal "/dev/urandom") (literal "/dev/random"))',
        '(allow mach-lookup (global-name "com.apple.system.logger"))',
        # Explicit backing denial also overrides accidental broader allow rules.
        '(deny file* (subpath '+quote(backing)+'))',
    ])+'\n'

def workload_environment(scratch):
    """Construct a fresh environment; host credentials and IPC hints never cross."""
    scratch=Path(scratch).resolve(strict=True)
    home=scratch/'home';temporary=scratch/'tmp'
    home.mkdir(mode=0o700);temporary.mkdir(mode=0o700)
    system_path='/usr/bin:/bin:/usr/sbin:/sbin'
    environment=dict(PATH=system_path,HOME=str(home),
                TMPDIR=str(temporary)+'/',LANG='en_US.UTF-8',LC_ALL='en_US.UTF-8',
                GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL='/dev/null',
                GIT_TERMINAL_PROMPT='0')
    # Avoid the xcrun shim consulting host selection files outside the sandbox.
    developer=Path('/Library/Developer/CommandLineTools')
    if developer.is_dir():
        environment['DEVELOPER_DIR']=str(developer)
        environment['PATH']=str(developer/'usr/bin')+':'+system_path
    return environment

def run(mount,backing,command,read_only=()):
    if platform.system()!='Darwin':raise RuntimeError('restricted workload launcher requires macOS')
    mount=Path(mount).resolve(strict=True)
    require_mount(mount)
    backing,backing_files=validate_backing(backing,read_only)
    validate_stdio(backing,backing_files=backing_files)
    # No ambient directory descriptors, credentials, preload settings or IPC hints.
    with tempfile.TemporaryDirectory(prefix='forkfs-workload-',dir='/tmp') as directory:
        scratch=Path(directory).resolve();env=workload_environment(scratch)
        requested=command[0]
        if not Path(requested).is_absolute() and os.path.sep in requested:
            requested=str((mount/requested).resolve())
        executable=shutil.which(requested,path=env['PATH'])
        if not executable:raise ValueError('workload executable was not found')
        policy=profile(mount,backing,scratch,read_only)
        return subprocess.run(['/usr/bin/sandbox-exec','-p',policy,executable,*command[1:]],
                              cwd=mount,env=env,close_fds=True).returncode

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mount',required=True,type=Path);p.add_argument('--backing',required=True,type=Path)
    p.add_argument('--read-only',action='append',default=[],type=Path);p.add_argument('command',nargs=argparse.REMAINDER)
    args=p.parse_args();command=args.command[1:] if args.command[:1]==['--'] else args.command
    if not command:p.error('provide a workload after --')
    return run(args.mount,args.backing,command,args.read_only)
if __name__=='__main__':raise SystemExit(main())
