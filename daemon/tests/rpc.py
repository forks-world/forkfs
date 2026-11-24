"""Resident namespace protocol, framing limits, isolation and restart."""
from concurrent.futures import ThreadPoolExecutor
import errno
import json
import os
from pathlib import Path
import signal
import select
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
        try:
            # A bound path can exist before listen(); the greeting follows it.
            assert select.select([p.stdout],[],[],5)[0], 'daemon readiness timed out'
            assert json.loads(p.stdout.readline())['state']=='leveldb'
        except BaseException:
            if p.poll() is None:p.kill();p.wait(timeout=5)
            raise
        return p
    session=None
    def raw(fields,ok=True,fragment=False,expected_code=None):
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
            if expected_code is not None:assert code==expected_code,(fields[:2],code,value)
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
        raw(['fs-write','/rpc-file',b'R'])
        raw(['fs-symlink','/rpc-file','/file-link'])
        raw(['fs-symlink','/src','/trail-alias'])
        raw(['fs-symlink','/missing-trailing-target','/trail-dangling'])
        raw(['fs-symlink','/trail-loop','/trail-loop'])
        src_id=json.loads(raw(['fs-stat','/src']))['inode_id']
        assert json.loads(raw(['fs-stat','/src/']))['inode_id']==src_id
        assert json.loads(raw(['fs-stat','////']))['inode_id']==json.loads(raw(['fs-stat','/']))['inode_id']
        assert json.loads(raw(['fs-stat','/trail-alias/']))['inode_id']==src_id
        assert json.loads(raw(['fs-lstat','/trail-alias/']))['inode_id']==src_id
        assert raw(['fs-cat','/rpc-file'])==b'R'
        trailing_seq=json.loads(raw(['status']))['last_committed_seq']
        for query, expected_error in [('/rpc-file/', errno.ENOTDIR),
                                      ('/trail-dangling/', errno.ENOENT),
                                      ('/trail-loop/', errno.ELOOP)]:
            expected=expected_error+1
            raw(['fs-stat',query],ok=False,expected_code=expected)
        assert json.loads(raw(['status']))['last_committed_seq']==trailing_seq
        cli('fs-mkdir','/src/hinted')
        payload=bytes(range(256))*1024;data.write_bytes(payload)
        cli('fs-write','/src/file',data)
        assert raw(['fs-cat','/src/file'])==payload
        raw(['fs-mkdir','/src/sub']);raw(['fs-symlink','/src/sub','/physical-alias'])
        raw(['fs-snapshot','before-trailing-mkdir'])
        assert json.loads(raw(['fs-stat','/physical-alias/../file']))['inode_id']==json.loads(raw(['fs-stat','/src/file']))['inode_id']
        raw(['fs-mkdir','/src//./created']);assert json.loads(raw(['fs-stat','/src/created']))['directory']
        raw(['fs-mkdir','/src/trailing///']);assert json.loads(raw(['fs-stat','/src/trailing']))['directory']
        raw(['fs-mkdir','/physical-alias/../trailing-physical///']);assert json.loads(raw(['fs-stat','/src/trailing-physical']))['directory']
        raw(['fs-rmdir','/src/trailing']);raw(['fs-rmdir','/src/trailing-physical'])
        raw(['fs-rmdir','/src/created']);raw(['fs-unlink','/physical-alias']);raw(['fs-rmdir','/src/sub'])
        dangling_id=json.loads(raw(['fs-lstat','/trail-dangling']))['inode_id']
        raw(['fs-mkdir','/trail-dangling/'])
        assert json.loads(raw(['fs-stat','/missing-trailing-target']))['directory']
        assert json.loads(raw(['fs-lstat','/trail-dangling']))['inode_id']==dangling_id
        assert raw(['fs-readlink','/trail-dangling'])==b'/missing-trailing-target'
        raw(['fs-stat','/missing-trailing-target','--revision','before-trailing-mkdir'],ok=False,expected_code=errno.ENOENT+1)
        before_invalid=json.loads(raw(['status']))['last_committed_seq']
        src_before=json.loads(raw(['fs-stat','/src']));listing_before=raw(['fs-ls','/src'])
        mkdir_errors={
            '/src/.':errno.EEXIST+1,'/src/./':errno.EEXIST+1,'/src/..':errno.EEXIST+1,
            '/src/':errno.EEXIST+1,'/trail-alias/':errno.EEXIST+1,
            '/missing/../child':errno.ENOENT+1,'/missing/child/':errno.ENOENT+1,
            '/src/file/../child':errno.ENOTDIR+1,'/rpc-file/':errno.ENOTDIR+1,
            '/file-link/':errno.ENOTDIR+1,'/rpc-file/child/':errno.ENOTDIR+1,
        }
        for invalid,expected in mkdir_errors.items():
            raw(['fs-mkdir',invalid],ok=False,expected_code=expected)
        raw(['fs-mkdir','/src/'+('/'*4092)],ok=False,expected_code=errno.ENAMETOOLONG+1)
        raw(['fs-mkdir','/src/'+('x'*256)+'/'],ok=False,expected_code=errno.ENAMETOOLONG+1)
        assert json.loads(raw(['status']))['last_committed_seq']==before_invalid
        src_after=json.loads(raw(['fs-stat','/src']));assert src_after['links']==src_before['links'] and src_after['modified_ns']==src_before['modified_ns'] and src_after['changed_ns']==src_before['changed_ns']
        assert raw(['fs-ls','/src'])==listing_before
        for created in ['/src/trailing','/src/trailing-physical']:
            raw(['fs-stat',created,'--revision','before-trailing-mkdir'],ok=False,expected_code=errno.ENOENT+1)
        # lstat seeds a long syntactic directory hint. inode-create appends a
        # child that would exceed 4096 bytes if the alias were reused.
        long_alias='/'+'./'*2041+'src/hinted'
        assert len(long_alias)==4093
        hint_session=json.loads(cli('session-open','60000'))['session']
        raw(['inode-root','--session',hint_session])
        hinted=json.loads(raw(['fs-lstat',long_alias]))['inode_id']
        old_session=session;session=hint_session
        created=json.loads(raw(['inode-create',hinted,'child',str(0o600),'--session',hint_session]))
        child=json.loads(raw(['fs-hstat',created['handle']]))['inode_id']
        assert json.loads(raw(['fs-stat','/src/hinted/child']))['inode_id']==child
        raw(['fs-close',created['handle']])
        trailing_hinted=json.loads(raw(['fs-lstat','/src/hinted/']))['inode_id']
        trailing_created=json.loads(raw(['inode-create',trailing_hinted,'trailing-child',str(0o600),'--session',hint_session]))
        trailing_child=json.loads(raw(['fs-hstat',trailing_created['handle']]))['inode_id']
        assert json.loads(raw(['fs-stat','/src/hinted/trailing-child']))['inode_id']==trailing_child
        raw(['fs-close',trailing_created['handle']])
        session=old_session
        cli('session-close',hint_session)
        cli('fs-link','/src/file','/alias')
        assert json.loads(cli('fs-stat','/alias'))['links']==2
        cli('fs-snapshot','base');cli('fs-fork','base','work')
        revision_seq=json.loads(raw(['status']))['last_committed_seq']
        assert json.loads(raw(['fs-stat','/src//./file','--revision','base']))['inode_id']==json.loads(raw(['fs-stat','/src/file']))['inode_id']
        assert json.loads(raw(['fs-stat','/src/','--revision','base']))['inode_id']==src_id
        assert json.loads(raw(['fs-lstat','/trail-alias/','--revision','base']))['inode_id']==src_id
        assert json.loads(raw(['status']))['last_committed_seq']==revision_seq
        raw(['fs-write','/src/./readonly-fail',b'x','--revision','base'],ok=False)
        raw(['fs-mkdir','/src/../readonly-fail','--revision','base'],ok=False)
        assert json.loads(raw(['status']))['last_committed_seq']==revision_seq
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
                       ['fs-truncate','/offset',str((1<<40)+1)],['fs-truncate','/offset','1x'],
                       ['fs-write-at','/offset',str(1<<40),b'x']]:raw(fields,ok=False)
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
                       b'FFRPC001'+struct.pack('<I',4)+struct.pack('<I',17),
                       frame(['fs-mkdir','/partial'])[:-1]]:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(5);s.connect(str(endpoint));s.sendall(packet)
                # The daemon may reject and close a malformed frame before
                # the client half-closes; truncated requests still need EOF.
                try:s.shutdown(socket.SHUT_WR)
                except OSError as e:
                    if e.errno!=errno.ENOTCONN:raise
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
