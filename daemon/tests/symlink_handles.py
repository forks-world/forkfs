"""Metadata-only symlink descriptors retain identity through sessions and crashes."""
import errno
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
BIN,FAULT=sys.argv[1:3]
def run(*args):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=20);assert p.returncode==0,(args,p.stderr);return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-linkhandle-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'db';data=root/'input';endpoint=root/'control.sock';data.write_bytes(b'target')
    run('init',db);run('fs-init',db);run('fs-write',db,'/file',data);run('fs-symlink',db,'/file','/link');data.write_bytes(b'link\0xattr');run('fs-xattr-set',db,'/link','user.a','create',data);run('fs-snapshot',db,'base')
    baseline=root/'baseline';shutil.copytree(db,baseline)
    def start(binary=BIN,env=None):
        p=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        assert select.select([p.stdout],[],[],5)[0];assert json.loads(p.stdout.readline())['state']=='leveldb';return p
    def exact(s,n):
        b=b''
        while len(b)<n:
            v=s.recv(n-len(b));assert v,'unexpected EOF';b+=v
        return b
    def rpc(*args,code=0):
        fields=[v if isinstance(v,bytes) else str(v).encode() for v in args];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload);h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0]);assert struct.unpack('<I',b[:4])[0]==code,(args,b);return b[4:]
    def obj(*args):return json.loads(rpc(*args))
    proc=start()
    try:
        session=obj('session-open',60000)['session'];sel=('--session',session);rid=obj('inode-root',*sel)['inode']['inode_id'];lid=obj('inode-lookup',rid,'link',*sel)['inode_id']
        rpc('inode-open',rid,'r',*sel,code=errno.EISDIR+1)
        handle=obj('inode-open',lid,'rw',*sel)['handle'];assert obj('fs-hstat',handle,*sel)['symlink']
        for fields in [('fs-hread',handle,0,1),('fs-hread-next',handle,0),('fs-hwrite',handle,0,b'x'),('fs-hwrite-next',handle,b''),('fs-htruncate',handle,0)]:rpc(*fields,*sel,code=1)
        assert rpc('fs-cat','/file')==b'target'
        for flags in ['rw,symlink,create','rw,symlink,truncate','rw,symlink,append','rw,symlink,nofollow']:rpc('fs-open','/link',flags,*sel,code=1)
        assert rpc('fs-hxattr-get',handle,'user.a',*sel)==b'link\0xattr'
        dup=obj('fs-dup',handle,*sel)['handle'];rpc('inode-remove',rid,'link',lid,'file',*sel);rpc('inode-symlink',rid,'link','/missing',*sel)
        attrs=obj('fs-hstat',dup,*sel);assert attrs['links']==0 and attrs['inode_id']==lid
        rpc('fs-hxattr-set',dup,'user.a','replace',b'orphan',*sel);rpc('fs-hsettimes',dup,1,2,3,*sel)
        assert obj('fs-hstat',handle,*sel)['birth_ns']==3 and rpc('fs-hxattr-get',handle,'user.a',*sel)==b'orphan'
        assert obj('fs-lstat','/link')['inode_id']!=lid
        rpc('inode-setattr-times',lid,0o777,attrs['size'],999,999,999,handle,*sel,code=errno.EINVAL+1)
        assert obj('fs-hstat',handle,*sel)['birth_ns']==3
        other=obj('session-open',60000)['session'];rpc('fs-hstat',handle,'--session',other,code=1);rpc('session-close',other)
        rpc('fs-close',handle,*sel);assert obj('fs-hstat',dup,*sel)['links']==0;rpc('fs-check');rpc('fs-close',dup,*sel);rpc('fs-check')
        rpc('session-close',session);session=obj('session-open',60000)['session'];sel=('--session',session)
        ro=('--revision','base',*sel);root_id=obj('inode-root',*ro)['inode']['inode_id'];old_id=obj('inode-lookup',root_id,'link',*ro)['inode_id'];old=obj('inode-open',old_id,'r',*ro)['handle']
        assert rpc('fs-hxattr-get',old,'user.a',*ro)==b'link\0xattr';rpc('fs-hxattr-remove',old,'user.a',*ro,code=1);rpc('fs-hread',old,0,1,*ro,code=1);rpc('fs-close',old,*ro);rpc('session-close',session)
        # Lease expiry closes an orphaned metadata fd and permits maintenance.
        short=obj('session-open',500)['session'];short_sel=('--session',short);h=obj('fs-open','/link','r,symlink',*short_sel)['handle'];rpc('fs-unlink','/link');time.sleep(.75)
        rpc('fs-hstat',h,*short_sel,code=1);rpc('fs-check')
    finally:
        proc.terminate();proc.wait(timeout=10)
    run('gc',db);run('fs-check',db);run('fs-check',db,'--revision','base')
    for stage in ['leveldb_before_write','leveldb_after_write']:
        shutil.rmtree(db);shutil.copytree(baseline,db);endpoint.unlink(missing_ok=True)
        proc=start(FAULT,{**os.environ,'FORKFS_TEST_CRASH':stage})
        try:
            session=obj('session-open',60000)['session'];sel=('--session',session);h=obj('fs-open','/link','r,symlink',*sel)['handle']
            try:rpc('fs-unlink','/link')
            except AssertionError as e:assert str(e)=='unexpected EOF',e
            else:raise AssertionError('fault did not terminate server')
            assert proc.wait(timeout=10)==86
        finally:
            if proc.poll() is None:proc.kill();proc.wait(timeout=10)
        run('fs-check',db);run('gc',db);run('fs-check',db,'--revision','base');assert run('fs-cat',db,'/file')==b'target'
        if stage=='leveldb_before_write':assert run('fs-readlink',db,'/link')==b'/file'
        else:assert b'link' not in run('fs-ls',db,'/').splitlines()
print('metadata-handle RPC, data guards, identity, ownership, dup, readonly revisions, lease expiry, crash orphan recovery and GC passed')
