"""Explicit immutable revision diff, byte identity and stateless bounded pages."""
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
BIN=sys.argv[1]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=20)
    assert (p.returncode==0)==ok,(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-dir-diff-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'db';data=root/'input';endpoint=root/'control.sock'
    run('init',db);run('fs-init',db)
    def write(path,value):data.write_bytes(value);run('fs-write',db,path,data)
    def stat(path):return json.loads(run('fs-stat',db,path))
    def diff(a,b,after='',limit=64):return json.loads(run('fs-diff',db,a,b,after,limit))
    def collect(a,b,limit):
        entries=[];cursor=''
        while True:
            page=diff(a,b,cursor,limit);assert len(page['changes'])<=limit;entries+=page['changes']
            if page['eof']:assert page['next_after']=='';return entries
            assert page['changes'] and page['next_after']==page['changes'][-1]['path'] and page['next_after']>cursor
            cursor=page['next_after']
    run('fs-mkdir',db,'/a');write('/a/inner',b'old');write('/stable',b'unchanged')
    write('/bytes',b'ab\0\xff');old_stat=stat('/bytes')
    write('/mode',b'same');write('/time',b'same');write('/replace',b'same')
    write('/moved',b'rename');moved_id=stat('/moved')['inode_id']
    run('fs-mkdir',db,'/type');write('/type/child',b'child')
    run('fs-symlink',db,'/loop','/loop');run('fs-symlink',db,'/missing-old','/link')
    write('/layout',bytes(8192));write('/large',b'head');run('fs-truncate',db,'/large',2*1024*1024)
    run('fs-snapshot',db,'before');run('fs-snapshot',db,'same-tree')
    assert diff('before','before')['changes']==[] and diff('before','same-tree')['changes']==[]
    write('/bytes',b'cd\0\xff');run('fs-setmtime',db,'/bytes',old_stat['modified_ns'])
    run('fs-chmod',db,'/mode','700');run('fs-setmtime',db,'/time','123456789')
    data.write_bytes(b'same');run('fs-save',db,'/replace',data)
    run('fs-rename',db,'/moved','/renamed');run('fs-link',db,'/mode','/hardlink')
    run('fs-unlink',db,'/type/child');run('fs-rmdir',db,'/type');write('/type',b'now a file')
    run('fs-unlink',db,'/link');run('fs-symlink',db,'/missing-new','/link')
    write('/a-',b'lexical sibling');write('/a/inner',b'new')
    weird='/中文📄"\\';write(weird,b'utf8')
    run('fs-truncate',db,'/layout',1024*1024);run('fs-truncate',db,'/layout',8192)
    data.write_bytes(b'TAIL');run('fs-write-at',db,'/large',1024*1024+65535,data)
    # A full 64-entry page near the path byte limit must fit the RPC frame.
    deep=''
    for _ in range(15):deep+='/'+'"'*255;run('fs-mkdir',db,deep)
    for i in range(70):write(deep+'/'+'"'*252+f'{i:03}',b'bounded response')
    run('fs-snapshot',db,'after');before_seq=json.loads(run('check',db))['last_committed_seq']
    changes=collect('before','after',64);by_path={e['path']:e for e in changes}
    assert [e['path'] for e in changes]==sorted(by_path) and len(changes)==len(by_path)
    assert '/stable' not in by_path and '/loop' not in by_path
    assert by_path['/bytes']['content_changed'] # same size and restored mtime
    assert by_path['/mode']['metadata_changed'] and not by_path['/mode']['content_changed']
    assert by_path['/time']['metadata_changed'] and not by_path['/time']['content_changed']
    assert not by_path['/replace']['content_changed'] and by_path['/replace']['before_inode']!=by_path['/replace']['after_inode']
    assert by_path['/moved']['change']=='removed' and by_path['/renamed']['change']=='added'
    assert by_path['/moved']['before_inode']==by_path['/renamed']['after_inode']==moved_id
    assert by_path['/hardlink']['after_inode']==by_path['/mode']['after_inode']
    assert by_path['/type']['before_type']=='directory' and by_path['/type']['after_type']=='file'
    assert by_path['/type/child']['change']=='removed' and by_path['/link']['content_changed']
    assert by_path['/layout']['metadata_changed'] and not by_path['/layout']['content_changed'] # storage layout differs
    assert by_path['/large']['content_changed'] and by_path[weird]['change']=='added'
    for limit in [1,2,7]:assert collect('before','after',limit)==changes
    reverse=collect('after','before',3);assert [e['path'] for e in reverse]==[e['path'] for e in changes]
    for a,b in zip(changes,reverse):
        assert a['before_inode']==b['after_inode'] and a['after_inode']==b['before_inode']
        assert a['content_changed']==b['content_changed'] and a['metadata_changed']==b['metadata_changed']
    assert diff('before','after',changes[-1]['path'])['changes']==[]
    assert json.loads(run('fs-diff',db,'before','after'))==diff('before','after')
    for limit in ['0','65','-1','18446744073709551615','1x']:run('fs-diff',db,'before','after','',limit,ok=False)
    run('fs-diff',db,'before','after','relative','1',ok=False)
    run('fs-diff',db,'main','after',ok=False) # World names are never selected implicitly
    run('fs-diff',db,'before','after','--world','main',ok=False)
    assert json.loads(run('check',db))['last_committed_seq']==before_seq
    # RPC paging remains tied to immutable roots while the current World changes.
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def exact(s,n):
        result=b''
        while len(result)<n:
            part=s.recv(n-len(result));assert part;result+=part
        return result
    def rpc(*fields,ok=True):
        encoded=[str(x).encode() for x in fields];payload=struct.pack('<I',len(encoded))+b''.join(struct.pack('<I',len(x))+x for x in encoded)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
            header=exact(s,12);assert header[:8]==b'FFRSP001';length=struct.unpack('<I',header[8:])[0];assert length<=1024*1024
            body=exact(s,length);code=struct.unpack('<I',body[:4])[0];assert (code==0)==ok,(fields,body)
            return body[4:]
    try:
        assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
        session=json.loads(rpc('session-open',60000))['session']
        rpc('inode-root','--session',session) # diff is a read during an exclusive World mount lease
        max_page=json.loads(rpc('fs-diff','before','after','',64));assert len(max_page['changes'])==64 and not max_page['eof']
        got=[];cursor=''
        while True:
            page=json.loads(rpc('fs-diff','before','after',cursor,2));got+=page['changes']
            rpc('fs-write','/post-snapshot','ignored','--session',session)
            if page['eof']:break
            cursor=page['next_after']
        assert got==changes
        rpc('fs-diff','before','after','',65,ok=False)
        rpc('fs-diff','before','after','',64,'--revision','before',ok=False)
        rpc('fs-diff','before','after','',64,'--world','main',ok=False)
        rpc('fs-diff','main','after','',64,ok=False)
        rpc('session-close',session)
    finally:
        proc.terminate();proc.wait(timeout=10)
    run('gc',db);assert collect('before','after',2)==changes
    run('fs-check',db,'--revision','before');run('fs-check',db,'--revision','after')
    print('revision-only diff, content identity, layout equivalence, metadata, hardlinks, symlinks, pagination, RPC, restart and GC passed')
