"""Nofollow attributes update an entry without traversing its final symlink."""
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
    def run(*args,ok=True):
        p=subprocess.run([binary,*map(str,args)],capture_output=True,timeout=20)
        assert (p.returncode==0)==ok,(args,p.stderr)
        return p.stdout
    with tempfile.TemporaryDirectory(prefix='ff-linkattrs-',dir='/tmp') as tmp:
        root=Path(tmp);db=root/'db';data=root/'input';data.write_bytes(b'target')
        run('init',db);run('fs-init',db);run('fs-write',db,'/file',data);run('fs-settimes',db,'/file',1,2,3)
        run('fs-mkdir',db,'/real');run('fs-symlink',db,'/real','/prefix')
        links={'/link':'/file','/dangling':'/absent','/cycle':'/cycle','/prefix/relative':'../file'}
        for path,target in links.items():run('fs-symlink',db,target,path)
        run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
        def attrs(path,*view):return json.loads(run('fs-lstat',db,path,*view))
        def times(path,*view):return [attrs(path,*view)[k] for k in ['accessed_ns','modified_ns','birth_ns']]
        def sequence():return json.loads(run('check',db))['last_committed_seq']
        target=attrs('/file');before=sequence()
        for path,literal in links.items():
            run('fs-lsettimes',db,path,111,222,333);run('fs-lchmod',db,path,'0701')
            assert times(path)==[111,222,333] and attrs(path)['mode']==0o701
            assert run('fs-readlink',db,path)==literal.encode() and attrs('/file')==target
        assert sequence()==before+2*len(links)
        # Ordinary setters retain follow-final behavior; nofollow also works on
        # regular files. Prefix symlinks are still traversed normally.
        run('fs-settimes',db,'/link',444,555,666);assert times('/file')==[444,555,666] and times('/link')==[111,222,333]
        run('fs-lsettimes',db,'/file','-',555,'-');run('fs-settimes',db,'/dangling',1,2,3,ok=False)
        run('fs-snapshot',db,'changed');changes=json.loads(run('fs-diff',db,'base','changed'))['changes']
        change=next(c for c in changes if c['path']=='/link');assert change['metadata_changed'] and not change['content_changed']
        assert times('/link','--revision','base')!=[111,222,333]
        assert times('/link','--world','work')!=[111,222,333]
        run('fs-lsettimes',db,'/link',0,0,0,'--world','work');assert times('/link','--world','work')==[0,0,0]
        assert times('/file','--world','work')==[1,2,3]
        before=sequence();run('fs-lsettimes',db,'/cycle','-','-','-')
        for command,args in [('fs-lchmod',('/link','1000')),('fs-lsettimes',('/link',1,2,9223372036854775808)),('fs-lsettimes',('/missing',1,2,3))]:
            run(command,db,*args,ok=False)
        run('fs-lchmod',db,'/link','700','--revision','base',ok=False)
        run('fs-lsettimes',db,'/link',1,2,3,'--revision','base',ok=False);assert sequence()==before
        run('fs-restore',db,'base','safety');assert attrs('/link')['mode']==0o777 and attrs('/file')==target
        assert times('/link','--revision','safety')==[111,222,333]
        if binary==BIN:
            baseline=root/'baseline';shutil.copytree(db,baseline);old=times('/dangling')
            for stage in ['leveldb_before_write','leveldb_after_write']:
                shutil.rmtree(db);shutil.copytree(baseline,db)
                p=subprocess.run([FAULT,'fs-lsettimes',str(db),'/dangling','7','8','9'],capture_output=True,timeout=20,env={**os.environ,'FORKFS_TEST_CRASH':stage})
                assert p.returncode==86,p.stderr;assert times('/dangling')==([7,8,9] if stage=='leveldb_after_write' else old)
                assert run('fs-readlink',db,'/dangling')==b'/absent';run('fs-check',db)
            endpoint=root/'control.sock';proc=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            def exact(s,n):
                b=b''
                while len(b)<n:
                    v=s.recv(n-len(b));assert v;b+=v
                return b
            def rpc(*args,code=0):
                fields=[str(v).encode() for v in args];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
                    h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0]);assert struct.unpack('<I',b[:4])[0]==code,(args,b);return b[4:]
            try:
                assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
                session=json.loads(rpc('session-open',60000))['session'];sel=('--session',session)
                inode=json.loads(rpc('inode-root',*sel))['inode']['inode_id']
                link=json.loads(rpc('inode-lookup',inode,'dangling',*sel))['inode_id']
                changed=json.loads(rpc('inode-setattr-times',link,0o640,'-',777,888,999,'-',*sel));assert changed['symlink'] and changed['birth_ns']==999
                assert json.loads(rpc('fs-lstat','/file'))==target
                # Even an unchanged symlink length must fail before mode/time
                # fields can mutate. Both inode setattr protocols use nofollow.
                rpc('inode-setattr-times',link,0o777,len('/absent'),1,2,3,'-',*sel,code=errno.EINVAL+1)
                assert json.loads(rpc('inode-stat',link,*sel))==changed
                changed=json.loads(rpc('inode-setattr',link,0o700,'-',123,'-',*sel));assert changed['accessed_ns']==888 and changed['birth_ns']==999 and changed['modified_ns']==123
                rpc('fs-lsettimes','/dangling',1,2,3,code=errno.EBUSY+1)
                rpc('fs-lchmod','/dangling','755',*sel)
                rpc('fs-lsettimes','/dangling',4,5,6,*sel);assert json.loads(rpc('fs-lstat','/dangling'))['birth_ns']==6
                rpc('session-close',session)
                session=json.loads(rpc('session-open',60000))['session'];sel=('--revision','base','--session',session)
                inode=json.loads(rpc('inode-root',*sel))['inode']['inode_id'];link=json.loads(rpc('inode-lookup',inode,'dangling',*sel))['inode_id']
                rpc('inode-setattr-times',link,'-','-',1,2,3,'-',*sel,code=errno.EROFS+1);rpc('session-close',session)
            finally:
                proc.terminate();proc.wait(timeout=10)
            run('gc',db)
        run('fs-check',db);run('fs-check',db,'--revision','base');run('fs-check',db,'--revision','safety')
        assert run('fs-cat',db,'/file')==b'target' and run('fs-readlink',db,'/dangling')==b'/absent'
print('nofollow attributes, dangling/cyclic/prefix links, target isolation, RPC leases, size rejection, revisions, restore, crash atomicity and GC passed')
