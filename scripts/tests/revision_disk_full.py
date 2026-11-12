#!/usr/bin/env python3
"""Qualify ENOSPC and reopen recovery inside an owned 256 MiB APFS image.

macOS manual acceptance only. Never fills the host filesystem or modifies an
existing volume. Removes the owned image after verifying and detaching it.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import stat
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=120)
    check(result.returncode == 0, result.stderr.decode(errors='replace'))
    return result.stdout


def exact(stream, length):
    result = b''
    while len(result) < length:
        part = stream.recv(length - len(result))
        check(bool(part), 'truncated RPC response')
        result += part
    return result


def rpc(endpoint, *fields):
    fields = [field if isinstance(field, bytes) else str(field).encode() for field in fields]
    payload = struct.pack('<I', len(fields)) + b''.join(struct.pack('<I', len(field)) + field for field in fields)
    with socket.socket(socket.AF_UNIX) as stream:
        stream.settimeout(60)
        stream.connect(str(endpoint))
        stream.sendall(b'FFRPC001' + struct.pack('<I', len(payload)) + payload)
        header = exact(stream, 12)
        check(header[:8] == b'FFRSP001', 'invalid RPC response')
        response = exact(stream, struct.unpack('<I', header[8:])[0])
        return struct.unpack('<I', response[:4])[0], response[4:]


def detach_owned_image(image, mount):
    """Discover even a partially failed attach; preserve scratch on any doubt."""
    def devices():
        info = plistlib.loads(run('/usr/bin/hdiutil', 'info', '-plist'))
        check(isinstance(info.get('images'), list), 'unknown image attachment state')
        found = []
        for item in info['images']:
            check(isinstance(item, dict) and isinstance(item.get('image-path'), str),
                  'unknown image attachment state')
            if Path(item['image-path']).resolve() != image.resolve():
                continue
            entities = item.get('system-entities')
            check(isinstance(entities, list) and bool(entities), 'unknown owned image devices')
            for entity in entities:
                device = entity.get('dev-entry') if isinstance(entity, dict) else None
                check(isinstance(device, str) and re.fullmatch(r'/dev/disk[0-9]+(?:s[0-9]+)*', device),
                      'invalid owned image device')
                if device not in found:
                    found.append(device)
        return found

    # Re-discover after every detach: whole-disk detach can remove child devices.
    for attempt in range(8):
        owned = devices()
        if not owned:
            info = mount.lstat()
            check(stat.S_ISDIR(info.st_mode) and info.st_dev == mount.parent.stat().st_dev,
                  'mount state unknown; preserving scratch')
            return
        run('/usr/bin/hdiutil', 'detach', owned[0])
    raise RuntimeError('image still attached; preserving scratch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', type=Path, required=True)
    parser.add_argument('--run', action='store_true', help='explicitly run the bounded disk-image acceptance')
    parser.add_argument('--expect-generic-error', action='store_true', help='qualify the older daemon failure baseline')
    args = parser.parse_args()
    check(args.run and sys.platform == 'darwin', 'requires macOS and explicit --run')
    binary = args.daemon.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix='forkfs-disk-full-', dir='/private/tmp'))
    check(shutil.disk_usage(root).free > 1024 * 1024 * 1024, 'need at least 1 GiB free on host')
    image, mount, endpoint = root / 'test.sparseimage', root / 'volume', root / 'control.sock'
    mount.mkdir(mode=0o700)
    process = None
    report = {'image_maximum_bytes': 256 * 1024 * 1024}
    try:
        run('/usr/bin/hdiutil', 'create', '-size', '256m', '-type', 'SPARSE', '-fs', 'APFS',
            '-volname', 'ForkFSDiskFull', '-quiet', image)
        attached = plistlib.loads(run('/usr/bin/hdiutil', 'attach', '-nobrowse', '-mountpoint', mount, '-plist', image))
        entities = attached['system-entities']
        check(any(entity.get('mount-point') == str(mount) for entity in entities), 'image did not mount at owned path')
        check(mount.stat().st_dev != root.stat().st_dev, 'refusing host filesystem')
        space = os.statvfs(mount)
        check(space.f_blocks * space.f_frsize <= 512 * 1024 * 1024, 'unexpected image capacity')
        repo = mount / 'repo'
        seed = root / 'input'
        original = b'BASE' * 1024
        seed.write_bytes(original)
        run(binary, 'init', repo)
        run(binary, 'fs-init', repo)
        run(binary, 'fs-write', repo, '/file', seed)
        run(binary, 'fs-link', repo, '/file', '/alias')
        run(binary, 'fs-snapshot', repo, 'base')
        with (mount / 'reserve').open('wb', buffering=0) as stream:
            for _ in range(8):
                check(stream.write(os.urandom(1024 * 1024)) == 1024 * 1024, 'reserve short write')
            os.fsync(stream.fileno())
        log = (root / 'daemon.log').open('wb')
        try:
            process = subprocess.Popen([str(binary), 'serve', str(repo), '--socket', str(endpoint), '--rpc'],
                                       stdout=subprocess.PIPE, stderr=log)
            check(bool(select.select([process.stdout], [], [], 20)[0]), 'daemon startup timed out')
            check(json.loads(process.stdout.readline())['state'] == 'leveldb', 'daemon did not start')
            code, response = rpc(endpoint, 'session-open', 300000)
            check(code == 0, response.decode(errors='replace'))
            session = json.loads(response)['session']
            selection = ('--session', session)
            code, response = rpc(endpoint, 'inode-root', *selection)
            check(code == 0, response.decode(errors='replace'))
            code, response = rpc(endpoint, 'fs-open', '/file', 'rw', *selection)
            check(code == 0, response.decode(errors='replace'))
            handle = json.loads(response)['handle']
            filled = 0
            with (mount / 'filler').open('wb', buffering=0) as stream:
                # The fixed budget remains bounded even if mount identity were
                # unexpectedly changed; the host is never filled in a loop.
                for block in [1024 * 1024, 65536, 4096]:
                    payload = os.urandom(block)
                    while filled < 320 * 1024 * 1024:
                        try:
                            written = stream.write(payload)
                            check(written > 0, 'zero filler write')
                            filled += written
                        except OSError as error:
                            check(error.errno == errno.ENOSPC, 'filler failed with unexpected errno')
                            break
                try:
                    os.fsync(stream.fileno())
                except OSError as error:
                    check(error.errno == errno.ENOSPC, 'filler fsync failed with unexpected errno')
            check(filled < 320 * 1024 * 1024, 'did not reach bounded image ENOSPC')
            last = original
            for attempt in range(64):
                attempted = os.urandom(256 * 1024)
                code, response = rpc(endpoint, 'fs-hwrite', handle, 0, attempted, *selection)
                if code:
                    break
                last = attempted
            else:
                raise RuntimeError('could not provoke storage ENOSPC in the full image')
            expected_code = 1 if args.expect_generic_error else errno.ENOSPC + 1
            check(code == expected_code and b'No space left on device' in response,
                  'wrong full-image error: %s %r' % (code, response))
            report.update(rpc_error_code=code, filler_bytes=filled, successful_writes_before_failure=attempt)
            status_code, status = rpc(endpoint, 'status')
            check(status_code == 0, 'poisoned status unavailable')
            requires_reopen = json.loads(status).get('requires_reopen')
            if not args.expect_generic_error:
                check(requires_reopen is True, 'store did not report reopen requirement')
            heartbeat, _ = rpc(endpoint, 'session-keepalive', session)
            check(heartbeat == (0 if args.expect_generic_error else errno.EIO + 1), 'poisoned keepalive did not fail closed')
            report.update(requires_reopen=requires_reopen, poisoned_keepalive_code=heartbeat)
            sync_code, _ = rpc(endpoint, 'inode-sync', *selection)
            check(sync_code == (0 if args.expect_generic_error else errno.EIO + 1), 'poisoned sync did not fail closed')
            report['poisoned_sync_code'] = sync_code
            (mount / 'reserve').unlink()
            (mount / 'filler').unlink()
            retry, message = rpc(endpoint, 'fs-hwrite', handle, 0, b'retry', *selection)
            check(retry != 0 and b'requires reopen' in message, 'poisoned store silently resumed writes')
            process.terminate()
            process.wait(timeout=15)
            process = None
        finally:
            log.close()
        recovered = run(binary, 'fs-cat', repo, '/file')
        check(recovered in [last, attempted], 'recovery mixed or lost a committed publication')
        check(run(binary, 'fs-cat', repo, '/alias') == recovered, 'hardlink recovery differs')
        check(run(binary, 'fs-cat', repo, '/file', '--revision', 'base') == original, 'retained revision changed')
        run(binary, 'fs-check', repo)
        run(binary, 'check', repo)
        seed.write_bytes(b'recovered')
        run(binary, 'fs-write', repo, '/file', seed)
        check(run(binary, 'fs-cat', repo, '/file') == b'recovered', 'write after reopen failed')
        report.update(atomic_recovery=True, retained_revision_unchanged=True, write_after_reopen=True,
                      recovered_sha256=hashlib.sha256(recovered).hexdigest())
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        # This also discovers an attachment when hdiutil attach timed out before
        # returning a device. Failed discovery/detach leaves the image intact.
        detach_owned_image(image, mount)
        shutil.rmtree(root)
    report['owned_image_detached_and_removed'] = True
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
