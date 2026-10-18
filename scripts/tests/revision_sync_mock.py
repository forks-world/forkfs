"""Local-only HTTP protocol prototype. Source uses an offline test fixture;
backup uses only HTTP and its own durable CAS. No production listener or SDK.
"""
import argparse
import contextlib
import copy
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
import os
from pathlib import Path
import re
import socket
import socketserver
import threading
import time
from urllib.parse import urlsplit
import uuid

from revision_sync_codec import source_closure, make_plan, encode_pack, digest, object_id, revision
from revision_sync_validator import decode_inventory, receive_pack, verify_closure, restore_views, checked, HEX


def atomic(path, data):
    path = Path(path);path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name('.' + path.name + '.' + uuid.uuid4().hex)
    try:
        with temp.open('xb') as out:
            out.write(data);out.flush();os.fsync(out.fileno())
        os.replace(temp, path)
        fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temp.unlink(missing_ok=True)


def json_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


class ApiError(Exception):
    def __init__(self, code, status=422):
        self.code, self.status = code, status
        super().__init__(code)


@contextlib.contextmanager
def remote(url, method, path, body=None, key=None):
    parsed = urlsplit(url)
    if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or parsed.path:
        raise ValueError('mock transport only accepts a loopback HTTP origin')
    connection = http.client.HTTPConnection('127.0.0.1', parsed.port, timeout=30)
    headers = {'Authorization': 'Bearer mock-test-only', 'Content-Type': 'application/json'}
    if key:
        headers['Idempotency-Key'] = key
    try:
        connection.request(method, path, None if body is None else json_bytes(body), headers)
        response = connection.getresponse()
        if response.status >= 400:
            failure = json.loads(response.read())
            raise ApiError(failure['code'], response.status)
        yield response
    finally:
        connection.close()


def call(url, method, path, body=None, key=None):
    with remote(url, method, path, body, key) as response:
        return json.loads(response.read())


class Backend:
    def __init__(self, role, root, source):
        self.role, self.root, self.source = role, Path(root), source
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.state_path = self.root / 'state.json'
        if self.state_path.exists():
            self.state = json.loads(self.state_path.read_text())
        else:
            self.state = dict(jobs={}, faults={}, clock_offset=0, stats={}, catalog={})
        if role == 'source':
            self.fixture = json.loads((self.root / 'fixture.json').read_text())
            self.state.setdefault('refs', self.fixture['refs'])
            self.base = '/v1/repositories/' + self.fixture['repository']
        self.save()

    def save(self):
        atomic(self.state_path, json_bytes(self.state))

    def count(self, name, n=1):
        self.state['stats'][name] = self.state['stats'].get(name, 0) + n

    def fault(self, name):
        if self.state['faults'].get(name):
            self.state['faults'][name] -= 1;self.save()
            return True
        return False

    def job(self, ident, active=True):
        if ident not in self.state['jobs']:
            raise ApiError('EXPORT_NOT_FOUND', 404)
        job = self.state['jobs'][ident]
        if job['state'] == 'ready' and time.time() + self.state['clock_offset'] >= job['expires']:
            job['state'] = 'expired';self.save()
        if active:
            if job['state'] == 'cancelled':
                raise ApiError('EXPORT_CANCELLED', 410)
            if job['state'] == 'expired':
                raise ApiError('EXPORT_EXPIRED', 410)
        return job

    def source_api(self, method, path, body, key):
        if path == self.base + '/sync-capabilities':
            version = 'unsupported/2' if self.fault('wrong_protocol') else 'forkfs.revision-sync/1'
            return dict(protocol=version, mock=True, source_repository=self.fixture['repository'],
                        max_object_bytes=262144, max_pack_bytes=16777216, namespace_codecs=['FFTREE01'])
        if path == self.base + '/consumers/mock-backup/coverage':
            roots = set()
            receipts = {}
            for job in self.state['jobs'].values():
                if job['state'] == 'acknowledged':
                    roots.update(job['receipt']['selected_revision_ids'])
                    receipts[job['receipt']['receipt_id']] = job['receipt']
            ancestors = set()
            for root in roots:
                queue = revision(bytes.fromhex(self.fixture['objects'][root]))[1]
                while queue:
                    current = queue.pop().hex()
                    if current not in ancestors:
                        ancestors.add(current)
                        queue.extend(revision(bytes.fromhex(self.fixture['objects'][current]))[1])
            return dict(frontier=sorted(roots - ancestors), receipts=list(receipts.values()))
        if path == self.base + '/revision-refs':
            return {'refs': self.state['refs']}
        if path == self.base + '/exports' and method == 'POST':
            if not key or set(body) != {'selectors', 'consumer_id', 'destination_id', 'scope', 'history', 'lease_seconds'}:
                raise ApiError('INVALID_ARGUMENT', 400)
            normalized = json_bytes(body).hex()
            for ident, job in self.state['jobs'].items():
                if job['key'] == key:
                    if job['request'] != normalized:
                        raise ApiError('IDEMPOTENCY_CONFLICT', 409)
                    return {'export_id': ident, 'state': self.job(ident, active=False)['state'], 'plan': job['plan']}
            if body['scope'] != 'revision-closure' or body['history'] != 'full' or not 1 <= len(body['selectors']) <= 256:
                raise ApiError('INVALID_ARGUMENT', 400)
            if body['consumer_id'] != 'mock-backup' or body['destination_id'] != 'mock-destination':
                raise ApiError('FORBIDDEN', 403)
            if type(body['lease_seconds']) is not int or not 1 <= body['lease_seconds'] <= 900:
                raise ApiError('INVALID_ARGUMENT', 400)
            roots = []
            for selector in body['selectors']:
                if 'name' in selector:
                    oid = self.state['refs'].get(selector['name'])
                    if not oid:
                        raise ApiError('REVISION_NOT_FOUND', 404)
                    if selector.get('expected_revision_id', oid) != oid:
                        raise ApiError('REF_CHANGED', 409)
                else:
                    oid = selector.get('revision_id')
                    if oid not in self.state['refs'].values():
                        raise ApiError('REVISION_NOT_FOUND', 404)
                roots.append(oid)
            entries = source_closure(roots, self.fixture['objects'])
            if self.fault('omit_inventory_object'):
                entries.pop(self.fixture['required_object'])
            plan, nodes = make_plan(self.fixture['repository'], self.fixture['sequence'], roots, entries)
            ident = uuid.uuid4().hex
            job = dict(key=key, request=normalized, state='ready', plan=plan, nodes=nodes, entries=entries,
                       consumer=body['consumer_id'], destination=body['destination_id'],
                       expires=time.time() + self.state['clock_offset'] + body['lease_seconds'])
            self.state['jobs'][ident] = job;self.save()
            return {'export_id': ident, 'state': 'ready', 'plan': plan}
        prefix = self.base + '/exports/'
        if not path.startswith(prefix):
            raise ApiError('NOT_FOUND', 404)
        ident, _, action = path[len(prefix):].partition('/')
        job = self.job(ident, active=not (method == 'DELETE' or (method == 'GET' and not action)))
        if job['state'] == 'acknowledged' and (action == 'renew' or action == 'packs' or action.startswith('inventory/')):
            raise ApiError('EXPORT_CLOSED', 410)
        if method == 'DELETE' and not action:
            if job['state'] == 'ready':
                job['state'] = 'cancelled';self.save()
            return {'state': job['state']}
        if not action and method == 'GET':
            return {'export_id': ident, 'state': job['state'], 'plan': job['plan']}
        if action == 'renew' and method == 'POST':
            seconds = body.get('lease_seconds', 0)
            if not isinstance(seconds, int) or not 1 <= seconds <= 900:
                raise ApiError('INVALID_ARGUMENT', 400)
            job['expires'] = time.time() + self.state['clock_offset'] + seconds;self.save()
            return {'expires': job['expires']}
        if action.startswith('inventory/') and method == 'GET':
            node = action.split('/')[1]
            if node not in job['nodes']:
                raise ApiError('FORBIDDEN', 403)
            self.count('inventory_requests');self.save()
            data = bytes.fromhex(job['nodes'][node])
            if self.fault('corrupt_inventory'):
                data = data[:-1] + bytes([data[-1] ^ 1])
            return data
        if action == 'packs' and method == 'POST':
            ids = body.get('object_ids', [])
            if not ids or ids != sorted(set(ids)) or len(ids) > 1024:
                raise ApiError('INVALID_ARGUMENT', 400)
            if any(not isinstance(i, str) or not HEX.fullmatch(i) or i not in job['entries'] for i in ids):
                raise ApiError('FORBIDDEN', 403)
            if sum(job['entries'][i] for i in ids) > 16777216:
                raise ApiError('LIMIT_EXCEEDED', 413)
            if not key:
                raise ApiError('INVALID_ARGUMENT', 400)
            previous = job.setdefault('pack_keys', {}).get(key)
            if previous is not None and previous != ids:
                raise ApiError('IDEMPOTENCY_CONFLICT', 409)
            job['pack_keys'][key] = ids
            data = encode_pack(job['plan']['plan_hash'], ids, self.fixture['objects'])
            self.count('pack_requests');self.count('requested_objects', len(ids));self.save()
            if self.fault('corrupt_pack_payload'):
                payload = bytearray(data[:-32]);offset = 44
                while True:
                    size = int.from_bytes(payload[offset+32:offset+36], 'little')
                    if size:
                        payload[offset+36] ^= 1;break
                    offset += 36
                data = bytes(payload) + digest('forkfs.sync.pack.v1', payload)
            if self.fault('truncate_pack'):
                n = int.from_bytes(data[76:80], 'little')
                return ('truncate', data, 80 + n)
            return data
        if action == 'acknowledgements' and method == 'POST':
            plan = job['plan']
            if (body.get('plan_hash') != plan['plan_hash'] or body.get('inventory_root') != plan['inventory_root']
                    or body.get('selected_revision_ids') != plan['selected_revision_ids']
                    or body.get('source_repository') != plan['source_repository']
                    or body.get('destination_id') != job['destination'] or body.get('consumer_id') != job['consumer']):
                raise ApiError('RECEIPT_MISMATCH', 409)
            if job['state'] == 'acknowledged' and body != job['receipt']:
                raise ApiError('RECEIPT_MISMATCH', 409)
            job['receipt'] = body;job['state'] = 'acknowledged';self.save()
            if self.fault('drop_ack_response'):
                return ('drop',)
            return {'state': 'acknowledged', 'receipt_id': body['receipt_id']}
        raise ApiError('NOT_FOUND', 404)

    def object_path(self, object_id):
        checked(bool(HEX.fullmatch(object_id)), 'invalid CAS ID')
        return self.root / 'objects' / object_id

    def load(self, object_id):
        return self.object_path(object_id).read_bytes()

    def sync(self, body):
        request_id = body['request_id'];selectors = body['selectors']
        jobs = self.state['jobs']
        if request_id in jobs and jobs[request_id]['selectors'] != selectors:
            raise ApiError('IDEMPOTENCY_CONFLICT', 409)
        if request_id not in jobs:
            base = '/v1/repositories/' + body['repository']
            capabilities = call(self.source, 'GET', base + '/sync-capabilities')
            checked(capabilities['protocol'] == 'forkfs.revision-sync/1' and capabilities['source_repository'] == body['repository']
                    and 'FFTREE01' in capabilities['namespace_codecs'], 'unsupported capabilities')
            request = dict(selectors=selectors, consumer_id='mock-backup', destination_id='mock-destination',
                           scope='revision-closure', history='full', lease_seconds=900)
            capture = call(self.source, 'POST', base + '/exports', request, request_id)
            jobs[request_id] = dict(selectors=selectors, export_id=capture['export_id'], plan=capture['plan'],
                                    base=base, state='staging')
            self.save()
        job = jobs[request_id];plan = job['plan'];path = job['base'] + '/exports/' + job['export_id']
        def inventory_node(node_hash):
            checked(bool(HEX.fullmatch(node_hash)), 'inventory ID')
            target = self.root / 'inventory' / node_hash
            if not target.exists():
                with remote(self.source, 'GET', path + '/inventory/' + node_hash) as response:
                    data = response.read(65537)
                checked(len(data) <= 65536 and digest('forkfs.sync.inventory.v1', data).hex() == node_hash, 'inventory hash mismatch')
                atomic(target, data);self.count('inventory_downloads')
            return target.read_bytes()
        entries = decode_inventory(plan, inventory_node)
        missing = []
        for ident, length in entries.items():
            target = self.object_path(ident)
            if not target.exists():
                missing.append(ident)
            else:
                data = target.read_bytes()
                checked(len(data) == length and object_id(data).hex() == ident, 'existing CAS corruption')
        missing.sort()
        def persist(ident, data):
            atomic(self.object_path(ident), data)
            self.count('received_objects');self.count('received_payload_bytes', len(data))
        while missing:
            batch, total = [], 0
            for ident in missing:
                if len(batch) == 1024 or total + entries[ident] > 16777216:
                    break
                batch.append(ident);total += entries[ident]
            with remote(self.source, 'POST', path + '/packs', {'object_ids': batch}, request_id + ':' + batch[0]) as response:
                receive_pack(response, plan, batch, entries, persist)
            self.save()
            missing = missing[len(batch):]
        views = verify_closure(plan, entries, self.load)
        if self.fault('crash_before_publish'):
            os._exit(86)
        backup_id = plan['plan_hash']
        if backup_id not in self.state['catalog']:
            generation = len(self.state['catalog']) + 1
            manifest = dict(plan=plan, entries=entries, generation=generation)
            atomic(self.root / 'manifests' / backup_id, json_bytes(manifest))
            receipt = dict(receipt_id=uuid.uuid4().hex, consumer_id='mock-backup', destination_id='mock-destination',
                           source_repository=plan['source_repository'], plan_hash=backup_id,
                           inventory_root=plan['inventory_root'], selected_revision_ids=plan['selected_revision_ids'],
                           backup_id=backup_id, backup_manifest_hash=hashlib.sha256(json_bytes(manifest)).hexdigest(),
                           catalog_generation=str(generation), durability='destination-committed', retention_expires_at=None)
            self.state['catalog'][backup_id] = dict(receipt=receipt, manifest=backup_id)
        job['state'] = 'committed';self.save()
        if self.fault('crash_after_publish'):
            os._exit(87)
        receipt = self.state['catalog'][backup_id]['receipt']
        if self.fault('expire_before_ack'):
            call(self.source, 'POST', '/__test__/clock', {'seconds': 901})
        try:
            call(self.source, 'POST', path + '/acknowledgements', receipt)
            job['ack'] = 'acknowledged'
        except ApiError as exc:
            job['ack'] = 'expired' if exc.status == 410 else ('pending' if exc.status >= 500 else 'rejected')
            job['ack_error'] = exc.code
        except (OSError, http.client.HTTPException):
            job['ack'] = 'pending'
        self.save()
        return dict(backup_id=backup_id, state='committed', ack=job['ack'], revisions=len(views), export_id=job['export_id'])

    def backup_api(self, method, path, body):
        if path == '/sync' and method == 'POST':
            return self.sync(body)
        if path == '/restore' and method == 'POST':
            backup_id = body['backup_id']
            if backup_id not in self.state['catalog']:
                raise ApiError('BACKUP_NOT_FOUND', 404)
            data = (self.root / 'manifests' / backup_id).read_bytes()
            receipt = self.state['catalog'][backup_id]['receipt']
            checked(hashlib.sha256(data).hexdigest() == receipt['backup_manifest_hash'], 'backup manifest hash')
            manifest = json.loads(data)
            views = verify_closure(manifest['plan'], manifest['entries'], self.load)
            name = uuid.uuid4().hex
            restore_views(views, self.root / 'restored' / name)
            return dict(restore_id=name, revisions=sorted(views))
        raise ApiError('NOT_FOUND', 404)

    def dispatch(self, method, path, body, key):
        if path == '/__test__/state':
            return copy.deepcopy(self.state)
        if path == '/__test__/faults' and method == 'POST':
            self.state['faults'] = body;self.save();return {'ok': True}
        if path == '/__test__/advance' and self.role == 'source':
            self.state['refs'][body['name']] = body['revision_id'];self.save();return {'ok': True}
        if path == '/__test__/clock' and self.role == 'source':
            self.state['clock_offset'] += body['seconds'];self.save();return {'ok': True}
        if path == '/__test__/drop-object' and self.role == 'backup':
            self.object_path(body['object_id']).unlink();return {'ok': True}
        if self.role == 'source':
            return self.source_api(method, path, body, key)
        return self.backup_api(method, path, body)


class LoopbackServer(ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer normally calls getfqdn(), which can perform slow external
        # DNS lookups even for 127.0.0.1 on CI hosts. This test needs no DNS.
        socketserver.TCPServer.server_bind(self)
        self.server_name = '127.0.0.1'
        self.server_port = self.server_address[1]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_GET(self):
        self.handle_api()
    def do_POST(self):
        self.handle_api()
    def do_DELETE(self):
        self.handle_api()
    def handle_api(self):
        if self.headers.get('Authorization') != 'Bearer mock-test-only':
            self.send_json({'code': 'FORBIDDEN'}, 403);return
        try:
            n = int(self.headers.get('Content-Length', '0'))
            if not 0 <= n <= 131072:
                raise ApiError('LIMIT_EXCEEDED', 413)
            body = json.loads(self.rfile.read(n)) if n else None
            with self.server.backend.lock:
                result = self.server.backend.dispatch(self.command, self.path, body, self.headers.get('Idempotency-Key'))
            if isinstance(result, tuple):
                if result[0] == 'drop':
                    self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
                _, full, cut = result
                self.send_response(200);self.send_header('Content-Length', str(len(full)));self.end_headers()
                self.wfile.write(full[:cut]);self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            if isinstance(result, bytes):
                self.send_response(200)
                self.send_header('Content-Type', 'application/vnd.forkfs.sync-pack' if self.path.endswith('/packs') else 'application/octet-stream')
                self.send_header('Content-Length', str(len(result)));self.end_headers();self.wfile.write(result)
            else:
                self.send_json(result, 202 if self.command == 'POST' and self.path.endswith('/exports') else 200)
        except ApiError as exc:
            self.send_json({'code': exc.code}, exc.status)
        except (ValueError, KeyError, OSError, http.client.HTTPException) as exc:
            self.server.backend.save()
            self.send_json({'code': 'PROTOTYPE_VALIDATION_FAILED', 'message': str(exc)}, 422)
    def send_json(self, value, status):
        data = json_bytes(value)
        self.send_response(status);self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)));self.end_headers();self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=['source', 'backup'], required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--endpoint-file', type=Path, required=True)
    parser.add_argument('--source-url')
    args = parser.parse_args()
    (args.root / 'restored').mkdir(parents=True, exist_ok=True)
    server = LoopbackServer(('127.0.0.1', 0), Handler)
    server.backend = Backend(args.role, args.root, args.source_url)
    atomic(args.endpoint_file, ('http://127.0.0.1:' + str(server.server_port)).encode())
    server.serve_forever()


if __name__ == '__main__':
    main()
