"""Kernel mount attestation and read-only FSKit installation preflight."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess
import struct
import uuid

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

def descriptor_mount_info(fd):
    """Attest the filesystem reached by an already pinned directory FD."""
    if platform.system()!='Darwin': raise RuntimeError('FSKit attestation requires macOS')
    result=DarwinStatFS();libc=ctypes.CDLL(None,use_errno=True)
    call=libc.fstatfs;call.argtypes=[ctypes.c_int,ctypes.POINTER(DarwinStatFS)];call.restype=ctypes.c_int
    if call(fd,ctypes.byref(result))!=0: raise OSError(ctypes.get_errno(),'fstatfs')
    return dict(fstype=result.fstype.decode(),mountpoint=os.fsdecode(result.mountpoint),
                fsid=list(result.fsid),fskit=bool(result.flags_ext & 2),read_only=bool(result.flags & 1),
                available_bytes=result.bavail*result.bsize)

def descriptor_volume_uuid(fd):
    """Read the kernel's FSKit volume UUID from a pinned directory FD."""
    if platform.system()!='Darwin': raise RuntimeError('volume UUID attestation requires macOS')
    class AttrList(ctypes.Structure):
        _fields_=[('bitmapcount',ctypes.c_uint16),('reserved',ctypes.c_uint16),
                  ('commonattr',ctypes.c_uint32),('volattr',ctypes.c_uint32),
                  ('dirattr',ctypes.c_uint32),('fileattr',ctypes.c_uint32),('forkattr',ctypes.c_uint32)]
    attrs=AttrList(5,0,0,0x80040000,0,0,0) # ATTR_VOL_INFO | ATTR_VOL_UUID
    output=(ctypes.c_ubyte*20)()
    libc=ctypes.CDLL(None,use_errno=True);call=libc.fgetattrlist
    call.argtypes=[ctypes.c_int,ctypes.POINTER(AttrList),ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong]
    call.restype=ctypes.c_int
    if call(fd,ctypes.byref(attrs),ctypes.byref(output),ctypes.sizeof(output),0)!=0:
        raise OSError(ctypes.get_errno(),'fgetattrlist volume UUID')
    length,=struct.unpack_from('=I',bytes(output),0)
    raw=bytes(output[4:20])
    if length!=20 or raw==b'\0'*16:
        raise RuntimeError('kernel did not return a supported FSKit volume UUID')
    return str(uuid.UUID(bytes=raw))

def mounted_fsids():
    """Return a bounded snapshot of mounted filesystems for post-unmount proof."""
    if platform.system()!='Darwin': raise RuntimeError('mount enumeration requires macOS')
    libc=ctypes.CDLL(None,use_errno=True);call=libc.getfsstat
    call.argtypes=[ctypes.POINTER(DarwinStatFS),ctypes.c_int,ctypes.c_int];call.restype=ctypes.c_int
    capacity=4096;items=(DarwinStatFS*capacity)()
    count=call(items,ctypes.sizeof(items),2) # MNT_NOWAIT
    if count<=0 or count>=capacity:raise RuntimeError('mounted filesystem enumeration failed or exceeded capacity')
    return {tuple(item.fsid) for item in items[:count]}

def funmount_fd(fd):
    """Unmount the mount pinned by fd, never resolving a replacement path."""
    libc=ctypes.CDLL(None,use_errno=True);call=libc.funmount
    call.argtypes=[ctypes.c_int,ctypes.c_int];call.restype=ctypes.c_int
    if call(fd,0)!=0:raise OSError(ctypes.get_errno(),'funmount')

def require_mount(path, expected_read_only=False):
    path=Path(path).resolve(strict=True);info=mount_info(path)
    if info['fstype']!='forkrevision' or not info['fskit'] or Path(info['mountpoint']).resolve()!=path:
        raise RuntimeError('not an attested forkfs FSKit mount root: '+json.dumps(info))
    if info['read_only'] != bool(expected_read_only): raise RuntimeError('mount read-only state differs from policy')
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
