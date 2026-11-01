"""Atomic multi-root batches, preconditions, deletion and crash recovery."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout

def encode(entries):
    out=b'FFBATCH1'+struct.pack('<I',len(entries))
    for key,value in entries:
        key=key.encode() if isinstance(key,str) else key
        out+=struct.pack('<III',2 if value is None else 1,len(key),0 if value is None else len(value))+key
        if value is not None:out+=value
    return out

def state(image):return json.loads(run('check',image))
with tempfile.TemporaryDirectory(prefix='ffb-',dir='/tmp') as tmp:
    root=Path(tmp);image=root/'store';batch=root/'batch';value=root/'input'
    run('init',image)
    batch.write_bytes(encode([('source',b'old'),('peer',b'old')]))
    run('object-batch',image,0,batch)
    old=image.read_bytes()
    assert state(image)['last_committed_seq']==1
    assert state(image)['indexed_objects']==1
    batch.write_bytes(encode([('source',None),('destination',b'new'),('peer',b'new')]))
    run('object-batch',image,1,batch)
    new=image.read_bytes()
    def view(committed):
        if committed:
            run('object-get',image,'source',ok=False)
            assert run('object-get',image,'destination')==b'new'
            assert run('object-get',image,'peer')==b'new'
        else:
            assert run('object-get',image,'source')==b'old'
            run('object-get',image,'destination',ok=False)
            assert run('object-get',image,'peer')==b'old'
    view(True)
    # Failed preconditions/validation have no file side effects.
    run('object-batch',image,1,batch,ok=False)
    assert image.read_bytes()==new
    for entries in [[('x',b'one'),('x',b'two')],[('absent',None)],[('z',b'value'),('absent',None)],[]]:
        batch.write_bytes(encode(entries))
        run('object-batch',image,2,batch,ok=False)
        assert image.read_bytes()==new
    # Every record boundary and torn record retains the complete previous state.
    cuts={len(old)};at=len(old)
    while at<len(new):
        n=int.from_bytes(new[at+8:at+12],'little')
        cuts.update([at,at+1,at+63,at+64,at+n-1,at+n]);at+=n
    for cut in sorted(cuts):
        image.write_bytes(new[:cut]);view(cut==len(new))
    # Published deletion survives checkpoint, including delete-only batches.
    image.write_bytes(new);run('checkpoint',image);view(True)
    batch.write_bytes(encode([('destination',None),('peer',None)]))
    run('object-batch',image,2,batch)
    assert state(image)['object_roots']==0
    run('checkpoint',image)
    assert state(image)['last_committed_seq']==3 and state(image)['object_roots']==0
    # Process exits at each durability boundary expose all or none of the changes.
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(old)
        batch.write_bytes(encode([('source',None),('destination',b'new'),('peer',b'new')]))
        p=subprocess.run([FAULT,'object-batch',str(image),'1',str(batch)],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        committed=stage.startswith('commit_');view(committed)
        value.write_bytes(b'next');run('object-put',image,'legacy',value)
        view(committed)
        run('checkpoint',image);view(committed)
    # Arbitrary binary keys, empty values, malformed transport bounds.
    image.write_bytes(old)
    batch.write_bytes(encode([(b'\x00\xffkey',b''),('empty',b'')]))
    run('object-batch',image,1,batch);run('checkpoint',image)
    assert state(image)['object_roots']==4
    assert run('object-get',image,'empty')==b''
    before=image.read_bytes()
    for bad in [b'',b'FFBATCH1'+struct.pack('<I',65),encode([('ok',b'v')])[:-1],encode([('ok',b'v')])+b'x']:
        batch.write_bytes(bad);run('object-batch',image,2,batch,ok=False)
        assert image.read_bytes()==before
    # Full checksum-corrupt multi-key prepare must fail closed.
    bad=bytearray(new);at=len(old)
    while int.from_bytes(bad[at+4:at+6],'little')!=7:
        at+=int.from_bytes(bad[at+8:at+12],'little')
    bad[at+64+44]^=1;image.write_bytes(bad)
    run('check',image,ok=False)
    print('multi-key atomicity, CAS conflicts, deletion, checkpoints and crashes passed')
