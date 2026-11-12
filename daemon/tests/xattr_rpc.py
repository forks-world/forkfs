"""Binary xattr CLI/RPC, inode errors, leases and revision isolation."""
import errno
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
BIN,LEGACY=sys.argv[1:3]
MISSING=getattr(errno,'ENOATTR',errno.ENODATA)
for binary in [BIN,LEGACY]:
    def run(*args,ok=True):
        p=subprocess.run([binary,*map(str,args)],capture_output=True,timeout=20);assert (p.returncode==0)==ok,(args,p.stderr);return p.stdout
    with tempfile.TemporaryDirectory(prefix='ff-attr-',dir='/tmp') as tmp:
        root=Path(tmp);db=root/'db';data=root/'input';endpoint=root/'control.sock';payload=b'x\0\xffy'
        run('init',db);run('fs-init',db);data.write_bytes(b'content');run('fs-write',db,'/f',data);data.write_bytes(payload)
        run('fs-xattr-set',db,'/f','user.test','create',data);assert run('fs-xattr-get',db,'/f','user.test')==payload
        assert run('fs-xattr-list',db,'/f')==b'user.test\n';run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
        data.write_bytes(b'work');run('fs-xattr-set',db,'/f','user.test','replace',data,'--world','work')
        assert run('fs-xattr-get',db,'/f','user.test','--world','work')==b'work'
        assert run('fs-xattr-get',db,'/f','user.test','--revision','base')==payload
        before=json.loads(run('check',db))['last_committed_seq']
        run('fs-xattr-set',db,'/f','user.test','create',data,ok=False);run('fs-xattr-set',db,'/f','absent','replace',data,ok=False)
        run('fs-xattr-set',db,'/f','name','unknown',data,ok=False);run('fs-xattr-remove',db,'/f','absent',ok=False)
        run('fs-xattr-remove',db,'/f','user.test','--revision','base',ok=False)
        assert json.loads(run('check',db))['last_committed_seq']==before
        if binary==BIN:
            proc=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            def exact(s,n):
                result=b''
                while len(result)<n:
                    part=s.recv(n-len(result));assert part;result+=part
                return result
            def rpc(*args,code=0):
                fields=[v if isinstance(v,bytes) else str(v).encode() for v in args];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload);head=exact(s,12);assert head[:8]==b'FFRSP001'
                    body=exact(s,struct.unpack('<I',head[8:])[0]);assert struct.unpack('<I',body[:4])[0]==code,(args[:2],body);return body[4:]
            try:
                assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
                session=json.loads(rpc('session-open',60000))['session'];sel=('--session',session)
                rid=json.loads(rpc('inode-root',*sel))['inode']['inode_id'];fid=json.loads(rpc('inode-lookup',rid,'f',*sel))['inode_id']
                rpc('inode-xattr-set',fid,'binary','create',payload,*sel);assert rpc('inode-xattr-get',fid,'binary',*sel)==payload
                assert json.loads(rpc('inode-xattr-list',fid,*sel))['names']==['binary','user.test']
                escaped='quote"back\\name'
                rpc('inode-xattr-set',fid,escaped,'create',payload,*sel)
                assert escaped in json.loads(rpc('inode-xattr-list',fid,*sel))['names']
                assert rpc('inode-xattr-get',fid,escaped,*sel)==payload
                rpc('inode-xattr-set',fid,'binary','create',b'',*sel,code=errno.EEXIST+1)
                rpc('inode-xattr-set',fid,'missing','replace',b'',*sel,code=MISSING+1)
                rpc('inode-xattr-get',fid,'missing',*sel,code=MISSING+1)
                large=(bytes(range(256))*1024)[:-1]
                rpc('inode-xattr-set',fid,'big','create',large,*sel)
                assert rpc('inode-xattr-get',fid,'big',*sel)==large
                rpc('inode-xattr-set',fid,'big','upsert',bytes(262144),*sel,code=errno.E2BIG+1)
                assert rpc('inode-xattr-get',fid,'big',*sel)==large
                rpc('inode-xattr-set',fid,'n'*256,'upsert',b'',*sel,code=errno.ENAMETOOLONG+1)
                rpc('inode-xattr-set',fid,b'bad\xff','upsert',b'',*sel,code=errno.EINVAL+1)
                rpc('fs-xattr-set','/f','foreign','upsert',b'',code=errno.EBUSY+1)
                names=json.loads(rpc('inode-xattr-list',fid,*sel))['names']
                for i in range(128-len(names)):
                    rpc('inode-xattr-set',fid,'count'+str(i),'create',b'',*sel)
                rpc('inode-xattr-set',fid,'count-overflow','create',b'',*sel,code=errno.ENOSPC+1)
                assert len(json.loads(rpc('inode-xattr-list',fid,*sel))['names'])==128
                rpc('inode-xattr-remove',fid,'count0',*sel)
                handle=json.loads(rpc('inode-open',fid,'r',*sel))['handle'];rpc('inode-remove',rid,'f',fid,'file',*sel)
                assert rpc('fs-hxattr-get',handle,'binary',*sel)==payload
                assert rpc('fs-hxattr-get',handle,'big',*sel)==large
                rpc('fs-hxattr-set',handle,'big','replace',large[::-1],*sel)
                assert rpc('fs-hxattr-get',handle,'big',*sel)==large[::-1]
                rpc('fs-hxattr-set',handle,'orphan','upsert',b'',*sel);assert rpc('fs-hxattr-get',handle,'orphan',*sel)==b''
                rpc('fs-hxattr-remove',handle,'binary',*sel);rpc('fs-close',handle,*sel)
                rpc('session-close',session)
            finally:
                proc.terminate();proc.wait(timeout=10)
            run('gc',db)
        else:
            run('fs-xattr-remove',db,'/f','user.test');assert run('fs-xattr-list',db,'/f')==b''
        run('fs-check',db);run('fs-check',db,'--world','work');run('fs-check',db,'--revision','base')
        assert run('fs-xattr-get',db,'/f','user.test','--revision','base')==payload
print('xattr CLI, legacy compatibility, binary RPC, typed inode errors, policies, mount leases, orphan metadata, revisions and GC passed')
