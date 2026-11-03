#!/usr/bin/env python3
"""Prepare a private runtime directory as a security-scoped FSKit resource.
The daemon must already serve RUNTIME/control.sock in --rpc mode.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('daemon',type=Path)
parser.add_argument('runtime',type=Path)
selector=parser.add_mutually_exclusive_group(required=True)
selector.add_argument('--world')
selector.add_argument('--revision')
args=parser.parse_args()
runtime=args.runtime.absolute()
st=runtime.lstat()
if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.geteuid() or st.st_mode&0o077:
    parser.error('runtime must be a private directory owned by the current user')
view=['--revision',args.revision] if args.revision is not None else ['--world',args.world]
def request(*fields):
    p=subprocess.run([str(args.daemon.resolve()),'request',str(runtime/'control.sock'),*fields],check=True,capture_output=True)
    return json.loads(p.stdout)
session=request('session-open','60000')['session']
try:
    root=request('inode-root',*view,'--session',session)
finally:
    request('session-close',session)
config={'schema_version':1,'repository':root['repository'],view[0][2:]:view[1]}
fd,name=tempfile.mkstemp(prefix='.forkfs-',dir=runtime)
try:
    with os.fdopen(fd,'w') as stream:
        json.dump(config,stream);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    os.replace(name,runtime/'forkfs.json')
finally:
    if os.path.exists(name):os.unlink(name)
print(runtime/'forkfs.json')
