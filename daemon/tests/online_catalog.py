"""Online catalog publication, checkpoint-bound retries and persisted crash stages."""
import json
import os
import subprocess
import struct
import sys
import tempfile
import time
from pathlib import Path
from replay_corruption import h
BIN, FAULT = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
def cli(*args, ok=True):
    p = subprocess.run([BIN, *map(str,args)], capture_output=True, timeout=30)
    assert p.returncode == (0 if ok else 1), (args,p.stderr)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='ff-online-catalog-') as directory:
    root = Path(directory);endpoint=root/'rpc.sock'
    def start(db, stage=None):
        env=dict(os.environ)
        if stage:env['FORKFS_TEST_CRASH']=stage
        proc=subprocess.Popen([FAULT if stage else BIN,'serve',str(db),'--socket',str(endpoint),'--rpc'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        deadline=time.monotonic()+10
        last_probe=None
        while True:
            if proc.poll() is not None:
                stdout,stderr=proc.communicate()
                endpoint.unlink(missing_ok=True)
                raise AssertionError(f'daemon exited before RPC readiness: {stdout!r} {stderr!r}')
            if endpoint.exists():
                try:
                    probe=subprocess.run([BIN,'request',str(endpoint),'status'],capture_output=True,timeout=2)
                    if probe.returncode==0:
                        status=json.loads(probe.stdout)
                        assert 'storage_engine' in status,status
                        if proc.poll() is not None:
                            stdout,stderr=proc.communicate()
                            endpoint.unlink(missing_ok=True)
                            raise AssertionError(f'daemon exited after binding: {stdout!r} {stderr!r}')
                        return proc
                    last_probe=(probe.returncode,probe.stdout,probe.stderr)
                except subprocess.TimeoutExpired as error:
                    last_probe=error
            if time.monotonic()>=deadline:
                if proc.poll() is None:proc.terminate()
                try:stdout,stderr=proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill();stdout,stderr=proc.communicate()
                endpoint.unlink(missing_ok=True)
                raise AssertionError(f'daemon RPC readiness timed out; probe={last_probe!r}; logs={stdout!r} {stderr!r}')
            time.sleep(.01)
    def stop(proc):
        if proc.poll() is None:proc.terminate()
        proc.wait(timeout=30);endpoint.unlink(missing_ok=True)
    def request(*args,ok=True):
        out=cli('request',endpoint,*args,ok=ok)
        return json.loads(out) if ok else None
    def finish(name,state='durable'):
        deadline=time.monotonic()+20
        while True:
            status=request('backup-status',name)
            if status['state']!='running':
                assert status['state']==state,status
                return status
            assert time.monotonic()<deadline
            time.sleep(.01)
    db=root/'db';cli('init',db);cli('fs-init',db)
    catalog=root/'catalog';base=root/'base'
    proc=start(db)
    try:
        # Old commands still work when the task file later upgrades to version 2.
        request('backup-start','legacy',root/'legacy');legacy=finish('legacy')
        request('backup-catalog-start','base',base,catalog);first=finish('base')
        assert first['archive_durable'] and first['catalog_published']
        assert first['archive_checkpoint']==first['checkpoint']
        assert request('backup-catalog-retry','base')==first
        request('backup-catalog-start','base',base,root/'different',ok=False)
        payload=root/'input';payload.write_bytes(b'recovered content')
        cli('request',endpoint,'fs-write','/file',payload)
        delta=root/'delta'
        request('replay-catalog-start','delta',first['sequence'],delta,catalog);last=finish('delta')
        assert last['catalog_published']
        blocker=root/'blocked';blocker.write_bytes(b'occupied')
        failed_archive=root/'failed-archive'
        request('backup-catalog-start','failed',failed_archive,blocker);failed=finish('failed','failed')
        assert failed['archive_durable'] and failed['archive_checkpoint'] and not failed['catalog_published']
        saved=failed_archive.read_bytes();blocker.unlink()
        # A valid archive from the same repository but a different checkpoint is refused.
        failed_archive.write_bytes(base.read_bytes())
        request('backup-catalog-retry','failed');finish('failed','failed')
        assert json.loads(cli('catalog-list',blocker))['archives']==[]
        failed_archive.write_bytes(saved)
    finally:stop(proc)
    assert (db/'BACKUP-TASKS').read_bytes().startswith(b'FFBKT002')
    proc=start(db)
    try:
        assert request('backup-status','legacy')==legacy
        assert request('backup-status','delta')==last
        request('backup-catalog-retry','failed');done=finish('failed')
        assert done['archive_checkpoint']==failed['archive_checkpoint']
        assert failed_archive.read_bytes()==saved
        request('backup-catalog-retry','legacy',ok=False)
        request('backup-catalog-start','relative',root/'relative','relative',ok=False)
        request('backup-catalog-start','empty',root/'empty','',ok=False)
        request('replay-catalog-start','empty-delta',0,root/'empty-delta','',ok=False)
    finally:stop(proc)
    point=last['checkpoint'];recovered=root/'recovered'
    cli('restore',recovered,point['sequence'],point['repository'],point['chain_hash'],'--catalog',catalog)
    assert cli('fs-cat',recovered,'/file')==b'recovered content'
    for stage in ('backup_task_after_intent','backup_task_after_archive','backup_task_after_archive_checkpoint',
                  'catalog_after_package','catalog_after_entry','backup_task_after_catalog','backup_task_after_completion'):
        db=root/stage;cli('init',db);cli('fs-init',db)
        archive=root/(stage+'.pack');cat=root/(stage+'.catalog')
        proc=start(db,stage)
        try:
            subprocess.run([BIN,'request',str(endpoint),'backup-catalog-start','job',str(archive),str(cat)],capture_output=True,timeout=30)
            assert proc.wait(timeout=30)==86
        finally:stop(proc)
        saved=archive.read_bytes() if archive.exists() else None
        proc=start(db)
        try:
            status=request('backup-status','job')
            assert status['state']==('durable' if stage=='backup_task_after_completion' else 'interrupted')
            assert request('backup-catalog-start','job',archive,cat)==status
            if stage in ('backup_task_after_intent','backup_task_after_archive'):
                assert not status['archive_durable']
                request('backup-catalog-retry','job',ok=False)
            else:
                assert status['archive_durable']
                request('backup-catalog-retry','job');done=finish('job')
                assert done['catalog_published'] and done['checkpoint']==status['archive_checkpoint']
                assert len(json.loads(cli('catalog-list',cat))['archives'])==1
            assert (archive.read_bytes() if archive.exists() else None)==saved
        finally:stop(proc)
    # Recompute checksums to exercise new v2 field validation, not only transport checks.
    ledger=db/'BACKUP-TASKS';original=ledger.read_bytes();body=original[:-32]
    assert body[:8]==b'FFBKT002' and body[-1:]==b'\x01'
    variants=[body[:-1]+bytes([flag]) for flag in (0,2)]
    at=44
    for _ in range(5):
        size,=struct.unpack_from('<I',body,at);at+=4+size
    at+=17
    size,=struct.unpack_from('<I',body,at)
    assert at+4+size+1==len(body)
    variants.append(body[:at]+struct.pack('<I',8)+b'relative'+body[-1:])
    for broken in variants:
        ledger.write_bytes(broken+h(broken));proc=start(db)
        try:
            request('backup-status','job',ok=False)
            cli('request',endpoint,'fs-check')  # Task ledger damage does not break filesystem reads.
        finally:stop(proc)
    ledger.write_bytes(original)
print('online catalog base/delta, persisted retries, replaced archives and crash boundaries passed')
