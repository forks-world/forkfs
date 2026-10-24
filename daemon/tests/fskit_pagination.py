"""Authenticated fake mount service for the FSKit pinned-pagination regression."""
import json
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

driver=sys.argv[1]
limit=1024*1024
generation='a'*32
session='b'*32
root_id='00000000000000000000000000000001'
entries=[]
for i in range(4097):
    name=f'file-{i:05d}'
    inode={
        'inode_id':f'{i+2:032x}','directory':False,'symlink':False,'mode':420,
        'links':1,'size':0,'modified_ns':0,'changed_ns':0,'uid':0,'gid':0,
        'accessed_ns':0,'created_ns':0,
    }
    entries.append({'name':name,'inode':inode})

class State:
    def __init__(self):
        self.lock=threading.Lock()
        self.counts={'dir_open':0,'page':0,'rewind':0,'dir_close':0,'keepalive':0,'revoke':0}
        self.positions={};self.revoked=False;self.next_cursor=0
    def handle(self,fields):
        op=fields[0]
        if op=='service-hello':return {'protocol':1,'generation':generation}
        if op in ('mount-keepalive','mount-revoke'):
            if fields[1]!=session or fields[-1]!=generation:raise ValueError('bad mount lifecycle credentials')
            with self.lock:
                if self.revoked:raise ValueError('mount session already revoked')
                self.counts['keepalive' if op=='mount-keepalive' else 'revoke']+=1
                if op=='mount-revoke':self.revoked=True;self.positions.clear()
            return {'generation':generation}
        try:
            si=fields.index('--session');gi=fields.index('--generation')
        except ValueError as e:raise ValueError('missing frontend credentials') from e
        if fields[si+1]!=session or fields[gi+1]!=generation:raise ValueError('bad frontend credentials')
        if self.revoked:raise ValueError('mount session revoked')
        if op=='fs-iroot':
            return {'inode_id':root_id,'directory':True,'symlink':False,'mode':493,'links':1,'size':0,
                    'modified_ns':0,'changed_ns':0,'uid':0,'gid':0,'accessed_ns':0,'created_ns':0}
        if op=='fs-iparent':
            return {'inode_id':root_id,'directory':True,'symlink':False,'mode':493,'links':1,'size':0,
                    'modified_ns':0,'changed_ns':0,'uid':0,'gid':0,'accessed_ns':0,'created_ns':0}
        if op=='fs-dir-iopen':
            with self.lock:
                self.next_cursor+=1;token=f'{self.next_cursor:032x}'
                self.counts['dir_open']+=1;self.positions[token]=0
            return {'cursor':token}
        if op=='fs-dir-entries-raw':
            token=fields[1];n=int(fields[2])
            if token not in self.positions or not 1<=n<=1024:raise ValueError('invalid directory page request')
            with self.lock:
                start=self.positions[token];stop=min(start+n,len(entries));self.positions[token]=stop
                self.counts['page']+=1
            page=entries[start:stop]
            return {'entries':[{'name_hex':e['name'].encode().hex(),'inode':e['inode']} for e in page],
                    'eof':stop==len(entries)}
        if op=='fs-dir-rewind':
            token=fields[1]
            if token not in self.positions:raise ValueError('invalid rewind cursor')
            with self.lock:self.positions[token]=0;self.counts['rewind']+=1
            return {'rewound':True}
        if op=='fs-dir-close':
            token=fields[1]
            with self.lock:self.positions.pop(token,None);self.counts['dir_close']+=1
            return {'closed':True}
        raise ValueError(f'unsupported operation {op}')

state=State()
def exact(peer,n):
    out=bytearray()
    while len(out)<n:
        part=peer.recv(n-len(out))
        if not part:raise EOFError('truncated RPC request')
        out.extend(part)
    return bytes(out)
class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        peer=self.request
        try:
            header=exact(peer,12)
            if header[:8]!=b'FFRPC001':raise ValueError('bad request magic')
            size=struct.unpack('<I',header[8:])[0]
            if not 4<=size<=limit:raise ValueError('invalid request size')
            body=exact(peer,size);at=0;count=struct.unpack_from('<I',body,at)[0];at+=4
            if not 1<=count<=16:raise ValueError('invalid request field count')
            fields=[]
            for _ in range(count):
                length=struct.unpack_from('<I',body,at)[0];at+=4
                if length>len(body)-at:raise ValueError('truncated request field')
                fields.append(body[at:at+length].decode());at+=length
            if at!=len(body):raise ValueError('trailing request data')
            result=state.handle(fields);code=0;payload=json.dumps(result,separators=(',',':')).encode()
        except Exception as e:
            code=1;payload=str(e).encode()
        reply=struct.pack('<I',code)+payload
        peer.sendall(b'FFRSP001'+struct.pack('<I',len(reply))+reply)

class Server(socketserver.ThreadingMixIn,socketserver.UnixStreamServer):
    daemon_threads=True

with tempfile.TemporaryDirectory(prefix='ff-pagination-',dir='/tmp') as temp:
    base=Path(temp);base.chmod(0o700);endpoint=base/'rpc.sock';config=base/'config'
    server=Server(str(endpoint),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    config.write_text(f'{endpoint}\n{os.geteuid()}\n{generation}\n{session}\npagination\n1\n')
    try:
        result=subprocess.run([driver,'--pagination-config',str(config)],capture_output=True,text=True,timeout=50)
        assert result.returncode==0,(result.stdout,result.stderr)
        assert result.stdout.strip()=='OK',(result.stdout,result.stderr)
        with state.lock:
            counts=dict(state.counts)
        assert counts['dir_open']==1,counts
        assert counts['rewind']==0,counts
        assert counts['page']<=(4097+127)//128+2,counts
        assert counts['revoke']==1,counts
    finally:
        server.shutdown();server.server_close();thread.join(timeout=2)
print('FSKit readonly pinned pagination uses bounded page RPCs without rewinds')
