"""Revision-name recovery using only catalog archives and independent log fixtures."""
import json
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from replay_corruption import unpack, pack, operations, encode_ops, rechain, corrupt_revision_tx, h
BIN=str(Path(sys.argv[1]).resolve())
def run(*args,error=None):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=30)
    assert p.returncode==(1 if error else 0),(args,p.stderr)
    if error:assert error.encode() in p.stderr,(args,p.stderr)
    return p.stdout

def catalog(path,*archives):
    for archive in archives:run('catalog-add',path,archive)
    return path

def point(path,name='wanted'):
    return json.loads(run('catalog-revision',path,name))

with tempfile.TemporaryDirectory(prefix='ff-catalog-revision-') as directory:
    root=Path(directory)
    for seed in (1,173,4294967295):
        folder=root/str(seed);folder.mkdir();rng=random.Random(seed)
        source=folder/'source';run('init',source);run('fs-init',source)
        run('fs-snapshot',source,'old')
        base=folder/'base';start=int(run('backup',source,base))
        branch=folder/'branch';shutil.copytree(source,branch)
        payload=folder/'input';value=rng.randbytes(rng.randrange(1,8193));payload.write_bytes(value)
        run('fs-write',source,'/file',payload);run('fs-snapshot',source,'wanted')
        expected=json.loads(run('fs-revision-point',source,'wanted'))
        prefix=folder/'prefix';run('replay-export',source,start,prefix)
        # Duplicate creation records across overlapping packages must not be ambiguous.
        payload.write_bytes(b'after target');run('fs-write',source,'/file',payload)
        delta=folder/'delta';run('replay-export',source,start,delta)
        full=folder/'full';run('backup',source,full)
        cat=catalog(folder/'catalog',full,delta,base,prefix)
        actual=point(cat)
        assert actual['revision_id']==expected['revision_id'] and actual['checkpoint']==expected['checkpoint']
        assert json.loads(run('restore-plan','--revision','wanted','--catalog',cat))['steps'][-1]['sequence']==expected['checkpoint']['sequence']
        # Source loss and catalog relocation do not change the mapping.
        shutil.rmtree(source);moved=folder/'moved';cat.rename(moved)
        assert point(moved)==actual
        restored=folder/'restored';run('restore',restored,'--revision','wanted','--catalog',moved)
        assert run('fs-cat',restored,'/file')==value
        assert json.loads(run('check',restored))['last_committed_seq']==actual['checkpoint']['sequence']
        run('fs-check',restored)
        run('catalog-revision',moved,'missing',error='revision not found')
        run('catalog-revision',moved,'../wanted',error='invalid')
        run('catalog-revision',moved,'old',error='creation transaction not retained')
        only_full=catalog(folder/'only-full',full)
        run('catalog-revision',only_full,'wanted',error='creation transaction not retained')
        absent=folder/'unavailable'
        run('restore',absent,'--revision','wanted','--catalog',only_full,error='creation transaction not retained')
        assert not absent.exists()
        # A receipt can be known even though no earlier full backup can reach it.
        only_delta=catalog(folder/'only-delta',delta);assert point(only_delta)==actual
        run('restore-plan','--revision','wanted','--catalog',only_delta,error='no continuous archive chain')
        # Same UUID and revision name, but a different branch: refuse to guess.
        payload.write_bytes(b'different branch');run('fs-write',branch,'/file',payload);run('fs-snapshot',branch,'wanted')
        branch_delta=folder/'branch-delta';run('replay-export',branch,start,branch_delta)
        ambiguous=catalog(folder/'ambiguous',base,delta,branch_delta)
        run('catalog-revision',ambiguous,'wanted',error='ambiguous catalog revision')
        run('restore',folder/'ambiguous-target','--revision','wanted','--catalog',ambiguous,error='ambiguous catalog revision')
        assert not (folder/'ambiguous-target').exists()
        header,frames=unpack(delta.read_bytes());key=b'R/\0forkfs-history/revision/wanted'
        event=next(i for i,f in enumerate(frames) if any(k==key for op,k,v in operations(f)))
        # Re-PUT of the same descriptor is not a second valid creation candidate.
        duplicate=list(frames);duplicate[-1]=duplicate[-1][:41]+encode_ops(operations(duplicate[-1])+[(1,key,bytes.fromhex(actual['revision_id']))])
        repeated=folder/'repeated';repeated.write_bytes(pack(*rechain(header,duplicate)))
        repeated_cat=catalog(folder/'repeated-cat',base,repeated)
        run('catalog-revision',repeated_cat,'wanted',error='ambiguous catalog revision creation events')
        # A sole later reference-only binding must not impersonate creation.
        tail=duplicate[-1];seq,=struct.unpack_from('<Q',tail,1)
        tail_header=header[:25]+struct.pack('<QQ',seq-1,seq)+tail[9:41]+h(tail)
        tail_pack=folder/'reference-only';tail_pack.write_bytes(pack(tail_header,[tail]))
        tail_cat=catalog(folder/'reference-only-cat',full,tail_pack)
        run('catalog-revision',tail_cat,'wanted',error='creation transaction not retained')
        # No source+1 inference: a descriptor source may precede its actual commit.
        delayed=list(frames);delayed[event]=corrupt_revision_tx(delayed[event],key,lambda b:b[:72]+struct.pack('<Q',start))
        delayed_pack=folder/'delayed';delayed_pack.write_bytes(pack(*rechain(header,delayed)))
        delayed_cat=catalog(folder/'delayed-cat',base,delayed_pack)
        assert point(delayed_cat)['checkpoint']['sequence']==expected['checkpoint']['sequence']>start+1
        # Hash-valid malformed descriptors must fail before any destination is published.
        for kind,mutate in enumerate((lambda b:b[:-1],lambda b:b'BADMAGIC'+b[8:],lambda b:b[:72]+struct.pack('<Q',2**64-1))):
            bad=list(frames);bad[event]=corrupt_revision_tx(bad[event],key,mutate)
            archive=folder/('bad'+str(kind));archive.write_bytes(pack(*rechain(header,bad)))
            bad_cat=catalog(folder/('bad-cat'+str(kind)),base,archive)
            run('catalog-revision',bad_cat,'wanted',error='invalid catalog revision')
            dest=folder/('bad-target'+str(kind))
            run('restore',dest,'--revision','wanted','--catalog',bad_cat,error='invalid catalog revision')
            assert not dest.exists()
print('catalog revision receipts, overlap, source loss, missing history, forks and descriptor corruption passed')
