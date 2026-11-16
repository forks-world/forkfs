"""Trailing slash rmdir/rename semantics through RPC and offline CLI."""
import errno, json, select, socket, struct, subprocess, sys, tempfile
from pathlib import Path

BIN=sys.argv[1]
def frame(fields):
    fields=[str(v).encode() for v in fields]
    body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
    return b'FFRPC001'+struct.pack('<I',len(body))+body
def exact(sock,n):
    out=b''
    while len(out)<n:
        part=sock.recv(n-len(out));assert part,'truncated response';out+=part
    return out
def response(sock):
    h=exact(sock,12);assert h[:8]==b'FFRSP001';body=exact(sock,struct.unpack('<I',h[8:])[0])
    return struct.unpack('<I',body[:4])[0],body[4:]

with tempfile.TemporaryDirectory(prefix='ff-trailing-mutations-',dir='/tmp') as td:
    root=Path(td);db=root/'rpc';endpoint=root/'control.sock'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok,(args,p.stderr.decode(errors='replace'))
        return p.stdout
    cli('init',db);cli('fs-init',db)
    for name in ('remove-target','rename-target','replace-source','replace-target','dangling-source','file','empty','dir'):
        if name in ('file',):
            data=root/'payload';data.write_bytes(b'payload');cli('fs-write',db,'/'+name,data)
        else:cli('fs-mkdir',db,'/'+name)
    for target,alias in (('/remove-target','/remove-alias'),('/rename-target','/rename-alias'),
                         ('/replace-target','/replace-alias'),('/made-target','/dangling-alias'),
                         ('/absent','/bad-alias')):
        cli('fs-symlink',db,target,alias)
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def raw(fields,expect=0):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(15);s.connect(str(endpoint));s.sendall(frame(fields));code,value=response(s)
        assert code==expect,(fields,code,value)
        return value
    def obj(*fields):return json.loads(raw(fields))
    def state():return obj('status')['last_committed_seq']
    try:
        assert select.select([proc.stdout],[],[],10)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
        remove_id=obj('fs-lstat','/remove-alias')['inode_id'];raw(['fs-rmdir','/remove-alias/'])
        assert obj('fs-lstat','/remove-alias')['inode_id']==remove_id and obj('fs-lstat','/remove-alias')['symlink']
        rename_id=obj('fs-stat','/rename-target')['inode_id'];link_id=obj('fs-lstat','/rename-alias')['inode_id']
        raw(['fs-rename','/rename-alias/','/moved'])
        assert obj('fs-lstat','/moved')['inode_id']==rename_id and obj('fs-lstat','/rename-alias')['inode_id']==link_id
        source_id=obj('fs-stat','/replace-source')['inode_id'];replace_link=obj('fs-lstat','/replace-alias')['inode_id']
        raw(['fs-rename','/replace-source','/replace-alias/'])
        assert obj('fs-stat','/replace-target')['inode_id']==source_id and obj('fs-lstat','/replace-alias')['inode_id']==replace_link
        dangling_source=obj('fs-stat','/dangling-source')['inode_id'];dangling_link=obj('fs-lstat','/dangling-alias')['inode_id']
        raw(['fs-rename','/dangling-source','/dangling-alias/'])
        assert obj('fs-stat','/made-target')['inode_id']==dangling_source and obj('fs-lstat','/dangling-alias')['inode_id']==dangling_link
        before=state()
        for args,code in [(['fs-rmdir','/file/'],errno.ENOTDIR+1),(['fs-rmdir','/bad-alias/'],errno.ENOENT+1),
                          (['fs-rename','/file/','/unused'],errno.ENOTDIR+1),
                          (['fs-rename','/missing-source','/bad-alias/'],errno.ENOENT+1),
                          (['fs-rename','/file','/bad-alias/'],errno.ENOENT+1),
                          (['fs-rename-excl','/file','/empty/'],errno.EEXIST+1)]:
            raw(args,code);assert state()==before,(args,state(),before)
        raw(['fs-check'])
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    # Offline CLI uses the same Namespace mutation path.
    cli('fs-mkdir',db,'/offline-target');cli('fs-symlink',db,'/offline-target','/offline-alias')
    cli('fs-rmdir',db,'/offline-alias/');assert json.loads(cli('fs-lstat',db,'/offline-alias'))['symlink']
    cli('fs-mkdir',db,'/offline-source');cli('fs-symlink',db,'/offline-missing','/offline-dangling')
    cli('fs-rename',db,'/offline-source','/offline-dangling/')
    assert json.loads(cli('fs-stat',db,'/offline-missing'))['directory']
    cli('fs-check',db)
    print(json.dumps({'rpc':'passed','offline_cli':'passed','typed_errors_atomic':True},sort_keys=True))
