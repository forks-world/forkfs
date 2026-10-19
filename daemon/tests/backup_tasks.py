"""Crash boundaries and corrupted local task catalogs, independent of archive data."""
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import time

from replay_corruption import h

BIN, FAULT = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])

def cli(*args):
    p = subprocess.run([BIN, *map(str,args)], capture_output=True, timeout=15)
    assert p.returncode == 0, p.stderr
    return p.stdout

with tempfile.TemporaryDirectory(prefix='ff-task-') as directory:
    root = Path(directory)
    endpoint = root/'rpc.sock'

    def start(db, stage=None):
        env = dict(os.environ)
        if stage:env['FORKFS_TEST_CRASH']=stage
        proc = subprocess.Popen([FAULT if stage else BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],
                                env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        deadline = time.monotonic()+10
        # bind creates the pathname before listen: require a successful RPC,
        # not merely existence of a socket, especially under parallel CI load.
        while True:
            assert proc.poll() is None, proc.communicate()
            assert time.monotonic()<deadline, 'RPC readiness timeout'
            if endpoint.exists():
                ready=subprocess.run([BIN,'request',str(endpoint),'status'],capture_output=True,timeout=5)
                if ready.returncode==0:break
            time.sleep(.01)
        return proc

    def request(*args, ok=True):
        p = subprocess.run([BIN,'request',str(endpoint),*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok, (args,p.stderr)
        return json.loads(p.stdout) if ok else p.stderr

    def stop(proc):
        if proc.poll() is None:proc.terminate()
        proc.wait(timeout=20)
        endpoint.unlink(missing_ok=True)

    for stage in ['backup_task_after_intent','backup_task_after_archive','backup_task_after_completion']:
        db=root/stage;cli('init',db);cli('fs-init',db)
        archive=root/(stage+'.pack')
        proc=start(db,stage)
        try:
            # A crash after accepting the request can occur before or after its reply.
            subprocess.run([BIN,'request',str(endpoint),'backup-start','job',str(archive)],capture_output=True,timeout=15)
            assert proc.wait(timeout=15)==86
        finally:stop(proc)
        proc=start(db)
        try:
            status=request('backup-status','job')
            expected='durable' if stage.endswith('completion') else 'interrupted'
            assert status['state']==expected,status
            if expected!='durable':assert status['checkpoint'] is None
            before=archive.read_bytes() if archive.exists() else None
            assert request('backup-start','job',archive)==status
            assert (archive.read_bytes() if archive.exists() else None)==before
            request('backup-start','job',root/'changed.pack',ok=False)
        finally:stop(proc)
        if stage!='backup_task_after_intent':
            cli('replay',archive,root/(stage+'-recovered'))
        else:assert not archive.exists()

    # Random bit flips must reject task state, without blocking filesystem access.
    original=(db/'BACKUP-TASKS').read_bytes();rng=random.Random(173)
    variants=[b'',original[:-1]]
    for _ in range(8):
        data=bytearray(original);pos=rng.randrange(len(data));data[pos]^=1<<rng.randrange(8);variants.append(bytes(data))
    # Independent codec: regenerate checksums to reach structural validation.
    identity=original[8:40];pos=44;records=[]
    for _ in range(struct.unpack_from('<I',original,40)[0]):
        fields=[]
        for _ in range(5):
            size,=struct.unpack_from('<I',original,pos);pos+=4
            fields.append(original[pos:pos+size]);pos+=size
        fields.extend(struct.unpack_from('<BQQ',original,pos));pos+=17
        records.append(fields)
    assert pos==len(original)-32
    def encode(rows):
        data=b'FFBKT001'+identity+struct.pack('<I',len(rows))
        for row in rows:
            data+=b''.join(struct.pack('<I',len(v))+v for v in row[:5])
            data+=struct.pack('<BQQ',*row[5:])
        return data+h(data)
    assert encode(records)==original
    semantic=[]
    for i in range(24):
        rows=[list(row) for row in records];row=rng.choice(rows)
        kind=rng.randrange(6)
        if kind==0:row[0]=b'bad\0'+str(i).encode()
        elif kind==1:row[1]=b'relative/'+str(i).encode()
        elif kind==2:row[2]=b'bad'+str(rng.getrandbits(32)).encode()
        elif kind==3:row[4]=row[4][:rng.randrange(32)]
        elif kind==4:row[5]=rng.randrange(2,256)
        else:row[5]=1;row[6]=row[7]+rng.randrange(1,1000)
        semantic.append(encode(rows))
    semantic.append(encode(records+records))
    for damaged in variants+semantic:
        (db/'BACKUP-TASKS').write_bytes(damaged)
        proc=start(db)
        try:
            request('status')
            error=request('backup-status','job',ok=False)
            if damaged in semantic:assert b'checksum mismatch' not in error,error
            request('backup-start','another',root/'another.pack',ok=False)
            request('fs-check')
            assert not (root/'another.pack').exists()
        finally:stop(proc)
    (db/'BACKUP-TASKS').write_bytes(original)
    # A catalog copied to a different repository must not authorize old tasks.
    other=root/'foreign';cli('init',other);cli('fs-init',other)
    (other/'BACKUP-TASKS').write_bytes(original)
    proc=start(other)
    try:
        assert b'repository mismatch' in request('backup-status','job',ok=False)
        request('fs-check')
    finally:stop(proc)
print('persistent task intents, receipts, crash boundaries and catalog corruption passed')
