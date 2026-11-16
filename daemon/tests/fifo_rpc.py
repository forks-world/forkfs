"""FIFO RPC creation, type fidelity and typed regular-file rejection."""
import errno,json,os,socket,struct,subprocess,sys,tempfile,time
BIN=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='ff-fifo-rpc-',dir='/tmp') as tmp:
    db=os.path.join(tmp,'db');endpoint=os.path.join(tmp,'rpc.sock')
    def cli(*args):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert p.returncode==0,(args,p.stderr);return p.stdout
    cli('init',db);cli('fs-init',db)
    cli('fs-mkfifo',db,'/cli-pipe','640');assert json.loads(cli('fs-stat',db,'/cli-pipe'))['mode']==0o640
    cli('fs-mkfifo',db,'/cli-default');assert json.loads(cli('fs-stat',db,'/cli-default'))['mode']==0o644
    for bad in ('888','620x','-1'):
        p=subprocess.run([BIN,'fs-mkfifo',db,'/bad-'+bad,bad],capture_output=True,timeout=10)
        assert p.returncode!=0,(bad,p.stdout,p.stderr)
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
        # Public fs-mkfifo takes an optional octal mode; stat publishes FIFO type.
        rpc('fs-mkfifo','/pipe','620');info=obj('fs-lstat','/pipe')
        assert info['fifo'] is True and info['directory'] is False and info['symlink'] is False and info['mode']==0o620 and info['size']==0
        before=obj('status')['last_committed_seq']
        for bad in ('888','620x','-1'):
            rpc('fs-mkfifo','/bad-'+bad,bad,error=0)
        assert obj('status')['last_committed_seq']==before
        for command in [('fs-write','/pipe',b'x'),('fs-save','/pipe',b'x'),('fs-read-at','/pipe','0','1'),('fs-truncate','/pipe','0'),('fs-punch-hole','/pipe','0','1')]:
            rpc(*command,error=errno.ENOTSUP)
        rpc('fs-seek-region','/pipe','0','data',error=errno.ESPIPE)
        assert obj('fs-lstat','/pipe')['inode_id']==info['inode_id'] and obj('status')['last_committed_seq']==before
        rpc('fs-mkfifo','/default');assert obj('fs-lstat','/default')['mode']==0o644
        session=obj('session-open','60000')['session'];sel=['--session',session]
        rootinfo=obj('inode-root',*sel);rid=rootinfo['inode']['inode_id']
        inodepipe=obj('inode-mkfifo',rid,'inode-pipe',str(0o600),*sel)
        assert inodepipe['fifo'] and inodepipe['mode']==0o600
        rpc('inode-xattr-set',inodepipe['inode_id'],'user.keep','create',b'v',*sel)
        pin=obj('inode-open',inodepipe['inode_id'],'r',*sel)['handle'];duplicate=obj('fs-dup',pin,*sel)['handle']
        rpc('fs-hseek',pin,'0','set',*sel,error=errno.ESPIPE)
        rpc('fs-hread',pin,'0','0',*sel,error=errno.ENOTSUP)
        rpc('inode-setattr',inodepipe['inode_id'],'-','0','-',pin,*sel,error=errno.EINVAL)
        rpc('fs-write','/replacement',b'file',*sel);rpc('fs-rename','/replacement','/inode-pipe',*sel)
        assert obj('fs-hstat',pin,*sel)['fifo'] and obj('fs-hstat',pin,*sel)['links']==0
        assert rpc('fs-hxattr-get',pin,'user.keep',*sel)==b'v'
        rpc('fs-hchmod',pin,'640',*sel);assert obj('fs-hstat',pin,*sel)['mode']==0o640
        rpc('fs-close',pin,*sel);assert obj('fs-hstat',duplicate,*sel)['links']==0
        rpc('fs-close',duplicate,*sel);rpc('fs-unlink','/inode-pipe',*sel)
        obj('fs-check');rpc('session-close',session)
        print('FIFO RPC create modes, type publication and typed regular-I/O guards passed')
    finally:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
