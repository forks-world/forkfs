"""Independent mock-receiver decoder/validator. Restricted to negotiated test codecs."""
from collections import Counter
import hashlib
import os
from pathlib import Path
import re
import struct

HEX = re.compile(r'^[0-9a-f]{64}$')
NS = b'\0forkfs/'


def oid(data):
    return hashlib.sha256(b'forkfs.object.v0' + struct.pack('<HHQ', 4, 0, len(data)) + data).hexdigest()


def checked(condition, message):
    if not condition:
        raise ValueError(message)


def validate_symlink_target(target):
    """Match daemon/namespace.cpp target_components validation."""
    checked(bool(target) and len(target) <= 4096, 'symlink target length')
    checked(b'\0' not in target, 'symlink target contains NUL')
    checked(all(not component or len(component) <= 255 for component in target.split(b'/')),
            'symlink target component length')


def index_directory_entries(values):
    """Group namespace entry records by their fixed-width parent inode ID."""
    prefix = NS + b'entry/'
    indexed = {}
    for key, child in values.items():
        if not key.startswith(prefix):
            continue
        parent, separator, name = key[len(prefix):].partition(b'/')
        if separator and len(parent) == 32:
            indexed.setdefault(parent, []).append((name, child))
    return indexed


class Input:
    def __init__(self, data):
        self.data, self.at = data, 0
    def take(self, count):
        checked(0 <= count <= len(self.data) - self.at, 'truncated encoding')
        out = self.data[self.at:self.at+count];self.at += count
        return out
    def number(self, count):
        return int.from_bytes(self.take(count), 'little')
    def end(self):
        checked(self.at == len(self.data), 'trailing encoding')


def validate_plan(plan):
    checked(plan['protocol'] == 'forkfs.revision-sync/1' and plan['scope'] == 'revision-closure'
            and plan['history'] == 'full', 'unsupported plan')
    roots = plan['selected_revision_ids']
    checked(roots == sorted(set(roots)) and 1 <= len(roots) <= 256, 'plan roots')
    for value in roots + [plan['inventory_root'], plan['plan_hash']]:
        checked(bool(HEX.fullmatch(value)), 'invalid plan hash ID')
    checked(bool(re.fullmatch('[0-9a-f]{32}', plan['source_repository'])), 'repository identity')
    ints = [plan[k] for k in ('observed_sequence', 'object_count', 'payload_bytes')]
    checked(all(re.fullmatch('0|[1-9][0-9]*', n) and int(n) < 2**64 for n in ints), 'plan integer')
    raw = (bytes.fromhex(plan['source_repository']) + int(ints[0]).to_bytes(8, 'little')
           + len(roots).to_bytes(4, 'little') + b''.join(bytes.fromhex(r) for r in roots)
           + bytes.fromhex(plan['inventory_root']) + int(ints[1]).to_bytes(8, 'little')
           + int(ints[2]).to_bytes(8, 'little') + (1).to_bytes(4, 'little'))
    checked(hashlib.sha256(b'forkfs.sync.plan.v1\0' + raw).hexdigest() == plan['plan_hash'], 'plan hash mismatch')


def decode_inventory(plan, load):
    validate_plan(plan)
    entries, visited = {}, set()
    def walk(node_hash, prefix):
        checked(node_hash not in visited and len(visited) < 100000, 'inventory cycle/budget')
        visited.add(node_hash)
        data = load(node_hash)
        checked(hashlib.sha256(b'forkfs.sync.inventory.v1\0' + data).hexdigest() == node_hash, 'inventory hash mismatch')
        inp = Input(data)
        checked(inp.take(8) == b'FFSIN001', 'inventory magic')
        kind, size = inp.number(1), inp.number(1)
        checked(size <= 64 and inp.take(size).decode() == prefix, 'inventory prefix')
        count = inp.number(8)
        checked(count <= 1000000, 'inventory object budget')
        if kind == 0:
            n = inp.number(4)
            checked(n == count and n <= 1024 and (n or not prefix), 'inventory leaf count')
            previous = ''
            for _ in range(n):
                object_id, length = inp.take(32).hex(), inp.number(4)
                checked(object_id > previous and object_id.startswith(prefix) and length <= 262144, 'inventory entry')
                checked(object_id not in entries, 'inventory duplicate')
                entries[object_id] = length;previous = object_id
        else:
            checked(kind == 1 and count > 1024 and len(prefix) < 64, 'inventory branch')
            bitmap = inp.number(2);total = 0
            for bit in range(16):
                if bitmap & (1 << bit):
                    total += walk(inp.take(32).hex(), prefix + format(bit, 'x'))
            checked(total == count, 'inventory branch count')
        inp.end()
        return count
    walk(plan['inventory_root'], '')
    checked(len(entries) == int(plan['object_count']) and sum(entries.values()) == int(plan['payload_bytes']), 'inventory totals')
    return entries


def receive_pack(stream, plan, expected, entries, persist):
    """Verify and persist complete objects even if a later record/footer is lost."""
    digest = hashlib.sha256(b'forkfs.sync.pack.v1\0')
    def take(n, hashed=True):
        data = stream.read(n)
        checked(len(data) == n, 'truncated pack')
        if hashed:
            digest.update(data)
        return data
    checked(take(8) == b'FFSPK001', 'pack magic')
    checked(take(32).hex() == plan['plan_hash'], 'pack plan mismatch')
    count = int.from_bytes(take(4), 'little')
    checked(count == len(expected) and 0 < count <= 1024, 'pack record count')
    total = 0
    for wanted in expected:
        object_id = take(32).hex();size = int.from_bytes(take(4), 'little')
        total += size
        checked(object_id == wanted and size == entries[wanted] and size <= 262144 and total <= 16777216, 'pack object/length')
        data = take(size)
        checked(oid(data) == object_id, 'object hash mismatch')
        persist(object_id, data)
    checked(take(32, False) == digest.digest(), 'pack footer mismatch')
    checked(not stream.read(1), 'pack trailing bytes')


def verify_closure(plan, entries, load, max_dag_bytes=64*1024*1024):
    """Independent DAG, ordered-tree and inode checks; return native restore views."""
    validate_plan(plan)
    checked(type(max_dag_bytes) is int and 0 <= max_dag_bytes <= 64*1024*1024,
            'revision DAG byte limit')
    used, views, colors, discovered, manifest_cache = set(), {}, {}, set(), {}
    dag_bytes = 0
    def read(object_id):
        checked(object_id in entries, 'dependency missing from inventory')
        data = load(object_id)
        checked(len(data) == entries[object_id] and oid(data) == object_id, 'CAS hash/length mismatch')
        used.add(object_id)
        return data
    def view(manifest):
        data = read(manifest)
        checked(data[:8] == b'FFTREE01' and len(data) == 40, 'namespace codec')
        values, visited = {}, set()
        def tree(object_id, low=None, high=None, depth=0):
            checked(depth < 256 and object_id not in visited, 'tree cycle/depth')
            visited.add(object_id);data = read(object_id)
            checked(data[:8] == b'FFTRNO01' and len(data) >= 108, 'tree node codec')
            n = int.from_bytes(data[8:12], 'little');key = data[108:]
            checked(8 <= n <= 1024 and len(key) == n and key.startswith(NS), 'tree key')
            checked((low is None or low < key) and (high is None or key < high), 'tree ordering')
            values[key] = read(data[12:44].hex())
            priority = (oid(b'forkfs.metadata.priority.v1' + key), key)
            for child, lo, hi in ((data[44:76], low, key), (data[76:108], key, high)):
                if any(child):
                    child_priority = tree(child.hex(), lo, hi, depth + 1)
                    checked(priority > child_priority, 'tree priority')
            return priority
        checked(any(data[8:]), 'zero tree root')
        tree(data[8:].hex())
        entries_by_parent = index_directory_entries(values)
        inode_ids = {}
        expected_keys = {NS + b'root'}
        indexed = NS + b'parent-index' in values
        for key, data in values.items():
            if key.startswith(NS + b'inode/'):
                ident = key[len(NS + b'inode/'):]
                checked(bool(re.fullmatch(b'[0-9a-f]{32}', ident)), 'inode ID')
                checked(len(data) >= 48 and data[:8] in (b'FFINOD01', b'FFINOD02', b'FFINOD03', b'FFINOD04', b'FFINOD05'), 'inode codec')
                kind, mode, links, size = struct.unpack_from('<IIQQ', data, 8)
                checked(kind in (1, 2, 3) and mode <= 0o777 and links > 0, 'inode attributes')
                checked(kind != 2 or (links >= 2 and size == 0), 'directory inode')
                if data[:8] in (b'FFINOD01', b'FFINOD02'):
                    checked(len(data) == 48, 'inode length')
                else:
                    checked(len(data) >= 80 and (data[:8] != b'FFINOD03' or len(data) == 80), 'extended inode length')
                    if data[:8] in (b'FFINOD04', b'FFINOD05'):
                        checked(84 <= len(data) <= 128 * 1024, 'xattr inode length')
                    flags, = struct.unpack_from('<I', data, 48)
                    checked((flags == 2 and kind == 1 if data[:8] == b'FFINOD05' else flags <= 1 and (not flags or kind == 2)) and data[60:64] == bytes(4), 'inode flags')
                    accessed, created = struct.unpack_from('<QQ', data, 64)
                    checked(accessed <= 2**63 - 1 and created <= 2**63 - 1, 'extended inode timestamp')
                    if data[:8] in (b'FFINOD04', b'FFINOD05'):
                        inp = Input(data[80:]);n = inp.number(4);previous = b''
                        checked(n <= 128, 'xattr count')
                        for _ in range(n):
                            ns, vs = inp.number(4), inp.number(4)
                            checked(1 <= ns <= 255 and vs <= 65536, 'xattr lengths')
                            name = inp.take(ns);inp.take(vs)
                            checked(name > previous and all(33 <= c <= 126 for c in name), 'xattr name/order');previous = name
                        inp.end()
                inode_ids[ident] = (kind, mode, links, size)
                indexed_inode = data[:8] == b'FFINOD02'
                if data[:8] in (b'FFINOD03', b'FFINOD04', b'FFINOD05'):
                    indexed_inode = bool(struct.unpack_from('<I', data, 48)[0] & 1)
                checked(indexed_inode == (indexed and kind == 2), 'directory inode/index version mismatch')
        root = values[NS + b'root']
        paths, refs, reached = {}, Counter(), set()
        pending = [(root, '/')]
        while pending:
            ident, path = pending.pop()
            checked(ident in inode_ids, 'directory missing inode')
            kind, mode, links, size = inode_ids[ident]
            checked(kind == 2 and ident not in reached, 'directory hardlink')
            inode_key = NS + b'inode/' + ident
            allocation_key = NS + b'allocated/' + ident
            checked(allocation_key in values and values[allocation_key] == b'', 'inode missing allocation record')
            expected_keys.update((inode_key, allocation_key))
            reached.add(ident);paths[path] = dict(kind='dir', mode=mode, inode=ident.decode())
            prefix = NS + b'entry/' + ident + b'/'
            children = entries_by_parent.get(ident, ())
            subdirs = 0
            for name, child in children:
                checked(name not in (b'', b'.', b'..') and len(name) <= 255 and b'/' not in name
                        and b'\0' not in name, 'unsafe entry name')
                checked(child in inode_ids and child != root, 'entry target')
                expected_keys.add(prefix + name)
                refs[child] += 1;typ, md, _, length = inode_ids[child]
                child_inode_key = NS + b'inode/' + child
                child_allocation_key = NS + b'allocated/' + child
                checked(child_allocation_key in values and values[child_allocation_key] == b'', 'inode missing allocation record')
                expected_keys.update((child_inode_key, child_allocation_key))
                # fsdecode uses surrogateescape, so every stored byte survives
                # path construction and can be passed back through fsencode.
                target = path.rstrip('/') + '/' + os.fsdecode(name)
                if typ == 2:
                    subdirs += 1
                    pending.append((child, target))
                else:
                    reached.add(child)
                    content_key = NS + b'content/' + child
                    checked(content_key in values, 'inode content missing')
                    payload = values[content_key]
                    if values[child_inode_key][:8] == b'FFINOD05':
                        checked(len(payload) == 48 and payload[:8] == b'FFFILE01'
                                and int.from_bytes(payload[8:16], 'little') == length
                                and length <= 8 * 1024 * 1024, 'mock extent file descriptor/restore budget')
                        output = bytearray(length)
                        def extent(object_id, level, first):
                            node = read(object_id)
                            checked(len(node) >= 11 and node[:8] == b'FFEXT001' and node[8] == level, 'extent node codec')
                            count = int.from_bytes(node[9:11], 'little')
                            checked(1 <= count <= 256 and len(node) == 11 + 33 * count, 'extent node length')
                            previous = -1
                            for i in range(count):
                                at = 11 + 33 * i; slot = node[at]; start = first + (slot << (8 * level))
                                checked(slot > previous and start * 65536 < length, 'extent order/EOF'); previous = slot
                                child_id = node[at+1:at+33].hex()
                                if level:
                                    extent(child_id, level-1, start)
                                else:
                                    block = read(child_id); n = min(65536, length-start*65536)
                                    checked(len(block) == 65536 and any(block) and not any(block[n:]), 'extent data/tail')
                                    output[start*65536:start*65536+n] = block[:n]
                        if any(payload[16:]):
                            checked(length > 0, 'nonempty zero-size extent tree');extent(payload[16:].hex(), 5, 0)
                        payload = bytes(output)
                    checked(len(payload) == length, 'inode content size')
                    if typ == 3:
                        validate_symlink_target(payload)
                    expected_keys.add(content_key)
                    paths[target] = dict(kind='symlink' if typ == 3 else 'file', mode=md, inode=child.decode(), data=payload)
                if typ == 2 and indexed:
                    parent_key = NS + b'parent/' + child
                    checked(parent_key in values and values[parent_key] == b'FFPAR001' + ident + name, 'parent backlink')
            checked(links == 2 + subdirs, 'directory link count')
        checked(set(inode_ids) == reached, 'unreachable live inode')
        for ident, (kind, _, links, _) in inode_ids.items():
            if kind != 2:
                checked(links == refs[ident], 'file link count')
            elif ident != root:
                checked(refs[ident] == 1, 'directory parent count')
        for key in values:
            if key.startswith(NS + b'allocated/'):
                ident = key[len(NS + b'allocated/'):]
                checked(bool(re.fullmatch(b'[0-9a-f]{32}', ident)) and values[key] == b'', 'invalid allocation record')
                expected_keys.add(key)
        if indexed:
            checked(values[NS + b'parent-index'] == b'FFPIDX01' and values.get(NS + b'parent/' + root) == b'FFPAR001' + root, 'root parent index')
            expected_keys.add(NS + b'parent-index')
            for ident, (kind, _, _, _) in inode_ids.items():
                if kind == 2:
                    expected_keys.add(NS + b'parent/' + ident)
        checked(set(values) == expected_keys, 'orphan namespace records')
        return paths
    for root in plan['selected_revision_ids']:
        pending = [(root, False, None)]
        while pending:
            object_id, exiting, manifest_id = pending.pop()
            if exiting:
                checked(colors.get(object_id) == 1, 'revision traversal state')
                views[object_id] = manifest_cache[manifest_id]
                colors[object_id] = 2
                continue
            if colors.get(object_id) == 2:
                continue
            checked(object_id not in colors, 'revision cycle')
            if object_id not in discovered:
                checked(len(discovered) < 100000, 'mock DAG budget')
                discovered.add(object_id)
            colors[object_id] = 1
            data = read(object_id)
            if data[:8] == b'FFREV001':
                checked(len(data) == 80, 'V1 revision length')
                parents = [] if not any(data[40:72]) else [data[40:72].hex()]
            else:
                checked(data[:8] == b'FFREV002' and len(data) >= 52, 'revision codec')
                count, = struct.unpack_from('<I', data, 48)
                checked(count <= 64 and len(data) == 52 + count * 32, 'revision length')
                parents = [data[52+i*32:84+i*32].hex() for i in range(count)]
            # RevisionDag::validate charges sizeof(Frame) + 32 bytes per
            # parent + 64 bytes. On the supported 64-bit ABI Frame is 104
            # bytes (ID 32, RevisionRecord 64, cursor 8).
            charge = 168 + 32 * len(parents)
            checked(charge <= max_dag_bytes - dag_bytes, 'revision DAG byte budget')
            dag_bytes += charge
            checked(len(parents) == len(set(parents)) and '0'*64 not in parents and object_id not in parents,
                    'revision parent IDs')
            manifest_id = data[8:40].hex()
            if manifest_id not in manifest_cache:
                manifest_cache[manifest_id] = view(manifest_id)
            pending.append((object_id, True, manifest_id))
            for parent in reversed(parents):
                pending.append((parent, False, None))
    checked(used == set(entries), 'inventory not exact revision closure')
    return views


def restore_views(views, root):
    """Restore into private native directories, without following stored symlinks."""
    root = Path(root);root.parent.mkdir(parents=True, exist_ok=True);root.mkdir()
    dir_flags = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)

    def components(path):
        return [] if path == '/' else [os.fsencode(part) for part in path.lstrip('/').split('/')]

    def open_parent(base_fd, parts):
        fd = os.dup(base_fd)
        try:
            for component in parts[:-1]:
                child = os.open(component, dir_flags, dir_fd=fd)
                os.close(fd);fd = child
            return fd, parts[-1]
        except BaseException:
            os.close(fd)
            raise

    root_fd = os.open(root, dir_flags)
    try:
        for revision_id, paths in views.items():
            os.mkdir(revision_id, 0o700, dir_fd=root_fd)
            destination_fd = os.open(revision_id, dir_flags, dir_fd=root_fd)
            try:
                links = {}
                for path, info in sorted(paths.items(), key=lambda p: (p[0].count('/'), p[0])):
                    if path == '/':
                        continue
                    parts = components(path);parent_fd, name = open_parent(destination_fd, parts)
                    try:
                        if info['kind'] == 'dir':
                            os.mkdir(name, 0o700, dir_fd=parent_fd)
                        elif info['kind'] == 'symlink' and info['inode'] not in links:
                            os.symlink(info['data'], name, dir_fd=parent_fd)
                            links[info['inode']] = parts
                        elif info['inode'] in links:
                            source_parent, source_name = open_parent(destination_fd, links[info['inode']])
                            try:
                                os.link(source_name, name, src_dir_fd=source_parent,
                                        dst_dir_fd=parent_fd, follow_symlinks=False)
                            finally:
                                os.close(source_parent)
                        else:
                            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0),
                                         0o600, dir_fd=parent_fd)
                            try:
                                data = memoryview(info['data'])
                                while data:
                                    written = os.write(fd, data)
                                    checked(written > 0, 'short native restore write')
                                    data = data[written:]
                                os.fchmod(fd, info['mode'])
                            finally:
                                os.close(fd)
                            links[info['inode']] = parts
                    finally:
                        os.close(parent_fd)
                # Apply descendants first and the revision root last. A root
                # mode such as 000 must not prevent opening child directories.
                for path, info in sorted(paths.items(), key=lambda p: len(components(p[0])), reverse=True):
                    if info['kind'] != 'dir':
                        continue
                    parts = components(path)
                    if not parts:
                        os.fchmod(destination_fd, info['mode'])
                        continue
                    parent_fd, name = open_parent(destination_fd, parts)
                    try:
                        fd = os.open(name, dir_flags, dir_fd=parent_fd)
                        try:
                            os.fchmod(fd, info['mode'])
                        finally:
                            os.close(fd)
                    finally:
                        os.close(parent_fd)
            finally:
                os.close(destination_fd)
    finally:
        os.close(root_fd)
    return root
