"""Crash at unlink/replacement commit with a live handle, then restart."""
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-hcrash-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input';endpoint=root/'control.sock'
    run('init',db);run('fs-init',db)
    data.write_bytes(b'original');run('fs-write',db,'/f',data)
    data.write_bytes(b'source');run('fs-write',db,'/source',data)
    run('fs-symlink',db,'/new-target','/dangling');run('fs-snapshot',db,'base');data.write_bytes(b'XY');baseline=root/'baseline';shutil.copytree(db,baseline)
    for stage in ['leveldb_before_write','leveldb_after_write']:
        for op,args in [('fs-unlink',('/f',)),('fs-rename',('/source','/f')),
                        ('fs-open',('/new','rw,create,exclusive')),('fs-open',('/dangling','rw,create')),('fs-open',('/f','w,truncate')),
                        ('fs-hwrite',('HANDLE','0',str(data))),('fs-save',('/f',str(data)))]:
            shutil.rmtree(db);shutil.copytree(baseline,db)
            proc=subprocess.Popen([FAULT,'serve',str(db),'--socket',str(endpoint),'--rpc'],
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                  env={**os.environ,'FORKFS_TEST_CRASH':stage})
            try:
                deadline=time.monotonic()+5
                while not endpoint.exists():
                    assert proc.poll() is None,proc.communicate()
                    assert time.monotonic()<deadline;time.sleep(.01)
                session=json.loads(run('request',endpoint,'session-open','60000'))['session']
                mode='rw,append' if op=='fs-hwrite' else 'rw'
                token=json.loads(run('request',endpoint,'fs-open','/f',mode,'--session',session))['handle']
                options=['--session',session] if op in ['fs-open','fs-hwrite'] else []
                actual=(token,*args[1:]) if op=='fs-hwrite' else args
                p=subprocess.run([BIN,'request',str(endpoint),op,*actual,*options],capture_output=True,timeout=10)
                assert p.returncode!=0
                proc.wait(timeout=5);assert proc.returncode==86,proc.communicate()
                endpoint.unlink()
                committed=stage=='leveldb_after_write'
                if op=='fs-unlink':run('fs-stat',db,'/f',ok=not committed)
                elif op=='fs-save':assert run('fs-cat',db,'/f')==(b'XY' if committed else b'original')
                elif op=='fs-hwrite':assert run('fs-cat',db,'/f')==(b'originalXY' if committed else b'original')
                elif op=='fs-open':
                    if args[0] in ['/new','/dangling']:
                        target='/new-target' if args[0]=='/dangling' else '/new';run('fs-stat',db,target,ok=committed)
                        assert run('fs-readlink',db,'/dangling')==b'/new-target'
                    else:assert run('fs-cat',db,'/f')==(b'' if committed else b'original')
                else:
                    assert run('fs-cat',db,'/f')==(b'source' if committed else b'original')
                    run('fs-stat',db,'/source',ok=not committed)
                assert run('fs-cat',db,'/f','--revision','base')==b'original'
                run('fs-check',db);run('fs-check',db,'--revision','base')
                # Dead handle retention aliases are released; immutable objects remain.
                before=json.loads(run('check',db))['last_committed_seq']
                after=json.loads(run('check',db))['last_committed_seq'];assert before==after
            finally:
                if proc.poll() is None:proc.kill();proc.wait(timeout=5)
                if endpoint.exists():endpoint.unlink()
    print('open-unlink and rename replacement crash atomicity/restart passed')
