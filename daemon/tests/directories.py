"""Bounded directory pages pinned to a CoW root and a leased session."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
BIN=sys.argv[1]
def packet(fields):
    fields=[str(x).encode() for x in fields];b=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(x))+x for x in fields)
    return b'FFRPC001'+struct.pack('<I',len(b))+b

def exact(s,n):
    b=b''
    while len(b)<n:
        p=s.recv(n-len(b));assert p;b+=p
    return b
with tempfile.TemporaryDirectory(prefix='ff-dir-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';endpoint=root/'control.sock'
    p=subprocess.run([BIN,'init',str(db)],capture_output=True,timeout=10);assert p.returncode==0,p.stderr
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def rpc(*args,ok=True):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(10);s.connect(str(endpoint));s.sendall(packet(args));h=exact(s,12);assert h[:8]==b'FFRSP001'
            b=exact(s,struct.unpack('<I',h[8:])[0]);code=struct.unpack('<I',b[:4])[0]
            assert (code==0)==ok,(args[:2],b);return b[4:]
    def open_dir(path,session,*view):return json.loads(rpc('fs-dir-open',path,*view,'--session',session))['cursor']
    def page(cursor,n,session,*view):return json.loads(rpc('fs-dir-page',cursor,n,*view,'--session',session))
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate();assert time.monotonic()<deadline;time.sleep(.01)
        rpc('fs-init');session=json.loads(rpc('session-open',60000))['session']
        initial=open_dir('/',session);rpc('fs-mkdir','/dir')
        assert page(initial,7,session)=={'names':[],'eof':True} # Flat initialization snapshot stays empty.
        rpc('fs-dir-close',initial,'--session',session)
        names=[f'f{i:04}' for i in range(1030)]+['quote"name','slash\\name']
        for name in names:rpc('fs-write','/dir/'+name,'')
        rpc('fs-symlink','/dir','/link');rpc('fs-snapshot','base');rpc('fs-fork','base','work')
        original_attrs=json.loads(rpc('fs-stat','/dir/f0000'))
        cursor=open_dir('/link',session);first=page(cursor,17,session);assert len(first['names'])==17 and not first['eof']
        rpc('fs-chmod','/dir/f0000','755');rpc('fs-write','/dir/f0000','grown')
        rpc('fs-unlink','/dir/f0020');rpc('fs-rename','/dir/f0021','/dir/renamed');rpc('fs-write','/dir/new','')
        rpc('fs-rename','/dir','/moved')
        got=list(first['names'])
        for invalid in [0,1025,-1,'1x']:rpc('fs-dir-page',cursor,invalid,'--session',session,ok=False)
        other=json.loads(rpc('session-open',60000))['session']
        rpc('fs-dir-page',cursor,1,'--session',other,ok=False)
        rpc('fs-dir-page',cursor,1,'--world','work','--session',session,ok=False)
        while True:
            p=page(cursor,1024,session);assert len(p['names'])<=1024;got+=p['names']
            if p['eof']:break
        assert got==sorted(names) and len(set(got))==len(names)
        assert page(cursor,1,session)=={'names':[],'eof':True}
        rpc('fs-dir-rewind',cursor,'--session',session)
        assert page(cursor,17,session)==first
        rpc('fs-dir-rewind',cursor,'--session',session)
        attrs=json.loads(rpc('fs-dir-entries',cursor,1024,'--session',session))
        assert len(attrs['entries'])==1024 and not attrs['eof']
        by_name={e['name']:e['inode'] for e in attrs['entries']}
        assert by_name['f0000']==original_attrs and by_name['f0020']['links']==1
        assert json.loads(rpc('fs-stat','/moved/f0000'))['size']==5
        remaining=json.loads(rpc('fs-dir-entries',cursor,1024,'--session',session))
        assert remaining['eof'] and [e['name'] for e in remaining['entries']]==sorted(names)[1024:]
        assert json.loads(rpc('fs-dir-entries',cursor,1,'--session',session))=={'entries':[],'eof':True}
        rpc('fs-dir-rewind',cursor,'--session',session)
        # Name-only and attribute pages share the same continuation.
        assert page(cursor,1,session)['names']==['f0000']
        assert json.loads(rpc('fs-dir-entries',cursor,1,'--session',session))['entries'][0]['name']=='f0001'
        rpc('fs-dir-entries',cursor,1,'--session',other,ok=False)

        old=open_dir('/dir',session,'--revision','base');assert page(old,1024,session,'--revision','base')['names']==sorted(names)[:1024]
        fresh=open_dir('/moved',session);live=[]
        while True:
            p=page(fresh,100,session);live+=p['names']
            if p['eof']:break
        assert live==sorted(set(names)-{'f0020','f0021'}|{'renamed','new'})
        rpc('fs-mkdir','/empty');empty=open_dir('/empty',session);rpc('fs-rmdir','/empty')
        assert page(empty,1,session)=={'names':[],'eof':True}
        rpc('fs-dir-open','/moved/f0000','--session',session,ok=False)
        rpc('fs-dir-open','/moved',ok=False)
        rpc('fs-dir-close',cursor,'--session',session);rpc('fs-dir-page',cursor,1,'--session',session,ok=False)
        rpc('fs-mkdir','/typed');rpc('fs-write','/typed/file','data');rpc('fs-link','/typed/file','/typed/alias')
        rpc('fs-symlink','file','/typed/symlink');rpc('fs-mkdir','/typed/subdir')
        typed=open_dir('/typed',session);types=json.loads(rpc('fs-dir-entries',typed,1024,'--session',session))
        typed_entries={e['name']:e['inode'] for e in types['entries']}
        assert typed_entries['file']['inode_id']==typed_entries['alias']['inode_id']
        assert typed_entries['file']['links']==2 and typed_entries['file']['size']==4
        assert typed_entries['symlink']['symlink'] and typed_entries['symlink']['mode']==0o777
        assert typed_entries['subdir']['directory'] and not typed_entries['subdir']['symlink']
        rpc('fs-dir-close',typed,'--session',session)
        # Quota can be reused after closing streams; session end drops all streams.
        for token in [old,fresh,empty]:rpc('fs-dir-close',token,'--revision','base','--session',session) if token==old else rpc('fs-dir-close',token,'--session',session)
        tokens=[open_dir('/moved',session) for _ in range(64)]
        rpc('fs-dir-open','/moved','--session',session,ok=False)
        rpc('fs-dir-close',tokens[0],'--session',session);open_dir('/moved',session)
        rpc('session-close',session);rpc('session-close',other)
        short=json.loads(rpc('session-open',100))['session'];expired=open_dir('/moved',short);time.sleep(.25)
        rpc('fs-dir-page',expired,1,'--session',short,ok=False)
        rpc('fs-check');rpc('fs-check','--revision','base')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0,proc.communicate()
        print('bounded pinned directory pages, mutation isolation, JSON escaping and session lifecycle passed')
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
