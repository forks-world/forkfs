"""Daemon startup discovers backlog and publishes with crash-safe recovery."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

BIN,FAULT=sys.argv[1:3]
def run(*args):
    p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=10)
    assert p.returncode==0,(args,p.stderr)
    return p.stdout

def runs(image):return json.loads(run('fs-layout',image))['runs']
with tempfile.TemporaryDirectory(prefix='ffsvc-',dir='/tmp') as tmp:
    root=Path(tmp);image=root/'store';inp=root/'input';endpoint=root/'control.sock'
    run('init',image);run('fs-init',image);inp.write_bytes(b'original');run('fs-write',image,'/file',inp)
    run('fs-snapshot',image,'retained')
    i=0
    while runs(image)<16:
        i+=1;inp.write_bytes(b'latest'+bytes([i]));run('fs-write',image,'/file',inp)
    baseline=image.read_bytes();latest=inp.read_bytes()
    proc=subprocess.Popen([BIN,'serve',str(image),'--socket',str(endpoint)],
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while True:
            assert proc.poll() is None,(proc.returncode,proc.communicate())
            assert time.monotonic()<deadline,'daemon compaction did not finish'
            if endpoint.exists():
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(2);client.connect(str(endpoint));data=b''
                    while chunk:=client.recv(4096):data+=chunk
                state=json.loads(data)
                assert state['compaction_running'] and state['compaction_failures']==0
                if state['compaction_completed']>0:break
            time.sleep(.01)
        proc.send_signal(signal.SIGTERM);assert proc.wait(timeout=3)==0
        assert not endpoint.exists() and runs(image)==1
        assert run('fs-cat',image,'/file')==latest
        assert run('fs-cat',image,'/file','--revision','retained')==b'original'
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=3)
        proc.communicate()
    for stage in ['batch_written','batch_synced','commit_written','commit_synced']:
        image.write_bytes(baseline)
        proc=subprocess.Popen([FAULT,'serve',str(image),'--socket',str(endpoint)],
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                              env={**os.environ,'FORKFS_TEST_CRASH':stage})
        try:
            out,err=proc.communicate(timeout=5)
            assert proc.returncode==86,(stage,proc.returncode,out,err)
        finally:
            if proc.poll() is None:proc.kill();proc.wait(timeout=3)
            if endpoint.exists():endpoint.unlink() # owner is confirmed exited
        assert runs(image)==(1 if stage.startswith('commit_') else 16)
        assert run('fs-cat',image,'/file')==latest
        assert run('fs-cat',image,'/file','--revision','retained')==b'original'
        run('fs-check',image);run('checkpoint',image)
    print('daemon backlog discovery, status, shutdown and worker publication crashes passed')
