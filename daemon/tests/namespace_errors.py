"""Namespace failures return operation-specific errno without publishing state."""
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
UNLINK_DIR=errno.EPERM if sys.platform=='darwin' else errno.EISDIR
with tempfile.TemporaryDirectory(prefix='ff-posix-errors-',dir='/tmp') as tmp:
    root=Path(tmp)
    def command(binary,*args,ok=True):
        p=subprocess.run([binary,*map(str,args)],capture_output=True,timeout=20)
        assert (p.returncode==0)==ok,(args,p.stderr)
        return p.stdout
    def prepare(binary,db):
        command(binary,'init',db);command(binary,'fs-init',db);data=root/'input';data.write_bytes(b'original')
        for name in ['file-src','file-dst']:command(binary,'fs-write',db,'/'+name,data)
        for name in ['dir-src','dir-src/child','dir-dst','nonempty']:command(binary,'fs-mkdir',db,'/'+name)
        command(binary,'fs-write',db,'/nonempty/file',data)
        command(binary,'fs-symlink',db,'/dir-dst','/dir-link');command(binary,'fs-snapshot',db,'base')
    db=root/'rpc';prepare(BIN,db);endpoint=root/'control.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def exact(s,n):
        b=b''
        while len(b)<n:
            v=s.recv(n-len(b));assert v;b+=v
        return b
    def rpc(*args,code=0):
        fields=[str(v).encode() for v in args];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload);h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0]);assert struct.unpack('<I',b[:4])[0]==code,(args,b,code);return b[4:]
    def obj(*args):return json.loads(rpc(*args))
    def stable_state():
        status=obj('status');result={k:status[k] for k in ['last_committed_seq','indexed_objects','object_roots']}
        result['entries']=rpc('fs-ls','/');result['attrs']={p:obj('fs-lstat',p) for p in ['/','/file-src','/file-dst','/dir-src','/dir-src/child','/dir-dst','/nonempty','/nonempty/file','/dir-link']}
        result['contents']=[rpc('fs-cat',p) for p in ['/file-src','/file-dst','/nonempty/file']]
        result['revisions']=rpc('fs-revisions');return result
    try:
        assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
        session=obj('session-open',60000)['session'];sel=('--session',session);rid=obj('inode-root',*sel)['inode']['inode_id']
        ids={name:obj('inode-lookup',rid,name,*sel)['inode_id'] for name in ['file-src','file-dst','dir-src','dir-dst','nonempty','dir-link']}
        def rename(src,dst):return ('inode-rename',rid,src,rid,dst,ids[src],ids[dst])
        cases=[(rename('file-src','dir-dst'),errno.EISDIR),
               (rename('dir-link','dir-dst'),errno.EISDIR),
               (rename('dir-src','file-dst'),errno.ENOTDIR),
               (rename('dir-src','dir-link'),errno.ENOTDIR),
               (rename('dir-src','nonempty'),errno.ENOTEMPTY),
               (('inode-remove',rid,'file-src',ids['file-src'],'directory'),errno.ENOTDIR),
               (('inode-remove',rid,'dir-link',ids['dir-link'],'directory'),errno.ENOTDIR),
               (('inode-remove',rid,'dir-src',ids['dir-src'],'file'),UNLINK_DIR),
               (('inode-remove',rid,'nonempty',ids['nonempty'],'directory'),errno.ENOTEMPTY),
               (('inode-link',ids['dir-src'],rid,'forbidden-link'),errno.EPERM),
               (('inode-link',rid,rid,'forbidden-root-link'),errno.EPERM),
               (('inode-link',ids['file-src'],rid,'file-dst'),errno.EEXIST),
               (('inode-rename',rid,'absent',rid,'new',ids['file-src'],'-'),errno.ENOENT),
               (('inode-rename',rid,'file-src',rid,'dir-dst',ids['file-dst'],ids['dir-dst']),errno.ESTALE),
               (('inode-rename',rid,'file-src',ids['file-dst'],'child',ids['file-src'],'-'),errno.ENOTDIR),
               (('inode-rename',rid,'dir-src',ids['dir-src'],'inside',ids['dir-src'],'-'),errno.EINVAL)]
        before=stable_state()
        for args,error in cases:
            rpc(*args,*sel,code=error+1);assert stable_state()==before,(args,error)
        rpc('fs-check');rpc('fs-check','--revision','base')
        # A destination symlink pointing to a directory is still replaceable by
        # a non-directory; checking its target type would reject this valid case.
        rpc(*rename('file-src','dir-link'),*sel)
        assert not obj('fs-lstat','/dir-link')['symlink'] and rpc('fs-cat','/dir-link')==b'original'
        assert obj('fs-stat','/dir-dst')['directory'];rpc('fs-check');rpc('session-close',session)
    finally:
        proc.terminate();proc.wait(timeout=10)
    command(BIN,'gc',db);command(BIN,'fs-check',db);command(BIN,'fs-check',db,'--revision','base')
    # Offline CLI keeps its generic process exit code, with the same no-commit
    # guarantees and namespace validation on both storage backends.
    for index,binary in enumerate([BIN,LEGACY]):
        db=root/('offline'+str(index));prepare(binary,db)
        before=json.loads(command(binary,'check',db))['last_committed_seq']
        for op,args in [('fs-rename',('/file-src','/dir-dst')),('fs-rename',('/dir-src','/file-dst')),('fs-unlink',('/dir-src',)),('fs-rmdir',('/file-src',)),('fs-link',('/dir-src','/forbidden-link'))]:command(binary,op,db,*args,ok=False)
        assert json.loads(command(binary,'check',db))['last_committed_seq']==before
        assert command(binary,'fs-cat',db,'/file-src')==b'original';command(binary,'fs-check',db)
print('POSIX namespace errno, stale guards, failed-operation atomicity, nofollow destination semantics, CLI compatibility, restart and GC passed')
