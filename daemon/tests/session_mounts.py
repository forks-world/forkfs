"""Durable mount registrations and the restore barrier over the manager RPC."""
import json, pathlib, socket, struct, subprocess, sys, tempfile, time
BIN=sys.argv[1]
def frame(fields):
    fs=[str(x).encode() for x in fields];p=struct.pack('<I',len(fs))+b''.join(struct.pack('<I',len(x))+x for x in fs);return b'FFRPC001'+struct.pack('<I',len(p))+p
def exact(s,n):
    b=b''
    while len(b)<n:
        part=s.recv(n-len(b));assert part,'truncated RPC response';b+=part
    return b
with tempfile.TemporaryDirectory(prefix='ff-mounts-') as td:
    root=pathlib.Path(td);db=root/'db';sockpath=root/'rpc.sock'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,'request',str(sockpath),*map(str,args)],capture_output=True,timeout=10)
        assert (p.returncode==0)==ok,(args,p.stderr);return p.stdout
    def rpc(*fields):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5);s.connect(str(sockpath));s.sendall(frame(fields));h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0]);return struct.unpack('<I',b[:4])[0],b[4:]
    def write(path,value):
        code,_=rpc('fs-write',path,value);assert code==0,code
    def busy_restore():
        code,_=rpc('fs-restore','base');assert code==(0x80000000|16),code
    assert subprocess.run([BIN,'init',str(db)]).returncode==0
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while not sockpath.exists():assert proc.poll() is None and time.monotonic()<deadline;time.sleep(.01)
        cli('fs-init');write('/f','old');cli('fs-snapshot','base');write('/f','new')
        gen=json.loads(cli('service-hello'))['generation']
        cli('mount-open','world','main','99','--generation',gen,ok=False);assert cli('fs-restore','base')
        write('/f','new')
        revision_mount=json.loads(cli('mount-open','revision','base','60000','--generation',gen))['session']
        assert cli('fs-restore','base')
        cli('mount-revoke',revision_mount,'--generation',gen);cli('mount-retire',gen,revision_mount,'--generation',gen)
        write('/f','new');cli('fs-fork','base','other')
        other_mount=json.loads(cli('mount-open','world','other','60000','--generation',gen))['session']
        assert cli('fs-restore','base')
        cli('mount-revoke',other_mount,'--generation',gen);cli('mount-retire',gen,other_mount,'--generation',gen)
        write('/f','new')
        full=[json.loads(cli('session-open','60000'))['session'] for _ in range(128)]
        cli('mount-open','world','main','60000','--generation',gen,ok=False)
        for token in full:cli('session-close',token)
        assert cli('fs-restore','base');write('/f','new')
        token=json.loads(cli('mount-open','world','main','1000','--generation',gen))['session']
        assert any(m['token']==token for m in json.loads(cli('mount-list','--generation',gen))['mounts'])
        check=json.loads(cli('mount-check',token,'--generation',gen));assert check=={'generation':gen,'name':'main','read_only':False}
        started=time.monotonic();deadline=started+4.0;checks=0;expired=False
        while time.monotonic()<deadline:
            code,_=rpc('mount-check',token,'--generation',gen)
            if code!=0:expired=True;break
            checks+=1;time.sleep(.12)
        assert expired,(checks,expired,time.monotonic()-started)
        busy_restore();assert cli('fs-cat','/f')==b'new'
        cli('mount-retire',gen,token,'--generation',gen);cli('mount-retire',gen,token,'--generation',gen)
        revoked=json.loads(cli('mount-open','world','main','60000','--generation',gen))['session']
        busy_restore();cli('mount-revoke',revoked,'--generation',gen);busy_restore()
        cli('mount-check',revoked,'--generation',gen,ok=False)
        assert cli('fs-cat','/f')==b'new'
        proc.terminate();proc.wait(timeout=5)
        offline=subprocess.run([BIN,'fs-restore',str(db),'base'],capture_output=True,timeout=10)
        assert offline.returncode!=0 and b'durable mount registrations' in offline.stderr,offline.stderr
        sockpath=root/'rpc-restarted.sock'
        proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+5
        while not sockpath.exists():assert proc.poll() is None and time.monotonic()<deadline;time.sleep(.01)
        newgen=json.loads(cli('service-hello'))['generation'];busy_restore()
        assert any(m['token']==revoked and m['generation']==gen for m in json.loads(cli('mount-list','--generation',newgen))['mounts'])
        cli('mount-retire',gen,revoked,'--generation',newgen);cli('mount-retire',gen,revoked,'--generation',newgen)
        assert cli('fs-restore','base');assert cli('fs-cat','/f')==b'old'
        proc.terminate();proc.wait(timeout=5)
        offline=subprocess.run([BIN,'fs-restore',str(db),'base'],capture_output=True,timeout=10)
        assert offline.returncode==0,offline.stderr
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
