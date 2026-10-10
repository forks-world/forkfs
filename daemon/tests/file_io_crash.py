"""Offset writes/truncate/append publish inode and content atomically."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
BIN,FAULT=sys.argv[1:3]
def run(*args):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert p.returncode==0,(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-io-crash-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input';data.write_bytes(b'abcdef')
    run('init',db);run('fs-init',db);run('fs-write',db,'/f',data);run('fs-link',db,'/f','/alias')
    run('fs-snapshot',db,'base');run('fs-fork',db,'base','work')
    baseline=root/'baseline';shutil.copytree(db,baseline);data.write_bytes(b'XY')
    for command,args,new in [
        ('fs-write-at',('/f','2',str(data)),b'abXYef'),
        ('fs-write-at',('/f','8',str(data)),b'abcdef\0\0XY'),
        ('fs-truncate',('/f','3'),b'abc'),
        ('fs-truncate',('/f','8'),b'abcdef\0\0'),
        ('fs-append',('/f',str(data)),b'abcdefXY'),
    ]:
        for stage in ['leveldb_before_write','leveldb_after_write']:
            shutil.rmtree(db);shutil.copytree(baseline,db)
            p=subprocess.run([FAULT,command,str(db),*args,'--world','work'],capture_output=True,timeout=10,
                             env={**os.environ,'FORKFS_TEST_CRASH':stage})
            assert p.returncode==86,(command,stage,p.stderr)
            expected=new if stage=='leveldb_after_write' else b'abcdef'
            for path in ['/f','/alias']:
                assert run('fs-cat',db,path,'--world','work')==expected
                assert json.loads(run('fs-stat',db,path,'--world','work'))['size']==len(expected)
            assert run('fs-cat',db,'/f')==b'abcdef'
            assert run('fs-cat',db,'/f','--revision','base')==b'abcdef'
            run('fs-check',db,'--world','work');run('fs-check',db,'--revision','base')
    print('offset write, zero fill, truncate and append crash publication passed')
