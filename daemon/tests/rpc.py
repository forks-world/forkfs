"""Resident namespace protocol, framing limits, isolation and restart."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1]
def offline(*args):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert p.returncode==0,(args,p.stderr)
    return p.stdout

def frame(fields):
    fields=[f.encode() if isinstance(f,str) else f for f in fields]
    payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(f))+f for f in fields)
    return b'FFRPC001'+struct.pack('<I',len(payload))+payload

def exact(sock,n):
    out=b''
    while len(out)<n:
        part=sock.recv(n-len(out));assert part,'truncated response';out+=part
    return out

def response(sock):
    h=exact(sock,12);assert h[:8]==b'FFRSP001'
    n=struct.unpack('<I',h[8:])[0];assert 4<=n<=1024*1024
    b=exact(sock,n);return struct.unpack('<I',b[:4])[0],b[4:]

with tempfile.TemporaryDirectory(prefix='ffrpc-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';endpoint=root/'control.sock';data=root/'input'
    offline('init',db)
    def start():
        p=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert p.poll() is None,p.communicate()
            assert time.monotonic()<deadline
            time.sleep(.01)
        return p
    session=None
    def raw(fields,ok=True,fragment=False):
        if fields[0] in ['fs-open','fs-close'] or fields[0].startswith('fs-h'):
            fields=[*fields,'--session',session]
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(10);s.connect(str(endpoint));b=frame(fields)
            if fragment:
                for part in [b[:3],b[3:11],b[11:20],b[20:]]:
                    s.sendall(part);time.sleep(.005)
            else:s.sendall(b)
            code,value=response(s)
            assert (code==0)==ok,(fields[:2],code,value)
            assert s.recv(1)==b''
            return value
    def cli(*args,ok=True):
        if args[0] in ['fs-open','fs-close'] or args[0].startswith('fs-h'):
            args=(*args,'--session',session)
        p=subprocess.run([BIN,'request',str(endpoint),*map(str,args)],capture_output=True,timeout=10)
        assert (p.returncode==0)==ok,(args,p.stderr)
        return p.stdout
    proc=start()
    try:
        session=json.loads(cli('session-open','60000'))['session']
        assert json.loads(raw(['status'],fragment=True))['storage_engine']=='leveldb'
        cli('fs-init');cli('fs-mkdir','/src')
        payload=bytes(range(256))*1024;data.write_bytes(payload)
        cli('fs-write','/src/file',data)
        assert raw(['fs-cat','/src/file'])==payload
        cli('fs-link','/src/file','/alias')
        assert json.loads(cli('fs-stat','/alias'))['links']==2
        cli('fs-snapshot','base');cli('fs-fork','base','work')
        raw(['fs-write','/src/file',b'branch\0bytes','--world','work'])
        assert cli('fs-cat','/src/file','--world','work')==b'branch\0bytes'
        assert cli('fs-cat','/src/file','--revision','base')==payload
        cli('fs-write','/src/file',data,'--revision','base',ok=False)
        cli('fs-rename','/src/file','/src/moved','--world','work')
        assert cli('fs-cat','/src/moved','--world','work')==b'branch\0bytes'
        cli('fs-unlink','/alias','--world','work')
        cli('fs-mkdir','/empty');cli('fs-rmdir','/empty')
        # Offset operations use the same resident transaction and retain snapshots.
        data.write_bytes(b'abcdef');cli('fs-write','/offset',data)
        data.write_bytes(b'XY');cli('fs-write-at','/offset','2',data)
        assert cli('fs-read-at','/offset','1','4')==b'bXYe'
        raw(['fs-write-at','/offset','9',b'Z\0'])
        assert cli('fs-cat','/offset')==b'abXYef\0\0\0Z\0'
        cli('fs-truncate','/offset','3');cli('fs-truncate','/offset','6')
        assert cli('fs-cat','/offset')==b'abX\0\0\0'
        data.write_bytes(b'!');result=json.loads(cli('fs-append','/offset',data))
        assert result=={'offset':6,'written':1}
        assert cli('fs-read-at','/offset','6','18446744073709551615')==b'!'
        assert cli('fs-read-at','/offset','9223372036854775807','5')==b''
        cli('fs-write-at','/src/file','0',data,'--revision','base',ok=False)
        cli('fs-truncate','/src/file','0','--revision','base',ok=False)
        cli('fs-append','/src/file',data,'--revision','base',ok=False)
        for fields in [['fs-write-at','/offset','-1',b'x'],
                       ['fs-read-at','/offset','0','18446744073709551616'],
                       ['fs-truncate','/offset','262145'],['fs-truncate','/offset','1x'],
                       ['fs-write-at','/offset','262144',b'x']]:raw(fields,ok=False)
        data.write_bytes(b'handle-original');cli('fs-write','/handle',data)
        handle=json.loads(cli('fs-open','/handle','rw'))['handle']
        second=json.loads(cli('fs-open','/handle','r'))['handle']
        cli('fs-rename','/handle','/renamed-handle');cli('fs-unlink','/renamed-handle')
        assert json.loads(cli('fs-hstat',handle))['links']==0
        raw(['fs-hwrite',handle,'0',b'H\0'])
        assert cli('fs-hread',second,'0','2')==b'H\0'
        assert json.loads(raw(['fs-happend',handle,b'!']))['offset']==15
        cli('fs-htruncate',handle,'3')
        data.write_bytes(b'new');cli('fs-write','/handle',data)
        assert cli('fs-cat','/handle')==b'new' and cli('fs-hread',handle,'0','10')==b'H\0n'
        cli('fs-hwrite',second,'0',data,ok=False)
        cli('fs-hstat',handle,'--world','work',ok=False)
        cli('fs-close',handle);assert cli('fs-hread',second,'0','10')==b'H\0n'
        cli('fs-close',second);cli('fs-hstat',second,ok=False)
        cli('fs-close',handle,ok=False)
        revision_handle=json.loads(cli('fs-open','/src/file','r','--revision','base'))['handle']
        assert cli('fs-hread',revision_handle,'0','4','--revision','base')==payload[:4]
        cli('fs-open','/src/file','rw','--revision','base',ok=False)
        cli('fs-close',revision_handle,'--revision','base')
        before=json.loads(cli('status'))['last_committed_seq']
        for fields in [['unknown'],['fs-write','/x'],['fs-write','/x',b'x'*(256*1024+1)],
                       ['fs-mkdir','/bad\0name'],['status','--world','work']]:
            raw(fields,ok=False)
        assert json.loads(cli('status'))['last_committed_seq']==before
        # Malformed/truncated inputs never publish a transaction.
        for packet in [b'BADRPC01'+struct.pack('<I',4)+struct.pack('<I',1),
                       b'FFRPC001'+struct.pack('<I',1024*1024+1),
                       frame(['status'])+b'extra',
                       b'FFRPC001'+struct.pack('<I',4)+struct.pack('<I',9),
                       frame(['fs-mkdir','/partial'])[:-1]]:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(5);s.connect(str(endpoint));s.sendall(packet);s.shutdown(socket.SHUT_WR)
                code,_=response(s);assert code!=0
        assert json.loads(cli('status'))['last_committed_seq']==before
        # An idle client cannot hold up other requests.
        slow=socket.socket(socket.AF_UNIX);slow.connect(str(endpoint));slow.sendall(b'FFR')
        try:
            assert cli('fs-cat','/src/file')==payload
            def write(i):return raw(['fs-write',f'/src/p{i}',f'v{i}'])
            with ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(write,range(24)))
            for i in range(24):assert raw(['fs-cat',f'/src/p{i}'])==f'v{i}'.encode()
        finally:slow.close()
        cli('fs-compact');cli('fs-check');cli('fs-check','--world','work')
        cli('fs-check','--revision','base')
        data.write_bytes(b'orphan');cli('fs-write','/dead-handle',data)
        expired=json.loads(cli('fs-open','/dead-handle','rw'))['handle']
        cli('fs-unlink','/dead-handle');raw(['fs-hwrite',expired,'0',b'Q'])
        cli('fs-check')
        # Successful writes survive process death, with revision isolation intact.
        proc.kill();proc.wait(timeout=5);endpoint.unlink();proc=start()
        session=json.loads(cli('session-open','60000'))['session']
        cli('fs-hstat',expired,ok=False);cli('fs-stat','/dead-handle',ok=False)
        assert cli('fs-cat','/src/file','--revision','base')==payload
        assert cli('fs-cat','/src/moved','--world','work')==b'branch\0bytes'
        for i in range(24):assert cli('fs-cat',f'/src/p{i}')==f'v{i}'.encode()
        cli('fs-check')
        # Shutdown remains bounded even with partial requests outstanding.
        partial=socket.socket(socket.AF_UNIX);partial.connect(str(endpoint));partial.sendall(b'FFR')
        proc.send_signal(signal.SIGTERM);proc.wait(timeout=3);partial.close()
        assert proc.returncode==0,proc.communicate()
        assert not endpoint.exists()
        offline('fs-check',db)
        print('resident RPC operations, binary data, branches, framing, concurrency and recovery passed')
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
