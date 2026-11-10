"""Crash at unlink/replacement commit with a live handle, then restart."""
import json
import os
from pathlib import Path
import shutil
import select
import socket
import struct
import subprocess
import sys
import tempfile
BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-hcrash-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';data=root/'input';endpoint=root/'control.sock'
    run('init',db);run('fs-init',db)
    original=b'original'+bytes(range(256))*1536
    data.write_bytes(original[:256*1024]);run('fs-write',db,'/f',data)
    data.write_bytes(original[256*1024:]);run('fs-write-at',db,'/f',str(256*1024),data)
    data.write_bytes(b'source');run('fs-write',db,'/source',data)
    data.write_bytes(b'original-xattr');run('fs-xattr-set',db,'/f','user.crash','create',data)
    run('fs-symlink',db,'/new-target','/dangling');run('fs-snapshot',db,'base');data.write_bytes(b'XY');baseline=root/'baseline';shutil.copytree(db,baseline)
    for stage in ['leveldb_before_write','leveldb_after_write']:
        for op,args in [('fs-unlink',('/f',)),('fs-rename',('/source','/f')),
                        ('fs-open',('/new','rw,create,exclusive')),('fs-open',('/dangling','rw,create')),('fs-open',('/f','w,truncate')),
                        ('fs-hwrite',('HANDLE','0',str(data))),('fs-save',('/f',str(data))),
                        ('fs-hxattr-set',('HANDLE','user.crash','replace',str(data))),('fs-hxattr-remove',('HANDLE','user.crash'))]:
            shutil.rmtree(db);shutil.copytree(baseline,db)
            proc=subprocess.Popen([FAULT,'serve',str(db),'--socket',str(endpoint),'--rpc'],
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                  env={**os.environ,'FORKFS_TEST_CRASH':stage})
            try:
                # bind() creates the socket before listen() and the ready
                # status. Waiting for the path alone races the first request.
                assert select.select([proc.stdout],[],[],5)[0], 'daemon readiness timed out'
                ready=json.loads(proc.stdout.readline())
                assert ready['state']=='leveldb' and proc.poll() is None,ready
                session=json.loads(run('request',endpoint,'session-open','60000'))['session']
                mode='rw,append' if op=='fs-hwrite' else 'rw'
                token=json.loads(run('request',endpoint,'fs-open','/f',mode,'--session',session))['handle']
                options=['--session',session] if op in ['fs-open','fs-hwrite','fs-hxattr-set','fs-hxattr-remove'] else []
                actual=(token,*args[1:]) if op in ['fs-hwrite','fs-hxattr-set','fs-hxattr-remove'] else args
                p=subprocess.run([BIN,'request',str(endpoint),op,*actual,*options],capture_output=True,timeout=10)
                assert p.returncode!=0
                proc.wait(timeout=5);assert proc.returncode==86,proc.communicate()
                endpoint.unlink()
                committed=stage=='leveldb_after_write'
                if op=='fs-unlink':run('fs-stat',db,'/f',ok=not committed)
                elif op=='fs-save':assert run('fs-cat',db,'/f')==(b'XY' if committed else original)
                elif op=='fs-hwrite':assert run('fs-cat',db,'/f')==(original+b'XY' if committed else original)
                elif op in ['fs-hxattr-set','fs-hxattr-remove']:assert run('fs-cat',db,'/f')==original
                elif op=='fs-open':
                    if args[0] in ['/new','/dangling']:
                        target='/new-target' if args[0]=='/dangling' else '/new';run('fs-stat',db,target,ok=committed)
                        assert run('fs-readlink',db,'/dangling')==b'/new-target'
                    else:assert run('fs-cat',db,'/f')==(b'' if committed else original)
                else:
                    assert run('fs-cat',db,'/f')==(b'source' if committed else original)
                    run('fs-stat',db,'/source',ok=not committed)
                assert run('fs-cat',db,'/f','--revision','base')==original
                assert run('fs-xattr-get',db,'/f','user.crash','--revision','base')==b'original-xattr'
                missing=committed and op in ['fs-unlink','fs-rename','fs-save','fs-hxattr-remove']
                value=run('fs-xattr-get',db,'/f','user.crash',ok=not missing)
                if not missing:assert value==(b'XY' if committed and op=='fs-hxattr-set' else b'original-xattr')
                run('fs-check',db);run('fs-check',db,'--revision','base')
                # Dead handle retention aliases are released; immutable objects remain.
                before=json.loads(run('check',db))['last_committed_seq']
                after=json.loads(run('check',db))['last_committed_seq'];assert before==after
            finally:
                if proc.poll() is None:proc.kill();proc.wait(timeout=5)
                if endpoint.exists():endpoint.unlink()
    print('open-unlink and rename replacement crash atomicity/restart passed')
