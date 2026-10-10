"""Inode mode/mtime, hardlinks, revisions, orphan handles and recovery."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-attr-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input';payload=bytes(range(256))*1024;data.write_bytes(payload)
    run('init',db);run('fs-init',db);run('fs-write',db,'/file',data);run('fs-link',db,'/file','/alias')
    run('fs-symlink',db,'/file','/link');run('fs-mkdir',db,'/dir');run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
    def stat(path,*view):return json.loads(run('fs-stat',db,path,*view))
    original=stat('/file');run('fs-chmod',db,'/link','0755');current=stat('/alias')
    assert current['mode']==0o755 and current['modified_ns']==original['modified_ns']
    assert current['changed_ns']>=original['changed_ns'] and current['inode_id']==original['inode_id']
    assert json.loads(run('fs-lstat',db,'/link'))['mode']==0o777
    run('fs-setmtime',db,'/alias','123456789');assert stat('/file')['modified_ns']==123456789
    assert stat('/file')['size']==len(payload) and run('fs-cat',db,'/file')==payload
    run('fs-chmod',db,'/file','0000');assert stat('/file')['mode']==0
    run('fs-setmtime',db,'/file','0');assert stat('/file')['modified_ns']==0
    run('fs-chmod',db,'/dir','0700');run('fs-setmtime',db,'/dir','42');assert stat('/dir')['mode']==0o700
    assert stat('/file','--revision','base')['mode']==original['mode']
    assert stat('/file','--world','work')['modified_ns']==original['modified_ns']
    run('fs-chmod',db,'/file','0711','--world','work');assert stat('/file','--world','work')['mode']==0o711
    before=json.loads(run('check',db))['last_committed_seq']
    for mode in ['888','1000','-1','0x755','777777777777777777777777']:
        run('fs-chmod',db,'/file',mode,ok=False)
    for stamp in ['-1','9223372036854775808','1x']:run('fs-setmtime',db,'/file',stamp,ok=False)
    run('fs-chmod',db,'/missing','755',ok=False)
    run('fs-chmod',db,'/file','755','--revision','base',ok=False)
    assert json.loads(run('check',db))['last_committed_seq']==before
    baseline=root/'baseline';shutil.copytree(db,baseline)
    for stage in ['leveldb_before_write','leveldb_after_write']:
        for op,value in [('fs-chmod','755'),('fs-setmtime','777')]:
            shutil.rmtree(db);shutil.copytree(baseline,db)
            p=subprocess.run([FAULT,op,str(db),'/file',value],capture_output=True,timeout=10,
                             env={**os.environ,'FORKFS_TEST_CRASH':stage})
            assert p.returncode==86,p.stderr
            attrs=stat('/file');committed=stage=='leveldb_after_write'
            assert attrs['mode']==(0o755 if committed and op=='fs-chmod' else 0)
            assert attrs['modified_ns']==(777 if committed and op=='fs-setmtime' else 0)
            assert run('fs-cat',db,'/file')==payload;run('fs-check',db)
    endpoint=root/'control.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate();assert time.monotonic()<deadline;time.sleep(.01)
        def rpc(*args,ok=True):return run('request',endpoint,*args,ok=ok)
        session=json.loads(rpc('session-open','60000'))['session']
        readonly=json.loads(rpc('fs-open','/file','r','--session',session))['handle']
        rpc('fs-hchmod',readonly,'0750','--session',session)
        rpc('fs-hsetmtime',readonly,'987','--session',session)
        assert json.loads(rpc('fs-hstat',readonly,'--session',session))['mode']==0o750
        rpc('fs-unlink','/file');rpc('fs-unlink','/alias')
        rpc('fs-hchmod',readonly,'0700','--session',session);rpc('fs-hsetmtime',readonly,'0','--session',session)
        attrs=json.loads(rpc('fs-hstat',readonly,'--session',session));assert attrs['links']==0 and attrs['modified_ns']==0
        assert rpc('fs-hread',readonly,'0',len(payload),'--session',session)==payload
        old=json.loads(rpc('fs-open','/file','r','--revision','base','--session',session))['handle']
        rpc('fs-hchmod',old,'755','--revision','base','--session',session,ok=False)
        rpc('fs-hsetmtime',old,'1','--revision','base','--session',session,ok=False)
        rpc('fs-hchmod',readonly,'777','--world','work','--session',session,ok=False)
        rpc('fs-check');rpc('session-close',session);rpc('fs-check')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0,proc.communicate()
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
    run('fs-check',db);assert run('fs-cat',db,'/file','--revision','base')==payload
    print('inode attributes, no content changes, branches, orphan handles and crash atomicity passed')
