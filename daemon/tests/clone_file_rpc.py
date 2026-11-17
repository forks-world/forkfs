"""Resident RPC and offline CLI coverage for daemon-only fs-clone-file."""
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
with tempfile.TemporaryDirectory(prefix='ff-clone-file-',dir='/tmp') as td:
    root=Path(td);db=root/'store';sockpath=root/'rpc.sock';payload=root/'payload'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok,(args,p.stderr.decode(errors='replace'))
        return p.stdout
    cli('init',db);cli('fs-init',db)
    payload.write_bytes(bytes((i*17)%251 for i in range(70000)));cli('fs-write',db,'/source',payload)
    cli('fs-chmod',db,'/source','0601');cli('fs-settimes',db,'/source','111','222','333')
    cli('fs-xattr-set',db,'/source','user.copy','upsert',payload)
    cli('fs-symlink',db,'/source','/link');cli('fs-write',db,'/occupied',payload);cli('fs-mkdir',db,'/dir')
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert select.select([proc.stdout],[],[],10)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
    def raw(fields,expect=0):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(15);s.connect(str(sockpath));s.sendall(frame(fields));code,value=response(s)
        assert code==expect,(fields,code,value)
        return value
    def state():
        v=json.loads(raw(['status']));return tuple(v[k] for k in ('last_committed_seq','object_roots','indexed_objects'))
    def clistate():
        v=json.loads(cli('check',db));return tuple(v[k] for k in ('last_committed_seq','object_roots','indexed_objects'))
    try:
        before=state();src=json.loads(raw(['fs-lstat','/source']));link=json.loads(raw(['fs-lstat','/link']))
        if UNSUPPORTED:
            raw(['fs-clone-file','/link','/rpc-copy'],expect=1)
            raw(['fs-clone-file','/source'],expect=1)
            assert state()==before and json.loads(raw(['fs-lstat','/source']))==src
            raw(['fs-stat','/rpc-copy'],expect=errno.ENOENT+1)
            assert raw(['fs-read-at','/source','0','70000'])==payload.read_bytes()
            report={'mode':'expect-unsupported','rpc_rejected':True,'state_unchanged':True}
        else:
            raw(['fs-clone-file','/link','/rpc-copy'])
            copy=json.loads(raw(['fs-lstat','/rpc-copy']))
            assert copy['inode_id']!=src['inode_id'] and copy['links']==1
            assert copy['mode']==src['mode'] and copy['modified_ns']==src['modified_ns']
            assert copy['accessed_ns']==src['accessed_ns'] and copy['birth_ns']==src['birth_ns']
            assert json.loads(raw(['fs-lstat','/source']))==src and json.loads(raw(['fs-lstat','/link']))==link
            assert json.loads(raw(['fs-stat','/link']))['inode_id']==src['inode_id']
            assert raw(['fs-read-at','/rpc-copy','0','70000'])==payload.read_bytes()
            assert raw(['fs-xattr-get','/rpc-copy','user.copy'])==payload.read_bytes()
            before=state()
            raw(['fs-clone-file','/source','/occupied'],expect=errno.EEXIST+1)
            raw(['fs-clone-file','/source','/link'],expect=errno.EEXIST+1)
            raw(['fs-clone-file','/source','/dir'],expect=errno.EEXIST+1)
            raw(['fs-clone-file','/absent','/absent-copy'],expect=1)
            raw(['fs-clone-file','/source'],expect=1)
            assert state()==before and json.loads(raw(['fs-lstat','/source']))==src
            lease=json.loads(raw(['session-open','60000']))['session']
            raw(['inode-root','--session',lease]);before=state()
            raw(['fs-clone-file','/source','/mounted-copy'],expect=errno.EBUSY+1)
            assert state()==before;raw(['session-close',lease])
            raw(['fs-snapshot','base']);raw(['fs-fork','base','work'])
            raw(['fs-clone-file','/source','/world-copy','--world','work'])
            raw(['fs-stat','/world-copy'],expect=errno.ENOENT+1)
            assert raw(['fs-read-at','/world-copy','0','70000','--world','work'])==payload.read_bytes()
            report={'mode':'fixed','rpc_clone':'passed','exclusive_errors':'atomic','world_selector':'passed'}
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
    before=clistate()
    if UNSUPPORTED:
        src=json.loads(cli('fs-lstat',db,'/source'))
        cli('fs-clone-file',db,'/source','/cli-copy',ok=False)
        assert clistate()==before and json.loads(cli('fs-lstat',db,'/source'))==src
        cli('fs-stat',db,'/cli-copy',ok=False)
        assert cli('fs-read-at',db,'/source',0,70000)==payload.read_bytes()
        report['offline_cli_rejected']=True
    else:
        cli('fs-clone-file',db,'/source','/cli-copy')
        c=json.loads(cli('fs-lstat',db,'/cli-copy'));s=json.loads(cli('fs-lstat',db,'/source'))
        assert c['inode_id']!=s['inode_id'] and c['links']==1 and c['mode']==s['mode']
        before=clistate();cli('fs-clone-file',db,'/source','/occupied',ok=False)
        assert clistate()==before and json.loads(cli('fs-lstat',db,'/source'))==s
        cli('fs-stat',db,'/occupied-copy',ok=False)
        assert cli('fs-read-at',db,'/cli-copy',0,70000)==payload.read_bytes()
        cli('fs-check',db);report['offline_cli']='passed'
    print(json.dumps(report,sort_keys=True))
