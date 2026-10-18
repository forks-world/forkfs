"""Run mock HTTP source and backup as separate processes over real forkfs objects."""
import argparse
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import tarfile
import time
import unittest

from revision_sync_mock import call, remote, ApiError, json_bytes
from revision_sync_codec import object_id, source_closure, make_plan, encode_pack
from revision_sync_validator import decode_inventory, receive_pack

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'daemon' / 'tests'))
from replay_corruption import unpack, base_rows

BINARY = None
ARTIFACTS = None


class Server:
    def __init__(self, role, root, source=None):
        self.role, self.root, self.source = role, root, source
        self.endpoint = root / 'endpoint'
        self.proc = None
    def start(self):
        self.endpoint.unlink(missing_ok=True)
        args = [sys.executable, str(ROOT/'scripts/tests/revision_sync_mock.py'), '--role', self.role,
                '--root', str(self.root), '--endpoint-file', str(self.endpoint)]
        if self.source:
            args += ['--source-url', self.source.url]
        self.log = (self.root/'server.log').open('ab')
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 20
        while not self.endpoint.exists():
            if self.proc.poll() is not None or time.monotonic() > deadline:
                raise AssertionError(('mock startup', self.role, (self.root/'server.log').read_text()))
            time.sleep(.01)
        self.url = self.endpoint.read_text()
    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
            self.proc.wait(timeout=10);self.log.close();self.proc = None
    def request(self, method, path, body=None, key=None):
        return call(self.url, method, path, body, key)
    def state(self):
        return self.request('GET', '/__test__/state')
    def faults(self, **faults):
        self.request('POST', '/__test__/faults', faults)


class SyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix='sync-mock-', dir=ARTIFACTS)).resolve()
        cls.db = cls.root/'real-repository'
        def run(*args):
            p = subprocess.run([BINARY, *map(str, args)], capture_output=True, timeout=30)
            if p.returncode:
                raise AssertionError((args, p.stderr))
            return p.stdout
        run('init', cls.db);run('fs-init', cls.db);run('fs-mkdir', cls.db, '/src')
        data = cls.root/'input'
        cls.expected = {}
        for i in range(160):
            payload = f'int source_{i} = {i};\n'.encode() + b'/*' + b'x'*256 + b'*/'
            data.write_bytes(payload);run('fs-write', cls.db, '/src/f'+str(i), data)
            cls.expected['/src/f'+str(i)] = payload
        cls.expected['/magic'] = b'FFREV002this is file data, not a descriptor'
        cls.expected['/empty'] = b''
        cls.expected['/binary'] = random.Random(173).randbytes(150000)
        for path in ('/magic','/empty','/binary'):
            data.write_bytes(cls.expected[path]);run('fs-write', cls.db, path, data)
        run('fs-chmod', cls.db, '/src/f0', '755');run('fs-link', cls.db, '/src/f0', '/alias')
        run('fs-symlink', cls.db, 'src/f0', '/current')
        run('fs-link', cls.db, '/current', '/current-alias')
        cls.expected['/alias'] = cls.expected['/src/f0']
        run('fs-snapshot', cls.db, 'base');run('fs-fork', cls.db, 'base', 'branch')
        data.write_bytes(b'left\0edit');run('fs-write', cls.db, '/src/f0', data);run('fs-snapshot', cls.db, 'left')
        data.write_bytes(b'right edit');run('fs-write', cls.db, '/src/f1', data, '--world', 'branch');run('fs-snapshot', cls.db, 'right', '--world', 'branch')
        run('fs-snapshot-parents', cls.db, 'merge', 'left,right')
        data.write_bytes(b'next edit');run('fs-write', cls.db, '/src/f2', data);run('fs-snapshot', cls.db, 'next')
        data.write_bytes(b'not in any revision');run('object-put', cls.db, 'unrelated', data)
        archive = cls.root/'source.pack';run('backup', cls.db, archive)
        header, frames = unpack(archive.read_bytes());rows = base_rows(frames)
        objects = {k[2:].hex(): v.hex() for k,v in rows.items() if k.startswith(b'O/')}
        refs = {k[len(b'R/\0forkfs-history/revision/'):].decode(): v.hex() for k,v in rows.items() if k.startswith(b'R/\0forkfs-history/revision/')}
        refs['latest'] = refs['merge']
        # Re-hash a semantically invalid inode and every changed ancestor. Hash
        # validation alone must not let the target publish this revision.
        descriptor = bytes.fromhex(objects[refs['merge']]);manifest = bytes.fromhex(objects[descriptor[8:40].hex()])
        def rewrite(ident):
            raw = bytes.fromhex(objects[ident]);left,right = raw[44:76],raw[76:108]
            changed = bytearray(raw)
            for offset, child in ((44,left),(76,right)):
                if any(child):
                    changed[offset:offset+32] = bytes.fromhex(rewrite(child.hex()))
            if raw[108:].startswith(b'\0forkfs/inode/'):
                inode = bytes.fromhex(objects[raw[12:44].hex()])
                if int.from_bytes(inode[8:12],'little') == 1:
                    inode = inode[:12] + struct.pack('<I', 0xffffffff) + inode[16:]
                    value = object_id(inode);objects[value.hex()] = inode.hex();changed[12:44] = value
            result = object_id(changed);objects[result.hex()] = bytes(changed).hex()
            return result.hex()
        bad_manifest = b'FFTREE01' + bytes.fromhex(rewrite(manifest[8:].hex()))
        bad_manifest_id = object_id(bad_manifest);objects[bad_manifest_id.hex()] = bad_manifest.hex()
        bad_revision = descriptor[:8] + bad_manifest_id + descriptor[40:]
        bad_id = object_id(bad_revision);objects[bad_id.hex()] = bad_revision.hex();refs['bad-semantic'] = bad_id.hex()
        # Build canonical treaps for re-hashed namespace records that violate
        # the trusted base validator's exact namespace key set.
        def namespace_values(tree_id):
            result = {}
            def visit(node_id):
                node = bytes.fromhex(objects[node_id])
                for child in (node[44:76], node[76:108]):
                    if any(child): visit(child.hex())
                result[node[108:]] = bytes.fromhex(objects[node[12:44].hex()])
            visit(tree_id)
            return result
        def bad_namespace(name, edit):
            values = namespace_values(manifest[8:].hex());edit(values)
            def build(items):
                if not items: return bytes(32)
                key, value = max(items, key=lambda pair: (object_id(b'forkfs.metadata.priority.v1' + pair[0]), pair[0]))
                at = next(i for i, pair in enumerate(items) if pair[0] == key)
                left = build(items[:at]);right = build(items[at+1:])
                value_id = object_id(value);objects[value_id.hex()] = value.hex()
                node = b'FFTRNO01' + len(key).to_bytes(4,'little') + value_id + left + right + key
                node_id = object_id(node);objects[node_id.hex()] = node.hex()
                return node_id
            tree_id = build(sorted(values.items()))
            new_manifest = b'FFTREE01' + tree_id
            manifest_id = object_id(new_manifest);objects[manifest_id.hex()] = new_manifest.hex()
            revision = descriptor[:8] + manifest_id + descriptor[40:]
            revision_id = object_id(revision);objects[revision_id.hex()] = revision.hex()
            refs[name] = revision_id.hex()
        alloc_key = next(k for k in namespace_values(manifest[8:].hex()) if k.startswith(b'\0forkfs/allocated/'))
        bad_namespace('missing-allocation', lambda v: v.pop(alloc_key))
        bad_namespace('nonempty-allocation', lambda v: v.__setitem__(alloc_key, b'x'))
        bad_namespace('extra-namespace-key', lambda v: v.__setitem__(b'\0forkfs/unknown/record', b''))
        bad_namespace('extra-parent-record', lambda v: v.__setitem__(b'\0forkfs/parent/' + b'0'*32, b'FFPAR001' + b'0'*16))
        bad_namespace('extra-content-record', lambda v: v.__setitem__(b'\0forkfs/content/' + b'0'*32, b''))
        # Preserve raw legacy bytes and a nonlocal maximal source watermark.
        base_descriptor = bytes.fromhex(objects[refs['base']])
        legacy = b'FFREV001' + base_descriptor[8:40] + bytes(32) + (2**64-1).to_bytes(8,'little')
        legacy_id = object_id(legacy);objects[legacy_id.hex()] = legacy.hex()
        merged = bytes.fromhex(objects[refs['merge']])
        mixed = b'FFREV002' + merged[8:40] + struct.pack('<QI',2**64-1,2) + bytes.fromhex(refs['merge']) + legacy_id
        mixed_id = object_id(mixed);objects[mixed_id.hex()] = mixed.hex();refs['mixed'] = mixed_id.hex()
        cls.fixture = dict(repository=header[9:25].hex(), sequence=int.from_bytes(header[33:41],'little'),
                           objects=objects, refs=refs, required_object=object_id(cls.expected['/magic']).hex())
        cls.base = '/v1/repositories/' + cls.fixture['repository']
        cls.unrelated = rows[b'R/unrelated'].hex()
        (cls.root/'fixture-template.json').write_bytes(json_bytes(cls.fixture))

    def setUp(self):
        self.case = self.root/self._testMethodName;self.case.mkdir()
        source_root = self.case/'source';source_root.mkdir()
        (source_root/'fixture.json').write_bytes(json_bytes(self.fixture))
        self.source = Server('source', source_root);self.addCleanup(self.source.stop);self.source.start()
        backup_root = self.case/'backup';backup_root.mkdir()
        self.backup = Server('backup', backup_root, self.source);self.addCleanup(self.backup.stop);self.backup.start()
        self.request = dict(repository=self.fixture['repository'], selectors=[{'name':'latest'}], request_id='request-a')

    def sync(self, **changes):
        return self.backup.request('POST', '/sync', self.request | changes)

    def capture(self, key='capture', selectors=None):
        request = dict(selectors=selectors or [{'name':'latest'}], consumer_id='mock-backup',
                       destination_id='mock-destination', scope='revision-closure', history='full', lease_seconds=900)
        return self.source.request('POST', self.base+'/exports', request, key)

    def no_ack(self):
        self.assertFalse(self.backup.state()['catalog'])
        self.assertTrue(all(j['state'] != 'acknowledged' for j in self.source.state()['jobs'].values()))

    def test_01_full_sync_source_offline_restore_all_parents(self):
        result = self.sync();self.assertEqual(result['revisions'], 4)
        self.source.stop();shutil.rmtree(self.source.root)
        # Even the producer's real repository is unavailable during restore.
        moved = self.root/'real-repository-offline'
        self.db.rename(moved)
        try:
            restored = self.backup.request('POST','/restore',{'backup_id':result['backup_id']})
            self.assertEqual(set(restored['revisions']), {self.fixture['refs'][n] for n in ('base','left','right','merge')})
            destination = self.backup.root/'restored'/restored['restore_id']
            for name in ('base','left','right','merge'):
                view = destination/self.fixture['refs'][name]
                expected = self.expected.copy()
                if name in ('left','merge'):
                    expected['/src/f0'] = expected['/alias'] = b'left\0edit'
                if name == 'right':
                    expected['/src/f1'] = b'right edit'
                actual = {('/'+str(p.relative_to(view))):p.read_bytes() for p in view.rglob('*') if p.is_file() and not p.is_symlink()}
                self.assertEqual(actual, expected)
                self.assertEqual(os.readlink(view/'current'), 'src/f0')
                self.assertEqual(os.readlink(view/'current-alias'), 'src/f0')
                self.assertEqual(os.lstat(view/'current').st_ino, os.lstat(view/'current-alias').st_ino)
                self.assertEqual(os.lstat(view/'current').st_nlink, 2)
                self.assertNotEqual(os.lstat(view/'current').st_ino, (view/'src/f0').stat().st_ino)
                self.assertEqual((view/'alias').stat().st_ino, (view/'src/f0').stat().st_ino)
                self.assertEqual((view/'src/f0').stat().st_mode & 0o777, 0o755)
        finally:
            moved.rename(self.db)

    def test_02_incremental_and_idempotent_no_duplicate_objects(self):
        first = self.sync();before = self.backup.state()
        self.assertEqual(self.sync(), first)
        self.assertEqual(self.backup.state()['stats'], before['stats'])
        second = self.sync(request_id='next-request', selectors=[{'name':'next'}])
        after = self.backup.state()
        old = source_closure([self.fixture['refs']['merge']], self.fixture['objects'])
        new = source_closure([self.fixture['refs']['next']], self.fixture['objects'])
        self.assertEqual(after['stats']['received_objects'] - before['stats']['received_objects'], len(set(new)-set(old)))
        self.assertEqual(after['stats']['received_payload_bytes'] - before['stats']['received_payload_bytes'], sum(new[k] for k in set(new)-set(old)))
        self.assertEqual(len(after['catalog']), 2)
        self.assertEqual(second['revisions'], 5)
        self.assertGreater(len(new), 1024)  # Exercise branch inventory nodes.
        jobs = {j['key']: j for j in self.source.state()['jobs'].values()}
        changed_nodes = set(jobs['next-request']['nodes']) - set(jobs['request-a']['nodes'])
        # Random inode IDs change treap paths. A tiny fixture may legitimately
        # touch every radix bucket; assert exact diff, not a flaky reuse ratio.
        self.assertEqual(after['stats']['inventory_downloads'] - before['stats']['inventory_downloads'], len(changed_nodes))

    def test_03_truncated_pack_resumes_durable_prefix(self):
        self.source.faults(truncate_pack=1)
        with self.assertRaises(ApiError):self.sync()
        self.no_ack()
        prefix = self.backup.state()['stats']['received_objects'];self.assertEqual(prefix, 1)
        result = self.sync()
        expected = source_closure([self.fixture['refs']['merge']], self.fixture['objects'])
        self.assertEqual(self.backup.state()['stats']['received_objects'], len(expected))
        self.assertEqual(result['ack'], 'acknowledged')

    def test_04_corrupt_payload_with_valid_pack_footer_rejected(self):
        self.source.faults(corrupt_pack_payload=1)
        with self.assertRaises(ApiError):self.sync()
        self.no_ack();self.assertEqual(self.sync()['state'], 'committed')

    def test_05_corrupt_inventory_rejected(self):
        self.source.faults(corrupt_inventory=1)
        with self.assertRaises(ApiError):self.sync()
        self.no_ack()
        self.assertFalse(self.backup.state()['stats'].get('received_objects',0))

    def test_06_rehashed_invalid_inode_rejected(self):
        with self.assertRaises(ApiError):self.sync(selectors=[{'name':'bad-semantic'}])
        self.no_ack()
        self.assertGreater(self.backup.state()['stats']['received_objects'], 0)

    def test_06b_rehashed_invalid_namespace_keysets_rejected(self):
        names = ('missing-allocation', 'nonempty-allocation', 'extra-namespace-key',
                 'extra-parent-record', 'extra-content-record')
        for index, name in enumerate(names):
            with self.subTest(namespace=name), self.assertRaises(ApiError):
                self.sync(request_id='invalid-namespace-'+str(index), selectors=[{'name':name}])
            self.no_ack()

    def test_07_inventory_omits_dependency_no_commit(self):
        self.source.faults(omit_inventory_object=1)
        with self.assertRaises(ApiError):self.sync()
        self.no_ack()

    def test_08_cached_inventory_does_not_hide_missing_cas_object(self):
        self.sync();before = self.backup.state()['stats'].copy()
        self.backup.request('POST','/__test__/drop-object',{'object_id':self.fixture['required_object']})
        self.sync(request_id='repair-request')
        after = self.backup.state()['stats']
        self.assertEqual(after['received_objects'] - before['received_objects'], 1)
        self.assertEqual(after['inventory_downloads'], before['inventory_downloads'])

    def test_09_source_restart_frozen_refs_and_request_conflict(self):
        capture = self.capture();old = capture['plan']
        self.source.request('POST','/__test__/advance', {'name':'latest','revision_id':self.fixture['refs']['next']})
        self.source.stop();self.source.start()
        self.assertEqual(self.capture(), capture)
        self.assertEqual(self.source.request('GET', self.base+'/exports/'+capture['export_id'])['plan'],old)
        with self.assertRaises(ApiError) as error:
            self.capture(key='other',selectors=[{'name':'latest','expected_revision_id':self.fixture['refs']['merge']}])
        self.assertEqual(error.exception.code, 'REF_CHANGED')
        with self.assertRaises(ApiError) as error:
            self.capture(selectors=[{'name':'next'}])
        self.assertEqual(error.exception.code,'IDEMPOTENCY_CONFLICT')

    def test_10_lease_renew_expiry_and_membership(self):
        captured = self.capture();path = self.base+'/exports/'+captured['export_id']
        with self.assertRaises(ApiError) as error:
            self.source.request('POST',path+'/packs',{'object_ids':[self.unrelated]},'pack')
        self.assertEqual(error.exception.code,'FORBIDDEN')
        self.source.request('POST','/__test__/clock',{'seconds':899})
        self.source.request('POST',path+'/renew',{'lease_seconds':900})
        self.source.request('POST','/__test__/clock',{'seconds':2})
        self.assertEqual(self.source.request('GET',path)['state'],'ready')
        self.source.request('POST','/__test__/clock',{'seconds':900})
        self.assertEqual(self.source.request('GET',path)['state'],'expired')
        self.assertEqual(self.capture()['state'],'expired')
        with self.assertRaises(ApiError) as error:
            self.source.request('GET',path+'/inventory/'+captured['plan']['inventory_root'])
        self.assertEqual(error.exception.code,'EXPORT_EXPIRED')

    def test_11_crash_before_publish_resume_without_object_retransfer(self):
        self.backup.faults(crash_before_publish=1)
        with self.assertRaises((OSError,http.client.HTTPException)):self.sync()
        self.assertEqual(self.backup.proc.wait(timeout=10),86)
        self.backup.stop();self.backup.start()
        before = self.backup.state()['stats']['received_objects'];self.no_ack()
        self.assertEqual(self.sync()['state'],'committed')
        self.assertEqual(self.backup.state()['stats']['received_objects'],before)

    def test_12_crash_after_publish_keeps_one_catalog_entry(self):
        self.backup.faults(crash_after_publish=1)
        with self.assertRaises((OSError,http.client.HTTPException)):self.sync()
        self.assertEqual(self.backup.proc.wait(timeout=10),87)
        self.backup.stop();self.backup.start()
        before = self.backup.state()
        self.assertEqual(len(before['catalog']),1)
        self.assertEqual(self.sync()['ack'],'acknowledged')
        self.assertEqual(self.backup.state()['stats'],before['stats'])
        self.assertEqual(len(self.backup.state()['catalog']),1)

    def test_13_ack_response_loss_idempotent_receipt(self):
        self.source.faults(drop_ack_response=1)
        result = self.sync();self.assertEqual(result['ack'],'pending')
        self.assertTrue(all(j['state']=='acknowledged' for j in self.source.state()['jobs'].values()))
        first = self.backup.state()
        again = self.sync();self.assertEqual(again['ack'],'acknowledged')
        self.assertEqual(result['backup_id'],again['backup_id'])
        self.assertEqual(self.backup.state()['catalog'],first['catalog'])

    def test_14_capabilities_rejected_before_transfer(self):
        self.source.faults(wrong_protocol=1)
        with self.assertRaises(ApiError):self.sync()
        self.no_ack()
        self.assertFalse(self.source.state()['jobs'])

    def test_15_frontier_preserves_divergent_branches(self):
        self.sync(request_id='left', selectors=[{'name':'left'}])
        self.sync(request_id='right', selectors=[{'name':'right'}])
        path = self.base + '/consumers/mock-backup/coverage'
        self.assertEqual(set(self.source.request('GET',path)['frontier']),
                         {self.fixture['refs']['left'],self.fixture['refs']['right']})
        self.sync(request_id='next', selectors=[{'name':'next'}])
        self.assertEqual(self.source.request('GET',path)['frontier'],[self.fixture['refs']['next']])
        self.assertEqual(len(self.backup.state()['catalog']),3)

    def test_16_pack_key_conflict_and_cancel_idempotence(self):
        capture = self.capture();path = self.base + '/exports/' + capture['export_id']
        entries = self.source.state()['jobs'][capture['export_id']]['entries']
        ids = sorted(entries)
        with remote(self.source.url,'POST',path+'/packs',{'object_ids':ids[:1]},'same-key') as response:
            original = response.read()
        with remote(self.source.url,'POST',path+'/packs',{'object_ids':ids[:1]},'same-key') as response:
            self.assertEqual(response.read(),original)
        with self.assertRaises(ApiError) as error:
            self.source.request('POST',path+'/packs',{'object_ids':ids[:2]},'same-key')
        self.assertEqual(error.exception.code,'IDEMPOTENCY_CONFLICT')
        self.assertEqual(self.source.request('DELETE',path)['state'],'cancelled')
        self.assertEqual(self.source.request('DELETE',path)['state'],'cancelled')
        with self.assertRaises(ApiError):self.source.request('GET',path+'/inventory/'+capture['plan']['inventory_root'])

    def test_17_acknowledged_export_no_longer_serves_unpinned_objects(self):
        result = self.sync();path = self.base + '/exports/' + result['export_id']
        with self.assertRaises(ApiError) as error:
            self.source.request('POST',path+'/packs',{'object_ids':[self.fixture['required_object']]},'after-ack')
        self.assertEqual(error.exception.code,'EXPORT_CLOSED')

    def test_18_mixed_revision_formats_preserve_nonlocal_watermarks(self):
        result = self.sync(selectors=[{'name':'mixed'}])
        self.assertEqual(result['revisions'],6)
        restored = self.backup.request('POST','/restore',{'backup_id':result['backup_id']})
        self.assertEqual(len(restored['revisions']),6)

    def test_19_lease_expiry_after_target_commit_preserves_backup(self):
        self.backup.faults(expire_before_ack=1)
        result = self.sync()
        self.assertEqual(result['state'],'committed')
        self.assertEqual(result['ack'],'expired')
        self.assertTrue(all(j['state']!='acknowledged' for j in self.source.state()['jobs'].values()))
        self.source.stop()
        restored = self.backup.request('POST','/restore',{'backup_id':result['backup_id']})
        self.assertEqual(len(restored['revisions']),4)

    def test_20_pack_and_inventory_negative_wire_inputs(self):
        data = b'codec data';ident = object_id(data).hex();objects = {ident:data.hex()}
        plan,nodes = make_plan('12'*16,7,['23'*32],{ident:len(data)})
        entries = decode_inventory(plan,lambda n:bytes.fromhex(nodes[n]))
        valid = encode_pack(plan['plan_hash'],[ident],objects)
        mutations = [valid[:-1],valid+b'x',valid[:44]+valid[44:-32]+bytes(32)]
        rng = random.Random(20261018)
        for _ in range(48):
            altered = bytearray(valid);altered[rng.randrange(len(altered))] ^= 1 << rng.randrange(8)
            mutations += [bytes(altered),valid[:rng.randrange(len(valid))]]
        for bad in mutations:
            with self.subTest(size=len(bad)),self.assertRaises(ValueError):
                receive_pack(io.BytesIO(bad),plan,[ident],entries,lambda *args:None)
        # Recompute outer inventory/plan hashes to reach structural validation.
        from revision_sync_codec import digest
        raw = bytes.fromhex(nodes[plan['inventory_root']])
        malformed = raw[:10] + (2).to_bytes(8,'little') + raw[18:]
        invalid = plan.copy();invalid['inventory_root'] = digest('forkfs.sync.inventory.v1',malformed).hex()
        preimage = (bytes.fromhex(plan['source_repository']) + struct.pack('<QI',7,1)
                    + bytes.fromhex(plan['selected_revision_ids'][0]) + bytes.fromhex(invalid['inventory_root'])
                    + struct.pack('<QQI',1,len(data),1))
        invalid['plan_hash'] = digest('forkfs.sync.plan.v1',preimage).hex()
        with self.assertRaisesRegex(ValueError,'leaf count'):
            decode_inventory(invalid,lambda _:malformed)
        # Independently framed known hash vector, not a codec round-trip assertion.
        expected = hashlib.sha256(b'forkfs.object.v0'+b'\x04\x00\x00\x00'+(0).to_bytes(8,'little')).digest()
        self.assertEqual(object_id(b''),expected)
        self.assertNotEqual(object_id(data),hashlib.sha256(data).digest())


def main():
    global BINARY,ARTIFACTS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args();BINARY = str(args.binary.resolve())
    args.output.mkdir(parents=True,exist_ok=True);ARTIFACTS = args.output.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SyncTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if hasattr(SyncTests,'root'):
        print('Mock protocol artifacts:',SyncTests.root,flush=True)
        (SyncTests.root/'result.json').write_text(json.dumps(dict(tests=result.testsRun,failures=len(result.failures),errors=len(result.errors)),indent=2))
        archive = ARTIFACTS/(SyncTests.root.name+'.tar.gz')
        with tarfile.open(archive,'w:gz') as out:
            out.add(SyncTests.root,arcname=SyncTests.root.name)
        print('Compressed artifacts:',archive,flush=True)
    raise SystemExit(not result.wasSuccessful())


if __name__ == '__main__':main()
