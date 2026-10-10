"""Single-transaction file replacement preserves old inode/handle semantics."""
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
with tempfile.TemporaryDirectory(prefix='ff-save-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input'
    run('init',db);run('fs-init',db);data.write_bytes(b'old');run('fs-write',db,'/file',data)
    run('fs-chmod',db,'/file','755');run('fs-link',db,'/file','/alias');run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
    old=json.loads(run('fs-stat',db,'/file'));before=json.loads(run('check',db))['last_committed_seq']
    data.write_bytes(b'new\0data');run('fs-save',db,'/file',data)
    new=json.loads(run('fs-stat',db,'/file'));assert new['inode_id']!=old['inode_id'] and new['mode']==old['mode']
    assert new['links']==1 and json.loads(run('fs-stat',db,'/alias'))['links']==1
    assert run('fs-cat',db,'/alias')==b'old' and run('fs-cat',db,'/file')==data.read_bytes()
    assert json.loads(run('check',db))['last_committed_seq']==before+1
    assert run('fs-cat',db,'/file','--world','work')==b'old'
    assert run('fs-cat',db,'/file','--revision','base')==b'old'
    run('fs-symlink',db,'/alias','/link');run('fs-save',db,'/link',data)
    assert not json.loads(run('fs-lstat',db,'/link'))['symlink'] and run('fs-cat',db,'/alias')==b'old'
    run('fs-mkdir',db,'/dir');run('fs-save',db,'/dir',data,ok=False)
    run('fs-save',db,'/file',data,'--revision','base',ok=False)
    run('fs-save',db,'/missing/child',data,ok=False)
    data.write_bytes(bytes(256*1024+1));run('fs-save',db,'/file',data,ok=False)
    data.write_bytes(b'crash-value');baseline=root/'baseline';shutil.copytree(db,baseline)
    for stage in ['leveldb_before_write','leveldb_after_write']:
        shutil.rmtree(db);shutil.copytree(baseline,db)
        p=subprocess.run([FAULT,'fs-save',str(db),'/file',str(data)],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,p.stderr
        assert run('fs-cat',db,'/file')==(b'crash-value' if stage=='leveldb_after_write' else b'new\0data')
        assert run('fs-cat',db,'/alias')==b'old';run('fs-check',db)
    endpoint=root/'control.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate();assert time.monotonic()<deadline;time.sleep(.01)
        def rpc(*args,ok=True):return run('request',endpoint,*args,ok=ok)
        session=json.loads(rpc('session-open',60000))['session']
        h=json.loads(rpc('fs-open','/file','rw','--session',session))['handle']
        previous=json.loads(rpc('fs-hstat',h,'--session',session))
        metrics_before=json.loads(rpc('status'));profile_before=json.loads(rpc('storage-profile'))
        data.write_bytes(b'latest');rpc('fs-save','/file',data)
        metrics_after=json.loads(rpc('status'))
        a=metrics_before['storage_metrics'];b=metrics_after['storage_metrics']
        assert b['transactions']==a['transactions']+1 and b['wal_sync_calls']==a['wal_sync_calls']+1
        assert b['wal_append_bytes']>a['wal_append_bytes'] and b['write_ns']>a['write_ns']
        assert metrics_after['metadata_prepare_ns']>metrics_before['metadata_prepare_ns']
        profile_after=json.loads(rpc('storage-profile'))
        pa=profile_before['storage_metrics'];pb=profile_after['storage_metrics']
        assert pb['root_get_calls']>pa['root_get_calls'] and pb['root_misses']>pa['root_misses']
        assert pb['object_get_calls']>pa['object_get_calls'] and pb['object_misses']>pa['object_misses']
        assert 'root_get_calls' not in metrics_after['storage_metrics']
        rpc('fs-cat','/file');rpc('fs-cat','/file')
        assert json.loads(rpc('storage-profile'))['storage_metrics']['object_cache_hits']>pb['object_cache_hits']
        assert rpc('fs-cat','/file')==b'latest' and rpc('fs-hread',h,0,100,'--session',session)==b'crash-value'
        attrs=json.loads(rpc('fs-hstat',h,'--session',session));assert attrs['inode_id']==previous['inode_id'] and attrs['links']==0
        rpc('fs-hwrite',h,0,data,'--session',session);assert rpc('fs-cat','/file')==b'latest'
        rpc('fs-check');rpc('session-close',session);rpc('fs-check')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0,proc.communicate()
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
    run('fs-check',db)
    print('atomic replacement, hardlink/mode/revision retention, orphan handles and crash recovery passed')
