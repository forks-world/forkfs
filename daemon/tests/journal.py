"""Persistent named roots, record truncation/corruption and segment rollover."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BIN=sys.argv[1]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout

def records(buf):
    at=65536+4096
    result=[]
    while at<len(buf):
        assert buf[at:at+4]==b'FFR0'
        n=int.from_bytes(buf[at+8:at+12],'little')
        result.append((at,n,int.from_bytes(buf[at+4:at+6],'little')))
        at+=n
    return result

with tempfile.TemporaryDirectory(prefix='ffj-',dir='/tmp') as tmp:
    root=Path(tmp); image=root/'store'; inp=root/'input'
    run('init',image)
    inp.write_bytes(b'first\x00value')
    assert json.loads(run('object-put',image,'alpha',inp))['last_committed_seq']==1
    assert run('object-get',image,'alpha')==inp.read_bytes()
    first=image.read_bytes()
    recs=records(first)
    assert [r[2] for r in recs]==[1,2,3]
    oid=hashlib.sha256(b'forkfs.object.v0'+struct.pack('<HHQ',4,0,len(inp.read_bytes()))+inp.read_bytes()).digest()
    assert first[recs[0][0]+24:recs[0][0]+56]==oid
    inp.write_bytes(b'replaced')
    run('object-put',image,'alpha',inp)
    second=image.read_bytes()
    assert run('object-get',image,'alpha')==b'replaced'
    assert json.loads(run('check',image))['object_roots']==1
    # All representative cut points after durable transaction 1 retain it.
    suffix=records(second)[3:]
    cuts={len(first),len(second)-1}
    for at,n,kind in suffix:
        cuts.update([at,at+1,at+63,at+64,at+n-16,at+n-1,at+n])
    for cut in sorted(cuts):
        if cut==len(second):continue
        image.write_bytes(second[:cut])
        assert run('object-get',image,'alpha')==b'first\x00value',cut
        before=image.read_bytes()
        run('check',image)
        assert image.read_bytes()==before,'inspection modified tail'
        inp.write_bytes(b'after recovery')
        assert json.loads(run('object-put',image,'beta',inp))['last_committed_seq']==2
        assert run('object-get',image,'alpha')==b'first\x00value'
        assert run('object-get',image,'beta')==b'after recovery'
    # Real process exits at publication boundaries (not a simulated power failure).
    if len(sys.argv)>2:
        for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
            image.write_bytes(first)
            inp.write_bytes(b'crash candidate')
            p=subprocess.run([sys.argv[2],'object-put',str(image),'alpha',str(inp)],
                             env={**os.environ,'FORKFS_TEST_CRASH':stage},capture_output=True,timeout=10)
            assert p.returncode==86,(stage,p.stderr)
            expected=b'first\x00value' if stage.startswith('batch_') else b'crash candidate'
            assert run('object-get',image,'alpha')==expected,stage
            run('object-put',image,'next',inp)
            assert run('object-get',image,'alpha')==expected
    # Complete damaged headers/payloads/trailers fail closed and stay unchanged.
    for at,n,kind in records(second):
        for relative in [0,24,60,64,n-16,n-1]:
            bad=bytearray(second);bad[at+relative]^=1
            image.write_bytes(bad)
            run('check',image,ok=False)
            assert image.read_bytes()==bad
    image.write_bytes(first)
    inp.write_bytes(b'')
    run('object-put',image,'empty',inp)
    assert run('object-get',image,'empty')==b''
    before=image.read_bytes()
    inp.write_bytes(bytes(256*1024+1))
    run('object-put',image,'large',inp,ok=False)
    assert image.read_bytes()==before
    run('object-get',image,'absent',ok=False)
    # Enough full objects to cross a segment boundary, reopening after every commit.
    inp.write_bytes(bytes(range(256))*1024)
    for i in range(65):run('object-put',image,'rolling',inp)
    state=json.loads(run('check',image))
    assert state['last_committed_seq']==67 and not state['uncommitted_tail']
    assert run('object-get',image,'rolling')==inp.read_bytes()
    assert run('object-get',image,'alpha')==b'first\x00value'
    assert image.stat().st_size>65536+16*1024*1024
    print('journal persistence, SHA identity, truncation, corruption and rollover passed')
