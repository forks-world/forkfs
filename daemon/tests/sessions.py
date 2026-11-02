"""RPC session ownership, heartbeat, quota and lease-driven handle release."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
BIN=sys.argv[1]
def frame(fields):
    fields=[f if isinstance(f,bytes) else str(f).encode() for f in fields]
    b=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(f))+f for f in fields)
    return b'FFRPC001'+struct.pack('<I',len(b))+b

def exact(s,n):
    b=b''
    while len(b)<n:
        part=s.recv(n-len(b));assert part;b+=part
    return b
with tempfile.TemporaryDirectory(prefix='ff-session-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';endpoint=root/'control.sock'
    p=subprocess.run([BIN,'init',str(db)],capture_output=True,timeout=10);assert p.returncode==0,p.stderr
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def rpc(*fields,ok=True):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5);s.connect(str(endpoint));s.sendall(frame(fields))
            h=exact(s,12);assert h[:8]==b'FFRSP001';b=exact(s,struct.unpack('<I',h[8:])[0])
            code=struct.unpack('<I',b[:4])[0];assert (code==0)==ok,(fields[:2],b)
            return b[4:]
    def session(ms=60000):return json.loads(rpc('session-open',ms))['session']
    def open_file(path,s,*view):return json.loads(rpc('fs-open',path,'rw',*view,'--session',s))['handle']
    def status():return json.loads(rpc('status'))
    def wait_released(before):
        deadline=time.monotonic()+3
        while True:
            after=status()
            if after['object_roots']==before['object_roots']-2:return after
            assert time.monotonic()<deadline,(before,after);time.sleep(.02)
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            assert proc.poll() is None,proc.communicate()
            assert time.monotonic()<deadline;time.sleep(.01)
        rpc('fs-init');rpc('fs-write','/f',b'original')
        for invalid in [0,99,300001,-1,'1x']:rpc('session-open',invalid,ok=False)
        a,b=session(),session();ha,hb=open_file('/f',a),open_file('/f',b)
        rpc('fs-unlink','/f');rpc('fs-hwrite',ha,0,b'X','--session',a)
        assert rpc('fs-hread',hb,0,8,'--session',b)==b'Xriginal'
        before=status()
        rpc('fs-hread',ha,0,8,'--session',b,ok=False)
        rpc('fs-close',ha,'--session',b,ok=False)
        rpc('fs-hstat',ha,ok=False);rpc('fs-open','/missing','r',ok=False)
        rpc('fs-hstat',ha,'--session',a,'--session',a,ok=False)
        assert status()['last_committed_seq']==before['last_committed_seq']
        rpc('session-close',a);rpc('fs-hread',ha,0,8,'--session',a,ok=False)
        assert rpc('fs-hread',hb,0,8,'--session',b)==b'Xriginal'
        before=status();rpc('session-close',b);after=status()
        assert after['object_roots']==before['object_roots']-2
        assert after['indexed_objects']==before['indexed_objects'] # No physical reclamation.
        rpc('session-keepalive',b,ok=False);rpc('session-close',b,ok=False)
        # One session owns handles in multiple Worlds and readonly revisions.
        rpc('fs-write','/multi',b'M');rpc('fs-snapshot','base');rpc('fs-fork','base','work')
        c=session();open_file('/multi',c);open_file('/multi',c,'--world','work')
        old=json.loads(rpc('fs-open','/multi','r','--revision','base','--session',c))['handle']
        assert rpc('fs-hread',old,0,1,'--session',c,'--revision','base')==b'M'
        rpc('fs-unlink','/multi');rpc('fs-unlink','/multi','--world','work')
        before=status();rpc('session-close',c);after=status()
        assert after['object_roots']==before['object_roots']-4
        assert rpc('fs-cat','/multi','--revision','base')==b'M'
        rpc('fs-check');rpc('fs-check','--world','work');rpc('fs-check','--revision','base')
        # No heartbeat: orphan retention releases automatically.
        rpc('fs-write','/expire',b'E');short=session(100);h=open_file('/expire',short)
        rpc('fs-unlink','/expire');before=status();after=wait_released(before)
        assert after['indexed_objects']==before['indexed_objects']
        rpc('fs-hstat',h,'--session',short,ok=False)
        # Both keepalive and ordinary requests renew the lease.
        rpc('fs-write','/live',b'L');live=session(500);h=open_file('/live',live)
        rpc('fs-unlink','/live')
        for _ in range(4):
            time.sleep(.2);rpc('session-keepalive',live)
        for _ in range(4):
            time.sleep(.2);assert rpc('fs-hread',h,0,1,'--session',live)==b'L'
        before=status();wait_released(before);rpc('session-keepalive',live,ok=False)
        # A lost open reply cannot leak the descriptor indefinitely.
        rpc('fs-write','/lost',b'R');lost=session(500)
        with socket.socket(socket.AF_UNIX) as s:
            s.connect(str(endpoint));s.sendall(frame(['fs-open','/lost','rw','--session',lost]))
        rpc('status');rpc('fs-unlink','/lost');before=status();wait_released(before)
        # Atomic create/exclusive/truncate over RPC, including malformed flags.
        creation=session();created=json.loads(rpc('fs-open','/created','rw,create,exclusive','--session',creation))['handle']
        rpc('fs-hwrite',created,0,b'C','--session',creation)
        rpc('fs-open','/created','rw,create,exclusive','--session',creation,ok=False)
        assert rpc('fs-cat','/created')==b'C'
        reopened=json.loads(rpc('fs-open','/created','r,create','--session',creation))['handle']
        assert json.loads(rpc('fs-hstat',reopened,'--session',creation))['inode_id']==json.loads(rpc('fs-hstat',created,'--session',creation))['inode_id']
        rpc('fs-open','/created','w,truncate','--session',creation)
        assert rpc('fs-hread',created,0,1,'--session',creation)==b''
        before=status()
        for mode in ['r,truncate','rw,exclusive','rw,create,create','rw,unknown','rw,create,']:
            rpc('fs-open','/untouched',mode,'--session',creation,ok=False)
        rpc('fs-open','/multi','r,create','--revision','base','--session',creation,ok=False)
        assert status()['last_committed_seq']==before['last_committed_seq']
        rpc('fs-stat','/untouched',ok=False);rpc('session-close',creation)
        app=session();h=json.loads(rpc('fs-open','/app','rw,create,append','--session',app))['handle']
        rpc('fs-hwrite',h,0,b'A','--session',app);rpc('fs-hwrite',h,999999,b'B','--session',app)
        assert rpc('fs-hread',h,0,5,'--session',app)==b'AB'
        rpc('fs-unlink','/app');rpc('fs-hwrite',h,0,b'\0C','--session',app)
        assert rpc('fs-hread',h,0,5,'--session',app)==b'AB\0C'
        before=status();rpc('fs-open','/bad-append','r,create,append','--session',app,ok=False)
        rpc('fs-open','/bad-append','rw,create,append,append','--session',app,ok=False)
        assert status()['last_committed_seq']==before['last_committed_seq']
        rpc('fs-stat','/bad-append',ok=False);rpc('session-close',app)
        cursor_session=session();h=json.loads(rpc('fs-open','/cursor','rw,create','--session',cursor_session))['handle']
        duplicate=json.loads(rpc('fs-dup',h,'--session',cursor_session))['handle']
        assert json.loads(rpc('fs-hwrite-next',h,b'a\0bc','--session',cursor_session))['written']==4
        assert json.loads(rpc('fs-hseek',duplicate,-2,'cur','--session',cursor_session))['offset']==2
        assert rpc('fs-hread-next',h,9,'--session',cursor_session)==b'bc'
        assert json.loads(rpc('fs-hseek',h,-1,'end','--session',cursor_session))['offset']==3
        rpc('fs-hseek',h,-9,'set','--session',cursor_session,ok=False)
        rpc('fs-hseek',h,'9223372036854775808','set','--session',cursor_session,ok=False)
        other=session();rpc('fs-dup',h,'--session',other,ok=False);rpc('session-close',other)
        rpc('fs-close',h,'--session',cursor_session)
        assert rpc('fs-hread-next',duplicate,1,'--session',cursor_session)==b'c'
        rpc('session-close',cursor_session)
        rpc('fs-write','/lock-release','L');owner=session();waiter=session()
        ha=open_file('/lock-release',owner);hb=open_file('/lock-release',waiter)
        hd=json.loads(rpc('fs-dup',ha,'--session',owner))['handle']
        def lock(h,mode,s):return json.loads(rpc('fs-hlock',h,mode,'--session',s))['granted']
        assert lock(ha,'exclusive',owner) and not lock(hb,'shared',waiter)
        rpc('fs-hlock',ha,'exclusive','--session',waiter,ok=False)
        rpc('fs-hlock',ha,'invalid','--session',owner,ok=False)
        rpc('fs-close',ha,'--session',owner);assert not lock(hb,'exclusive',waiter)
        rpc('session-close',owner);assert lock(hb,'exclusive',waiter)
        assert lock(hb,'unlock',waiter)
        timed=session(100);ht=open_file('/lock-release',timed);assert lock(ht,'exclusive',timed)
        assert not lock(hb,'exclusive',waiter)
        deadline=time.monotonic()+3
        while not lock(hb,'exclusive',waiter):
            assert time.monotonic()<deadline;time.sleep(.02)
        # Locks are advisory: ordinary writes do not implicitly acquire them.
        rpc('fs-write','/lock-release','unlocked-writer')
        rpc('fs-write','/lock-world','W');rpc('fs-snapshot','lock-base');rpc('fs-fork','lock-base','lock-work')
        hm=open_file('/lock-world',waiter);hw=open_file('/lock-world',waiter,'--world','lock-work')
        hr=json.loads(rpc('fs-open','/lock-world','r','--revision','lock-base','--session',waiter))['handle']
        assert lock(hm,'exclusive',waiter)
        assert json.loads(rpc('fs-hlock',hw,'exclusive','--world','lock-work','--session',waiter))['granted']
        assert json.loads(rpc('fs-hlock',hr,'shared','--revision','lock-base','--session',waiter))['granted']
        rpc('session-close',waiter)
        range_owner=session();range_waiter=session();ra=open_file('/lock-release',range_owner);rb=open_file('/lock-release',range_waiter)
        def rlock(h,mode,bounds,s,*view):return json.loads(rpc('fs-hrange-lock',h,mode,bounds,*view,'--session',s))['granted']
        assert rlock(ra,'exclusive','0:10',range_owner)
        assert rlock(rb,'exclusive','10:10',range_waiter) and not rlock(rb,'shared','9:2',range_waiter)
        assert not lock(rb,'exclusive',range_waiter)
        assert rlock(ra,'unlock','3:4',range_owner) and rlock(rb,'exclusive','3:4',range_waiter)
        rpc('fs-hrange-lock',ra,'unlock','0:0','--session',range_waiter,ok=False)
        for bounds in ['-1:0','0:-1','1:2:3',':1','1:','9223372036854775807:2']:
            rpc('fs-hrange-lock',ra,'shared',bounds,'--session',range_owner,ok=False)
        rpc('session-close',range_owner);assert lock(rb,'exclusive',range_waiter)
        rpc('session-close',range_waiter)
        scoped=session();world_handle=open_file('/lock-world',scoped,'--world','lock-work')
        assert rlock(world_handle,'exclusive','0:0',scoped,'--world','lock-work') # Fits eight-field protocol.
        rpc('session-close',scoped)
        # Bounded session and per-session descriptor counts; release restores capacity.
        rpc('fs-write','/quota',b'Q');quota=session(300000)
        handles=[open_file('/quota',quota) for _ in range(256)]
        rpc('fs-open','/quota','r','--session',quota,ok=False)
        rpc('fs-open','/quota-new','rw,create','--session',quota,ok=False)
        rpc('fs-open','/quota','w,truncate','--session',quota,ok=False)
        rpc('fs-stat','/quota-new',ok=False);assert rpc('fs-cat','/quota')==b'Q'
        rpc('fs-close',handles[0],'--session',quota);open_file('/quota',quota)
        rpc('session-close',quota)
        tokens=[session(300000) for _ in range(128)]
        rpc('session-open',300000,ok=False)
        for token in tokens:rpc('session-close',token)
        rpc('session-close',session())
        rpc('fs-check')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0,proc.communicate()
        assert not endpoint.exists()
        print('session ownership, expiry, heartbeat, lost replies and bounded quotas passed')
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
