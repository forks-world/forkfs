"""RPC/offline CLI tests for fs-rename-excl; --expect-unsupported probes baseline binaries."""
import errno, json, select, socket, struct, subprocess, sys, tempfile
from pathlib import Path

BIN=sys.argv[1];UNSUPPORTED='--expect-unsupported' in sys.argv[2:]
def frame(fields):
    fields=[v.encode() if isinstance(v,str) else v for v in fields]
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
with tempfile.TemporaryDirectory(prefix='ff-rename-excl-',dir='/tmp') as td:
    root=Path(td);db=root/'store';sockpath=root/'rpc.sock';payload=root/'payload'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok,(args,p.stderr.decode(errors='replace'))
        return p.stdout
    cli('init',db);cli('fs-init',db)
    for name,data in [('src',b'alpha'),('collision',b'beta'),('cli-src',b'gamma')]:
        payload.write_bytes(data);cli('fs-write',db,'/'+name,payload)
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert select.select([proc.stdout],[],[],10)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
    session=None
    def raw(fields,expect=0,token=None):
        if fields[0].startswith('fs-h') or fields[0] in ('fs-open','fs-close'):fields=[*fields,'--session',token or session]
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(15);s.connect(str(sockpath));s.sendall(frame(fields));code,value=response(s)
        assert code==expect,(fields[:3],code,value)
        return value
    def state():
        v=json.loads(raw(['status']));return tuple(v[k] for k in ('last_committed_seq','object_roots','indexed_objects'))
    def clistate():
        v=json.loads(cli('check',db));return tuple(v[k] for k in ('last_committed_seq','object_roots','indexed_objects'))
    try:
        session=json.loads(raw(['session-open','60000']))['session'];before=state()
        metas={p:json.loads(raw(['fs-lstat',p])) for p in ('/src','/collision')}
        if UNSUPPORTED:
            raw(['fs-rename-excl','/src','/collision'],expect=1)
            raw(['fs-rename-excl','/src','/absent-target'],expect=1)
            assert state()==before
            assert json.loads(raw(['fs-lstat','/src']))==metas['/src'] and json.loads(raw(['fs-lstat','/collision']))==metas['/collision']
            assert raw(['fs-read-at','/src','0','5'])==b'alpha' and raw(['fs-read-at','/collision','0','4'])==b'beta'
            report={'mode':'expect-unsupported','rpc_rejected':True,'namespace_unchanged':True}
        else:
            raw(['fs-rename-excl','/src','/rpc-moved'])
            assert json.loads(raw(['fs-lstat','/rpc-moved']))['inode_id']==metas['/src']['inode_id']
            before=state();raw(['fs-rename-excl','/cli-src','/collision'],expect=errno.EEXIST+1);assert state()==before
            raw(['fs-rename-excl','/missing','/new'],expect=1);assert state()==before
            raw(['fs-rename-excl','/cli-src'],expect=1);assert state()==before
            lease=json.loads(raw(['session-open','60000']))['session'];raw(['inode-root','--session',lease])
            raw(['fs-rename-excl','/cli-src','/lease-target'],expect=errno.EBUSY+1);assert state()==before;raw(['session-close',lease])
            report={'mode':'fixed','rpc_move':'passed','existing_target_errno':'EEXIST','missing_and_arity':'atomic'}
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    before=clistate()
    if UNSUPPORTED:
        cli_src=json.loads(cli('fs-lstat',db,'/cli-src'));cli_dst=json.loads(cli('fs-lstat',db,'/collision'))
        cli('fs-rename-excl',db,'/cli-src','/collision',ok=False)
        cli('fs-rename-excl',db,'/cli-src','/cli-absent-target',ok=False)
        assert clistate()==before
        assert json.loads(cli('fs-lstat',db,'/cli-src'))==cli_src and json.loads(cli('fs-lstat',db,'/collision'))==cli_dst
        assert cli('fs-read-at',db,'/cli-src',0,5)==b'gamma' and cli('fs-read-at',db,'/collision',0,4)==b'beta'
        report['offline_cli_rejected']=True
    else:
        cli('fs-rename-excl',db,'/cli-src','/cli-moved');assert cli('fs-read-at',db,'/cli-moved',0,5)==b'gamma'
        before=clistate();src_meta=json.loads(cli('fs-lstat',db,'/cli-moved'));dst_meta=json.loads(cli('fs-lstat',db,'/collision'))
        cli('fs-rename-excl',db,'/cli-moved','/collision',ok=False)
        assert clistate()==before and json.loads(cli('fs-lstat',db,'/cli-moved'))==src_meta and json.loads(cli('fs-lstat',db,'/collision'))==dst_meta
        assert cli('fs-read-at',db,'/cli-moved',0,5)==b'gamma'
        cli('fs-check',db);report['offline_cli']='passed'
    print(json.dumps(report,sort_keys=True))
