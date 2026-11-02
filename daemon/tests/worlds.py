"""Retained revisions, shared forks and isolated namespace changes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout

def cat(image,path,*view):return run('fs-cat',image,path,*view)
def check(image,*view):run('fs-check',image,*view)
with tempfile.TemporaryDirectory(prefix='ffw-',dir='/tmp') as tmp:
    root=Path(tmp);image=root/'store';inp=root/'input'
    run('init',image);run('fs-init',image);run('fs-mkdir',image,'/src')
    inp.write_bytes(b'original'*8192)
    run('fs-write',image,'/src/file',inp);run('fs-link',image,'/src/file','/src/alias')
    original=inp.read_bytes()
    identity=json.loads(run('fs-stat',image,'/src/file'))['inode_id']
    revision=json.loads(run('fs-snapshot',image,'r0'))['revision_id']
    assert len(revision)==64
    assert run('fs-revisions',image)==b'r0\n'
    before=image.stat().st_size
    run('fs-fork',image,'r0','work')
    assert image.stat().st_size-before<8192,'fork copied content or manifest'
    assert run('fs-worlds',image)==b'main\nwork\n'
    work=('--world','work');old=('--revision','r0')
    assert cat(image,'/src/file',*work)==original
    assert json.loads(run('fs-stat',image,'/src/file',*work))['inode_id']==identity
    inp.write_bytes(b'branch changed')
    run('fs-write',image,'/src/file',inp,*work)
    assert cat(image,'/src/alias',*work)==inp.read_bytes()
    assert cat(image,'/src/file')==original and cat(image,'/src/file',*old)==original
    run('fs-unlink',image,'/src/alias',*work)
    assert json.loads(run('fs-stat',image,'/src/file',*work))['links']==1
    assert json.loads(run('fs-stat',image,'/src/file'))['links']==2
    run('fs-mkdir',image,'/build',*work)
    run('fs-rename',image,'/src/file','/build/moved',*work)
    check(image,*work);check(image,*old);check(image)
    run('fs-snapshot',image,'r1',*work)
    run('fs-fork',image,'r1','child')
    child=('--world','child')
    inp.write_bytes(b'child changed');run('fs-write',image,'/build/moved',inp,*child)
    assert cat(image,'/build/moved',*work)==b'branch changed'
    assert cat(image,'/build/moved','--revision','r1')==b'branch changed'
    assert cat(image,'/build/moved',*child)==b'child changed'
    # Main moves on while old revision/forks retain their state.
    inp.write_bytes(b'main changed');run('fs-write',image,'/src/file',inp)
    assert cat(image,'/src/file',*old)==original
    run('checkpoint',image)
    for view in [(),work,child,old,('--revision','r1')]:check(image,*view)
    assert cat(image,'/src/file')==b'main changed'
    assert cat(image,'/src/file',*old)==original
    baseline=image.read_bytes()
    # Readonly views and duplicate/invalid catalog names cause no writes.
    for command,args in [
        ('fs-write',('/src/file',str(inp),*old)),
        ('fs-unlink',('/src/file',*old)),
        ('fs-snapshot',('no',*old)),
        ('fs-fork',('r0','work')),('fs-fork',('r0','main')),
        ('fs-fork',('r0','bad/name')),('fs-fork',('missing','new')),
        ('fs-snapshot',('r0',)),('fs-init',work),
    ]:
        run(command,image,*args,ok=False);assert image.read_bytes()==baseline
    # Branch head update crash: old or new manifest, never mixed records.
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-rename',str(image),'/build/moved','/src/renamed',*work],
                         capture_output=True,timeout=10,env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        if stage.startswith('commit_'):
            run('fs-cat',image,'/build/moved',*work,ok=False)
            assert cat(image,'/src/renamed',*work)==b'branch changed'
        else:
            assert cat(image,'/build/moved',*work)==b'branch changed'
            run('fs-cat',image,'/src/renamed',*work,ok=False)
        assert cat(image,'/build/moved','--revision','r1')==b'branch changed'
        check(image,*work);check(image,*child);check(image,*old)
        run('checkpoint',image);check(image,*work)
    # Alias-only fork publication has a durable, atomic world/base pair.
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-fork',str(image),'r0','crashfork'],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        present=b'crashfork\n' in run('fs-worlds',image)
        assert present==stage.startswith('commit_')
        if present:
            assert cat(image,'/src/file','--world','crashfork')==original
            check(image,'--world','crashfork')
        run('checkpoint',image)
    # Revision publication must atomically install the manifest/descriptor/base.
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-snapshot',str(image),'crash-revision',*work],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        present=b'crash-revision\n' in run('fs-revisions',image)
        assert present==stage.startswith('commit_')
        if present:
            assert cat(image,'/build/moved','--revision','crash-revision')==b'branch changed'
            check(image,'--revision','crash-revision')
        run('checkpoint',image);check(image,*work)
    # Names resembling selectors are data when supplied in normal positions.
    image.write_bytes(baseline)
    run('fs-snapshot',image,'--world')
    run('fs-fork',image,'--world','selector-name')
    check(image,'--world','selector-name')
    print('revision retention, shared forks, isolation, readonly views and crash publication passed')
