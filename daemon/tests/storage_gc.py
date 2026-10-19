"""Offline collection: independent content oracle, replay retention and crash barriers."""
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import time

BIN, FAULT = (str(Path(p).resolve()) for p in sys.argv[1:3])
def run(*args, binary=BIN, env=None, code=0):
    p=subprocess.run([binary,*map(str,args)],capture_output=True,env=env,timeout=40)
    assert p.returncode==code,(args,p.returncode,p.stderr)
    return p.stdout

with tempfile.TemporaryDirectory(prefix='ff-gc-') as directory:
    root=Path(directory);db=root/'store';payload=root/'payload'
    run('init',db);run('fs-init',db)
    rng=random.Random(7321)
    original=rng.randbytes(180000) if hasattr(rng,'randbytes') else bytes(rng.randrange(256) for _ in range(180000))
    payload.write_bytes(original);run('fs-write',db,'/file',payload)
    run('fs-snapshot',db,'retained');run('fs-fork',db,'retained','branch')
    for i in range(8):
        current=bytes((x+i+1)%256 for x in original)
        payload.write_bytes(current);run('fs-write',db,'/file',payload)
    opaque=b'FFINOD99-not-an-inode-root'
    payload.write_bytes(opaque);run('object-put',db,'opaque',payload)
    payload.write_bytes(b'edge');run('fs-write',db,'/sparse',payload)
    run('fs-truncate',db,'/sparse',10000001)
    run('fs-write-at',db,'/sparse',10000000,payload)
    (db/'.forkfs-fskit-world').write_bytes(b'main\n')
    corrupted=root/'corrupted';shutil.copytree(db,corrupted)
    segment=next(corrupted.glob('DATA-*.seg'))
    with segment.open('r+b') as f:
        f.seek(100);byte=f.read(1);f.seek(100);f.write(bytes([byte[0]^1]))
    run('gc',corrupted,code=1)
    assert not list(root.glob('corrupted.gc-*'))
    plan=json.loads(run('gc-plan',db));assert not plan['applied']
    assert plan['objects_after']<plan['objects_before'],plan
    def verify(path):
        assert run('fs-cat',path,'/file')==current
        assert run('fs-cat',path,'/file','--revision','retained')==original
        assert run('fs-cat',path,'/file','--world','branch')==original
        assert run('object-get',path,'opaque')==opaque
        assert run('fs-read-at',path,'/sparse',9999997,7)==bytes(3)+b'edge'
        run('fs-check',path)
    for stage in ('gc_copy_batch','gc_after_verified','gc_before_swap','gc_after_swap','gc_after_parent_sync'):
        trial=root/stage;shutil.copytree(db,trial)
        run('gc',trial,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH=stage),code=86)
        verify(trial);run('gc',trial);verify(trial)
        # Fully prepared crash leftovers must be recovered on the retry.
        assert not [p for p in root.glob(stage+'.gc-*') if (p/'GC-READY').exists()]
    report=json.loads(run('gc',db));assert report['data_bytes_after']<report['data_bytes_before'],report
    verify(db)
    assert (db/'.forkfs-fskit-world').read_bytes()==b'main\n'
    ceiling=report['data_bytes_after']+32768
    for cycle in range(4):
        for edit in range(4):
            current=bytes((x+31+cycle*4+edit)%256 for x in original)
            payload.write_bytes(current);run('fs-write',db,'/file',payload)
        collected=json.loads(run('gc',db));verify(db)
        assert collected['data_bytes_after']<=ceiling,collected
    base=root/'base';point=int(run('backup',db,base))
    payload.write_bytes(b'new');run('fs-write',db,'/file',payload);current=b'new'
    pinned=json.loads(run('gc',db));assert pinned['replay_pins_all_objects'],pinned
    delta=root/'delta';run('replay-export',db,point,delta)
    restored=root/'restored';run('replay',base,restored);run('replay',delta,restored);verify(restored)
    fresh=root/'fresh';reset=json.loads(run('gc',db,'--checkpoint',fresh))
    assert reset['new_replay_generation'] and not reset['replay_pins_all_objects'],reset
    recovery=root/'recovery';run('replay',fresh,recovery);verify(recovery)
    # Deterministic quota rejection leaves committed state intact.
    usage=sum(p.stat().st_size for p in db.glob('DATA-*.seg'))
    run('capacity',db,usage+4096,0)
    payload.write_bytes(bytes(rng.randrange(256) for _ in range(180000)));run('fs-write',db,'/file',payload,code=1);verify(db)
    endpoint=root/'rpc.sock'
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+10
        while True:
            assert proc.poll() is None,proc.communicate()
            ready=subprocess.run([BIN,'request',str(endpoint),'status'],capture_output=True)
            if ready.returncode==0:break
            assert time.monotonic()<deadline
            time.sleep(.01)
        run('gc',db,code=1)  # exclusive owner also spans mounted engines
        run('capacity',db,0,0,code=1)
        run('request',endpoint,'fs-write','/file',payload,code=1)
        run('request',endpoint,'status')  # admission rejection must not poison owner
        run('request',endpoint,'fs-check')
        assert run('request',endpoint,'fs-cat','/file')==current
    finally:
        proc.terminate();proc.wait(timeout=10)

    run('capacity',db,0,0);run('fs-write',db,'/file',payload);current=payload.read_bytes();verify(db)
    policy=(db/'CAPACITY').read_bytes();(db/'CAPACITY').write_bytes(policy[:-1]+bytes([policy[-1]^1]))
    run('check',db,code=1);(db/'CAPACITY').write_bytes(policy);verify(db)
print('GC crash, retention, replay checkpoint and capacity regressions passed')
