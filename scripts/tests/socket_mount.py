#!/usr/bin/env python3
"""Check AF_UNIX pathname behavior on APFS and an optional already mounted ForkFS volume."""
import argparse
from contextlib import ExitStack
import errno
import json
import os
from pathlib import Path
import socket
import stat
import tempfile
from filesystem_compat import check, filesystem


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        block = sock.recv(count - len(data))
        check(bool(block), "connected socket closed before full transfer")
        data.extend(block)
    return bytes(data)


def exercise(root, label):
    path = root / "endpoint.sock"
    encoded = os.fsencode(path)
    check(len(encoded) < 104, f"{label}: Unix socket path exceeds Darwin sun_path capacity: {path}")
    result = {"filesystem": label, "path": str(path)}
    with ExitStack() as stack:
        first = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        first.settimeout(3)
        first.bind(str(path))
        first.listen(2)
        original_inode = path.lstat().st_ino
        check(stat.S_ISSOCK(path.lstat().st_mode), f"{label}: bind did not publish a socket pathname")

        collision = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        collision.settimeout(3)
        try:
            collision.bind(str(path))
        except OSError as exc:
            check(exc.errno == errno.EADDRINUSE, f"{label}: occupied bind returned {exc.errno}, expected EADDRINUSE")
        else:
            raise AssertionError(f"{label}: binding an occupied socket pathname succeeded")

        client = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        client.settimeout(3)
        client.connect(str(path))
        accepted, _ = first.accept()
        accepted.settimeout(3)
        stack.callback(accepted.close)
        client.sendall(b"before-unlink")
        check(exact(accepted, len(b"before-unlink")) == b"before-unlink", f"{label}: initial transfer failed")

        path.unlink()
        second = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        second.settimeout(3)
        second.bind(str(path))
        second.listen(1)
        rebound_inode = path.lstat().st_ino
        check(stat.S_ISSOCK(path.lstat().st_mode) and rebound_inode != original_inode,
              f"{label}: rebind did not create a distinct socket pathname inode")
        rebound = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
        rebound.settimeout(3)
        rebound.connect(str(path))
        accepted_new, _ = second.accept()
        accepted_new.settimeout(3)
        stack.callback(accepted_new.close)
        rebound.sendall(b"after-rebind")
        check(exact(accepted_new, len(b"after-rebind")) == b"after-rebind", f"{label}: rebound transfer failed")

        client.sendall(b"still-connected")
        check(exact(accepted, len(b"still-connected")) == b"still-connected",
              f"{label}: unlink interrupted the established connection")
        for flags in (os.O_RDONLY | os.O_NONBLOCK, os.O_WRONLY | os.O_NONBLOCK):
            try:
                fd = os.open(path, flags)
            except OSError as exc:
                check(exc.errno == errno.EOPNOTSUPP,
                      f"{label}: os.open flags {flags} returned {exc.errno}, expected EOPNOTSUPP")
            else:
                os.close(fd)
                raise AssertionError(f"{label}: os.open unexpectedly opened a socket pathname with flags {flags}")
    result["original_inode"] = original_inode
    result["rebound_inode"] = rebound_inode
    return result


def run_on_mount(mountpoint, expected, label):
    mountpoint = mountpoint.resolve(strict=True)
    mounted, kind = filesystem(mountpoint)
    if expected == "forkfs":
        check(mounted == mountpoint, f"{label}: expected the exact candidate mount root, got {mounted}")
    check(kind == expected, f"{label}: expected {expected}, found {kind}")
    with tempfile.TemporaryDirectory(prefix="ff-sock-", dir=mountpoint) as directory:
        return exercise(Path(directory), label)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mountpoint", type=Path, nargs="?", help="already mounted ForkFS candidate")
    parser.add_argument("--reference-apfs", action="store_true", help="run only the native APFS reference")
    args = parser.parse_args()
    check(args.reference_apfs or args.mountpoint is not None, "supply a candidate mountpoint or --reference-apfs")
    results = [run_on_mount(Path("/private/tmp"), "apfs", "reference-apfs")]
    if args.mountpoint is not None and not args.reference_apfs:
        results.append(run_on_mount(args.mountpoint, "forkfs", "candidate"))
    print(json.dumps({"claims": {"reference_apfs_tested": True,
                                  "candidate_tested": len(results) == 2,
                                  "bind_routing_into_fskit_createitem_callback_verified": False},
                      "results": results}, sort_keys=True))


if __name__ == "__main__":
    main()
