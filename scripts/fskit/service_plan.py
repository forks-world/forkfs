"""Generate, never install, a dedicated-identity launchd control service plan."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import pwd
import re
import stat

LABEL='world.forks.forkfsd'

def configuration(service_user,admin_uid,binary,backing,runtime,frontend_uid=None):
    if not re.fullmatch(r'_[a-z][a-z0-9_]{0,30}',service_user):
        raise ValueError('use a dedicated underscore-prefixed service account')
    if not isinstance(admin_uid,int) or isinstance(admin_uid,bool) or not 0<=admin_uid<2**32-1:
        raise ValueError('invalid management UID')
    if frontend_uid is not None and (not isinstance(frontend_uid,int) or isinstance(frontend_uid,bool)
            or not 0<=frontend_uid<2**32-1 or frontend_uid==admin_uid):
        raise ValueError('frontend UID must be valid and distinct from management')
    paths=[Path(p) for p in (binary,backing,runtime)]
    if any(not p.is_absolute() or '..' in p.parts for p in paths):
        raise ValueError('service paths must be absolute without parent traversal')
    binary,backing,runtime=map(str,paths)
    if Path(runtime)==Path(backing) or Path(backing) in Path(runtime).parents or Path(runtime) in Path(backing).parents:
        raise ValueError('runtime and backing must be separate trees')
    config=dict(Label=LABEL,UserName=service_user,
                ProgramArguments=[binary,'serve',backing,'--socket',str(Path(runtime)/'control.sock'),
                                  '--rpc','--control-uid',str(admin_uid)],
                WorkingDirectory=backing,EnvironmentVariables=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin'),
                Umask=0o077,RunAtLoad=True,KeepAlive=dict(SuccessfulExit=False),ThrottleInterval=5,
                ProcessType='Background',SoftResourceLimits=dict(Core=0),HardResourceLimits=dict(Core=0))
    if frontend_uid is not None:config['ProgramArguments']+=['--frontend-uid',str(frontend_uid)]
    return config

def check_path(path,owner,mode=None,executable=False):
    """POSIX admission only; does not certify ACLs or installation authenticity."""
    path=Path(path)
    info=os.lstat(path)
    if stat.S_ISLNK(info.st_mode):raise ValueError('service paths must not be symlinks')
    if info.st_uid!=owner:raise ValueError('unexpected path owner: '+str(path))
    if executable:
        if not stat.S_ISREG(info.st_mode) or not info.st_mode&0o111 or info.st_mode&0o6022 or info.st_nlink!=1:
            raise ValueError('binary must be a single-link executable, not group/world writable')
    elif not stat.S_ISDIR(info.st_mode) or stat.S_IMODE(info.st_mode)!=mode:
        raise ValueError('unexpected directory type/mode: '+str(path))
    for parent in path.parents:
        parent_info=os.lstat(parent)
        if (not stat.S_ISDIR(parent_info.st_mode) or parent_info.st_uid not in (0,owner)
                or parent_info.st_mode&0o022):
            raise ValueError('untrusted service path ancestor: '+str(parent))

def preflight(config):
    failures=[]
    try:
        account=pwd.getpwnam(config['UserName']);args=config['ProgramArguments'];admin=int(args[args.index('--control-uid')+1])
        if '--frontend-uid' in args and account.pw_uid==int(args[args.index('--frontend-uid')+1]):
            raise ValueError('frontend UID must differ from backing service UID')
        if account.pw_uid==0 or account.pw_uid==admin:raise ValueError('service UID must be non-root and distinct from management UID')
        if account.pw_shell not in ('/usr/bin/false','/bin/false','/usr/sbin/nologin','/sbin/nologin'):
            raise ValueError('service account must have a non-login shell')
    except (KeyError,ValueError) as error:
        return dict(status='blocked',blockers=[str(error)],installed=False)
    for args in ((config['ProgramArguments'][0],0,None,True),
                 (config['WorkingDirectory'],account.pw_uid,0o700,False),
                 (str(Path(config['ProgramArguments'][4]).parent),account.pw_uid,0o711,False)):
        try:check_path(*args)
        except (OSError,ValueError) as error:failures.append(str(error))
    return dict(status='blocked' if failures else 'posix_checks_passed',blockers=failures,installed=False,
                note='ACLs, root-owned launchd plist, code signature and real cross-UID operation still require deployment validation.')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--service-user',required=True);p.add_argument('--admin-uid',type=int,required=True);p.add_argument('--frontend-uid',type=int)
    for name in ('binary','backing','runtime','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--preflight',action='store_true');a=p.parse_args()
    config=configuration(a.service_user,a.admin_uid,a.binary,a.backing,a.runtime,a.frontend_uid)
    report=preflight(config) if a.preflight else dict(status='plan_only',installed=False)
    # Never overwrite an existing file or follow an output symlink.
    fd=os.open(a.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f:plistlib.dump(config,f);f.flush();os.fsync(f.fileno())
    print(json.dumps(report));return 2 if report['status']=='blocked' else 0
if __name__=='__main__':raise SystemExit(main())
