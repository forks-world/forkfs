"""Resident namespace protocol, framing limits, isolation and restart."""
from concurrent.futures import ThreadPoolExecutor
import json
import errno
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

with tempfile.TemporaryDirectory(prefix='ffrpc-') as tmp:
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
        if fields[0] in ['fs-open','fs-close','fs-iopen','fs-icreate'] or fields[0].startswith(('fs-h','fs-dir-')):
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
        if args[0] in ['fs-open','fs-close','fs-iopen','fs-icreate'] or args[0].startswith(('fs-h','fs-dir-')):
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
        # Inode-addressed RPC must survive parent rename and replacement of its old path.
        root_id=json.loads(cli('fs-iroot'))['inode_id']
        d=json.loads(cli('fs-icreate',root_id,'remote-dir','directory','mode=755'))['inode']['inode_id']
        created=json.loads(cli('fs-icreate',d,'file','file','mode=600'))
        inode=created['inode']['inode_id'];pin=created['handle'];assert pin
        raw(['fs-hwrite',pin,'0',b'inode bytes'])
        assert cli('fs-iread',inode,'0','64')==b'inode bytes'
        unscoped=subprocess.run([BIN,'request',str(endpoint),'fs-icreate',d,'no-session','file','mode=600'],capture_output=True,timeout=10)
        assert unscoped.returncode!=0 and b'require --session' in unscoped.stderr
        cli('fs-ilookup',d,'no-session',ok=False)
        cursor=json.loads(cli('fs-dir-iopen',d))['cursor']
        cli('fs-irename',root_id,'remote-dir',root_id,'moved-dir',d,'')
        replacement=json.loads(cli('fs-icreate',root_id,'remote-dir','directory','mode=755'))['inode']['inode_id']
        assert replacement!=d and json.loads(cli('fs-ilookup',d,'file'))['inode_id']==inode
        assert json.loads(cli('fs-iparent',d))['inode_id']==root_id
        page=json.loads(cli('fs-dir-entries',cursor,'128'));assert any(e['name']=='file' and e['inode']['inode_id']==inode for e in page['entries'])
        cli('fs-dir-close',cursor)
        second_pin=json.loads(cli('fs-iopen',inode,'r'))['handle']
        assert cli('fs-hread',second_pin,'0','64')==b'inode bytes'
        cli('fs-close',second_pin)
        assert json.loads(cli('fs-ilink',inode,d,'alias'))['inode_id']==inode
        # Expected-inode mismatches must report ESTALE, never unlink a replacement.
        for stale in (['fs-iremove',d,'file',replacement,'file'],
                      ['fs-irename',d,'file',d,'alias',inode,replacement]):
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(5);client.connect(str(endpoint));client.sendall(frame(stale))
                code,_=response(client);assert code==0x80000000|errno.ESTALE,code
        assert json.loads(cli('fs-ilookup',d,'file'))['inode_id']==inode
        assert json.loads(cli('fs-ilookup',d,'alias'))['inode_id']==inode
        cli('fs-irename',d,'file',d,'renamed',inode,'')
        cli('fs-iremove',d,'renamed',inode,'file');cli('fs-iremove',d,'alias',inode,'file')
        newer=json.loads(cli('fs-icreate',d,'renamed','file','mode=600'))
        assert newer['inode']['inode_id']!=inode
        assert cli('fs-hread',pin,'0','64')==b'inode bytes'
        cli('fs-close',pin);cli('fs-close',newer['handle'])
        cli('fs-iremove',d,'renamed',newer['inode']['inode_id'],'file')
        link=json.loads(cli('fs-isymlink',d,'link','target','mode=777'))['inode_id']
        assert cli('fs-ireadlink',link)==b'target';cli('fs-iremove',d,'link',link,'file')
        cli('fs-iremove',root_id,'moved-dir',d,'directory');cli('fs-iremove',root_id,'remote-dir',replacement,'directory')
        cli('fs-icreate',root_id,'forbidden','directory','mode=755','--revision','base',ok=False)
        # Mount sessions are bound to one view and one service process generation.
        generation=json.loads(cli('service-hello'))['generation']
        mount=json.loads(cli('mount-open','world','work','60000','--generation',generation))['session']
        def mounted(fields,token=mount,epoch=generation,ok=True):
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(5);client.connect(str(endpoint))
                client.sendall(frame([*fields,'--session',token,'--generation',epoch]))
                code,value=response(client);assert (code==0)==ok,(fields,code,value)
                return value
        work_root=json.loads(mounted(['fs-iroot']))['inode_id']
        mounted(['fs-iroot','--world','main'],ok=False)
        for fields in (['status'],['backup-start','forbidden','/tmp/not-created'],
                       ['fs-restore','base'],['session-open','60000'],['service-hello']):mounted(fields,ok=False)
        mounted(['fs-iroot'],epoch='wrong-generation',ok=False)
        # A request with no generation must not execute or silently become management.
        denied=subprocess.run([BIN,'request',str(endpoint),'fs-iroot','--session',mount],capture_output=True,timeout=10)
        assert denied.returncode!=0 and b'requires service generation' in denied.stderr
        pinned=json.loads(mounted(['fs-icreate',work_root,'mount-orphan','file','mode=600']))
        mh=pinned['handle'];mi=pinned['inode']['inode_id']
        mounted(['fs-hwrite',mh,'0',b'mount retained data'])
        mc=json.loads(mounted(['fs-dir-iopen',work_root]))['cursor']
        mounted(['fs-iremove',work_root,'mount-orphan',mi,'file'])
        assert mounted(['fs-hread',mh,'0','100'])==b'mount retained data'
        cli('mount-keepalive',mount,'--generation',generation)
        cli('session-close',mount,ok=False)
        cli('mount-revoke',mount,'--generation',generation)
        mounted(['fs-iroot'],ok=False);mounted(['fs-hread',mh,'0','100'],ok=False)
        mounted(['fs-dir-entries',mc,'128'],ok=False)
        cli('session-keepalive',mount,ok=False)
        ro=json.loads(cli('mount-open','revision','base','60000','--generation',generation))['session']
        ro_root=json.loads(mounted(['fs-iroot'],token=ro))['inode_id']
        mounted(['fs-icreate',ro_root,'forbidden','directory','mode=755'],token=ro,ok=False)
        cli('mount-revoke',ro,'--generation',generation)
        # A World restore revokes mount authority as well as its retained handles.
        restored=json.loads(cli('mount-open','world','work','60000','--generation',generation))['session']
        cli('fs-restore','base','--world','work')
        mounted(['fs-iroot'],token=restored,ok=False)
        expiring=json.loads(cli('mount-open','world','main','100','--generation',generation))['session']
        time.sleep(.2);mounted(['fs-iroot'],token=expiring,ok=False)
        restart_mount=json.loads(cli('mount-open','world','main','60000','--generation',generation))['session']
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
                       ['fs-truncate','/offset','18446744073709551615'],['fs-truncate','/offset','1x'],
                       ['fs-write-at','/offset','9223372036854775807',b'x']]:raw(fields,ok=False)
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
        for fields in [['unknown'],['fs-write','/x'],['fs-write-at','/x','9223372036854775807',b'x'],
                       ['fs-mkdir','/bad\0name'],['status','--world','work']]:
            raw(fields,ok=False)
        assert json.loads(cli('status'))['last_committed_seq']==before
        # Malformed/truncated inputs never publish a transaction.
        for packet in [b'BADRPC01'+struct.pack('<I',4)+struct.pack('<I',1),
                       b'FFRPC001'+struct.pack('<I',1024*1024+1),
                       frame(['status'])+b'extra',
                       b'FFRPC001'+struct.pack('<I',4)+struct.pack('<I',17),
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
        next_generation=json.loads(cli('service-hello'))['generation'];assert next_generation!=generation
        mounted(['fs-iroot'],token=restart_mount,epoch=generation,ok=False)
        cli('mount-open','world','main','60000','--generation',generation,ok=False)
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

# Select the management identity explicitly; peer credentials are authoritative.
with tempfile.TemporaryDirectory(prefix='ffrpc-identity-') as tmp:
    root=Path(tmp);db=root/'store';offline('init',db)
    for value in ('-1','+1','1x','4294967295','18446744073709551616'):
        p=subprocess.run([BIN,'serve',str(db),'--socket',str(root/'invalid.sock'),'--rpc','--control-uid',value],capture_output=True,timeout=10)
        assert p.returncode!=0 and b'invalid control UID' in p.stderr,(value,p.stderr)
        assert not (root/'invalid.sock').exists()
    for selected in (os.geteuid(),os.geteuid()+1):
        separate=selected!=os.geteuid();runtime=root/str(selected);runtime.mkdir(mode=0o711 if separate else 0o700)
        endpoint=runtime/'control.sock'
        p=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc','--control-uid',str(selected)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            deadline=time.monotonic()+5
            while not endpoint.exists():
                assert p.poll() is None,p.communicate()
                assert time.monotonic()<deadline
                time.sleep(.01)
            assert json.loads(p.stdout.readline())['service']=='forkfsd'
            assert endpoint.stat().st_mode&0o777==(0o666 if separate else 0o700),oct(endpoint.stat().st_mode)
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(5);client.connect(str(endpoint))
                if separate:
                    try:
                        client.sendall(frame(['fs-init']))
                        assert client.recv(12)==b'','unauthorized UID received a response'
                    except OSError as error:
                        assert error.errno in (errno.ECONNRESET,errno.EPIPE,errno.ENOTCONN),error
                else:
                    client.sendall(frame(['status']));code,value=response(client);assert code==0,(code,value)
                    verified=subprocess.run([BIN,'request',str(endpoint),'--server-uid',str(os.geteuid()),'status'],capture_output=True,timeout=10)
                    assert verified.returncode==0 and json.loads(verified.stdout)['service']=='forkfsd',verified.stderr
            assert p.poll() is None
        finally:
            p.send_signal(signal.SIGTERM);out,err=p.communicate(timeout=10);assert p.returncode==0,(out,err)
        assert not endpoint.exists()
    # Unauthorized fs-init must not have changed the repository.
    result=subprocess.run([BIN,'fs-stat',str(db),'/'],capture_output=True,timeout=10)
    assert result.returncode!=0

# A convincing endpoint path is insufficient: no payload reaches a wrong-UID server.
with tempfile.TemporaryDirectory(prefix='ffrpc-spoof-') as tmp:
    root=Path(tmp);endpoint=root/'fake.sock';secret=root/'payload';secret.write_bytes(b'private management payload')
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as listener,ThreadPoolExecutor(max_workers=1) as workers:
        listener.bind(str(endpoint));listener.listen();listener.settimeout(5)
        def receive():
            client,_=listener.accept()
            with client:
                client.settimeout(5)
                return client.recv(1024)
        future=workers.submit(receive)
        result=subprocess.run([BIN,'request',str(endpoint),'--server-uid',str(os.geteuid()+1),
                               'fs-write','/file',str(secret)],capture_output=True,timeout=10)
        assert result.returncode!=0 and b'RPC server identity mismatch' in result.stderr,result.stderr
        assert future.result(timeout=6)==b'','client leaked request bytes to an untrusted service'

# Real peer UID is admitted only as frontend, not as management. Running both
# test processes under one UID checks protocol roles, not deployment isolation.
with tempfile.TemporaryDirectory(prefix='ffrpc-role-') as tmp:
    root=Path(tmp);db=root/'store';offline('init',db);root.chmod(0o711);endpoint=root/'control.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc','--control-uid',str(os.geteuid()+1),
                           '--frontend-uid',str(os.geteuid())],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate()
            assert time.monotonic()<deadline;time.sleep(.01)
        assert json.loads(proc.stdout.readline())['service']=='forkfsd'
        hello=subprocess.run([BIN,'request',str(endpoint),'service-hello'],capture_output=True,timeout=10)
        assert hello.returncode==0,hello.stderr
        epoch=json.loads(hello.stdout)['generation']
        for fields in (['status'],['session-open','60000'],['fs-init'],
                       ['mount-open','world','main','60000','--generation',epoch],
                       ['fs-iroot'],['fs-iroot','--session','forged','--generation',epoch]):
            result=subprocess.run([BIN,'request',str(endpoint),*fields],capture_output=True,timeout=10)
            assert result.returncode!=0,(fields,result.stdout)
        assert proc.poll() is None
    finally:
        proc.send_signal(signal.SIGTERM);out,err=proc.communicate(timeout=10);assert proc.returncode==0,(out,err)
    assert not endpoint.exists()
    assert subprocess.run([BIN,'fs-stat',str(db),'/'],capture_output=True,timeout=10).returncode!=0
