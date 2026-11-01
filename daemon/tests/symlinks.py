"""World-local symbolic links, physical traversal and namespace publication."""
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
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-symlink-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input'
    run('init',db);run('fs-init',db);run('fs-mkdir',db,'/src');run('fs-mkdir',db,'/src/sub')
    data.write_bytes(b'original');run('fs-write',db,'/src/file',data)
    original=json.loads(run('fs-stat',db,'/src/file'))['inode_id']
    run('fs-symlink',db,'file','/src/relative');run('fs-symlink',db,'/src/file','/absolute')
    run('fs-symlink',db,'../file','/src/sub/up');run('fs-symlink',db,'/src','/dir')
    run('fs-symlink',db,'../../../src/file','/root-clamp')
    for p in ['/src/relative','/absolute','/src/sub/up','/dir/file','/root-clamp']:
        assert run('fs-cat',db,p)==b'original'
        assert json.loads(run('fs-stat',db,p))['inode_id']==original
    assert json.loads(run('fs-lstat',db,'/absolute'))['symlink']
    assert run('fs-readlink',db,'/src/sub/up')==b'../file'
    run('fs-link',db,'/absolute','/absolute-alias')
    assert json.loads(run('fs-lstat',db,'/absolute'))['links']==2
    run('fs-unlink',db,'/absolute');assert run('fs-readlink',db,'/absolute-alias')==b'/src/file'
    run('fs-symlink',db,'missing','/src/dangling');run('fs-cat',db,'/src/dangling',ok=False)
    run('fs-symlink',db,'/loop-b','/loop-a');run('fs-symlink',db,'/loop-a','/loop-b')
    run('fs-cat',db,'/loop-a',ok=False);assert run('fs-readlink',db,'/loop-a')==b'/loop-b'
    run('fs-symlink',db,'file/','/src/trailing');run('fs-cat',db,'/src/trailing',ok=False)
    run('fs-symlink',db,'./sub/../file','/src/dotted');assert run('fs-cat',db,'/src/dotted')==b'original'
    run('fs-symlink',db,'/','/world-root');assert 'src' in run('fs-ls',db,'/world-root').decode().splitlines()
    run('fs-mkdir',db,'/dir/new-dir');assert json.loads(run('fs-stat',db,'/src/new-dir'))['directory']
    # Physical ancestry detects a directory rename through its own symlink.
    run('fs-rename',db,'/src','/dir/inside',ok=False)
    # Host absolute paths cannot escape the selected World's root.
    host=root/'host-secret';host.write_bytes(b'host-only')
    run('fs-symlink',db,str(host),'/host-link');run('fs-cat',db,'/host-link',ok=False)
    run('fs-write',db,'/host-link',data,ok=False);assert host.read_bytes()==b'host-only'
    run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
    data.write_bytes(b'branch');run('fs-write',db,'/src/relative',data,'--world','work')
    assert run('fs-cat',db,'/src/file','--world','work')==b'branch'
    assert run('fs-cat',db,'/src/file')==b'original'
    assert run('fs-cat',db,'/absolute-alias','--revision','base')==b'original'
    assert run('fs-readlink',db,'/src/relative','--world','work')==b'file'
    run('fs-symlink',db,'/src/file','/readonly','--revision','base',ok=False)
    run('fs-rename',db,'/src/relative','/src/moved-link')
    assert run('fs-cat',db,'/src/moved-link')==b'original'
    run('fs-unlink',db,'/src/moved-link');assert run('fs-cat',db,'/src/file')==b'original'
    before=json.loads(run('check',db))['last_committed_seq']
    for target in ['', 'x\n', 'x'*4097, 'a'*256]:run('fs-symlink',db,target,'/bad',ok=False)
    run('fs-readlink',db,'/src/file',ok=False)
    assert json.loads(run('check',db))['last_committed_seq']==before
    run('fs-check',db);run('fs-check',db,'--revision','base');run('fs-check',db,'--world','work')
    baseline=root/'baseline';shutil.copytree(db,baseline)
    for stage in ['leveldb_before_write','leveldb_after_write']:
        shutil.rmtree(db);shutil.copytree(baseline,db)
        p=subprocess.run([FAULT,'fs-symlink',str(db),'/src/file','/crash-link'],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,p.stderr
        run('fs-lstat',db,'/crash-link',ok=stage=='leveldb_after_write');run('fs-check',db)
    endpoint=root/'control.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate();assert time.monotonic()<deadline;time.sleep(.01)
        def rpc(*args,ok=True):return run('request',endpoint,*args,ok=ok)
        rpc('fs-symlink','/src/file','/rpc-link');assert rpc('fs-readlink','/rpc-link')==b'/src/file'
        assert json.loads(rpc('fs-lstat','/rpc-link'))['symlink']
        session=json.loads(rpc('session-open','60000'))['session']
        handle=json.loads(rpc('fs-open','/rpc-link','rw','--session',session))['handle']
        rpc('fs-unlink','/rpc-link');data.write_bytes(b'via-handle')
        rpc('fs-hwrite',handle,'0',data,'--session',session)
        assert rpc('fs-cat','/src/file')==b'via-handle'
        rpc('fs-open','/src/dangling','rw,create,exclusive','--session',session,ok=False)
        before=json.loads(rpc('status'))['last_committed_seq']
        rpc('fs-open','/src/dangling','rw,create,nofollow','--session',session,ok=False)
        rpc('fs-open','/absolute-alias','r,nofollow','--session',session,ok=False)
        assert json.loads(rpc('status'))['last_committed_seq']==before
        created=json.loads(rpc('fs-open','/src/dangling','rw,create','--session',session))['handle']
        assert rpc('fs-readlink','/src/dangling')==b'missing'
        assert rpc('fs-cat','/src/missing')==b''
        rpc('fs-hwrite-next',created,data,'--session',session)
        assert rpc('fs-cat','/src/dangling')==b'via-handle'
        rpc('fs-cat','/src/dangling','--revision','base',ok=False)
        rpc('fs-cat','/src/dangling','--world','work',ok=False)
        prefix=json.loads(rpc('fs-open','/dir/file','r,nofollow','--session',session))['handle']
        rpc('fs-close',prefix,'--session',session)
        rpc('fs-symlink','/src/new-target','/chain-end');rpc('fs-symlink','chain-end','/chain-start')
        rpc('fs-open','/chain-start','rw,create','--session',session)
        assert rpc('fs-cat','/src/new-target')==b''
        rpc('fs-symlink','/absent-parent/file','/bad-parent')
        rpc('fs-open','/bad-parent','rw,create','--session',session,ok=False)
        rpc('fs-symlink','/src/trailing-target/','/bad-trailing')
        rpc('fs-open','/bad-trailing','rw,create','--session',session,ok=False)
        rpc('fs-stat','/src/trailing-target',ok=False)
        rpc('session-close',session);rpc('fs-check')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0,proc.communicate()
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
    run('fs-check',db)
    print('symlink traversal, no-follow operations, branches, RPC and crash publication passed')
