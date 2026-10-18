"""Test-only source encoders for REVISION_SYNC_API.md; not a production SDK."""
import hashlib
import struct

ZERO = bytes(32)
MAX_OBJECT = 256 * 1024
MAX_PACK = 16 * 1024 * 1024


def object_id(data):
    return hashlib.sha256(b'forkfs.object.v0' + struct.pack('<HHQ', 4, 0, len(data)) + data).digest()


def digest(domain, data):
    return hashlib.sha256(domain.encode() + b'\0' + data).digest()


def revision(data):
    if data[:8] == b'FFREV001' and len(data) == 80:
        parents = [] if data[40:72] == ZERO else [data[40:72]]
        return data[8:40], parents
    if data[:8] != b'FFREV002' or len(data) < 52:
        raise ValueError('unsupported revision')
    count, = struct.unpack_from('<I', data, 48)
    if count > 64 or len(data) != 52 + 32 * count:
        raise ValueError('invalid revision parents')
    parents = [data[52+i*32:84+i*32] for i in range(count)]
    if ZERO in parents or len(set(parents)) != count:
        raise ValueError('invalid parent IDs')
    return data[8:40], parents


def source_closure(roots, objects):
    """Source graph walker. Receiver validation is independently implemented."""
    visited, ids = set(), set()
    pending = [(bytes.fromhex(root), 'revision') for root in roots]
    while pending:
        oid, role = pending.pop()
        if (oid, role) in visited:
            continue
        visited.add((oid, role))
        data = bytes.fromhex(objects[oid.hex()])
        if object_id(data) != oid or len(data) > MAX_OBJECT:
            raise ValueError('source object hash/length')
        ids.add(oid.hex())
        if role == 'revision':
            manifest, parents = revision(data)
            pending.extend((p, 'revision') for p in parents)
            pending.append((manifest, 'manifest'))
        elif role == 'manifest':
            if data[:8] != b'FFTREE01' or len(data) != 40:
                raise ValueError('mock supports FFTREE01 only')
            pending.append((data[8:], 'tree'))
        elif role == 'tree':
            if len(data) < 108 or data[:8] != b'FFTRNO01':
                raise ValueError('source tree codec')
            pending.append((data[12:44], 'value'))
            pending.extend((child, 'tree') for child in (data[44:76], data[76:108]) if child != ZERO)
    return {oid: len(bytes.fromhex(objects[oid])) for oid in sorted(ids)}


def inventory(entries):
    nodes = {}
    def build(prefix, rows):
        header = b'FFSIN001' + bytes([len(rows) > 1024, len(prefix)]) + prefix.encode() + struct.pack('<Q', len(rows))
        if len(rows) <= 1024:
            data = header + struct.pack('<I', len(rows))
            data += b''.join(bytes.fromhex(oid) + struct.pack('<I', size) for oid, size in rows)
        else:
            children = []
            bitmap = 0
            for nibble in '0123456789abcdef':
                part = [(oid, size) for oid, size in rows if oid[len(prefix)] == nibble]
                if part:
                    bitmap |= 1 << int(nibble, 16)
                    children.append(build(prefix + nibble, part))
            data = header + struct.pack('<H', bitmap) + b''.join(children)
        oid = digest('forkfs.sync.inventory.v1', data)
        nodes[oid.hex()] = data.hex()
        return oid
    root = build('', sorted(entries.items())).hex()
    return root, nodes


def make_plan(repository, sequence, roots, entries):
    root, nodes = inventory(entries)
    roots = sorted(set(roots))
    body = (bytes.fromhex(repository) + struct.pack('<QI', sequence, len(roots))
            + b''.join(bytes.fromhex(r) for r in roots) + bytes.fromhex(root)
            + struct.pack('<QQI', len(entries), sum(entries.values()), 1))
    plan = dict(protocol='forkfs.revision-sync/1', source_repository=repository,
                observed_sequence=str(sequence), scope='revision-closure', history='full',
                selected_revision_ids=roots, inventory_root=root, object_count=str(len(entries)),
                payload_bytes=str(sum(entries.values())), plan_hash=digest('forkfs.sync.plan.v1', body).hex())
    return plan, nodes


def encode_pack(plan_hash, ids, objects):
    if ids != sorted(set(ids)) or not 1 <= len(ids) <= 1024:
        raise ValueError('pack IDs')
    payloads = [bytes.fromhex(objects[oid]) for oid in ids]
    if sum(map(len, payloads)) > MAX_PACK:
        raise ValueError('pack budget')
    data = b'FFSPK001' + bytes.fromhex(plan_hash) + struct.pack('<I', len(ids))
    for oid, payload in zip(ids, payloads):
        data += bytes.fromhex(oid) + struct.pack('<I', len(payload)) + payload
    return data + digest('forkfs.sync.pack.v1', data)
