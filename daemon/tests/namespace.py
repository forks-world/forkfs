"""Real namespace behavior, stable identities, atomic rename and restart."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout

def stat(image,path):return json.loads(run('fs-stat',image,path))
def check(image):run('fs-check',image)
with tempfile.TemporaryDirectory(prefix='ffn-',dir='/tmp') as tmp:
    root=Path(tmp);image=root/'store';value=root/'value'
    run('init',image)
    run('fs-ls',image,'/',ok=False)
    run('fs-init',image)
    rootid=stat(image,'/')['inode_id']
    run('fs-init',image,ok=False)
    run('fs-mkdir',image,'/a');run('fs-mkdir',image,'/b')
    assert stat(image,'/')['links']==4
    value.write_bytes(b'hello\x00world')
    run('fs-write',image,'/a/file',value)
    fileid=stat(image,'/a/file')['inode_id']
    # Dots and redundant separators are traversed physically, without lexical
    # collapse across a symlink, and successful writes use the resolved parent.
    run('fs-mkdir',image,'/a/sub')
    run('fs-symlink',image,'/a/sub','/alias')
    run('fs-symlink',image,'file','/a/final-link')
    assert stat(image,'/a/./final-link')['inode_id']==fileid
    assert json.loads(run('fs-lstat',image,'/a/./final-link'))['symlink']
    run('fs-unlink',image,'/a/final-link')
    assert stat(image,'/a//./file')['inode_id']==fileid
    assert stat(image,'/alias/../file')['inode_id']==fileid
    assert stat(image,'/a/.')['directory'] and stat(image,'/a/..')['inode_id']==rootid
    assert stat(image,'/../../a/file')['inode_id']==fileid
    run('fs-write',image,'/a//./file',value)
    run('fs-mkdir',image,'/a/./dotted')
    run('fs-mkdir',image,'/a/dotted/../created')
    assert stat(image,'/a/created')['directory']
    run('fs-rename',image,'/a/created','/a/dotted/../renamed')
    assert stat(image,'/a/renamed')['directory']
    run('fs-rmdir',image,'/a/renamed')
    run('fs-rmdir',image,'/a/dotted')
    run('fs-unlink',image,'/alias');run('fs-rmdir',image,'/a/sub')
    assert run('fs-cat',image,'/a/file')==value.read_bytes()
    # Exact raw path bytes are checked before component processing, including
    # redundant separators and dot components.
    run('fs-snapshot',image,'dots-base')
    exact='/'+'./'*2044+'/a/file'
    assert len(exact)==4096 and stat(image,exact)['inode_id']==fileid
    oversized=exact+'/'
    assert len(oversized)==4097
    result=subprocess.run([BIN,'fs-stat',str(image),oversized],capture_output=True,timeout=10)
    assert result.returncode!=0 and b'4096' in result.stderr,(result.returncode,result.stderr)
    run('fs-cat',image,'/../../a/./file','--revision','dots-base')
    readonly_seq=json.loads(run('check',image))['last_committed_seq']
    run('fs-write',image,'/a/../readonly-fail',value,'--revision','dots-base',ok=False)
    run('fs-mkdir',image,'/a/../readonly-fail-dir','--revision','dots-base',ok=False)
    assert json.loads(run('check',image))['last_committed_seq']==readonly_seq
    assert stat(image,'/../../a/./file')['inode_id']==fileid
    assert json.loads(run('fs-stat',image,'/../../a/./file','--revision','dots-base'))['inode_id']==fileid
    run('fs-link',image,'/a/file','/b/alias')
    assert stat(image,'/b/alias')['inode_id']==fileid
    assert stat(image,'/a/file')['links']==2
    value.write_bytes(b'updated')
    run('fs-write',image,'/b/alias',value)
    assert run('fs-cat',image,'/a/file')==b'updated'
    run('fs-rename',image,'/a/file','/b/alias') # same inode: POSIX no-op
    assert stat(image,'/a/file')['links']==2
    run('fs-rename',image,'/a/file','/b/moved')
    run('fs-stat',image,'/a/file',ok=False)
    assert stat(image,'/b/moved')['inode_id']==fileid
    run('fs-unlink',image,'/b/alias')
    assert stat(image,'/b/moved')['links']==1
    run('checkpoint',image);check(image)
    assert stat(image,'/')['inode_id']==rootid
    assert run('fs-cat',image,'/b/moved')==b'updated'
    run('fs-mkdir',image,'/a/sub');run('fs-mkdir',image,'/b/empty')
    assert stat(image,'/a')['links']==3 and stat(image,'/b')['links']==3
    subid=stat(image,'/a/sub')['inode_id']
    run('fs-rename',image,'/a/sub','/b/empty')
    assert stat(image,'/b/empty')['inode_id']==subid
    assert stat(image,'/a')['links']==2 and stat(image,'/b')['links']==3
    check(image)
    # Replacing another hard-linked file only removes its replaced name.
    value.write_bytes(b'victim');run('fs-write',image,'/a/victim',value)
    run('fs-link',image,'/a/victim','/a/survivor')
    run('fs-rename',image,'/b/moved','/a/victim')
    assert stat(image,'/a/victim')['inode_id']==fileid
    assert stat(image,'/a/survivor')['links']==1
    assert run('fs-cat',image,'/a/survivor')==b'victim'
    check(image)
    # Failed operations must not append data or damage namespace.
    run('fs-mkdir',image,'/b/empty/child')
    before=image.read_bytes()
    for args in [
        ('fs-rename','/b','/b/empty/child/self'),
        ('fs-rename','/a','/b/empty'),
        ('fs-rename','/a/victim','/b/empty'),
        ('fs-link','/b','/a/dirlink'),
        ('fs-rmdir','/a'),('fs-unlink','/b'),('fs-unlink','/'),
        ('fs-mkdir','/a/trailing/'),('fs-mkdir','/a/\ninvalid'),
        ('fs-mkdir','/a/../'),('fs-mkdir','/a/.'),('fs-mkdir','/a/..'),
        ('fs-write','/b/empty',str(value)),('fs-mkdir','/missing/child'),
        ('fs-mkdir','/missing/../child'),('fs-mkdir','/b/moved/../child'),
    ]:
        run(args[0],image,*args[1:],ok=False)
        assert image.read_bytes()==before,args
    value.write_bytes(bytes(256*1024+1));run('fs-write',image,'/oversized',value,ok=False)
    assert image.read_bytes()==before
    # Reserved binary metadata namespace cannot be mutated via generic object API.
    batch=root/'batch';key=b'\0forkfs/root'
    batch.write_bytes(b'FFBATCH1'+struct.pack('<I',1)+struct.pack('<III',1,len(key),1)+key+b'x')
    seq=json.loads(run('check',image))['last_committed_seq']
    run('object-batch',image,seq,batch,ok=False)
    assert image.read_bytes()==before
    # Publish atomic overwrite rename across directories, then crash at each boundary.
    value.write_bytes(b'source');run('fs-write',image,'/a/source',value)
    value.write_bytes(b'target');run('fs-write',image,'/b/target',value)
    # Isolate rename publication from an offline threshold compaction transaction.
    run('fs-compact',image)
    run('checkpoint',image);baseline=image.read_bytes()
    sourceid=stat(image,'/a/source')['inode_id'];targetid=stat(image,'/b/target')['inode_id']
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-rename',str(image),'/a/source','/b/target'],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        if stage.startswith('commit_'):
            run('fs-stat',image,'/a/source',ok=False)
            assert stat(image,'/b/target')['inode_id']==sourceid
            assert run('fs-cat',image,'/b/target')==b'source'
        else:
            assert stat(image,'/a/source')['inode_id']==sourceid
            assert stat(image,'/b/target')['inode_id']==targetid
            assert run('fs-cat',image,'/b/target')==b'target'
        check(image)
        value.write_bytes(b'continued');run('fs-write',image,'/continued',value)
        run('checkpoint',image);check(image)
    image.write_bytes(baseline)
    run('fs-unlink',image,'/a/source');run('fs-unlink',image,'/b/target')
    run('fs-unlink',image,'/a/victim');run('fs-unlink',image,'/a/survivor')
    run('fs-rmdir',image,'/b/empty/child');run('fs-rmdir',image,'/b/empty')
    run('fs-rmdir',image,'/a');run('fs-rmdir',image,'/b')
    assert run('fs-ls',image,'/')==b'' and stat(image,'/')['links']==2
    run('checkpoint',image);check(image)
    print('namespace CRUD, hardlinks, stable identity, atomic rename and crash recovery passed')
