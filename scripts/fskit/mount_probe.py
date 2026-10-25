"""Kernel mount attestation and read-only FSKit installation preflight."""
import argparse
import ctypes
import json
import os
import plistlib
from pathlib import Path
import platform
import subprocess
import struct
import uuid

CODESIGN='/usr/bin/codesign'
SECURITY='/usr/bin/security'
PLUGINKIT='/usr/bin/pluginkit'
PREFLIGHT_COMMAND_TIMEOUT=15

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

def _single_codesign_value(text, key):
    values=[line[len(key)+1:].strip() for line in text.splitlines() if line.startswith(key+'=')]
    if len(values)!=1 or not values[0] or values[0]=='not set': return None
    return values[0]


def _signed_bundle(bundle, role, expected_identifier, required_entitlements):
    result=dict(bundle_identifier=None, team_identifier=None, signature_verified=False,
                entitlements_valid=False, entitlements={key:False for key in required_entitlements}, blockers=[])
    if not bundle.is_dir():
        result['blockers'].append(f'{role} bundle is missing')
        return result
    info_path=bundle/'Contents/Info.plist'
    try:
        with info_path.open('rb') as stream: info=plistlib.load(stream)
        info_identifier=info.get('CFBundleIdentifier') if isinstance(info,dict) else None
        if info_identifier != expected_identifier:
            result['blockers'].append(f'{role} Info.plist bundle identifier is missing or unexpected')
    except Exception:
        info_identifier=None
        result['blockers'].append(f'{role} Info.plist is malformed or unreadable')
    try:
        verified=subprocess.run([CODESIGN,'--verify','--deep','--strict',str(bundle)],
                                capture_output=True,timeout=PREFLIGHT_COMMAND_TIMEOUT)
        result['signature_verified']=verified.returncode==0
        if not result['signature_verified']:
            result['blockers'].append(f'{role} code signature verification failed')
    except Exception:
        result['blockers'].append(f'{role} code signature verification could not run')
    try:
        details=subprocess.run([CODESIGN,'-dv','--verbose=4',str(bundle)],
                               capture_output=True,timeout=PREFLIGHT_COMMAND_TIMEOUT)
        if details.returncode:
            raise ValueError('codesign display failed')
        text=details.stderr.decode('utf-8','strict') if isinstance(details.stderr,bytes) else str(details.stderr)
        result['bundle_identifier']=_single_codesign_value(text,'Identifier')
        result['team_identifier']=_single_codesign_value(text,'TeamIdentifier')
        if result['bundle_identifier'] != expected_identifier or result['bundle_identifier'] != info_identifier:
            result['blockers'].append(f'{role} signed bundle identifier does not match its expected Info.plist identifier')
        if result['team_identifier'] is None:
            result['blockers'].append(f'{role} signature has no verified signing team identifier')
    except Exception:
        result['blockers'].append(f'{role} signed bundle metadata is malformed or unavailable')
    try:
        entitlements=subprocess.run([CODESIGN,'--display','--entitlements',':-','--xml',str(bundle)],
                                    capture_output=True,timeout=PREFLIGHT_COMMAND_TIMEOUT)
        if entitlements.returncode:
            raise ValueError('codesign entitlement display failed')
        payload=entitlements.stdout
        parsed=plistlib.loads(payload,fmt=plistlib.FMT_XML)
        if not isinstance(parsed,dict): raise ValueError('signed entitlements are not a dictionary')
        result['entitlements']={key:(type(parsed.get(key)) is bool and parsed.get(key) is True)
                                for key in required_entitlements}
        result['entitlements_valid']=all(result['entitlements'].values())
        for key, valid in result['entitlements'].items():
            if not valid: result['blockers'].append(f'{role} signed entitlement {key} is missing or not true')
    except Exception:
        result['blockers'].append(f'{role} signed entitlements are malformed or unavailable')
    return result


def preflight(app):
    system=platform.system();version=platform.mac_ver()[0]
    report=dict(platform=system,version=version,mounted=False,signature_verified=False,
                host_signature_verified=False,extension_signature_verified=False,
                host_bundle_identifier=None,extension_bundle_identifier=None,
                host_team_identifier=None,extension_team_identifier=None,team_identifier=None,
                team_identifiers_match=False,team_signed=False,
                host_entitlements={'com.apple.developer.fskit.mount':False,
                                   'com.apple.security.app-sandbox':False,
                                   'com.apple.security.network.client':False},
                extension_entitlements={'com.apple.developer.fskit.fsmodule':False,
                                        'com.apple.security.app-sandbox':False,
                                        'com.apple.security.network.client':False},
                host_entitlements_valid=False,extension_entitlements_valid=False,
                host_blockers=[],extension_blockers=[],signing_identity_available=False,
                extension_registered=False)
    failures=[]
    if system!='Darwin': failures.append('macOS is required')
    else:
        try:
            if int(version.split('.')[0])<27: failures.append('the mount client requires macOS 27+')
        except (ValueError,IndexError): failures.append('macOS version could not be identified')
    app=Path(app);extension=app/'Contents/PlugIns/ForkRevisionExtension.appex'
    report['bundle_present']=app.is_dir() and extension.is_dir()
    if system=='Darwin':
        host=_signed_bundle(app,'host','world.forks.cow.revision',(
            'com.apple.developer.fskit.mount','com.apple.security.app-sandbox',
            'com.apple.security.network.client'))
        ext=_signed_bundle(extension,'extension','world.forks.cow.revision.extension',(
            'com.apple.developer.fskit.fsmodule','com.apple.security.app-sandbox',
            'com.apple.security.network.client'))
        report['host_signature_verified']=host['signature_verified']
        report['extension_signature_verified']=ext['signature_verified']
        report['host_bundle_identifier']=host['bundle_identifier']
        report['extension_bundle_identifier']=ext['bundle_identifier']
        report['host_team_identifier']=host['team_identifier']
        report['extension_team_identifier']=ext['team_identifier']
        report['host_entitlements']=host['entitlements']
        report['extension_entitlements']=ext['entitlements']
        report['host_entitlements_valid']=host['entitlements_valid']
        report['extension_entitlements_valid']=ext['entitlements_valid']
        report['host_blockers']=host['blockers']
        report['extension_blockers']=ext['blockers']
        report['signature_verified']=host['signature_verified'] and ext['signature_verified']
        teams_match=(host['team_identifier'] is not None and ext['team_identifier'] is not None
                     and host['team_identifier']==ext['team_identifier'])
        report['team_identifiers_match']=teams_match
        report['team_signed']=teams_match
        report['team_identifier']=host['team_identifier']
        failures.extend(host['blockers']);failures.extend(ext['blockers'])
        if not teams_match: failures.append('host and extension signing team identifiers do not match')
        try:
            identities=subprocess.run([SECURITY,'find-identity','-v','-p','codesigning'],capture_output=True,
                                      text=True,timeout=PREFLIGHT_COMMAND_TIMEOUT)
            report['signing_identity_available']=(identities.returncode==0 and
                '0 valid identities found' not in identities.stdout)
        except subprocess.TimeoutExpired:
            report['signing_identity_available']=False
            failures.append('codesigning identity inspection timed out')
        except Exception: report['signing_identity_available']=False
        try:
            registered=subprocess.run([PLUGINKIT,'-m','-A','-D','-i','world.forks.cow.revision.extension'],
                                      capture_output=True,text=True,timeout=PREFLIGHT_COMMAND_TIMEOUT)
            report['extension_registered']=registered.returncode==0 and 'world.forks.cow.revision.extension' in registered.stdout
        except subprocess.TimeoutExpired:
            report['extension_registered']=False
            failures.append('FSKit extension registration check timed out')
        except Exception: report['extension_registered']=False
        if not report['extension_registered']: failures.append('FSKit extension registration is not visible')
    else:
        report.update(signature_verified=False,host_signature_verified=False,extension_signature_verified=False,
                      team_signed=False,team_identifiers_match=False,host_entitlements_valid=False,
                      extension_entitlements_valid=False,host_blockers=[],extension_blockers=[],
                      signing_identity_available=False,extension_registered=False)
    report['status']='blocked' if failures else 'prerequisites_available'
    report['blockers']=failures
    report['isolation_verified']=False
    report['note']='Prerequisite checks do not prove the filesystem is mounted or agent isolation is verified.'
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
