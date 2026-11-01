"""Checkpoint publication, tail replay, indexed reads and corruption handling."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BIN,FAULT=sys.argv[1:3]
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert (p.returncode==0)==ok,(args,p.stdout,p.stderr)
    return p.stdout

def state(image):return json.loads(run('check',image))
def newest(data):
    return max((0,4096),key=lambda off:int.from_bytes(data[off+32:off+40],'little'))

with tempfile.TemporaryDirectory(prefix='ffc-',dir='/tmp') as tmp:
    root=Path(tmp); image=root/'store'; value=root/'value'
    run('init',image)
    run('checkpoint',image)
    assert state(image)['last_committed_seq']==0
    for name,contents in [('one',b'old'),('two',b'second'),('one',b'new'),('alias',b'new')]:
        value.write_bytes(contents);run('object-put',image,name,value)
    before=image.read_bytes()
    assert state(image)['replayed_transactions']==4
    run('checkpoint',image)
    stable=image.read_bytes()
    s=state(image)
    assert s['checkpoint_seq']==4 and s['replayed_transactions']==0
    assert s['indexed_objects']==3 and s['object_roots']==3
    assert run('object-get',image,'one')==b'new'
    assert run('object-get',image,'two')==b'second'
    value.write_bytes(b'after checkpoint')
    run('object-put',image,'tail',value)
    with_tail=image.read_bytes()
    s=state(image)
    assert s['last_committed_seq']==5 and s['replayed_transactions']==1
    assert run('object-get',image,'tail')==value.read_bytes()
    # Losing the new anchor must replay the checkpoint hint and all later commits.
    bad=bytearray(with_tail);bad[newest(bad)+4092]^=1
    image.write_bytes(bad)
    assert state(image)['last_committed_seq']==5
    assert run('object-get',image,'one')==b'new'
    run('checkpoint',image)
    assert state(image)['replayed_transactions']==0
    # A published checkpoint's corrupted body must fail, not silently roll back.
    bad=bytearray(stable);off=newest(bad)
    checkpoint=int.from_bytes(bad[off+64:off+72],'little')
    bad[checkpoint+64]^=1
    image.write_bytes(bad)
    run('check',image,ok=False)
    assert image.read_bytes()==bad
    # Indexed content corruption is checked even though startup skips old log bytes.
    bad=bytearray(stable)
    at=65536+4096
    while at<len(bad):
        n=int.from_bytes(bad[at+8:at+12],'little')
        kind=int.from_bytes(bad[at+4:at+6],'little')
        if kind==1:
            bad[at+68]^=1
            break
        at+=n
    image.write_bytes(bad)
    run('check',image,ok=False)
    # Every publication crash point preserves all committed roots.
    for stage in ['checkpoint_written','checkpoint_synced','anchor_written','anchor_synced']:
        image.write_bytes(before)
        p=subprocess.run([FAULT,'checkpoint',str(image)],capture_output=True,timeout=10,
                         env={**os.environ,'FORKFS_TEST_CRASH':stage})
        assert p.returncode==86,(stage,p.stderr)
        assert state(image)['last_committed_seq']==4
        assert run('object-get',image,'one')==b'new'
        assert run('object-get',image,'two')==b'second'
        value.write_bytes(b'continued')
        run('object-put',image,'continued',value)
        run('checkpoint',image)
        assert state(image)['last_committed_seq']==5
        assert state(image)['replayed_transactions']==0
    # Torn unpublished checkpoint body is a tail; next write safely replaces it.
    off=newest(stable);cp=int.from_bytes(stable[off+64:off+72],'little')
    for cut in [cp,cp+1,cp+63,cp+64,len(stable)-1]:
        data=bytearray(stable[:cut]);data[:8192]=before[:8192]
        image.write_bytes(data)
        assert run('object-get',image,'one')==b'new'
        value.write_bytes(b'repaired tail')
        run('object-put',image,'recovered',value)
        run('checkpoint',image)
        assert run('object-get',image,'recovered')==value.read_bytes()
    print('checkpoint publication, fallback, tail replay, integrity and process crashes passed')
