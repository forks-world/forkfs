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
import stat
import struct
import subprocess
import sys
import tempfile
import tarfile
import time
import unittest

from revision_sync_mock import call, remote, ApiError, json_bytes
from revision_sync_codec import object_id, source_closure, make_plan, encode_pack
from revision_sync_validator import decode_inventory, receive_pack, verify_closure

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
            priorities = {key: (object_id(b'forkfs.metadata.priority.v1' + key), key) for key in values}
            stack = [];tree = None
            for key, value in sorted(values.items()):
                node = [key, value, None, None];left = None
                while stack and priorities[stack[-1][0]] < priorities[key]:
                    left = stack.pop()
                node[2] = left
                if stack: stack[-1][3] = node
                else: tree = node
                stack.append(node)
            def emit(node):
                if node is None: return bytes(32)
                key, value = node[0], node[1]
                left = emit(node[2]);right = emit(node[3])
                value_id = object_id(value);objects[value_id.hex()] = value.hex()
                node = b'FFTRNO01' + len(key).to_bytes(4,'little') + value_id + left + right + key
                node_id = object_id(node);objects[node_id.hex()] = node.hex()
                return node_id
            tree_id = emit(tree)
            new_manifest = b'FFTREE01' + tree_id
            manifest_id = object_id(new_manifest);objects[manifest_id.hex()] = new_manifest.hex()
            revision = descriptor[:8] + manifest_id + descriptor[40:]
            revision_id = object_id(revision);objects[revision_id.hex()] = revision.hex()
            refs[name] = revision_id.hex()
        alloc_key = next(k for k in namespace_values(manifest[8:].hex()) if k.startswith(b'\0forkfs/allocated/'))
        bad_namespace('missing-allocation', lambda v: v.pop(alloc_key))
        bad_namespace('nonempty-allocation', lambda v: v.__setitem__(alloc_key, b'x'))
        bad_namespace('extra-namespace-key', lambda v: v.__setitem__(b'\0forkfs/unknown/record', b''))
        # Malformed entry keys must remain visible to the exact keyset check
        # even though they cannot be assigned to a parent by the fast index.
        bad_namespace('malformed-entry-parent', lambda v: v.__setitem__(
            b'\0forkfs/entry/not-an-inode/name', v[b'\0forkfs/root']))
        bad_namespace('extra-parent-record', lambda v: v.__setitem__(b'\0forkfs/parent/' + b'0'*32, b'FFPAR001' + b'0'*16))
        bad_namespace('extra-content-record', lambda v: v.__setitem__(b'\0forkfs/content/' + b'0'*32, b''))
        def set_symlink_payload(values, payload):
            inode_key, inode = next((key, bytearray(value)) for key, value in values.items()
                                    if key.startswith(b'\0forkfs/inode/') and int.from_bytes(value[8:12], 'little') == 3)
            ident = inode_key[len(b'\0forkfs/inode/'):]
            struct.pack_into('<Q', inode, 24, len(payload))
            values[inode_key] = bytes(inode)
            values[b'\0forkfs/content/' + ident] = payload
        bad_namespace('valid-symlink-boundary', lambda v: set_symlink_payload(v, b'/'.join([b'a'*255]*16) + b'/'))
        bad_namespace('empty-symlink-target', lambda v: set_symlink_payload(v, b''))
        bad_namespace('oversized-symlink-target', lambda v: set_symlink_payload(v, b'a'*4097))
        bad_namespace('nonprintable-symlink-target', lambda v: set_symlink_payload(v, b'a\0b'))
        bad_namespace('long-symlink-component', lambda v: set_symlink_payload(v, b'a'*256))
        def long_entry(values, length):
            old = next(k for k, ident in values.items() if k.startswith(b'\0forkfs/entry/')
                       and int.from_bytes(values[b'\0forkfs/inode/' + ident][8:12], 'little') == 1)
            value = values.pop(old)
            prefix = old.rsplit(b'/', 1)[0] + b'/'
            values[prefix + b'a'*length] = value
        bad_namespace('long-entry-name', lambda v: long_entry(v, 256))
        bad_namespace('valid-entry-boundary', lambda v: long_entry(v, 255))
        def add_deep_directories(values):
            root_id = values[b'\0forkfs/root']
            root_inode_key = b'\0forkfs/inode/' + root_id
            root_inode = bytearray(values[root_inode_key])
            directory_template = next(value for key, value in values.items()
                                      if key.startswith(b'\0forkfs/inode/')
                                      and int.from_bytes(value[8:12], 'little') == 2)
            used_ids = {key[len(b'\0forkfs/inode/'):] for key in values if key.startswith(b'\0forkfs/inode/')}
            new_ids = []
            candidate = 2**128 - 1
            for _ in range(1000):
                while format(candidate, '032x').encode() in used_ids:
                    candidate -= 1
                ident = format(candidate, '032x').encode();candidate -= 1
                used_ids.add(ident);new_ids.append(ident)
            top_name = b'deep'
            while b'\0forkfs/entry/' + root_id + b'/' + top_name in values:
                top_name += b'x'
            values[b'\0forkfs/entry/' + root_id + b'/' + top_name] = new_ids[0]
            indexed = b'\0forkfs/parent-index' in values
            for index, ident in enumerate(new_ids):
                inode = bytearray(directory_template)
                struct.pack_into('<Q', inode, 16, 3 if index + 1 < len(new_ids) else 2)
                values[b'\0forkfs/inode/' + ident] = bytes(inode)
                values[b'\0forkfs/allocated/' + ident] = b''
                if indexed:
                    parent = root_id if index == 0 else new_ids[index-1]
                    name = top_name if index == 0 else b'd'
                    values[b'\0forkfs/parent/' + ident] = b'FFPAR001' + parent + name
                if index + 1 < len(new_ids):
                    values[b'\0forkfs/entry/' + ident + b'/d'] = new_ids[index+1]
            struct.pack_into('<Q', root_inode, 16, int.from_bytes(root_inode[16:24], 'little') + 1)
            values[root_inode_key] = bytes(root_inode)
        bad_namespace('deep-directories', add_deep_directories)
        parent = refs['merge'];shared_manifest = descriptor[8:40]
        for index in range(1500):
            revision = b'FFREV002' + shared_manifest + struct.pack('<QI', index, 1) + bytes.fromhex(parent)
            revision_id = object_id(revision);objects[revision_id.hex()] = revision.hex()
            parent = revision_id.hex()
        refs['deep-history'] = parent
        def add_long_native_path(values):
            root_id = values[b'\0forkfs/root']
            root_inode_key = b'\0forkfs/inode/' + root_id
            root_inode = bytearray(values[root_inode_key])
            directory_template = next(value for key, value in values.items()
                                      if key.startswith(b'\0forkfs/inode/')
                                      and int.from_bytes(value[8:12], 'little') == 2)
            used_ids = {key[len(b'\0forkfs/inode/'):] for key in values if key.startswith(b'\0forkfs/inode/')}
            new_ids = []
            candidate = 2**128 - 1
            for _ in range(15):
                while format(candidate, '032x').encode() in used_ids:
                    candidate -= 1
                ident = format(candidate, '032x').encode();candidate -= 1
                used_ids.add(ident);new_ids.append(ident)
            name = b'z'*255
            values[b'\0forkfs/entry/' + root_id + b'/' + name] = new_ids[0]
            indexed = b'\0forkfs/parent-index' in values
            for index, ident in enumerate(new_ids):
                inode = bytearray(directory_template)
                struct.pack_into('<Q', inode, 16, 3 if index + 1 < len(new_ids) else 2)
                values[b'\0forkfs/inode/' + ident] = bytes(inode)
                values[b'\0forkfs/allocated/' + ident] = b''
                if indexed:
                    parent = root_id if index == 0 else new_ids[index-1]
                    values[b'\0forkfs/parent/' + ident] = b'FFPAR001' + parent + name
                if index + 1 < len(new_ids):
                    values[b'\0forkfs/entry/' + ident + b'/' + name] = new_ids[index+1]
            file_key, file_template = next((key, value) for key, value in values.items()
                                            if key.startswith(b'\0forkfs/inode/')
                                            and int.from_bytes(value[8:12], 'little') == 1)
            while format(candidate, '032x').encode() in used_ids:
                candidate -= 1
            file_id = format(candidate, '032x').encode()
            file_inode = bytearray(file_template);struct.pack_into('<Q', file_inode, 16, 1)
            old_file_id = file_key[len(b'\0forkfs/inode/'):]
            values[b'\0forkfs/inode/' + file_id] = bytes(file_inode)
            values[b'\0forkfs/allocated/' + file_id] = b''
            values[b'\0forkfs/content/' + file_id] = values[b'\0forkfs/content/' + old_file_id]
            cls.long_path_file_data = values[b'\0forkfs/content/' + old_file_id]
            values[b'\0forkfs/entry/' + new_ids[-1] + b'/' + b'f'*255] = file_id
            struct.pack_into('<Q', root_inode, 16, int.from_bytes(root_inode[16:24], 'little') + 1)
            values[root_inode_key] = bytes(root_inode)
        bad_namespace('long-native-path', add_long_native_path)
        def set_v4_inode(values, accessed=0, created=0, xattrs=(), legacy=False):
            inode_key, old = next((key, value) for key, value in values.items()
                                  if key.startswith(b'\0forkfs/inode/') and int.from_bytes(value[8:12], 'little') == 1)
            flags_ids = old[48:64] if old[:8] in (b'FFINOD03', b'FFINOD04') else bytes(16)
            inode = bytearray(b'FFINOD04' + old[8:48] + flags_ids + struct.pack('<QQI', accessed, created, len(xattrs)))
            for name, value in xattrs:
                inode += struct.pack('<II', len(name), len(value)) + name + value
            values[inode_key] = b'FFINOD03' + bytes(inode[8:80]) if legacy else bytes(inode)
        bad_namespace('invalid-extended-timestamp', lambda v: set_v4_inode(v, accessed=2**63))
        bad_namespace('invalid-created-timestamp', lambda v: set_v4_inode(v, created=2**63))
        bad_namespace('invalid-v3-accessed-timestamp', lambda v: set_v4_inode(v, accessed=2**63, legacy=True))
        bad_namespace('invalid-v3-created-timestamp', lambda v: set_v4_inode(v, created=2**63, legacy=True))
        bad_namespace('oversized-v4-inode', lambda v: set_v4_inode(v, xattrs=((b'a', b'x'*65536), (b'b', b'y'*65536))))
        bad_namespace('valid-v4-inode-boundary', lambda v: set_v4_inode(v, accessed=2**63-1, created=2**63-1, xattrs=((b'a', b'x'*65536), (b'b', b'y'*65434))))
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
                 'malformed-entry-parent',
                 'extra-parent-record', 'extra-content-record', 'empty-symlink-target',
                 'oversized-symlink-target', 'nonprintable-symlink-target',
                 'long-symlink-component', 'long-entry-name', 'invalid-extended-timestamp',
                 'invalid-created-timestamp', 'invalid-v3-accessed-timestamp',
                 'invalid-v3-created-timestamp', 'oversized-v4-inode')
        for index, name in enumerate(names):
            with self.subTest(namespace=name), self.assertRaises(ApiError) as raised:
                self.sync(request_id='invalid-namespace-'+str(index), selectors=[{'name':name}])
            self.assertEqual(raised.exception.code, 'PROTOTYPE_VALIDATION_FAILED')
            self.no_ack()

    def test_06c_valid_symlink_component_and_target_boundaries(self):
        # Exactly 4096 bytes, with 255-byte components and a trailing slash.
        result = self.sync(request_id='valid-symlink-boundary',
                           selectors=[{'name':'valid-symlink-boundary'}])
        self.assertEqual(result['state'], 'committed')
        self.assertEqual(result['ack'], 'acknowledged')
        entry = self.sync(request_id='valid-entry-boundary', selectors=[{'name':'valid-entry-boundary'}])
        self.assertEqual(entry['ack'], 'acknowledged')
        inode = self.sync(request_id='valid-v4-inode-boundary',
                          selectors=[{'name':'valid-v4-inode-boundary'}])
        self.assertEqual(inode['ack'], 'acknowledged')
        deep = self.sync(request_id='deep-directories', selectors=[{'name':'deep-directories'}])
        self.assertEqual(deep['state'], 'committed')
        self.assertEqual(deep['ack'], 'acknowledged')
        history = self.sync(request_id='deep-history', selectors=[{'name':'deep-history'}])
        self.assertGreaterEqual(history['revisions'], 1500)
        self.assertEqual(history['ack'], 'acknowledged')

    def test_06d_restore_long_path_with_directory_fds(self):
        result = self.sync(request_id='long-native-path', selectors=[{'name':'long-native-path'}])
        restored = self.backup.request('POST', '/restore', {'backup_id':result['backup_id']})
        destination = self.backup.root/'restored'/restored['restore_id']
        revision_root = destination/self.fixture['refs']['long-native-path']
        components = [b'z'*255] * 15
        file_name = b'f'*255
        dir_flags = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)
        revision_fd = current_fd = None
        try:
            self.assertEqual(1 + sum(map(len, components)) + len(components)-1 + 1 + len(file_name), 4096)
            revision_fd = os.open(revision_root, dir_flags)
            current_fd = os.dup(revision_fd)
            for component in components:
                child_fd = os.open(component, dir_flags, dir_fd=current_fd)
                os.close(current_fd);current_fd = child_fd
            self.assertTrue(stat.S_ISDIR(os.fstat(current_fd).st_mode))
            file_fd = os.open(file_name, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0), dir_fd=current_fd)
            try:
                restored_data = bytearray()
                while True:
                    block = os.read(file_fd, 65536)
                    if not block: break
                    restored_data += block
            finally:
                os.close(file_fd)
            self.assertEqual(bytes(restored_data), self.long_path_file_data)
        finally:
            if current_fd is not None:
                try:
                    os.unlink(file_name, dir_fd=current_fd)
                except OSError:
                    pass
                os.close(current_fd)
            if revision_fd is None:
                try:
                    revision_fd = os.open(revision_root, dir_flags)
                except OSError:
                    pass
            if revision_fd is not None:
                for depth in reversed(range(len(components))):
                    parent_fd = os.dup(revision_fd)
                    try:
                        for component in components[:depth]:
                            child_fd = os.open(component, dir_flags, dir_fd=parent_fd)
                            os.close(parent_fd);parent_fd = child_fd
                        os.rmdir(components[depth], dir_fd=parent_fd)
                    except OSError:
                        pass
                    finally:
                        os.close(parent_fd)
                os.close(revision_fd)

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

    def test_09b_export_selector_schema_is_unambiguous_and_atomic(self):
        root = self.fixture['refs']['base']
        base = dict(consumer_id='mock-backup', destination_id='mock-destination',
                    scope='revision-closure', history='full', lease_seconds=900)
        invalid = [
            [{'name':'base','revision_id':root}],
            [{'name':'base','unexpected':True}],
            [{'revision_id':None}], [{'revision_id':[]}],
            [{'name':None}], [{'name':1}],
            [{'name':'base','expected_revision_id':None}],
            [{'name':'base','expected_revision_id':'z'*64}],
            [None], ['base'], None, {'name':'base'},
        ]
        before_jobs = self.source.state()['jobs']
        before_catalog = self.backup.state()['catalog']
        for index, selectors in enumerate(invalid):
            request = base | {'selectors':selectors}
            with self.subTest(selectors=selectors), self.assertRaises(ApiError) as raised:
                self.source.request('POST', self.base+'/exports', request,
                                    'invalid-selector-'+str(index))
            self.assertEqual(raised.exception.code, 'INVALID_ARGUMENT')
            self.assertEqual(raised.exception.status, 400)
            self.assertEqual(self.source.state()['jobs'], before_jobs)
            self.assertEqual(self.backup.state()['catalog'], before_catalog)
        for index, request in enumerate((None, [], 'invalid', base | {'selectors':[{'name':'base'}], 'extra':True})):
            with self.subTest(export_body=request), self.assertRaises(ApiError) as raised:
                self.source.request('POST', self.base+'/exports', request, 'invalid-export-'+str(index))
            self.assertEqual(raised.exception.code, 'INVALID_ARGUMENT')
            self.assertEqual(raised.exception.status, 400)
            self.assertEqual(self.source.state()['jobs'], before_jobs)
            self.assertEqual(self.backup.state()['catalog'], before_catalog)

        valid = ([{'revision_id':root}], [{'name':'base'}],
                 [{'name':'base','expected_revision_id':root}])
        for index, selectors in enumerate(valid):
            with self.subTest(valid_selector=selectors):
                result = self.source.request('POST', self.base+'/exports',
                                             base | {'selectors':selectors},
                                             'valid-selector-'+str(index))
                self.assertEqual(result['state'], 'ready')

        for index, selectors in enumerate(([{'name':'missing'}],
                                            [{'revision_id':'f'*64}],
                                            [{'name':'base','expected_revision_id':self.fixture['refs']['merge']}])):
            before_jobs = self.source.state()['jobs']
            before_catalog = self.backup.state()['catalog']
            with self.subTest(unknown_selector=selectors), self.assertRaises(ApiError) as raised:
                self.source.request('POST', self.base+'/exports', base | {'selectors':selectors},
                                    'unknown-selector-'+str(index))
            changed = 'expected_revision_id' in selectors[0]
            self.assertEqual(raised.exception.code, 'REF_CHANGED' if changed else 'REVISION_NOT_FOUND')
            self.assertEqual(raised.exception.status, 409 if changed else 404)
            self.assertEqual(self.source.state()['jobs'], before_jobs)
            self.assertEqual(self.backup.state()['catalog'], before_catalog)

    def test_10_lease_renew_expiry_and_membership(self):
        captured = self.capture();path = self.base+'/exports/'+captured['export_id']
        before = self.source.state()['jobs']
        for action, malformed in (('renew', {'lease_seconds':900,'extra':1}),
                                   ('renew', None), ('renew', []),
                                   ('packs', {'object_ids':[self.unrelated],'extra':1}),
                                   ('packs', {'object_ids':None}),
                                   ('packs', {'object_ids':[[self.unrelated]]})):
            with self.subTest(action=action, body=malformed), self.assertRaises(ApiError) as raised:
                self.source.request('POST',path+'/'+action,malformed,'schema-check')
            self.assertEqual(raised.exception.code, 'INVALID_ARGUMENT')
            self.assertEqual(raised.exception.status, 400)
            self.assertEqual(self.source.state()['jobs'], before)
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

    def test_10b_sync_renews_during_long_transfer(self):
        self.backup.faults(advance_sync_clock=4)
        result = self.sync(request_id='long-transfer-renewal')
        self.assertEqual(result['ack'], 'acknowledged')
        source = self.source.state()
        self.assertGreaterEqual(source['clock_offset'], 1200)
        self.assertGreaterEqual(source['stats']['renewals'], 3)

    def test_10c_expired_staging_keeps_cas_and_requires_new_explicit_request(self):
        self.source.faults(truncate_pack=1)
        with self.assertRaises(ApiError):
            self.sync(request_id='staging-expired')
        self.backup.stop();self.backup.start()
        before = self.backup.state()
        saved = before['jobs']['staging-expired']
        cas_before = sorted(p.name for p in (self.backup.root/'objects').iterdir())
        plan = saved['plan']
        self.source.request('POST','/__test__/advance',
                            {'name':'latest','revision_id':self.fixture['refs']['next']})
        self.source.request('POST','/__test__/clock',{'seconds':901})
        with self.assertRaises(ApiError) as error:
            self.sync(request_id='staging-expired')
        self.assertEqual(error.exception.code, 'EXPORT_EXPIRED')
        after = self.backup.state()
        self.assertEqual(after['catalog'], before['catalog'])
        self.no_ack()
        self.assertEqual(after['jobs']['staging-expired']['plan'], plan)
        self.assertEqual(sorted(p.name for p in (self.backup.root/'objects').iterdir()), cas_before)
        explicit = [{'revision_id':ident} for ident in plan['selected_revision_ids']]
        resumed = self.sync(request_id='staging-expired-new', selectors=explicit)
        self.assertEqual(resumed['ack'], 'acknowledged')
        final = self.backup.state()
        self.assertEqual(final['jobs']['staging-expired-new']['plan'], plan)
        expected = source_closure(plan['selected_revision_ids'], self.fixture['objects'])
        self.assertEqual(final['stats']['received_objects'],
                         before['stats']['received_objects'] + len(expected) - len(cas_before))
        self.assertTrue(set(cas_before).issubset(
            {p.name for p in (self.backup.root/'objects').iterdir()}))

    def test_11_crash_before_publish_resume_without_object_retransfer(self):
        self.backup.faults(crash_before_publish=1)
        with self.assertRaises((OSError,http.client.HTTPException)):self.sync()
        self.assertEqual(self.backup.proc.wait(timeout=10),86)
        self.backup.stop();self.backup.start()
        before = self.backup.state()['stats']['received_objects'];self.no_ack()
        self.assertEqual(self.sync()['state'],'committed')
        self.assertEqual(self.backup.state()['stats']['received_objects'],before)
        self.assertGreaterEqual(self.source.state()['stats']['renewals'],1)

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

    def test_13b_malformed_ack_receipt_is_not_persisted(self):
        capture = self.capture(key='ack-schema')
        job_path = self.base + '/exports/' + capture['export_id']
        plan = capture['plan']
        receipt = dict(receipt_id='1'*32, consumer_id='mock-backup', destination_id='mock-destination',
                       source_repository=plan['source_repository'], plan_hash=plan['plan_hash'],
                       inventory_root=plan['inventory_root'], selected_revision_ids=plan['selected_revision_ids'],
                       backup_id=plan['plan_hash'], backup_manifest_hash='a'*64, catalog_generation='1',
                       durability='destination-committed', retention_expires_at=None)
        before = self.source.state()
        missing = receipt.copy();missing.pop('receipt_id')
        malformed_receipts = [(missing, 'INVALID_ARGUMENT')]
        for field, value in [('receipt_id', ''), ('backup_manifest_hash', 'bad'),
                             ('catalog_generation', str(2**64)), ('durability', 'uncommitted')]:
            malformed_receipts.append((receipt | {field: value}, 'RECEIPT_MISMATCH'))
        for malformed, code in malformed_receipts:
            with self.subTest(receipt=malformed), self.assertRaises(ApiError) as raised:
                self.source.request('POST', job_path+'/acknowledgements', malformed)
            self.assertEqual(raised.exception.code, code)
            self.assertEqual(self.source.state()['jobs'], before['jobs'])
        self.source.stop();self.source.start()
        self.assertEqual(self.source.state()['jobs'], before['jobs'])
        self.assertEqual(self.source.request('POST', job_path+'/acknowledgements', receipt)['state'], 'acknowledged')
        saved = self.source.state()['jobs'][capture['export_id']]
        self.assertEqual(saved['state'], 'acknowledged')
        self.assertEqual(saved['receipt'], receipt)

    def test_13c_receipt_id_conflicts_across_exports(self):
        first = self.capture(key='receipt-first', selectors=[{'name':'base'}])
        same_plan = self.capture(key='receipt-same-plan', selectors=[{'name':'base'}])
        second = self.capture(key='receipt-second', selectors=[{'name':'latest'}])

        def receipt_for(plan, receipt_id, manifest_hash):
            return dict(receipt_id=receipt_id, consumer_id='mock-backup', destination_id='mock-destination',
                        source_repository=plan['source_repository'], plan_hash=plan['plan_hash'],
                        inventory_root=plan['inventory_root'], selected_revision_ids=plan['selected_revision_ids'],
                        backup_id=plan['plan_hash'], backup_manifest_hash=manifest_hash, catalog_generation='1',
                        durability='destination-committed', retention_expires_at=None)

        receipt_id = '2'*32
        first_path = self.base + '/exports/' + first['export_id'] + '/acknowledgements'
        second_path = self.base + '/exports/' + second['export_id'] + '/acknowledgements'
        first_receipt = receipt_for(first['plan'], receipt_id, 'a'*64)
        colliding = receipt_for(second['plan'], receipt_id, 'b'*64)
        self.assertEqual(self.source.request('POST', first_path, first_receipt)['state'], 'acknowledged')
        same_plan_path = self.base + '/exports/' + same_plan['export_id'] + '/acknowledgements'
        self.assertEqual(self.source.request('POST', same_plan_path, first_receipt)['state'], 'acknowledged')
        before = self.source.state()
        with self.assertRaises(ApiError) as raised:
            self.source.request('POST', second_path, colliding)
        self.assertEqual(raised.exception.code, 'RECEIPT_ID_CONFLICT')
        self.assertEqual(self.source.state()['jobs'], before['jobs'])
        self.source.stop();self.source.start()
        self.assertEqual(self.source.state()['jobs'], before['jobs'])
        with self.assertRaises(ApiError) as raised:
            self.source.request('POST', second_path, colliding)
        self.assertEqual(raised.exception.code, 'RECEIPT_ID_CONFLICT')
        corrected = receipt_for(second['plan'], '3'*32, 'b'*64)
        self.assertEqual(self.source.request('POST', second_path, corrected)['state'], 'acknowledged')

    def test_13d_sync_idempotency_includes_repository(self):
        self.sync()
        before = self.backup.state()
        with self.assertRaises(ApiError) as raised:
            self.sync(repository='00'*16)
        self.assertEqual(raised.exception.code, 'IDEMPOTENCY_CONFLICT')
        self.assertEqual(self.backup.state(), before)

    def test_13e_acknowledged_status_is_monotonic_when_source_is_unavailable(self):
        first = self.sync()
        self.assertEqual(first['ack'], 'acknowledged')
        before = self.backup.state()
        self.source.stop()
        retry = self.sync()
        self.assertEqual(retry['ack'], 'acknowledged')
        after = self.backup.state()
        self.assertEqual(after['jobs']['request-a']['ack'], 'acknowledged')
        self.assertEqual(after['catalog'], before['catalog'])

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

    def test_21_revision_dag_byte_budget_matches_canonical_boundary(self):
        root = self.fixture['refs']['base']
        entries = source_closure([root], self.fixture['objects'])
        plan, _ = make_plan(self.fixture['repository'], self.fixture['sequence'], [root], entries)
        load = lambda ident: bytes.fromhex(self.fixture['objects'][ident])
        with self.assertRaisesRegex(ValueError, 'byte budget'):
            verify_closure(plan, entries, load, max_dag_bytes=167)
        self.assertEqual(set(verify_closure(plan, entries, load, max_dag_bytes=168)), {root})

        objects = self.fixture['objects'].copy()
        base_descriptor = bytes.fromhex(objects[root])
        child = (b'FFREV002' + base_descriptor[8:40] + struct.pack('<QI', 3, 1)
                 + bytes.fromhex(root))
        child_id = object_id(child).hex();objects[child_id] = child.hex()
        roots = [root, child_id]
        entries = source_closure(roots, objects)
        plan, _ = make_plan(self.fixture['repository'], self.fixture['sequence'], roots, entries)
        load = lambda ident: bytes.fromhex(objects[ident])
        with self.assertRaisesRegex(ValueError, 'byte budget'):
            verify_closure(plan, entries, load, max_dag_bytes=367)
        self.assertEqual(set(verify_closure(plan, entries, load, max_dag_bytes=368)), set(roots))


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
