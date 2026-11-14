"""Poisoned storage must not turn session cleanup into an RPC loop or daemon crash."""
import errno
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

BIN, FAULT = sys.argv[1:3]

def command(*args, ok=True):
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, timeout=15)
    assert (p.returncode == 0) == ok, (args, p.stderr)
    return p.stdout

def exact(s, n):
    out = b''
    while len(out) < n:
        part = s.recv(n-len(out)); assert part; out += part
    return out

with tempfile.TemporaryDirectory(prefix='ff-session-cleanup-', dir='/tmp') as temp:
    root = Path(temp)
    for stage, storage_errno in [('wal-append', errno.ENOSPC), ('wal-sync', errno.EDQUOT), ('wal-append', errno.EIO)]:
        db, endpoint, armed, data = [root / f'{stage}-{storage_errno}-{n}' for n in ['db', 'sock', 'armed', 'input']]
        command('init', db); command('fs-init', db)
        data.write_bytes(b'retained revision bytes')
        command('fs-write', db, '/orphan', data); command('fs-snapshot', db, 'base')
        proc = subprocess.Popen([FAULT, 'serve', str(db), '--socket', str(endpoint), '--rpc'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env={**os.environ, 'FORKFS_TEST_IO_ERROR': f'{stage}:{storage_errno}',
                                     'FORKFS_TEST_IO_ERROR_FILE': str(armed)})
        def rpc(*fields, code=0):
            fields = [v if isinstance(v, bytes) else str(v).encode() for v in fields]
            payload = struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(v))+v for v in fields)
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(3); s.connect(str(endpoint)); s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
                h=exact(s,12); assert h[:8]==b'FFRSP001'; body=exact(s,struct.unpack('<I',h[8:])[0])
            actual=struct.unpack('<I',body[:4])[0]
            assert actual==code,(fields[:3],actual,code,body[4:])
            return body[4:]
        try:
            assert select.select([proc.stdout],[],[],5)[0]
            assert json.loads(proc.stdout.readline())['state']=='leveldb'
            token=json.loads(rpc('session-open',60000))['session']
            handle=json.loads(rpc('fs-open','/orphan','rw','--session',token))['handle']
            rpc('fs-unlink','/orphan')
            rpc('inode-root','--session',token)  # Keep the mutable mount fence owned by this session.
            before=json.loads(rpc('status'))
            armed.touch()
            # Final close is the first failing WAL operation and must preserve its errno.
            rpc('session-close',token,code=storage_errno+1)
            poisoned=json.loads(rpc('status'))
            assert poisoned['requires_reopen'] and proc.poll() is None
            stable={key:poisoned[key] for key in ['last_committed_seq','indexed_objects','object_roots']}
            for _ in range(3):
                time.sleep(.12)  # Each request crosses another daemon poll/expiry iteration.
                current=json.loads(rpc('status'))
                profile=json.loads(rpc('storage-profile'))
                assert all(current[key]==value for key,value in stable.items())
                assert profile['storage_metrics']
                assert proc.poll() is None
            # The failed close leaves the token closing and retains its orphan roots.
            rpc('fs-hstat',handle,'--session',token,code=1)
            assert all(poisoned[key]==before[key] for key in stable)
            other=json.loads(rpc('session-open',60000))['session']
            rpc('fs-write','/fenced',b'x','--session',other,code=errno.EBUSY+1)
            rpc('fs-restore','base','cleanup-safety',code=errno.EBUSY+1)
            rpc('session-close',other)
            proc.terminate(); proc.wait(timeout=5)
            assert proc.returncode==0,proc.communicate()
            assert not endpoint.exists()
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=5)
            endpoint.unlink(missing_ok=True)
        # Restart drops only transient orphan ownership and preserves the revision.
        assert command('fs-cat',db,'/orphan','--revision','base')==b'retained revision bytes'
        command('fs-check',db); command('check',db)
        assert command('fs-stat',db,'/orphan',ok=False)==b''
        # A poisoned session reaching its short lease is retried once per poll,
        # remains accounted for, and keeps its mount fence while the daemon runs.
        db2, endpoint2, armed2, input2 = [root / f'expire-{stage}-{storage_errno}-{n}' for n in ['db','sock','armed','input']]
        command('init',db2); command('fs-init',db2); input2.write_bytes(b'expiring orphan')
        command('fs-write',db2,'/orphan',input2); command('fs-snapshot',db2,'base')
        exp = subprocess.Popen([FAULT,'serve',str(db2),'--socket',str(endpoint2),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                               env={**os.environ,'FORKFS_TEST_IO_ERROR':f'{stage}:{storage_errno}','FORKFS_TEST_IO_ERROR_FILE':str(armed2)})
        try:
            assert select.select([exp.stdout],[],[],5)[0]; assert json.loads(exp.stdout.readline())['state']=='leveldb'
            def erpc(*fields, code=0):
                fields=[v if isinstance(v,bytes) else str(v).encode() for v in fields]
                payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(3);s.connect(str(endpoint2));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
                    h=exact(s,12);body=exact(s,struct.unpack('<I',h[8:])[0])
                actual=struct.unpack('<I',body[:4])[0];assert actual==code,(fields[:3],actual,code,body[4:]);return body[4:]
            short=json.loads(erpc('session-open',100))['session']
            orphan=json.loads(erpc('fs-open','/orphan','rw','--session',short))['handle']
            erpc('fs-unlink','/orphan'); erpc('inode-root','--session',short)
            before_fault=json.loads(erpc('status'))
            armed2.touch(); erpc('fs-hwrite',orphan,0,b'X','--session',short,code=storage_errno+1)
            poisoned=json.loads(erpc('status')); assert poisoned['requires_reopen']
            stable={key:poisoned[key] for key in ['last_committed_seq','indexed_objects','object_roots']}
            assert all(poisoned[key]==before_fault[key] for key in stable)
            # The failed request renewed the lease. Wait beyond its 100 ms deadline,
            # then sample across two poll intervals without touching that session.
            time.sleep(.25)
            for _ in range(2):
                assert exp.poll() is None
                snapshot=json.loads(erpc('status'))
                assert all(snapshot[key]==value for key,value in stable.items())
                time.sleep(.12)
            erpc('fs-hstat',orphan,'--session',short,code=1)
            other=json.loads(erpc('session-open',60000))['session']
            erpc('fs-write','/fenced',b'x','--session',other,code=errno.EBUSY+1)
            erpc('fs-restore','base','expiry-safety',code=errno.EBUSY+1)
            exp.terminate();exp.wait(timeout=5);assert exp.returncode==0,exp.communicate();assert not endpoint2.exists()
        finally:
            if exp.poll() is None: exp.kill();exp.wait(timeout=5)
            endpoint2.unlink(missing_ok=True)
        command('fs-check',db2); command('check',db2)

        # An unexpired linked handle is still present when SIGTERM begins.
        # Poison first, verify explicit close returns EIO, then let shutdown
        # attempt the remaining linked-handle cleanup exactly once.
        db3, endpoint3, armed3, linked_data, orphan_data = [root / f'shutdown-{storage_errno}-{n}' for n in ['db','sock','armed','linked','orphan']]
        command('init',db3); command('fs-init',db3)
        linked_data.write_bytes(b'linked original'); orphan_data.write_bytes(b'orphan original')
        command('fs-write',db3,'/linked',linked_data); command('fs-write',db3,'/orphan',orphan_data)
        command('fs-snapshot',db3,'base')
        shutdown_proc=subprocess.Popen([FAULT,'serve',str(db3),'--socket',str(endpoint3),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                       env={**os.environ,'FORKFS_TEST_IO_ERROR':f'{stage}:{storage_errno}','FORKFS_TEST_IO_ERROR_FILE':str(armed3)})
        try:
            assert select.select([shutdown_proc.stdout],[],[],5)[0]; assert json.loads(shutdown_proc.stdout.readline())['state']=='leveldb'
            def srpc(*fields, code=0):
                fields=[v if isinstance(v,bytes) else str(v).encode() for v in fields]
                payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(3);s.connect(str(endpoint3));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
                    h=exact(s,12);body=exact(s,struct.unpack('<I',h[8:])[0])
                actual=struct.unpack('<I',body[:4])[0];assert actual==code,(fields[:3],actual,code,body[4:]);return body[4:]
            doomed=json.loads(srpc('session-open',60000))['session']
            linked=json.loads(srpc('session-open',60000))['session']
            linked_handle=json.loads(srpc('fs-open','/linked','rw','--session',linked))['handle']
            orphan_handle=json.loads(srpc('fs-open','/orphan','rw','--session',doomed))['handle']
            srpc('fs-unlink','/orphan')
            before_fault=json.loads(srpc('status'))
            armed3.touch(); srpc('fs-hwrite',orphan_handle,0,b'X','--session',doomed,code=storage_errno+1)
            poisoned=json.loads(srpc('status')); assert poisoned['requires_reopen']
            stable={key:poisoned[key] for key in ['last_committed_seq','indexed_objects','object_roots']}
            assert all(poisoned[key]==before_fault[key] for key in stable)
            srpc('session-close',doomed,code=errno.EIO+1)
            for _ in range(3):
                current=json.loads(srpc('status'))
                profile=json.loads(srpc('storage-profile'))
                assert all(current[key]==value for key,value in stable.items())
                assert profile['storage_metrics']
            assert shutdown_proc.poll() is None
            # The second token is still live, unexpired, and owns a linked handle.
            srpc('fs-hstat',linked_handle,'--session',linked,code=errno.EIO+1)
            shutdown_proc.terminate(); shutdown_proc.wait(timeout=5)
            assert shutdown_proc.returncode==0,shutdown_proc.communicate(); assert not endpoint3.exists()
        finally:
            if shutdown_proc.poll() is None: shutdown_proc.kill();shutdown_proc.wait(timeout=5)
            endpoint3.unlink(missing_ok=True)
        assert command('fs-cat',db3,'/linked')==b'linked original'
        assert command('fs-stat',db3,'/orphan',ok=False)==b''
        assert command('fs-cat',db3,'/orphan','--revision','base')==b'orphan original'
        command('fs-check',db3); command('check',db3)
    print('poisoned explicit-close cleanup, WAL errno, mount fencing, shutdown, and restart recovery passed')
