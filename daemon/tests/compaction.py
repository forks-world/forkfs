"""Full-view compaction publication preserves snapshots and tombstones."""
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

def runs(image,*view):return json.loads(run('fs-layout',image,*view))['runs']
with tempfile.TemporaryDirectory(prefix='ffcompact-',dir='/tmp') as tmp:
    root=Path(tmp);image=root/'store';value=root/'input'
    run('init',image);run('fs-init',image)
    value.write_bytes(b'original');run('fs-write',image,'/file',value)
    run('fs-snapshot',image,'original');run('fs-link',image,'/file','/alias')
    for i in range(5):
        value.write_bytes(b'updated'+bytes([i]));run('fs-write',image,'/alias',value)
    run('fs-unlink',image,'/alias');run('fs-snapshot',image,'deleted')
    old=runs(image);assert old>1
    run('checkpoint',image);baseline=image.read_bytes()
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-compact',str(image)],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        assert runs(image)==(1 if stage.startswith('commit_') else old)
        assert run('fs-cat',image,'/file')==b'updated\x04'
        run('fs-stat',image,'/alias',ok=False)
        assert run('fs-cat',image,'/file','--revision','original')==b'original'
        assert runs(image,'--revision','deleted')==old
        run('fs-stat',image,'/alias','--revision','deleted',ok=False)
        run('fs-check',image);run('fs-check',image,'--revision','deleted')
        run('checkpoint',image)
        assert run('fs-cat',image,'/file')==b'updated\x04'
    before=image.read_bytes()
    run('fs-compact',image,'--revision','deleted',ok=False)
    assert image.read_bytes()==before
    print('compaction publication crashes, retained runs and tombstone coverage passed')
