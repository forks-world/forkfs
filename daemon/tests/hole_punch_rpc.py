"""Resident and offline hole-punch API coverage; --expect-unsupported probes old binaries."""
import json
import errno
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile

BIN=sys.argv[1];EXPECT_UNSUPPORTED='--expect-unsupported' in sys.argv[2:]
def frame(fields):
    fields=[v.encode() if isinstance(v,str) else v for v in fields]
    body=struct.pack('<I',len(fields))+b''.join(struct.pack('<I',len(v))+v for v in fields)
    return b'FFRPC001'+struct.pack('<I',len(body))+body
def exact(sock,n):
    data=b''
    while len(data)<n:
        part=sock.recv(n-len(data));assert part,'truncated response';data+=part
    return data
def status(sock):
    header=exact(sock,12);assert header[:8]==b'FFRSP001';n=struct.unpack('<I',header[8:])[0]
    body=exact(sock,n);return struct.unpack('<I',body[:4])[0],body[4:]
with tempfile.TemporaryDirectory(prefix='ff-punch-rpc-',dir='/tmp') as tmp:
    root=Path(tmp);db=root/'store';sockpath=root/'rpc.sock';input_file=root/'input'
    def cli(*args,ok=True):
        p=subprocess.run([BIN,*map(str,args)],capture_output=True,timeout=15)
        assert (p.returncode==0)==ok,(args,p.stderr.decode(errors='replace'))
        return p.stdout
    cli('init',db)
    cli('fs-init',db)
    payload=bytes([0x5a])*(5*65536);input_file.write_bytes(payload[:262144]);cli('fs-write',db,'/f',input_file)
    input_file.write_bytes(payload[262144:]);cli('fs-write-at',db,'/f',262144,input_file)
    proc=subprocess.Popen([BIN,'serve',str(db),'--socket',str(sockpath),'--rpc'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert select.select([proc.stdout],[],[],10)[0];assert json.loads(proc.stdout.readline())['state']=='leveldb'
    session=None
    def raw(fields,expect=0,session_override=None):
        token=session if session_override is None else session_override
        if fields[0] in ('fs-open','fs-close') or fields[0].startswith('fs-h'):
            fields=[*fields,'--session',token]
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(15);s.connect(str(sockpath));s.sendall(frame(fields));code,value=status(s)
            assert code==expect,(fields[:2],code,value)
            return value
    try:
        session=json.loads(raw(['session-open','60000']))['session']
        if EXPECT_UNSUPPORTED:
            handle=json.loads(raw(['fs-open','/f','rw']))['handle']
            raw(['fs-hseek',handle,'7','set'])
            seq=json.loads(raw(['status']))['last_committed_seq']
            original_stat=json.loads(raw(['fs-stat','/f']))
            raw(['fs-punch-hole','/f','0','1'],expect=1)
            raw(['fs-hpunch-hole',handle,'0','1'],expect=1)
            assert json.loads(raw(['status']))['last_committed_seq']==seq
            after_rpc_stat=json.loads(raw(['fs-stat','/f']))
            assert after_rpc_stat['size']==original_stat['size'] and after_rpc_stat['modified_ns']==original_stat['modified_ns']
            assert raw(['fs-read-at','/f','0','8'])==payload[:8]
            assert json.loads(raw(['fs-hseek',handle,'0','cur']))['offset']==7
            raw(['fs-close',handle])
            report={'mode':'expect-unsupported','path_rpc':'rejected','handle_rpc':'rejected','data_preserved':True,
                    'size_mtime_preserved':True,'sequence_preserved':True,'cursor_preserved':True}
        else:
            # A valid sparse chunked file provides populated edges and a full interior chunk.
            raw(['fs-punch-hole','/f','65533','131080'])
            data=raw(['fs-read-at','/f','0','262144'])+raw(['fs-read-at','/f','262144','65536'])
            expected=bytearray(payload);expected[65533:196613]=bytes(131080)
            assert data==expected
            handle=json.loads(raw(['fs-open','/f','rw']))['handle']
            raw(['fs-hpunch-hole',handle,'0','3']);assert raw(['fs-hread',handle,'0','3'])==bytes(3)
            status_before=json.loads(raw(['status']))
            data_before=raw(['fs-read-at','/f','0','8']);stat_before=json.loads(raw(['fs-stat','/f']))
            raw(['fs-hseek',handle,'7','set'])
            for offset,length in [('-1','1'),('0',str(1<<63)),(str(1<<63),'1')]:
                raw(['fs-punch-hole','/f',offset,length],expect=1)
            assert json.loads(raw(['status']))['last_committed_seq']==status_before['last_committed_seq']
            assert raw(['fs-read-at','/f','0','8'])==data_before
            stat_after=json.loads(raw(['fs-stat','/f']))
            assert stat_after['size']==stat_before['size'] and stat_after['modified_ns']==stat_before['modified_ns']
            assert json.loads(raw(['fs-hseek',handle,'0','cur']))['offset']==7
            foreign=json.loads(raw(['session-open','60000']))['session']
            raw(['fs-hpunch-hole',handle,'0','1'],expect=errno.EBADF+1,session_override=foreign)
            raw(['fs-close',handle]);raw(['fs-hpunch-hole',handle,'0','1'],expect=errno.EBADF+1)
            readonly=json.loads(raw(['fs-open','/f','r']))['handle']
            ro_seq=json.loads(raw(['status']))['last_committed_seq']
            raw(['fs-hpunch-hole',readonly,'0','0'],expect=errno.EBADF+1)
            assert json.loads(raw(['status']))['last_committed_seq']==ro_seq
            raw(['fs-close',readonly])
            lease=json.loads(raw(['session-open','60000']))['session']
            raw(['inode-root','--session',lease])
            lease_seq=json.loads(raw(['status']))['last_committed_seq']
            raw(['fs-punch-hole','/f','0','1'],expect=errno.EBUSY+1)
            assert json.loads(raw(['status']))['last_committed_seq']==lease_seq
            raw(['session-close',lease])
            report={'mode':'fixed','path_rpc':'passed','handle_rpc':'passed','readonly':'EBADF',
                    'foreign_session':'EBADF','mount_lease':'EBUSY','malformed_ranges':'rejected_without_changes'}
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=10)

    if EXPECT_UNSUPPORTED:
        before=cli('fs-read-at',db,'/f',0,8)
        p=subprocess.run([BIN,'fs-punch-hole',str(db),'/f','0','1'],capture_output=True,timeout=10)
        assert p.returncode!=0 and p.stderr
        assert cli('fs-read-at',db,'/f',0,8)==before
        after_stat=json.loads(cli('fs-stat',db,'/f'))
        assert after_stat['size']==original_stat['size'] and after_stat['modified_ns']==original_stat['modified_ns']
        report['offline_cli']='rejected'
    else:
        # Offline command shares the same range semantics and leaves file size fixed.
        cli('fs-punch-hole',db,'/f',196608,65536)
        assert json.loads(cli('fs-stat',db,'/f'))['size']==len(payload)
        cli('fs-check',db)
        report['offline_cli']='passed'
    print(json.dumps(report,sort_keys=True))
