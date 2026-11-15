"""RPC and offline CLI coverage for fs-exchange; --expect-unsupported probes baseline binaries."""
import errno
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile

BIN=sys.argv[1];EXPECT_UNSUPPORTED='--expect-unsupported' in sys.argv[2:]
def frame(fields):
    fields=[v.encode() if isinstance(v,str) else v for v in fields]
    body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
    return b'FFRPC001'+struct.pack('<I',len(body))+body
def stable(meta):
    return {k:v for k,v in meta.items() if k!='changed_ns'}
def exact(sock,n):
    data=b''
    while len(data)<n:
        part=sock.recv(n-len(data));assert part,'truncated response';data+=part
    return data
def response(sock):
    header=exact(sock,12);assert header[:8]==b'FFRSP001';n=struct.unpack('<I',header[8:])[0]
    body=exact(sock,n);return struct.unpack('<I',body[:4])[0],body[4:]
with tempfile.TemporaryDirectory(prefix='ff-exchange-rpc-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';sockpath=root/'rpc.sock';payload=root/'payload'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok,(args,p.stderr.decode(errors='replace'))
        return p.stdout
    cli('init',db);cli('fs-init',db)
    payload.write_bytes(b'alpha');cli('fs-write',db,'/a',payload)
    payload.write_bytes(b'beta');cli('fs-write',db,'/b',payload)
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert select.select([proc.stdout],[],[],10)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
    session=None
    def raw(fields,expect=0,token=None):
        if fields[0].startswith('fs-h') or fields[0]=='fs-open' or fields[0]=='fs-close':fields=[*fields,'--session',token or session]
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(15);sock.connect(str(sockpath));sock.sendall(frame(fields));code,value=response(sock)
        assert code==expect,(fields[:3],code,value)
        return value
    def state():
        value=json.loads(raw(['status']))
        return {k:value[k] for k in ('last_committed_seq','object_roots','indexed_objects')}
    def cli_state():
        value=json.loads(cli('check',db))
        return {k:value[k] for k in ('last_committed_seq','object_roots','indexed_objects')}
    try:
        session=json.loads(raw(['session-open','60000']))['session']
        metadata={p:json.loads(raw(['fs-lstat',p])) for p in ('/a','/b')}
        ids={p:value['inode_id'] for p,value in metadata.items()}
        before=state()
        if EXPECT_UNSUPPORTED:
            raw(['fs-exchange','/a','/b'],expect=1)
            assert state()==before
            assert json.loads(raw(['fs-lstat','/a']))==metadata['/a']
            assert json.loads(raw(['fs-lstat','/b']))==metadata['/b']
            assert raw(['fs-read-at','/a','0','5'])==b'alpha' and raw(['fs-read-at','/b','0','4'])==b'beta'
            report={'mode':'expect-unsupported','rpc_rejected':True,'inode_ids_unchanged':True,'data_unchanged':True,'root_state_unchanged':True}
        else:
            raw(['fs-exchange','/a','/b'])
            assert json.loads(raw(['fs-lstat','/a']))['inode_id']==ids['/b'] and json.loads(raw(['fs-lstat','/b']))['inode_id']==ids['/a']
            assert raw(['fs-read-at','/a','0','4'])==b'beta' and raw(['fs-read-at','/b','0','5'])==b'alpha'
            before=state();raw(['fs-exchange','/missing','/b'],expect=1);assert state()==before
            raw(['fs-exchange','/a'],expect=1);assert state()==before
            lease=json.loads(raw(['session-open','60000']))['session'];raw(['inode-root','--session',lease])
            raw(['fs-exchange','/a','/b'],expect=errno.EBUSY+1);assert state()==before;raw(['session-close',lease])
            report={'mode':'fixed','rpc_exchange':'passed','missing_and_arity':'rejected_atomically','mount_lease':'EBUSY'}
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    if EXPECT_UNSUPPORTED:
        cli('fs-exchange',db,'/a','/b',ok=False)
        assert stable(json.loads(cli('fs-lstat',db,'/a')))==stable(metadata['/a']) and stable(json.loads(cli('fs-lstat',db,'/b')))==stable(metadata['/b'])
        assert cli('fs-read-at',db,'/a',0,5)==b'alpha' and cli('fs-read-at',db,'/b',0,4)==b'beta'
        assert cli_state()==before
        report['offline_cli_rejected']=True
    else:
        cli('fs-exchange',db,'/a','/b');assert cli('fs-read-at',db,'/a',0,5)==b'alpha' and cli('fs-read-at',db,'/b',0,4)==b'beta'
        assert stable(json.loads(cli('fs-lstat',db,'/a')))==stable(metadata['/a']) and stable(json.loads(cli('fs-lstat',db,'/b')))==stable(metadata['/b'])
        cli('fs-check',db);report['offline_cli']='passed'
    print(json.dumps(report,sort_keys=True))
