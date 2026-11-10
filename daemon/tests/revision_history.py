"""Immutable parent-chain history, branch ancestry and bounded resumable pages."""
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
BIN,LEGACY=sys.argv[1:3]
for binary in [BIN,LEGACY]:
    def run(*args,ok=True):
        p=subprocess.run([binary,*map(str,args)],capture_output=True,timeout=20)
        assert (p.returncode==0)==ok,(args,p.stderr)
        return p.stdout
    with tempfile.TemporaryDirectory(prefix='ff-dir-history-',dir='/tmp') as tmp:
        root=Path(tmp);db=root/'db';data=root/'input';endpoint=root/'control.sock'
        run('init',db);run('fs-init',db)
        def sequence():return json.loads(run('check',db))['last_committed_seq']
        def snapshot(name,*view):return json.loads(run('fs-snapshot',db,name,*view))['revision_id']
        def page(tip,after='',limit=64):return json.loads(run('fs-history',db,tip,after,limit))
        def collect(tip,limit=64):
            entries=[];cursor='';tip_id=None
            while True:
                result=page(tip,cursor,limit);assert len(result['entries'])<=limit
                if tip_id is None:tip_id=result['tip_revision_id']
                assert result['tip_revision_id']==tip_id;entries+=result['entries']
                if result['eof']:assert result['next_after']=='';break
                assert result['entries'] and result['next_after']==result['entries'][-1]['revision_id'] and result['next_after']!=cursor
                cursor=result['next_after']
            assert len({e['revision_id'] for e in entries})==len(entries)
            for i,e in enumerate(entries):
                assert len(e['manifest_id'])==64
                assert e['parent_revision_id']==(entries[i+1]['revision_id'] if i+1<len(entries) else None)
                if i+1<len(entries):assert e['captured_sequence']>entries[i+1]['captured_sequence']
            return entries
        captured=sequence();r0=snapshot('r0');initial=page('r0')
        assert initial['tip_revision_id']==r0 and initial['eof'] and len(initial['entries'])==1
        assert initial['entries'][0]['revision_id']==r0 and initial['entries'][0]['parent_revision_id'] is None
        assert initial['entries'][0]['captured_sequence']==captured
        assert json.loads(run('fs-history',db,'r0'))==initial
        run('fs-fork',db,'r0','work')
        data.write_bytes(b'main');run('fs-write',db,'/file',data);main1=snapshot('main1')
        data.write_bytes(b'work');run('fs-write',db,'/file',data,'--world','work');w1=snapshot('w1','--world','work')
        w2=snapshot('w2','--world','work') # no namespace change, but a distinct committed revision
        run('fs-fork',db,'w1','child');c1=snapshot('c1','--world','child')
        assert [e['revision_id'] for e in collect('main1')]==[main1,r0]
        work=collect('w2');assert [e['revision_id'] for e in work]==[w2,w1,r0]
        assert work[0]['manifest_id']==work[1]['manifest_id']
        assert [e['revision_id'] for e in collect('c1')]==[c1,w1,r0]
        assert page('w2',w1,1)['entries']==[work[2]]
        assert page('w2',r0)['entries']==[] and page('w2',r0)['eof']
        # More than a default page: order follows parents, not label sorting.
        expected=[main1,r0]
        for i in range(70):expected.insert(0,snapshot(f'chain_{i:03}'))
        all_entries=collect('chain_069');assert [e['revision_id'] for e in all_entries]==expected
        assert len(page('chain_069')['entries'])==64 and not page('chain_069')['eof']
        before=sequence()
        for limit in [1,2,7,64]:assert collect('chain_069',limit)==all_entries
        assert sequence()==before
        for cursor in ['short','g'*64,'F'*64,'0'*64,w1,c1]:run('fs-history',db,'chain_069',cursor,64,ok=False)
        for limit in ['0','65','-1','18446744073709551615','1x']:run('fs-history',db,'r0','',limit,ok=False)
        run('fs-history',db,'main',ok=False) # a mutable World is not a named revision
        run('fs-history',db,'r0','--world','main',ok=False)
        run('fs-history',db,'r0','--revision','r0',ok=False)
        run('fs-history',db,'',ok=False);assert sequence()==before
        if binary==BIN:
            proc=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            def exact(s,n):
                result=b''
                while len(result)<n:
                    part=s.recv(n-len(result));assert part;result+=part
                return result
            def rpc(*fields,error=None):
                fields=[str(v).encode() for v in fields];payload=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(20);s.connect(str(endpoint));s.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
                    header=exact(s,12);assert header[:8]==b'FFRSP001';size=struct.unpack('<I',header[8:])[0];assert size<=1024*1024
                    body=exact(s,size);code=struct.unpack('<I',body[:4])[0];assert code==(0 if error is None else 1),(fields,code,body)
                    if error is not None:assert error.encode() in body[4:],(error,body)
                    return body[4:]
            try:
                assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
                session=json.loads(rpc('session-open',60000))['session'];rpc('inode-root','--session',session)
                assert json.loads(rpc('fs-history','r0','',64,'--session',session))==initial
                rpc('fs-history','r0','',64,'--session','missing',error='invalid or expired session')
                cursor='';received=[]
                while True:
                    result=json.loads(rpc('fs-history','chain_069',cursor,7));received+=result['entries']
                    rpc('fs-write','/later','new committed World content','--session',session)
                    if result['eof']:break
                    cursor=result['next_after']
                assert received==all_entries
                new_id=json.loads(rpc('fs-snapshot','new-tip'))['revision_id']
                newest=json.loads(rpc('fs-history','new-tip','',1))['entries'][0]
                assert newest['revision_id']==new_id and newest['parent_revision_id']==expected[0]
                assert json.loads(rpc('fs-history','chain_069','',64))['entries']==all_entries[:64]
                for cursor in ['bad','0'*64,w1]:rpc('fs-history','chain_069',cursor,64,error='invalid history cursor')
                rpc('fs-history','r0','',65,error='invalid history page limit')
                rpc('fs-history','main','',64,error='object root not found')
                rpc('fs-history','r0','',64,'--world','main',error='invalid view selector')
                rpc('fs-history','r0','',64,'--revision','r0',error='invalid view selector')
                rpc('session-close',session)
            finally:
                proc.terminate();proc.wait(timeout=10)
            run('gc',db);assert collect('chain_069',7)==all_entries and collect('w2')==work
        run('fs-check',db,'--revision','r0');run('fs-check',db,'--revision','w2');run('check',db)
print('revision ancestry, fork isolation, unchanged snapshots, paging, read-only queries, RPC leases, restart, GC and legacy compatibility passed')
