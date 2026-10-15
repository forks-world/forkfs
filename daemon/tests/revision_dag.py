"""Real multi-parent histories, mixed format import and catalog replay."""
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from replay_corruption import unpack,pack,base_rows,base_frames,h,revision_with_source
BIN=str(Path(sys.argv[1]).resolve())
def run(*args,ok=True):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=30)
    assert p.returncode==(0 if ok else 1),(args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-dag-') as directory:
    root=Path(directory);db=root/'db';run('init',db);run('fs-init',db)
    run('fs-snapshot',db,'a');run('fs-fork',db,'a','branch')
    payload=root/'input';payload.write_bytes(b'left');run('fs-write',db,'/file',payload);run('fs-snapshot',db,'b')
    payload.write_bytes(b'right');run('fs-write',db,'/file',payload,'--world','branch');run('fs-snapshot',db,'c','--world','branch')
    base=root/'base';start=int(run('backup',db,base))
    run('fs-snapshot-parents',db,'merge','b,c');run('fs-snapshot',db,'next')
    run('fs-snapshot-parents',db,'duplicate','b,b',ok=False);run('fs-snapshot-parents',db,'missing','b,unknown',ok=False)
    run('fs-snapshot-parents',db,'too-many',','.join(['b']*65),ok=False)
    full=root/'full';run('backup',db,full);header,frames=unpack(full.read_bytes());rows=base_rows(frames)
    prefix=b'R/\0forkfs-history/revision/'
    def descriptor(name):return rows[b'O/'+rows[prefix+name.encode()]]
    def parents(b):
        assert b[:8]==b'FFREV002';n,=struct.unpack_from('<I',b,48);assert len(b)==52+n*32
        return [b[52+32*i:84+32*i] for i in range(n)]
    assert parents(descriptor('merge'))==[rows[prefix+b'b'],rows[prefix+b'c']]
    assert parents(descriptor('next'))==[rows[prefix+b'merge']]
    delta=root/'delta';run('replay-export',db,start,delta);catalog=root/'catalog'
    for p in (base,delta):run('catalog-add',catalog,p)
    restored=root/'restored';run('restore',restored,'--revision','merge','--catalog',catalog)
    assert run('fs-cat',restored,'/file')==b'left' # Metadata ancestry does not merge content.
    assert run('fs-cat',restored,'/file','--world','branch')==b'right'
    run('fs-check',restored)
    # Mixed V1/V2 ancestry and large source watermarks remain valid DAGs.
    d=descriptor('a');legacy=b'FFREV001'+d[8:40]+bytes(32)+struct.pack('<Q',2**64-1)
    oid=h(legacy);rows[b'O/'+oid]=legacy;rows[prefix+b'legacy']=oid
    d=revision_with_source(descriptor('merge'),0);d=d[:48]+struct.pack('<I',2)+oid+rows[prefix+b'c']
    oid=h(d);rows[b'O/'+oid]=d;rows[prefix+b'imported']=oid
    rows[b'M/objects']=struct.pack('<Q',sum(k.startswith(b'O/') for k in rows));rows[b'M/roots']=struct.pack('<Q',sum(k.startswith(b'R/') for k in rows))
    mixed=root/'mixed';mixed.write_bytes(pack(header,base_frames(rows)));run('replay',mixed,root/'mixed-db')
    run('fs-fork',root/'mixed-db','imported','imported-world');run('fs-check',root/'mixed-db','--world','imported-world')
    run('backup',root/'mixed-db',root/'mixed-roundtrip')
    # The RPC entry point also accepts explicit parents.
    endpoint=root/'rpc.sock';proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+10
        while not endpoint.exists():
            assert proc.poll() is None and time.monotonic()<deadline;time.sleep(.01)
        run('request',endpoint,'fs-snapshot-parents','rpc-merge','b,c')
        run('request',endpoint,'fs-snapshot-parents','rpc-bad','b,b',ok=False)
    finally:proc.terminate();proc.wait(timeout=30)
print('revision DAG diamond, multi-parent CLI/RPC, mixed format and source-independent ancestry passed')
