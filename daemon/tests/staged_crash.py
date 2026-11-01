"""Crash during staged outputs must never publish a partial merged view."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BIN,FAULT,GEN=sys.argv[1:4]
def run(*args):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=20)
    assert p.returncode==0,(args,p.stderr)
    return p.stdout

def runs(image,*view):return json.loads(run('fs-layout',image,*view))['runs']
with tempfile.TemporaryDirectory(prefix='ffstage-',dir='/tmp') as tmp:
    image=Path(tmp)/'store'
    p=subprocess.run([GEN,'--generate',str(image)],capture_output=True,timeout=20)
    assert p.returncode==0,p.stderr
    baseline=image.read_bytes();old=runs(image)
    stages=['batch_written','batch_synced','commit_written','commit_synced',
            'compaction_stage_committed','compaction_before_publish','compaction_published']
    for stage in stages:
        image.write_bytes(baseline)
        p=subprocess.run([FAULT,'fs-compact',str(image)],capture_output=True,timeout=20,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.returncode,p.stderr)
        current=runs(image)
        assert (current>16) if stage=='compaction_published' else (current==old),(stage,current,old)
        assert runs(image,'--revision','original')==old
        assert run('fs-ls',image,'/')==b''
        run('check',image);run('checkpoint',image)
        # Restart builds anew and reuses committed output objects deterministically.
        run('fs-compact',image)
        assert runs(image)>16 and runs(image,'--revision','original')==old
        assert run('fs-ls',image,'/')==b''
    print('staged output and final publication crash recovery passed')
