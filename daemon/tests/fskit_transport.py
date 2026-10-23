"""Real daemon/callback transport, lease renewal and restart invalidation.

Same-UID fixture tests the wire path; root CI separately tests distinct UIDs and
resource admission. No actual FSKit mount or installed signature is claimed.
"""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary, driver = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='ff-transport-', dir='/tmp') as temp:
    root=Path(temp);root.chmod(0o700);store=root/'store';endpoint=root/'rpc.sock'
    subprocess.run([binary,'init',str(store)],check=True,capture_output=True,timeout=20)
    process=None
    def serve():
        global process
        process=subprocess.Popen([binary,'serve',str(store),'--socket',str(endpoint),'--rpc'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+10
        while not endpoint.exists():
            assert process.poll() is None,process.communicate()
            assert time.monotonic()<deadline;time.sleep(.02)
    def rpc(*fields):
        payload=struct.pack('<I',len(fields))
        for field in fields:
            data=str(field).encode();payload+=struct.pack('<I',len(data))+data
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(10);peer.connect(str(endpoint));peer.sendall(b'FFRPC001'+struct.pack('<I',len(payload))+payload)
            def exact(n):
                out=b''
                while len(out)<n:
                    part=peer.recv(n-len(out));assert part;out+=part
                return out
            header=exact(12);assert header[:8]==b'FFRSP001';data=exact(struct.unpack('<I',header[8:])[0]);assert data[:4]==b'\0'*4,data
            return json.loads(data[4:])
    child=None
    try:
        serve();rpc('fs-init')
        generation=rpc('service-hello')['generation']
        session=rpc('mount-open','world','main',3000,'--generation',generation)['session']
        config=root/'config';config.write_text(f'{endpoint}\n{os.geteuid()}\n{generation}\n{session}\nmain\n0\n')
        child=subprocess.Popen([driver,'--test-config',str(config),'wait-restart'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        # communicate is bounded externally by CTest; do not send restart until
        # callbacks and idle renewal actually ran.
        assert child.stdout.readline().strip()=='READY',child.communicate(timeout=10)
        process.terminate();process.wait(timeout=10);serve()
        child.stdin.write('R');child.stdin.flush()
        out,err=child.communicate(timeout=20);assert child.returncode==0,(out,err)
        assert out.strip()=='OK',(out,err)
        assert rpc('service-hello')['generation']!=generation
        rpc('fs-snapshot','base')
        generation=rpc('service-hello')['generation']
        session=rpc('mount-open','revision','base',3000,'--generation',generation)['session']
        config.write_text(f'{endpoint}\n{os.geteuid()}\n{generation}\n{session}\nbase\n1\n')
        result=subprocess.run([driver,'--test-config',str(config)],capture_output=True,text=True,timeout=20)
        assert result.returncode==0,(result.stdout,result.stderr)

    finally:
        if child and child.poll() is None:child.kill();child.communicate(timeout=5)
        if process and process.poll() is None:process.terminate();process.wait(timeout=10)
print('RPC callbacks, binary names, retained cursors, idle lease and restart invalidation passed')
