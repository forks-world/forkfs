"""Directory inode metadata pins retain identity and xattrs after path removal."""
import errno, json, socket, struct, subprocess, sys, tempfile, time
from pathlib import Path
BIN=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='ff-dirhandle-rpc-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'db';endpoint=root/'rpc.sock'
    def cli(*args):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=20);assert p.returncode==0,(args,p.stderr);return p.stdout
    xattr_file=root/'xattr';xattr_file.write_bytes(b'old')
    cli('init',db);cli('fs-init',db);cli('fs-mkdir',db,'/pinned');cli('fs-xattr-set',db,'/pinned','user.old','create',xattr_file);cli('fs-snapshot',db,'base')
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def raw(fields):
        fields=[x if isinstance(x,bytes) else str(x).encode() for x in fields];body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(x))+x for x in fields)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(body))+body)
            def read(n):
                result=b''
                while len(result)<n:
                    b=s.recv(n-len(result));assert b,'truncated RPC reply';result+=b
                return result
            h=read(12);assert h[:8]==b'FFRSP001';payload=read(struct.unpack('<I',h[8:])[0]);return struct.unpack('<I',payload[:4])[0],payload[4:]
    def rpc(*args,error=None):
        code,value=raw(args);assert code==(0 if error is None else error+1),(args,code,value,error);return value
    def obj(*args,**kw):
        value=rpc(*args,**kw);return json.loads(value) if value else None
    try:
        deadline=time.monotonic()+10
        while True:
            if proc.poll() is not None:raise RuntimeError(proc.communicate())
            try:
                if raw(['status'])[0]==0:break
            except (OSError,AssertionError):pass
            assert time.monotonic()<deadline;time.sleep(.01)
        session=obj('session-open','60000')['session'];sel=('--session',session);rid=obj('inode-root',*sel)['inode']['inode_id'];did=obj('inode-lookup',rid,'pinned',*sel)['inode_id']
        rpc('fs-open','/pinned','r',*sel,error=errno.EISDIR)
        h=obj('inode-open',did,'r',*sel)['handle'];dup=obj('fs-dup',h,*sel)['handle'];other=obj('session-open','60000')['session']
        rpc('fs-hstat',h,'--session',other,error=errno.EBADF)
        rpc('fs-hmkdir',h,'foreign',str(0o755),'--session',other,error=errno.EBADF)
        rpc('fs-hmkdir',h,'wrong-view',str(0o755),'--revision','base',*sel,error=errno.EBADF)
        rpc('fs-hread',h,'0','0',*sel,error=errno.EISDIR)
        # Handle-aware mkdir follows the open directory across rename, even when
        # its former pathname is reused by a different directory.
        obj('fs-mkdir','/mkdir-parent',*sel);parent_id=obj('inode-lookup',rid,'mkdir-parent',*sel)['inode_id']
        mkdir_handle=obj('inode-open',parent_id,'r',*sel)['handle'];mkdir_dup=obj('fs-dup',mkdir_handle,*sel)['handle']
        rpc('fs-rename','/mkdir-parent','/mkdir-moved',*sel);obj('fs-mkdir','/mkdir-parent',*sel)
        child=obj('fs-hmkdir',mkdir_handle,'child',str(0o711),*sel)['inode_id']
        assert obj('inode-lookup',parent_id,'child',*sel)['inode_id']==child
        assert rpc('fs-ls','/mkdir-parent',*sel).splitlines()==[]
        rpc('fs-rmdir','/mkdir-moved/child',*sel);rpc('fs-rmdir','/mkdir-moved',*sel);obj('fs-mkdir','/mkdir-moved',*sel)
        reused=obj('fs-lstat','/mkdir-moved',*sel)['inode_id'];before_deleted=obj('status')
        rpc('fs-hmkdir',mkdir_handle,'orphan',str(0o755),*sel,error=errno.ENOENT)
        rpc('fs-hmkdir',mkdir_dup,'orphan',str(0o755),*sel,error=errno.ENOENT)
        rpc('inode-mkdir',parent_id,'orphan',str(0o755),*sel,error=errno.ESTALE)
        rpc('inode-mkdir','f'*32,'unknown',str(0o755),*sel,error=errno.ESTALE)
        after_deleted=obj('status')
        assert after_deleted['last_committed_seq']==before_deleted['last_committed_seq']
        assert after_deleted['object_roots']==before_deleted['object_roots']
        assert obj('fs-lstat','/mkdir-moved',*sel)['inode_id']==reused and rpc('fs-ls','/mkdir-moved',*sel).splitlines()==[]
        rpc('fs-close',mkdir_handle,*sel);rpc('fs-hmkdir',mkdir_handle,'closed',str(0o755),*sel,error=errno.EBADF)
        rpc('fs-hmkdir',mkdir_dup,'still-open',str(0o701),*sel,error=errno.ENOENT);rpc('fs-close',mkdir_dup,*sel)
        file_handle=obj('fs-open','/not-directory','rw,create',*sel)['handle']
        rpc('fs-hmkdir',file_handle,'child',str(0o755),*sel,error=errno.ENOTDIR);rpc('fs-close',file_handle,*sel)
        before=obj('status')['last_committed_seq'];rpc('inode-remove',rid,'pinned',did,'directory',*sel)
        obj('inode-mkdir',rid,'pinned',str(0o755),*sel);new_id=obj('inode-lookup',rid,'pinned',*sel)['inode_id'];assert new_id!=did
        assert obj('fs-hstat',h,*sel)['inode_id']==did and obj('fs-hstat',h,*sel)['links']==2
        assert rpc('fs-hxattr-get',h,'user.old',*sel)==b'old'
        rpc('fs-hxattr-set',dup,'user.old','replace',b'old-inode',*sel);rpc('fs-hchmod',h,format(0o701,'o'),*sel)
        assert obj('fs-hstat',dup,*sel)['mode']==0o701 and rpc('fs-hxattr-get',dup,'user.old',*sel)==b'old-inode'
        assert obj('fs-xattr-list','/pinned',*sel)['names']==[]
        obj('fs-mkdir','/source',*sel);obj('fs-mkdir','/replace',*sel);rpc('fs-xattr-set','/replace','user.replace','create',b'retained',*sel)
        old_replace=obj('inode-lookup',rid,'replace',*sel)['inode_id'];replace_pin=obj('inode-open',old_replace,'r',*sel)['handle'];source_id=obj('inode-lookup',rid,'source',*sel)['inode_id']
        rpc('fs-rename','/source','/replace',*sel);assert obj('fs-lstat','/replace',*sel)['inode_id']==source_id
        assert obj('fs-hstat',replace_pin,*sel)['inode_id']==old_replace and obj('fs-hstat',replace_pin,*sel)['links']==2
        assert rpc('fs-hxattr-get',replace_pin,'user.replace',*sel)==b'retained';rpc('fs-close',replace_pin,*sel)
        base=obj('session-open','60000')['session'];base_sel=('--revision','base','--session',base);base_root=obj('inode-root',*base_sel)['inode']['inode_id'];base_dir=obj('inode-lookup',base_root,'pinned',*base_sel)['inode_id'];ro=obj('inode-open',base_dir,'r',*base_sel)['handle']
        rpc('fs-hmkdir',ro,'child',str(0o755),*base_sel,error=errno.EROFS)
        rpc('inode-xattr-set',base_dir,'user.old','replace',b'x',*base_sel,error=errno.EROFS);assert rpc('fs-hxattr-get',ro,'user.old',*base_sel)==b'old'
        rpc('fs-close',ro,*base_sel);rpc('session-close',base)
        rpc('fs-close',h,*sel);rpc('fs-hstat',h,*sel,error=errno.EBADF);assert obj('fs-hstat',dup,*sel)['inode_id']==did;rpc('fs-close',dup,*sel);rpc('fs-check');rpc('session-close',other);rpc('session-close',session)
        expired_before=obj('status');expired_code,expired_reply=raw(['fs-hmkdir',h,'expired-session',str(0o755),'--session',session]);expired_after=obj('status')
        assert expired_code==1 and expired_after['last_committed_seq']==expired_before['last_committed_seq'] and expired_after['object_roots']==expired_before['object_roots']
    finally:
        proc.terminate();proc.wait(timeout=10)
    cli('fs-check',db);cli('gc',db);cli('fs-check',db)
print('directory metadata RPC pins retain old inode identity/xattrs, enforce ownership and readonly revisions, and reclaim on last close')
