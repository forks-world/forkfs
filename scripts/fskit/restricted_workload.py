"""Run a workload under an explicit macOS sandbox; require a real forkfs mount.
This constrains this process tree, not other processes owned by the host user.
"""
import argparse
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

def inside(path,parent):
    try:path.relative_to(parent);return True
    except ValueError:return False

def profile(mount,backing,scratch,read_only=()):
    mount,backing,scratch=(Path(p).resolve() for p in (mount,backing,scratch))
    if any(inside(a,b) or inside(b,a) for a,b in ((mount,backing),(scratch,backing),(mount,scratch))):
        raise ValueError('mount, backing and scratch must be separate directories')
    readonly=[Path(p).resolve() for p in ('/System','/usr','/bin','/sbin','/Library/Apple','/Library/Developer') if Path(p).exists()]
    readonly += [Path(p).resolve(strict=True) for p in read_only]
    if any(inside(backing,p) or inside(p,backing) for p in readonly):raise ValueError('read-only grants overlap backing')
    quote=lambda p:json.dumps(str(p),ensure_ascii=False)
    terms=lambda paths:' '.join('(subpath '+quote(p)+')' for p in paths)
    return '\n'.join([
        '(version 1)','(deny default)',
        '(allow process-exec process-fork sysctl-read)',
        '(allow file-read* (literal "/"))',
        '(allow file-read* '+terms(readonly+[mount,scratch])+')',
        '(allow file-write* '+terms([mount,scratch])+')',
        '(allow file-read* file-write* (literal "/dev/null") (literal "/dev/urandom") (literal "/dev/random"))',
        '(allow mach-lookup (global-name "com.apple.system.logger"))',
        # Explicit backing denial also overrides accidental broader allow rules.
        '(deny file* (subpath '+quote(backing)+'))',
    ])+'\n'

def run(mount,backing,command,read_only=()):
    if platform.system()!='Darwin':raise RuntimeError('restricted workload launcher requires macOS')
    require_mount(mount)
    backing=Path(backing).resolve(strict=True)
    if not (backing/'CURRENT').is_file():raise ValueError('backing is not a repository')
    executable=shutil.which(command[0])
    if not executable:raise ValueError('workload executable was not found')
    # close_fds retains stdio: reject inherited regular backing descriptors too.
    for descriptor in (0,1,2):
        try:
            mode=os.fstat(descriptor).st_mode
            if stat.S_ISREG(mode) or stat.S_ISDIR(mode):
                raw=fcntl.fcntl(descriptor,50,bytes(1024))  # Darwin F_GETPATH
                inherited=Path(os.fsdecode(raw.split(b'\0',1)[0])).resolve(strict=True)
                if inside(inherited,backing):raise ValueError('stdio refers to backing storage')
        except OSError as error:
            raise RuntimeError('cannot validate inherited stdio') from error
    # No ambient directory descriptors, preload settings or control sockets.
    env={k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','LD_','PYTHON','GIT_'))}
    with tempfile.TemporaryDirectory(prefix='forkfs-workload-',dir='/tmp') as directory:
        scratch=Path(directory).resolve();env['TMPDIR']=str(scratch)
        policy=profile(mount,backing,scratch,read_only)
        return subprocess.run(['/usr/bin/sandbox-exec','-p',policy,str(Path(executable).resolve()),*command[1:]],
                              cwd=mount,env=env,close_fds=True).returncode

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mount',required=True,type=Path);p.add_argument('--backing',required=True,type=Path)
    p.add_argument('--read-only',action='append',default=[],type=Path);p.add_argument('command',nargs=argparse.REMAINDER)
    args=p.parse_args();command=args.command[1:] if args.command[:1]==['--'] else args.command
    if not command:p.error('provide a workload after --')
    return run(args.mount,args.backing,command,args.read_only)
if __name__=='__main__':raise SystemExit(main())
