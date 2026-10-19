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
    # Test both coordinator preflight and append admission at exact byte limits.
    edge=root/'quota-edge';edge_input=root/'quota-input';run('init',edge)
    edge_input.write_bytes(b'x');run('capacity',edge,92,0)
    run('object-put',edge,'first',edge_input,code=1)
    assert not list(edge.glob('DATA-*.seg'))
    run('capacity',edge,93,0);run('object-put',edge,'first',edge_input)
    assert sum(p.stat().st_size for p in edge.glob('DATA-*.seg'))==93
    edge_input.write_bytes(b'y');run('capacity',edge,137,0)
    before=json.loads(run('check',edge));run('object-put',edge,'second',edge_input,code=1)
    after=json.loads(run('check',edge))
    for key in ('last_committed_seq','indexed_objects','object_roots'):assert after[key]==before[key]
    assert run('object-get',edge,'first')==b'x'
    run('object-get',edge,'second',code=1)
    assert sum(p.stat().st_size for p in edge.glob('DATA-*.seg'))==93
    run('capacity',edge,138,0);run('object-put',edge,'second',edge_input)
    assert sum(p.stat().st_size for p in edge.glob('DATA-*.seg'))==138
    edge_input.write_bytes(b'x');run('object-put',edge,'deduplicated',edge_input)
    assert run('object-get',edge,'deduplicated')==b'x'
    assert sum(p.stat().st_size for p in edge.glob('DATA-*.seg'))==138
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
        for attempt in range(2 if stage=='gc_copy_batch' else 1):
            run('gc',trial,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH=stage),code=86)
            verify(trial)
        verify(trial);run('gc',trial);verify(trial)
        # Every safely bound pre-handoff or fully prepared crash leftover is reclaimed.
        assert not list(root.glob(stage+'.gc-*'))
    # A checkpoint is published only after the exchanged generation is durable.
    for stage in ('gc_copy_batch','gc_after_verified','gc_before_swap','gc_after_swap','gc_after_parent_sync',
                  'gc_before_checkpoint_publish','gc_after_checkpoint_publish','gc_after_checkpoint_complete'):
        trial=root/('checkpoint-'+stage);archive=root/('archive-'+stage);shutil.copytree(db,trial)
        run('gc',trial,'--checkpoint',archive,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH=stage),code=86)
        if stage in ('gc_copy_batch','gc_after_verified','gc_before_swap'):
            assert not archive.exists()
        if stage in ('gc_after_swap','gc_after_parent_sync','gc_before_checkpoint_publish'):
            run('check',trial,code=1)  # normal opens cannot pass a pending receipt
            wrong=root/('wrong-'+stage)
            pending=(trial/'GC-PENDING').read_bytes();baseline=(trial/'GC-BASELINE.pack').read_bytes()
            run('gc-plan',trial,'--checkpoint',archive,code=1)
            assert (trial/'GC-PENDING').read_bytes()==pending and (trial/'GC-BASELINE.pack').read_bytes()==baseline and not archive.exists()
            run('gc',trial,'--checkpoint',wrong,code=1)
            assert not wrong.exists()
        retried=json.loads(run('gc',trial,'--checkpoint',archive))
        assert retried['new_replay_generation'] and retried['applied'],retried
        if stage not in ('gc_after_verified','gc_before_swap'):
            assert json.loads(run('gc',trial,'--checkpoint',archive))==retried
        assert archive.is_file() and not (trial/'GC-PENDING').exists() and not (trial/'GC-BASELINE.pack').exists()
        assert not list(root.glob(trial.name+'.gc-*'))
        verify(trial)
        if stage=='gc_after_checkpoint_complete':
            advanced=root/'after-checkpoint';advanced.write_bytes(b'newer generation transaction')
            run('fs-write',trial,'/file',advanced)
            assert json.loads(run('gc',trial,'--checkpoint',archive))==retried
    conflict_trial=root/'checkpoint-publish-conflict';shutil.copytree(db,conflict_trial);conflicted=root/'checkpoint-publish.pack'
    run('gc',conflict_trial,'--checkpoint',conflicted,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH='gc_before_checkpoint_publish'),code=86)
    protected_bytes=b'user archive';conflicted.write_bytes(protected_bytes)
    pending=(conflict_trial/'GC-PENDING').read_bytes();baseline=(conflict_trial/'GC-BASELINE.pack').read_bytes()
    run('gc',conflict_trial,'--checkpoint',conflicted,code=1)
    assert conflicted.read_bytes()==protected_bytes and (conflict_trial/'GC-PENDING').read_bytes()==pending and (conflict_trial/'GC-BASELINE.pack').read_bytes()==baseline
    moved=root/'checkpoint-publish-moved.pack';conflicted.rename(moved)
    recovered=json.loads(run('gc',conflict_trial,'--checkpoint',conflicted))
    assert recovered['applied'] and not (conflict_trial/'GC-PENDING').exists()
    assert json.loads(run('gc',conflict_trial,'--checkpoint',conflicted))==recovered
    verify(conflict_trial)
    corrupt_trial=root/'checkpoint-corrupt-receipt';shutil.copytree(db,corrupt_trial);corrupt_archive=root/'corrupt-receipt.pack'
    run('gc',corrupt_trial,'--checkpoint',corrupt_archive,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH='gc_before_checkpoint_publish'),code=86)
    corrupt=(corrupt_trial/'GC-PENDING').read_bytes();(corrupt_trial/'GC-PENDING').write_bytes(corrupt[:-1]+bytes([corrupt[-1]^1]))
    corrupt=(corrupt_trial/'GC-PENDING').read_bytes()
    run('check',corrupt_trial,code=1);run('gc',corrupt_trial,'--checkpoint',corrupt_archive,code=1)
    assert (corrupt_trial/'GC-PENDING').read_bytes()==corrupt and not corrupt_archive.exists()
    mode_trial=root/'directory-mode';shutil.copytree(db,mode_trial);os.chmod(mode_trial,0o750)
    before=os.stat(mode_trial);run('gc',mode_trial);after=os.stat(mode_trial)
    assert (after.st_uid,after.st_gid,after.st_mode&0o7777)==(before.st_uid,before.st_gid,0o750)
    verify(mode_trial)
    # A root-owned read-only toolchain directory exercises the real foreign-UID
    # admission path as an ordinary user, without chown privileges or fixtures
    # outside the sandbox's readable toolchain resources.
    if os.geteuid()!=os.stat('/usr').st_uid:
        refused=subprocess.run([BIN,'gc','/usr'],capture_output=True,timeout=10)
        assert refused.returncode==1 and b'cross-identity GC is unsupported' in refused.stderr,refused.stderr
    if os.geteuid()==0:
        foreign_trial=root/'foreign-owner';shutil.copytree(db,foreign_trial);before=os.stat(foreign_trial)
        os.chown(foreign_trial,before.st_uid+1,before.st_gid);foreign=os.stat(foreign_trial)
        run('gc',foreign_trial,code=1);after=os.stat(foreign_trial)
        assert (after.st_uid,after.st_gid,after.st_mode&0o7777)==(foreign.st_uid,foreign.st_gid,foreign.st_mode&0o7777)
        assert not (foreign_trial/'GC-READY').exists()
    conflict=root/'checkpoint-conflict';shutil.copytree(db,conflict);protected=root/'protected.pack';protected.write_bytes(b'user data')
    run('gc',conflict,'--checkpoint',protected,code=1);assert protected.read_bytes()==b'user data';verify(conflict)
    # Existing garbage leaves room for the second generation within the source
    # max_data policy; a bound below the live output rejects before exchange.
    capacity_trial=root/'capacity-gc';shutil.copytree(db,capacity_trial)
    usage=sum(p.stat().st_size for p in capacity_trial.glob('DATA-*.seg'))
    run('capacity',capacity_trial,usage,0)
    compacted=json.loads(run('gc',capacity_trial));assert compacted['data_bytes_after']<usage,compacted;verify(capacity_trial)
    too_small=root/'capacity-gc-too-small';shutil.copytree(db,too_small)
    run('capacity',too_small,1,0)
    run('gc',too_small,code=1);verify(too_small)
    assert not [p for p in root.glob('capacity-gc-too-small.gc-*') if (p/'GC-READY').exists()]
    stale=root/'minfree-stale';shutil.copytree(db,stale)
    run('gc',stale,binary=FAULT,env=dict(os.environ,FORKFS_TEST_CRASH='gc_copy_batch'),code=86)
    abandoned=next(root.glob('minfree-stale.gc-*'))
    # Neither a matching marker alone nor a matching lease alone permits deletion.
    unrelated=root/'minfree-stale.gc-unrelated';unrelated.mkdir()
    (unrelated/'GC-PREPARED').write_bytes((abandoned/'GC-PREPARED').read_bytes())
    (unrelated/'GC-SOURCE-OWNERSHIP').write_bytes(b'unrelated inode')
    wrong=root/'minfree-stale.gc-wrong-marker';wrong.mkdir()
    os.link(stale/'OWNERSHIP',wrong/'GC-SOURCE-OWNERSHIP')
    (wrong/'GC-PREPARED').write_bytes(b'wrong repository marker')
    alias=root/'minfree-stale.gc-symlink';alias.symlink_to(unrelated,target_is_directory=True)
    run('capacity',stale,0,18446744073709551615)
    run('gc',stale,code=1);verify(stale)
    assert not abandoned.exists()
    assert unrelated.is_dir() and wrong.is_dir() and alias.is_symlink()
    assert (unrelated/'GC-SOURCE-OWNERSHIP').read_bytes()==b'unrelated inode'
    alias.unlink();shutil.rmtree(unrelated);shutil.rmtree(wrong)
    run('capacity',stale,0,0);run('gc',stale);verify(stale)
    # min_free applies to DATA staging and both checkpoint archive copies.
    run('capacity',capacity_trial,0,2**63)
    run('gc',capacity_trial,code=1);verify(capacity_trial)
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
