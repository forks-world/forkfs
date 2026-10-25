"""ASCII base + byte-name deltas survive detached disaster replay and restore."""
import subprocess
from pathlib import Path
import random
import sys
import tempfile

binary=sys.argv[1]
def run(*args):
    result=subprocess.run([binary,*[a if isinstance(a,bytes) else str(a) for a in args]],
        capture_output=True,timeout=20)
    assert result.returncode==0,(args,result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix='ff-name-replay-') as temp:
    root=Path(temp);source=root/'source';recovered=root/'recovered';payload=root/'payload'
    run('init',source);run('fs-init',source);payload.write_bytes(b'legacy-ascii')
    run('fs-write',source,'/ascii',payload)
    base=root/'ascii-base.pack';sequence=int(run('backup',source,base))
    run('replay',base,recovered)
    assert run('fs-cat',recovered,'/ascii')==b'legacy-ascii'
    parent='/父目录😀'.encode();run('fs-mkdir',source,parent)
    names=['中文', '🧪', 'é', 'e\u0301', 'Case', 'case', 'line\nfeed', 'quote"\\'].copy()
    names=[name.encode() for name in names]+[b'raw-\xff']
    rng=random.Random(0xC0FFEE)
    names += [('文件'+str(rng.randrange(10**9))+'🛰️').encode() for _ in range(16)]
    expected={}
    for i,name in enumerate(names):
        value=bytes([i,0,255])+name;payload.write_bytes(value)
        run('fs-write',source,parent+b'/'+name,payload);expected[name]=value
    run('fs-link',source,parent+b'/'+names[0],parent+'/别名'.encode())
    run('fs-symlink',source,names[0],parent+'/链接'.encode())
    run('fs-snapshot',source,'names0');run('fs-fork',source,'names0','work')
    moved='/移动目录🧵'.encode();run('fs-rename',source,parent,moved,'--world','work')
    payload.write_bytes(b'changed');run('fs-write',source,moved+b'/'+names[0],payload,'--world','work')
    run('fs-snapshot',source,'names1','--world','work')
    delta=root/'names.delta';end=int(run('replay-export',source,sequence,delta))
    assert int(run('replay',delta,recovered))==end
    for name,value in expected.items():
        assert run('fs-cat',recovered,parent+b'/'+name)==value
        assert run('fs-cat',recovered,parent+b'/'+name,'--revision','names0')==value
        assert run('fs-cat',recovered,moved+b'/'+name,'--world','work')==(b'changed' if name==names[0] else value)
    assert run('fs-cat',recovered,moved+'/别名'.encode(),'--world','work')==b'changed'
    assert run('fs-readlink',recovered,parent+'/链接'.encode())==names[0]
    for selector in ([],['--world','work'],['--revision','names0'],['--revision','names1']):run('fs-check',recovered,*selector)
    run('fs-restore',recovered,'names0','--world','work')
    assert run('fs-cat',recovered,parent+b'/'+names[0],'--world','work')==expected[names[0]]
    run('fs-check',recovered,'--world','work')
print('ASCII base, Unicode/raw-name replay, hardlinks, symlinks and restore passed')
