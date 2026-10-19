"""Kernel mount attestation and read-only FSKit installation preflight."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess

class DarwinStatFS(ctypes.Structure):
    _fields_=[('bsize',ctypes.c_uint32),('iosize',ctypes.c_int32),
              ('blocks',ctypes.c_uint64),('bfree',ctypes.c_uint64),('bavail',ctypes.c_uint64),
              ('files',ctypes.c_uint64),('ffree',ctypes.c_uint64),('fsid',ctypes.c_int32*2),
              ('owner',ctypes.c_uint32),('type',ctypes.c_uint32),('flags',ctypes.c_uint32),('subtype',ctypes.c_uint32),
              ('fstype',ctypes.c_char*16),('mountpoint',ctypes.c_char*1024),('source',ctypes.c_char*1024),
              ('flags_ext',ctypes.c_uint32),('reserved',ctypes.c_uint32*7)]

def mount_info(path):
    if platform.system()!='Darwin': raise RuntimeError('FSKit attestation requires macOS')
    result=DarwinStatFS();libc=ctypes.CDLL(None,use_errno=True)
    # The public 64-bit Darwin statfs layout, including MNT_EXT_FSKIT.
    call=libc.statfs;call.argtypes=[ctypes.c_char_p,ctypes.POINTER(DarwinStatFS)];call.restype=ctypes.c_int
    if call(os.fsencode(path),ctypes.byref(result))!=0: raise OSError(ctypes.get_errno(),'statfs')
    return dict(fstype=result.fstype.decode(),mountpoint=os.fsdecode(result.mountpoint),
                fsid=list(result.fsid),fskit=bool(result.flags_ext & 2),read_only=bool(result.flags & 1),
                available_bytes=result.bavail*result.bsize)

def require_mount(path):
    path=Path(path).resolve(strict=True);info=mount_info(path)
    if info['fstype']!='forkrevision' or not info['fskit'] or Path(info['mountpoint']).resolve()!=path:
        raise RuntimeError('not an attested forkfs FSKit mount root: '+json.dumps(info))
    if info['read_only']: raise RuntimeError('acceptance requires a writable World mount')
    return info

def preflight(app):
    failures=[];report=dict(platform=platform.system(),version=platform.mac_ver()[0],mounted=False)
    if platform.system()!='Darwin': failures.append('macOS is required')
    elif int(platform.mac_ver()[0].split('.')[0])<27: failures.append('the mount client requires macOS 27+')
    app=Path(app);extension=app/'Contents/PlugIns/ForkRevisionExtension.appex'
    report['bundle_present']=app.is_dir() and extension.is_dir()
    if not report['bundle_present']: failures.append('host/extension app bundle is missing')
    if platform.system()=='Darwin':
        signed=subprocess.run(['codesign','--verify','--deep','--strict',str(app)],capture_output=True)
        report['signature_verified']=signed.returncode==0
        if signed.returncode: failures.append('app signature verification failed')
        details=subprocess.run(['codesign','-dv','--verbose=4',str(app)],capture_output=True,text=True)
        report['team_signed']=details.returncode==0 and any(line.startswith('TeamIdentifier=') and line!='TeamIdentifier=not set' for line in details.stderr.splitlines())
        if not report['team_signed']: failures.append('app has no verified signing team')
        identities=subprocess.run(['security','find-identity','-v','-p','codesigning'],capture_output=True,text=True)
        report['signing_identity_available']='0 valid identities found' not in identities.stdout and identities.returncode==0
        if not report['signing_identity_available'] and not report['signature_verified']: failures.append('no valid signing identity is available to sign the app')
        registered=subprocess.run(['pluginkit','-m','-A','-D','-i','world.forks.cow.revision.extension'],capture_output=True,text=True)
        report['extension_registered']=registered.returncode==0 and 'world.forks.cow.revision.extension' in registered.stdout
        if not report['extension_registered']: failures.append('FSKit extension registration is not visible')
    report['status']='blocked' if failures else 'prerequisites_available'
    report['blockers']=failures
    report['isolation_verified']=False
    report['note']='Signing and registration alone do not prove mount, provisioning or workload isolation.'
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--app',type=Path);p.add_argument('--mount',type=Path);p.add_argument('--output',type=Path)
    args=p.parse_args()
    if bool(args.app)==bool(args.mount):p.error('provide exactly one of --app or --mount')
    try: result=preflight(args.app) if args.app else dict(status='attested',mount=require_mount(args.mount))
    except Exception as error: result=dict(status='blocked',blockers=[str(error)])
    text=json.dumps(result,indent=2)
    if args.output:
        with args.output.open('x') as f:f.write(text+'\n')
    print(text);return 2 if result['status']=='blocked' else 0
if __name__=='__main__':raise SystemExit(main())
