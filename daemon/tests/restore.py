"""Restore and its safety revision publish atomically, including crashes and RPC leases."""
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
BIN,LEGACY,FAULT=sys.argv[1:4]
for binary in [BIN,LEGACY]:
    def run(*args,ok=True,program=binary,env=None):
        p=subprocess.run([program,*map(str,args)],capture_output=True,timeout=20,env=env);assert (p.returncode==0)==ok,(args,p.returncode,p.stderr);return p.stdout
    with tempfile.TemporaryDirectory(prefix='ff-restore-',dir='/tmp') as tmp:
        root=Path(tmp);db=root/'db';data=root/'input';endpoint=root/'control.sock';run('init',db);run('fs-init',db)
        def snapshot(name,*view):return json.loads(run('fs-snapshot',db,name,*view))['revision_id']
        def seq():return json.loads(run('check',db))['last_committed_seq']
        def exists(name):return name.encode() in run('fs-revisions',db).splitlines()
        data.write_bytes(b'original');run('fs-write',db,'/f',data);run('fs-link',db,'/f','/alias');run('fs-symlink',db,'/f','/link')
        data.write_bytes(b'attrs\0\xff');run('fs-xattr-set',db,'/f','user.test','create',data);base=snapshot('base');run('fs-fork',db,'base','work')
        data.write_bytes(b'changed');run('fs-write',db,'/f',data);dirty=snapshot('dirty');data.write_bytes(b'unsnapped');run('fs-write',db,'/f',data)
        before=seq();result=json.loads(run('fs-restore',db,'base','safety'));assert seq()==before+1
        assert result['target_revision_id']==base and result['safety_revision_id']==json.loads(run('fs-history',db,'safety'))['tip_revision_id']
        assert run('fs-cat',db,'/f')==b'original' and run('fs-cat',db,'/f','--revision','safety')==b'unsnapped'
        assert run('fs-xattr-get',db,'/alias','user.test')==b'attrs\0\xff' and run('fs-readlink',db,'/link')==b'/f'
        restored=snapshot('restored');history=json.loads(run('fs-history',db,'restored'))['entries']
        assert [e['revision_id'] for e in history]==[restored,result['safety_revision_id'],dirty,base]
        assert json.loads(run('fs-diff',db,'base','restored'))['changes']==[]
        before=seq()
        for target,safety,view in [('absent','unused',()),('base','safety',()),('base','unused',('--revision','base')),('base','bad/name',())]:run('fs-restore',db,target,safety,*view,ok=False)
        assert seq()==before and not exists('unused')
        run('fs-restore',db,'safety','undo');assert run('fs-cat',db,'/f')==b'unsnapped'
        run('fs-restore',db,'dirty','work-safety','--world','work');assert run('fs-cat',db,'/f','--world','work')==b'changed'
        assert run('fs-cat',db,'/f')==b'unsnapped' and run('fs-cat',db,'/f','--revision','base')==b'original'
        if binary==BIN:
            baseline=root/'baseline';shutil.copytree(db,baseline)
            for stage in ['leveldb_before_write','leveldb_after_write']:
                shutil.rmtree(db);shutil.copytree(baseline,db)
                p=subprocess.run([FAULT,'fs-restore',str(db),'base','crash-safety'],capture_output=True,timeout=20,env={**os.environ,'FORKFS_TEST_CRASH':stage})
                assert p.returncode==86,p.stderr;committed=stage=='leveldb_after_write'
                assert exists('crash-safety')==committed and run('fs-cat',db,'/f')==(b'original' if committed else b'unsnapped')
                if committed:assert run('fs-cat',db,'/f','--revision','crash-safety')==b'unsnapped'
                run('fs-check',db);run('fs-check',db,'--revision','base')
            shutil.rmtree(db);shutil.copytree(baseline,db)
            proc=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            def exact(s,n):
                result=b''
                while len(result)<n:
                    p=s.recv(n-len(result));assert p;result+=p
                return result
            def rpc(*args,code=0):
                fields=[str(v).encode() for v in args];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload);h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0]);assert struct.unpack('<I',b[:4])[0]==code,(args,b);return b[4:]
            try:
                assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
                session=json.loads(rpc('session-open',60000))['session'];sel=('--session',session);rpc('inode-root',*sel)
                rpc('fs-restore','base','mounted-foreign',code=errno.EBUSY+1)
                rpc('fs-restore','base','mounted-owner',*sel,code=errno.EBUSY+1)
                assert b'mounted-owner' not in rpc('fs-revisions') and b'mounted-foreign' not in rpc('fs-revisions');rpc('session-close',session)
                session=json.loads(rpc('session-open',60000))['session'];sel=('--session',session)
                handle=json.loads(rpc('fs-open','/f','r',*sel))['handle'];rpc('fs-restore','base','handle-busy',code=1)
                assert b'handle-busy' not in rpc('fs-revisions');rpc('fs-close',handle,*sel)
                rpc('fs-restore','base','rpc-safety');assert rpc('fs-cat','/f')==b'original'
                assert rpc('fs-cat','/f','--revision','rpc-safety')==b'unsnapped'
                # A read-only revision mount does not lease the mutable World.
                rpc('inode-root','--revision','base',*sel);rpc('fs-restore','safety','revision-mounted')
                assert rpc('fs-cat','/f')==b'unsnapped';rpc('session-close',session)
            finally:
                proc.terminate();proc.wait(timeout=10)
            run('gc',db)
        run('fs-check',db);run('fs-check',db,'--revision','safety');assert run('fs-cat',db,'/f')==b'unsnapped'
print('restore CLI/RPC, safety ancestry, undo, World isolation, mount/handle guards, crash atomicity, restart, GC and legacy compatibility passed')
