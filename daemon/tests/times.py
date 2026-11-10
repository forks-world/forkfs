"""Atomic timestamp updates survive snapshots, crashes and orphan-handle I/O."""
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
BIN, LEGACY, FAULT = sys.argv[1:4]
for binary in [BIN, LEGACY]:
    def run(*args, ok=True, program=binary, env=None):
        p = subprocess.run([program, *map(str,args)], capture_output=True, timeout=20, env=env)
        assert (p.returncode==0)==ok, (args,p.stderr)
        return p.stdout
    with tempfile.TemporaryDirectory(prefix='ff-times-',dir='/tmp') as tmp:
        root=Path(tmp);db=root/'db';data=root/'input';data.write_bytes(b'content')
        run('init',db);run('fs-init',db);run('fs-write',db,'/f',data);run('fs-link',db,'/f','/alias')
        def stat(*view):return json.loads(run('fs-stat',db,'/f',*view))
        def sequence():return json.loads(run('check',db))['last_committed_seq']
        run('fs-settimes',db,'/f',111,222,333);run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
        before=sequence();run('fs-settimes',db,'/alias',0,0,0);assert sequence()==before+1
        assert [stat()[k] for k in ['accessed_ns','modified_ns','birth_ns']]==[0,0,0]
        assert [stat('--revision','base')[k] for k in ['accessed_ns','modified_ns','birth_ns']]==[111,222,333]
        assert stat('--world','work')['birth_ns']==333
        before=sequence();run('fs-settimes',db,'/f','-','-','-');run('fs-cat',db,'/f');assert sequence()==before
        for args in [('-1','-','-'),('1x','-','-'),('1','2','9223372036854775808'),('18446744073709551616','-','-')]:
            run('fs-settimes',db,'/f',*args,ok=False)
        run('fs-settimes',db,'/f',1,2,3,'--revision','base',ok=False);assert sequence()==before
        run('fs-settimes',db,'/f',9223372036854775807,'-',9223372036854775807)
        assert stat()['accessed_ns']==9223372036854775807 and stat()['birth_ns']==9223372036854775807
        run('fs-settimes',db,'/f',0,0,0)
        if binary==BIN:
            baseline=root/'baseline';shutil.copytree(db,baseline)
            for stage in ['leveldb_before_write','leveldb_after_write']:
                shutil.rmtree(db);shutil.copytree(baseline,db)
                p=subprocess.run([FAULT,'fs-settimes',str(db),'/f','111','222','333'],capture_output=True,timeout=20,env={**os.environ,'FORKFS_TEST_CRASH':stage})
                assert p.returncode==86,p.stderr;committed=stage=='leveldb_after_write'
                assert [stat()[k] for k in ['accessed_ns','modified_ns','birth_ns']]==([111,222,333] if committed else [0,0,0]);run('fs-check',db)
            endpoint=root/'control.sock';proc=subprocess.Popen([binary,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                assert select.select([proc.stdout],[],[],5)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
                def rpc(*args,ok=True):return run('request',endpoint,*args,ok=ok)
                session=json.loads(rpc('session-open',60000))['session'];sel=('--session',session)
                handle=json.loads(rpc('fs-open','/f','r',*sel))['handle']
                rpc('fs-hsettimes',handle,444,'-',555,*sel);attrs=json.loads(rpc('fs-hstat',handle,*sel));assert attrs['accessed_ns']==444 and attrs['birth_ns']==555
                rpc('fs-unlink','/f');rpc('fs-unlink','/alias');rpc('fs-hsettimes',handle,0,1,2,*sel)
                attrs=json.loads(rpc('fs-hstat',handle,*sel));assert attrs['links']==0 and attrs['birth_ns']==2
                assert rpc('fs-hread',handle,0,7,*sel)==b'content'
                old=json.loads(rpc('fs-open','/f','r','--revision','base',*sel))['handle']
                rpc('fs-hsettimes',old,1,2,3,'--revision','base',*sel,ok=False)
                rpc('fs-hsettimes',handle,1,2,3,'--world','work',*sel,ok=False)
                rpc('session-close',session)
                run('request',endpoint,'fs-check')
            finally:
                proc.terminate();proc.wait(timeout=10)
            run('fs-restore',db,'base','safety');run('gc',db)
        run('fs-check',db);run('fs-check',db,'--revision','base');assert stat('--revision','base')['birth_ns']==333
print('CLI/RPC timestamps, partial requests, boundaries, atomic crashes, handles, revisions, restore, GC and legacy compatibility passed')
