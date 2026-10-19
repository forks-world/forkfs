"""Stateful differential test against real OS files, with replayable action traces.
No forkfs codecs, block/index constants or storage readers are imported.
"""
from collections import Counter
import argparse
import errno
import json
import os
from pathlib import Path
import random
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import time

class Mismatch(AssertionError): pass

class Trial:
    def __init__(self, binary, root):
        self.binary, self.root = binary, root
        self.db=root/'db';self.socket_dir=tempfile.TemporaryDirectory(prefix='ff-native-',dir='/tmp');self.endpoint=Path(self.socket_dir.name)/'rpc.sock'
        self.proc = None; self.session = None; self.handles = {};self.monitors={}; self.trace = [];self.error_counts=Counter()
        self.world = 'main'; self.native = root/'native'; self.native.mkdir()
        self.worlds={'main':self.native};self.snapshots={}
        self.cli('init', self.db)
        try: self.start();self.rpc('fs-init')
        except BaseException: self.stop();self.socket_dir.cleanup();raise
    def cli(self, *args):
        p = subprocess.run([self.binary, *map(str, args)], capture_output=True, timeout=30)
        if p.returncode: raise RuntimeError(p.stderr.decode())
        return p.stdout
    def start(self):
        self.endpoint.unlink(missing_ok=True)
        self.log = (self.root/'daemon.log').open('ab')
        self.proc = subprocess.Popen([self.binary, 'serve', str(self.db), '--socket', str(self.endpoint), '--rpc'], stdout=self.log, stderr=self.log)
        deadline = time.monotonic()+15
        while True:
            if self.proc.poll() is not None or time.monotonic()>deadline: raise RuntimeError('daemon failed to start')
            try:
                self.session=json.loads(self.rpc('session-open','60000',world=False))['session'];break
            except (FileNotFoundError,ConnectionRefusedError):
                # bind creates the socket path before listen is ready.
                time.sleep(.01)

    def stop(self):
        for fd, _, _ in self.handles.values(): os.close(fd)
        self.handles.clear()
        for fd,_ in self.monitors.values(): os.close(fd)
        self.monitors.clear()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate(); self.proc.wait(timeout=30)
        if self.proc: self.log.close(); self.proc = None
    def rpc(self, op, *args, world=True, revision=None):
        fields = [op, *args]
        if revision is not None: fields += ['--revision',revision]
        elif world: fields += ['--world', self.world]
        if op in ('fs-open', 'fs-close', 'fs-dup') or op.startswith('fs-h'): fields += ['--session', self.session]
        fields = [v if isinstance(v, bytes) else str(v).encode() for v in fields]
        body = struct.pack('<I', len(fields))+b''.join(struct.pack('<I', len(v))+v for v in fields)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(15); sock.connect(str(self.endpoint)); sock.sendall(b'FFRPC001'+struct.pack('<I', len(body))+body)
            def exact(n):
                value = bytearray()
                while len(value)<n:
                    part=sock.recv(n-len(value))
                    if not part: raise RuntimeError('truncated RPC response')
                    value.extend(part)
                return bytes(value)
            header=exact(12)
            if header[:8]!=b'FFRSP001': raise RuntimeError('RPC framing')
            reply=exact(struct.unpack_from('<I', header, 8)[0]); code=struct.unpack_from('<I', reply)[0]
        if code:
            if code & 0x80000000: raise OSError(code & 0x7fffffff, reply[4:].decode())
            raise OSError(None, reply[4:].decode())
        return reply[4:]
    def clone_native(self, source, destination):
        destination.mkdir();groups={}
        for path in sorted(source.iterdir()):
            target=destination/path.name;inode=path.stat().st_ino
            if inode in groups: os.link(groups[inode],target)
            else: shutil.copy2(path,target);groups[inode]=target
    def close_handles(self):
        for fd,token,_ in self.handles.values(): os.close(fd);self.rpc('fs-close',token)
        self.handles.clear()
        for fd,token in self.monitors.values(): os.close(fd);self.rpc('fs-close',token)
        self.monitors.clear()
    def compare_tree(self, directory, revision=None):
        expected = sorted(p.name for p in directory.iterdir())
        got = self.rpc('fs-ls', '/',revision=revision).decode().splitlines()
        if expected!=got: raise Mismatch(('entries', expected, got))
        pairs=[]
        for name in expected:
            host=directory/name; a=host.stat(); b=json.loads(self.rpc('fs-stat', '/'+name,revision=revision))
            if (a.st_size, a.st_nlink, stat.S_IMODE(a.st_mode)) != (b['size'], b['links'], b['mode']):
                raise Mismatch(('attributes', name, a.st_size, a.st_nlink, stat.S_IMODE(a.st_mode), b))
            if host.read_bytes()!=self.rpc('fs-cat', '/'+name,revision=revision): raise Mismatch(('content', name,revision))
            pairs.append((a.st_ino, b['inode_id']))
        for x in pairs:
            for y in pairs:
                if (x[0]==y[0])!=(x[1]==y[1]): raise Mismatch(('hardlink identity', pairs))
    def compare(self):
        self.compare_tree(self.native)
        for key,(fd,token,_) in self.handles.items():
            a=os.fstat(fd);b=json.loads(self.rpc('fs-hstat', token))
            if (a.st_size,a.st_nlink)!=(b['size'],b['links']): raise Mismatch(('retained handle',key,a.st_size,a.st_nlink,b))
            native_position=os.lseek(fd,0,os.SEEK_CUR);forkfs_position=json.loads(self.rpc('fs-hseek',token,0,'cur'))['offset']
            if native_position!=forkfs_position: raise Mismatch(('cursor',key,native_position,forkfs_position))
            reader,reference=self.monitors[key]
            if os.pread(reader,a.st_size,0)!=self.rpc('fs-hread',reference,0,a.st_size): raise Mismatch(('retained content',key))
    def action(self, a):
        self.trace.append(a)
        op=a['op']; path=a.get('path'); host=self.native/path.lstrip('/') if path else None
        value=bytes.fromhex(a.get('data',''))
        if op=='write':
            fd=os.open(host,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o644)
            try: os.write(fd,value)
            finally: os.close(fd)
            self.rpc('fs-write',path,value)
        elif op=='at':
            fd=os.open(host,os.O_WRONLY)
            try: os.pwrite(fd,value,a['offset'])
            finally: os.close(fd)
            self.rpc('fs-write-at',path,a['offset'],value)
        elif op=='truncate': os.truncate(host,a['size']);self.rpc('fs-truncate',path,a['size'])
        elif op=='link': os.link(host,self.native/a['to'].lstrip('/'));self.rpc('fs-link',path,a['to'])
        elif op=='rename': os.replace(host,self.native/a['to'].lstrip('/'));self.rpc('fs-rename',path,a['to'])
        elif op=='unlink': host.unlink();self.rpc('fs-unlink',path)
        elif op=='replace':
            mode=stat.S_IMODE(host.stat().st_mode) if host.exists() else 0o644
            tmp=self.root/'replacement';tmp.write_bytes(value);os.chmod(tmp,mode);os.replace(tmp,host);self.rpc('fs-save',path,value)
        elif op=='chmod': os.chmod(host,a['mode']);self.rpc('fs-chmod',path,format(a['mode'],'o'))
        elif op=='open':
            access={'r':os.O_RDONLY,'w':os.O_WRONLY,'rw':os.O_RDWR}[a['access'].split(',')[0]]
            if ',append' in a['access']: access |= os.O_APPEND
            fd=os.open(host,access)
            try: token=json.loads(self.rpc('fs-open',path,a['access']))['handle']
            except BaseException: os.close(fd);raise
            self.handles[a['handle']]=(fd,token,a['access'])
            reader=os.open(host,os.O_RDONLY)
            try: reference=json.loads(self.rpc('fs-open',path,'r'))['handle']
            except BaseException: os.close(reader);raise
            self.monitors[a['handle']]=(reader,reference)
        elif op=='dup':
            fd,token,access=self.handles[a['source']]
            duplicate=os.dup(fd)
            try: result=json.loads(self.rpc('fs-dup',token))['handle']
            except BaseException: os.close(duplicate);raise
            self.handles[a['handle']]=(duplicate,result,access)
            reader,reference=self.monitors[a['source']];duplicate=os.dup(reader)
            try: result=json.loads(self.rpc('fs-dup',reference))['handle']
            except BaseException: os.close(duplicate);raise
            self.monitors[a['handle']]=(duplicate,result)
        elif op=='close':
            fd,token,_=self.handles.pop(a['handle']);os.close(fd);self.rpc('fs-close',token)
            reader,reference=self.monitors.pop(a['handle']);os.close(reader);self.rpc('fs-close',reference)
        elif op in ('hwrite','hread','htruncate','hseek','hnext','hreadnext'):
            fd,token,_=self.handles[a['handle']]
            def native():
                if op=='hwrite': return os.pwrite(fd,value,a['offset'])
                if op=='hread': return os.pread(fd,a['count'],a['offset'])
                if op=='htruncate': return os.ftruncate(fd,a['size'])
                if op=='hseek': return os.lseek(fd,a['offset'],os.SEEK_SET)
                if op=='hnext': return os.write(fd,value)
                return os.read(fd,a['count'])
            def forkfs():
                if op=='hwrite': self.rpc('fs-hwrite',token,a['offset'],value);return len(value)
                if op=='hread': return self.rpc('fs-hread',token,a['offset'],a['count'])
                if op=='htruncate': self.rpc('fs-htruncate',token,a['size']);return None
                if op=='hseek': return json.loads(self.rpc('fs-hseek',token,a['offset'],'set'))['offset']
                if op=='hnext': return json.loads(self.rpc('fs-hwrite-next',token,value))['written']
                return self.rpc('fs-hread-next',token,a['count'])
            try: expected=('ok',native())
            except OSError as e:
                # Native ftruncate uses EINVAL for a non-writable descriptor on
                # some hosts; forkfs explicitly uses EBADF for handle access.
                code=errno.EBADF if op=='htruncate' and self.handles[a['handle']][2]=='r' and e.errno==errno.EINVAL else e.errno
                expected=('error',code)
            try: actual=('ok',forkfs())
            except OSError as e: actual=('error',e.errno)
            if actual!=expected: raise Mismatch(('operation',a,expected,actual))
            if expected[0]=='error': self.error_counts[f'{op}:{expected[1]}']+=1
        elif op=='fork':
            self.close_handles();self.rpc('fs-snapshot',a['revision']);self.rpc('fs-fork',a['revision'],a['world'])
            frozen=self.root/('snapshot-'+a['revision']);self.clone_native(self.native,frozen);self.snapshots[a['revision']]=frozen
            branch=self.root/('world-'+a['world']);self.clone_native(frozen,branch);self.worlds[a['world']]=branch
            self.world=a['world'];self.native=branch
        elif op=='switch':
            self.close_handles();self.world=a['world'];self.native=self.worlds[self.world]
        elif op=='restore':
            self.close_handles();self.rpc('fs-restore',a['revision']);shutil.rmtree(self.native);self.clone_native(self.snapshots[a['revision']],self.native)
        elif op=='restart': self.stop();self.start()
        else: raise RuntimeError(op)
        self.compare()
        if len(self.trace)%10==0 or op in ('fork','restore','restart'):
            for revision,directory in self.snapshots.items(): self.compare_tree(directory,revision)

def finish_trial(t, seed):
    t.close_handles()
    for world,directory in t.worlds.items():
        t.world=world;t.native=directory;t.compare();t.rpc('fs-check')
    for revision,directory in t.snapshots.items(): t.compare_tree(directory,revision)
    return dict(seed=seed,actions=len(t.trace),operations=dict(Counter(a['op'] for a in t.trace)),
                access_errors=dict(t.error_counts),worlds=len(t.worlds),revisions=len(t.snapshots))

def run_trial(binary, root, seed, actions=None, steps=160):
    t=Trial(binary,root)
    try:
        if actions is not None:
            for a in actions: t.action(a)
            return finish_trial(t,seed)
        rng=random.Random(seed);handle=0
        for i in range(3): t.action(dict(op='write',path=f'/p{i}',data=rng.randbytes(270000+i*17).hex()))
        for step in range(steps):
            names=sorted(p.name for p in t.native.iterdir());free=[f'p{i}' for i in range(6) if not (t.native/f'p{i}').exists()]
            paths=['/'+n for n in names];op=rng.randrange(21);a=None
            if step%40==39 and len(t.worlds)<4: op=15
            if not paths or op==0: a=dict(op='write',path='/'+rng.choice(free or names),data=rng.randbytes(rng.randrange(3000)).hex())
            elif op==1: a=dict(op='at',path=rng.choice(paths),offset=rng.choice([65535,262143,270001,rng.randrange(400000)]),data=rng.randbytes(rng.randrange(2000)).hex())
            elif op==2: a=dict(op='truncate',path=rng.choice(paths),size=rng.choice([0,65535,262144,270003,rng.randrange(500000)]))
            elif op==3 and free: a=dict(op='link',path=rng.choice(paths),to='/'+rng.choice(free))
            elif op==4: a=dict(op='rename',path=rng.choice(paths),to='/'+rng.choice(free+names))
            elif op==5: a=dict(op='unlink',path=rng.choice(paths))
            elif op==6: a=dict(op='replace',path=rng.choice(paths),data=rng.randbytes(rng.choice([0,17,270007])).hex())
            elif op==7 and len(t.handles)<8:
                handle+=1;a=dict(op='open',path=rng.choice(paths),access=rng.choice(['r','w','rw','w,append','rw,append']),handle=str(handle))
            elif op in (8,9,10,11,12,13) and t.handles:
                h=rng.choice(list(t.handles));kind={8:'hwrite',9:'hread',10:'htruncate',11:'hseek',12:'hnext',13:'hreadnext'}[op]
                if kind=='hwrite' and ',append' in t.handles[h][2]: kind='hnext'
                a=dict(op=kind,handle=h,offset=rng.randrange(300000),size=rng.choice([0,65537,300001]),count=rng.randrange(2000),data=rng.randbytes(rng.randrange(257)).hex())
            elif op==14: a=dict(op='restart')
            elif op==15 and len(t.worlds)<4:
                n=len(t.worlds);a=dict(op='fork',revision=f'rev{n}',world=f'w{n}')
            elif op==16 and len(t.worlds)>1: a=dict(op='switch',world=rng.choice(list(t.worlds)))
            elif op==17 and t.snapshots: a=dict(op='restore',revision=rng.choice(list(t.snapshots)))
            elif op==18 and t.handles: a=dict(op='close',handle=rng.choice(list(t.handles)))
            elif op==19: a=dict(op='chmod',path=rng.choice(paths),mode=rng.choice([0o600,0o640,0o644,0o755]))
            elif op==20 and t.handles and len(t.handles)<8:
                handle+=1;a=dict(op='dup',source=rng.choice(list(t.handles)),handle=str(handle))
            if a: t.action(a)
        return finish_trial(t,seed)
    except Exception as error:
        (root/'trace.json').write_text(json.dumps(dict(seed=seed,actions=t.trace,error=repr(error)),indent=2))
        raise
    finally: t.stop();t.socket_dir.cleanup()

def minimize(binary, actions, output):
    def signature(error):
        detail=error.args[0]
        return (detail[0], detail[1]['op'], detail[2], detail[3]) if detail[0]=='operation' else (detail[0],)
    def check(candidate, wanted=None):
        root=Path(tempfile.mkdtemp(prefix='reduce-',dir=output))
        try:
            run_trial(binary,root,0,candidate)
        except Mismatch as e:
            return signature(e) if wanted is None else signature(e)==wanted
        except (OSError,KeyError,RuntimeError):
            return False  # Removing setup may invalidate an action dependency.
        finally: shutil.rmtree(root)
        return False
    wanted=check(actions)
    if not wanted: raise RuntimeError('trace no longer reproduces a differential mismatch')
    n=2
    while len(actions)>=2:
        chunk=(len(actions)+n-1)//n
        for at in range(0,len(actions),chunk):
            candidate=actions[:at]+actions[at+chunk:]
            if check(candidate,wanted): actions=candidate;n=max(2,n-1);break
        else:
            if n>=len(actions): break
            n=min(len(actions),n*2);continue
        continue
    for i,a in enumerate(actions):
        if 'data' in a:
            for value in ('','01'):
                candidate=[dict(x) for x in actions];candidate[i]['data']=value
                if check(candidate,wanted): actions=candidate;break
    result=output/'minimized.json';result.write_text(json.dumps(dict(actions=actions),indent=2))
    print(f'minimized to {len(actions)} actions: {result}')


def main():
    p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('--seed',type=int);p.add_argument('--steps',type=int,default=160);p.add_argument('--replay',type=Path);p.add_argument('--minimize',type=Path);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    if args.seed is None: args.seed=int.from_bytes(os.urandom(8),'little')
    print(f'seed={args.seed}',flush=True)
    args.output.mkdir(parents=True,exist_ok=True)
    if args.minimize:
        minimize(str(Path(args.binary).resolve()),json.loads(args.minimize.read_text())['actions'],args.output);return
    root=Path(tempfile.mkdtemp(prefix=f'seed-{args.seed}-',dir=args.output))
    try: result=run_trial(str(Path(args.binary).resolve()),root,args.seed,json.loads(args.replay.read_text())['actions'] if args.replay else None,args.steps)
    except Exception:
        print(f'failure artifacts: {root}',flush=True);raise
    else:
        (args.output/(root.name+'.summary.json')).write_text(json.dumps(result,indent=2))
        shutil.rmtree(root);print(json.dumps(result,sort_keys=True))
if __name__=='__main__': main()
