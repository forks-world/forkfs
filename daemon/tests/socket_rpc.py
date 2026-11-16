"""Socket pathname metadata RPC, type publication and regular-I/O rejection."""
import errno,json,os,socket,struct,subprocess,sys,tempfile,time
BIN=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='ff-socket-rpc-',dir='/tmp') as tmp:
    db=os.path.join(tmp,'db');endpoint=os.path.join(tmp,'rpc.sock')
    def cli(*args):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15);assert p.returncode==0,(args,p.stderr);return p.stdout
    cli('init',db);cli('fs-init',db)
    cli('fs-mksocket',db,'/cli','600');assert json.loads(cli('fs-stat',db,'/cli'))['socket']
    cli('fs-mksocket',db,'/default');assert json.loads(cli('fs-stat',db,'/default'))['mode']==0o777
    proc=subprocess.Popen([BIN,'serve',db,'--socket',endpoint,'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def raw(fields):
        fields=[x if isinstance(x,bytes) else str(x).encode() for x in fields]
        body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(x))+x for x in fields)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10);sock.connect(endpoint);sock.sendall(b'FFRPC001'+struct.pack('<I',len(body))+body)
            def read(n):
                value=b''
                while len(value)<n:
                    block=sock.recv(n-len(value));assert block,'truncated RPC response';value+=block
                return value
            header=read(12);assert header[:8]==b'FFRSP001';payload=read(struct.unpack('<I',header[8:])[0])
            return struct.unpack('<I',payload[:4])[0],payload[4:]
    def rpc(*fields,error=None):
        code,value=raw(fields);assert code==(0 if error is None else error+1),(fields,code,value,error);return value
    def obj(*fields,**kw):
        value=rpc(*fields,**kw);return json.loads(value) if value else None
    try:
        end=time.monotonic()+8
        while True:
            assert proc.poll() is None,proc.communicate()
            try:
                if raw(['status'])[0]==0:break
            except (OSError,AssertionError):pass
            assert time.monotonic()<end;time.sleep(.01)
        node=obj('fs-lstat','/cli');assert node['socket'] and not node['fifo'] and not node['symlink'] and node['size']==0 and node['allocated_size']==0
        session=obj('session-open','60000')['session'];sel=['--session',session]
        root=obj('inode-root',*sel);rootid=root['inode']['inode_id']
        created=obj('inode-mksocket',rootid,'rpc-socket',str(0o600),*sel);assert created['socket'] and created['mode']==0o600
        rpc('inode-mksocket',rootid,'decimal-mode',str(0o777),*sel);assert obj('fs-lstat','/decimal-mode',*sel)['mode']==0o777
        for command in [('fs-write','/cli',b'x'),('fs-save','/cli',b'x'),('fs-read-at','/cli','0','1'),('fs-truncate','/cli','0'),('fs-punch-hole','/cli','0','1'),('fs-clone-file','/cli','/copy')]:rpc(*command,*sel,error=errno.ENOTSUP)
        rpc('fs-seek-region','/cli','0','data',*sel,error=errno.ESPIPE)
        rpc('fs-open','/cli','r',*sel,error=errno.ENOTSUP)
        rpc('inode-setattr',node['inode_id'],'-','0','-','-',*sel,error=errno.EINVAL)
        rpc('fs-xattr-set','/cli','user.keep','create',b'v',*sel);rpc('fs-link','/cli','/alias',*sel)
        assert obj('fs-lstat','/alias',*sel)['socket'] and obj('fs-lstat','/alias',*sel)['links']==2
        rpc('fs-rename','/alias','/moved',*sel);assert obj('fs-lstat','/moved',*sel)['socket']
        rpc('fs-snapshot','base',*sel)
        rpc('session-close',session)
        ro=obj('session-open','60000')['session'];rosel=['--revision','base','--session',ro]
        ro_root=obj('inode-root',*rosel);before=obj('status')['last_committed_seq']
        rpc('inode-mksocket',ro_root['inode']['inode_id'],'forbidden','511',*rosel,error=errno.EROFS)
        assert obj('status')['last_committed_seq']==before
        assert obj('inode-stat',created['inode_id'],*rosel)['socket']
        rpc('session-close',ro);rpc('fs-check')
        print('socket RPC creation modes, type publication and typed regular-I/O guards passed')
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
