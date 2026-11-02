"""Physical collection preserves retained history and is crash atomic."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
BIN, FAULT = sys.argv[1:3]
def run(*args, binary=BIN):
    p = subprocess.run([binary, *map(str,args)], capture_output=True, timeout=30)
    assert p.returncode == 0, (args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-gc-',dir='/tmp') as tmp:
    root=Path(tmp); db=root/'db'; data=root/'input'
    run('init',db); run('fs-init',db)
    data.write_bytes(b'retained'); run('fs-write',db,'/file',data)
    run('fs-link',db,'/file','/alias'); run('fs-snapshot',db,'base')
    run('fs-fork',db,'base','work')
    for i in range(12):
        data.write_bytes(bytes([i])*65536); run('fs-write',db,'/file',data)
    run('fs-unlink',db,'/file'); run('fs-unlink',db,'/alias')
    data.write_bytes(b'opaque'); run('object-put',db,'user-pin',data)
    baseline=root/'baseline'; shutil.copytree(db,baseline)
    def verify():
        run('check',db);run('fs-check',db)
        for view in [('--revision','base'),('--world','work')]:
            run('fs-check',db,*view)
            assert run('fs-cat',db,'/file',*view)==b'retained'
            assert run('fs-cat',db,'/alias',*view)==b'retained'
        assert run('object-get',db,'user-pin')==b'opaque'
    collected=json.loads(run('gc',db))['collected_objects']; assert collected>0
    verify(); assert json.loads(run('gc',db))['collected_objects']==0
    for stage in ['gc_before_write','gc_after_write']:
        shutil.rmtree(db);shutil.copytree(baseline,db)
        p=subprocess.run([FAULT,'gc',str(db)],capture_output=True,timeout=30,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        verify(); reclaimed=json.loads(run('gc',db))['collected_objects']
        assert (reclaimed>0)==(stage=='gc_before_write')
        verify()
    print('GC collection, idempotency, retained revisions/Worlds, hardlinks, opaque pins and crash atomicity passed')
