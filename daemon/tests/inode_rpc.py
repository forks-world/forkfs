"""Inode RPC mount leases, identity after rename, orphan I/O and atomic attributes."""
import errno
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
BIN=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='ff-inode-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'db';endpoint=root/'control.sock'
    def cli(*args):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert p.returncode==0,(args,p.stderr)
        return p.stdout
    cli('init',db);cli('fs-init',db);cli('fs-snapshot',db,'base')
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def raw(fields):
        fields=[x if isinstance(x,bytes) else str(x).encode() for x in fields]
        body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(x))+x for x in fields)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(15);sock.connect(str(endpoint));sock.sendall(b'FFRPC001'+struct.pack('<I',len(body))+body)
            def read(n):
                result=b''
                while len(result)<n:
                    data=sock.recv(n-len(result));assert data,'truncated reply';result+=data
                return result
            header=read(12);assert header[:8]==b'FFRSP001';payload=read(struct.unpack('<I',header[8:])[0])
            return struct.unpack('<I',payload[:4])[0],payload[4:]
    def rpc(*fields,error=None):
        code,value=raw(fields)
        assert code==(0 if error is None else error+1),(fields,code,value,error)
        return value
    def obj(*fields,**kw):
        data=rpc(*fields,**kw);return json.loads(data) if data else None
    try:
        end=time.monotonic()+10
        while True:
            assert proc.poll() is None,proc.communicate()
            try:
                if raw(['status'])[0]==0:break
            except (OSError,AssertionError):pass
            assert time.monotonic()<end;time.sleep(.01)
        session=obj('session-open','60000')['session'];sel=['--session',session]
        info=obj('inode-root',*sel);rid=info['inode']['inode_id'];assert info['repository']
        assert obj('inode-space',*sel)['block_size']>0
        other=obj('session-open','60000')['session']
        rpc('inode-root','--session',other,error=errno.EBUSY)
        rpc('fs-mkdir','/foreign',error=errno.EBUSY)
        directory=obj('inode-mkdir',rid,'dir',str(0o700),*sel);did=directory['inode_id'];assert directory['mode']==0o700
        handle=obj('inode-create',rid,'file',str(0o600),*sel)['handle']
        st=obj('fs-hstat',handle,*sel);fid=st['inode_id'];assert st['mode']==0o600
        readonly=obj('inode-open',fid,'r',*sel)['handle']
        rpc('inode-setattr',fid,'-','1','-',readonly,*sel,error=errno.EBADF)
        rpc('fs-close',readonly,*sel)
        rpc('fs-hwrite',handle,'0',b'abc\x00def',*sel)
        assert rpc('fs-hread',handle,'0','99',*sel)==b'abc\x00def'
        rpc('inode-lookup',rid,'absent',*sel,error=errno.ENOENT)
        rpc('inode-rename',rid,'file',did,'moved',fid,'-',*sel)
        assert obj('inode-stat',fid,*sel)['inode_id']==fid
        assert obj('inode-lookup',did,'moved',*sel)['inode_id']==fid
        obj('inode-link',fid,rid,'alias',*sel)
        assert obj('inode-stat',fid,*sel)['links']==2
        top=obj('inode-mkdir',rid,'x',str(0o700),*sel);topid=top['inode_id'];deep_parent=topid
        component='d'*240;depth=16
        for _ in range(depth):
            deep_parent=obj('inode-mkdir',deep_parent,component,str(0o700),*sel)['inode_id']
        old_path_length=2+depth*(1+len(component))
        new_path_length=1+250+depth*(1+len(component))
        assert old_path_length<=4096<new_path_length,(old_path_length,new_path_length)
        deep_file=obj('inode-create',deep_parent,'leaf',str(0o600),*sel);deep_id=obj('fs-hstat',deep_file['handle'],*sel)['inode_id']
        rpc('fs-close',deep_file['handle'],*sel)
        obj('inode-link',deep_id,rid,'deep-alias',*sel)
        rpc('inode-rename',rid,'x',rid,'z'*250,topid,'-',*sel)
        rpc('inode-stat',deep_parent,*sel,error=errno.ENAMETOOLONG)
        assert obj('inode-stat',deep_id,*sel)['inode_id']==deep_id
        assert obj('inode-stat',fid,*sel)['inode_id']==fid
        short_read=obj('inode-open',fid,'r',*sel)['handle'];rpc('fs-close',short_read,*sel)
        before=obj('status')['last_committed_seq']
        rpc('inode-setattr',fid,str(0o777),str((1<<40)+1),'0',handle,*sel,error=errno.EFBIG)
        assert obj('status')['last_committed_seq']==before
        assert obj('fs-hstat',handle,*sel)['mode']==0o600
        st=obj('inode-setattr',fid,str(0o701),'10','0',handle,*sel)
        assert (st['mode'],st['size'],st['modified_ns'])==(0o701,10,0)
        assert rpc('fs-hread',handle,'0','99',*sel)==b'abc\x00def\0\0\0'
        rpc('inode-remove',did,'moved',rid,'file',*sel,error=errno.ESTALE)
        rpc('inode-remove',did,'moved',fid,'file',*sel)
        rpc('inode-remove',rid,'alias',fid,'file',*sel)
        assert obj('fs-hstat',handle,*sel)['links']==0
        rpc('fs-hwrite',handle,'0',b'X',*sel)
        st=obj('inode-setattr',fid,str(0o700),'4','1',handle,*sel);assert st['size']==4
        assert rpc('fs-hread',handle,'0','99',*sel)==b'Xbc\x00'
        rpc('inode-stat',fid,*sel,error=errno.ESTALE)
        rpc('fs-close',handle,*sel)
        link=obj('inode-symlink',rid,'link','dir',*sel)
        assert rpc('inode-readlink',link['inode_id'],*sel)==b'dir'
        cursor=obj('inode-dir-open',rid,*sel)['cursor']
        page=obj('fs-dir-entries',cursor,'100',*sel);assert page['eof'] and {x['name'] for x in page['entries']}=={'dir','link','deep-alias','z'*250}
        rpc('fs-dir-close',cursor,*sel)
        ro=obj('session-open','60000')['session'];ro_sel=['--revision','base','--session',ro]
        ro_root=obj('inode-root',*ro_sel)['inode']['inode_id']
        rpc('inode-mkdir',ro_root,'forbidden','493',*ro_sel,error=errno.EROFS)
        rpc('inode-stat',rid,'--world','elsewhere',*sel,error=errno.ESTALE)
        rpc('fs-snapshot','mounted-checkpoint');rpc('fs-check')
        rpc('session-close',session);rpc('fs-mkdir','/released')
        rpc('session-close',other);rpc('session-close',ro)
        if len(sys.argv)>2:
            p=subprocess.run([sys.executable,sys.argv[2],BIN,str(root),'--world','main'],capture_output=True,timeout=15)
            assert p.returncode==0,p.stderr
            descriptor=json.loads((root/'forkfs.json').read_text())
            assert descriptor=={'schema_version':1,'repository':info['repository'],'world':'main'}
            assert (root/'forkfs.json').stat().st_mode&0o077==0
            before=(root/'forkfs.json').read_bytes()
            p=subprocess.run([sys.executable,sys.argv[2],BIN,str(root),'--world','absent'],capture_output=True,timeout=15)
            assert p.returncode!=0 and (root/'forkfs.json').read_bytes()==before
        proc.terminate();proc.wait(timeout=10);assert proc.returncode==0,proc.communicate()
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=10)
    cli('fs-check',db);cli('fs-check',db,'--revision','base');cli('gc',db);cli('fs-check',db)
print('inode mount RPC, exclusive World lease, typed errors, rename/hardlink/orphan I/O and atomic setattr passed')
