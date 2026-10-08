#!/usr/bin/env python3
"""Disposable APFS image fork probe. Never operates on a physical volume.

Requires macOS and hdiutil. Emits JSON with phase timings. A failed detach leaves
its private directory in place and prints its path so a mounted image is never deleted.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def command(argv, timings):
    start = time.perf_counter_ns()
    result = subprocess.run(argv, capture_output=True, text=True, timeout=90)
    timings.append({"operation": argv[0] + " " + argv[1], "ms": round((time.perf_counter_ns() - start) / 1e6, 3)})
    if result.returncode:
        raise RuntimeError(f"{argv[0]} {argv[1]} failed ({result.returncode}): {result.stdout} {result.stderr}")


def main():
    if sys.platform != "darwin":
        raise RuntimeError("this probe requires macOS")
    private = Path(tempfile.mkdtemp(prefix="forkfs-apfs-probe-", dir="/private/tmp"))
    mount = private / "mount"
    mount.mkdir()
    base = private / "base.sparseimage"
    fork = private / "fork.sparseimage"
    timings = []
    mounted = False
    try:
        command(["hdiutil", "create", "-size", "128m", "-type", "SPARSE", "-fs", "APFS", "-volname", "forkfs-probe", str(base)], timings)
        command(["hdiutil", "attach", "-nobrowse", "-owners", "on", "-mountpoint", str(mount), str(base)], timings)
        mounted = True
        if mount.stat().st_dev == private.stat().st_dev:
            raise RuntimeError("image did not mount on a distinct device")
        (mount / "sentinel").write_text("base", encoding="utf-8")
        with (mount / "sentinel").open("rb") as handle:
            os.fsync(handle.fileno())
        command(["hdiutil", "detach", str(mount)], timings)
        mounted = False
        command(["cp", "-c", str(base), str(fork)], timings)
        command(["hdiutil", "attach", "-nobrowse", "-owners", "on", "-mountpoint", str(mount), str(fork)], timings)
        mounted = True
        if mount.stat().st_dev == private.stat().st_dev:
            raise RuntimeError("fork image did not mount on a distinct device")
        if (mount / "sentinel").read_text(encoding="utf-8") != "base":
            raise RuntimeError("fork image lost initial content")
        (mount / "sentinel").write_text("fork", encoding="utf-8")
        command(["hdiutil", "detach", str(mount)], timings)
        mounted = False
        command(["hdiutil", "attach", "-nobrowse", "-owners", "on", "-mountpoint", str(mount), str(base)], timings)
        mounted = True
        if mount.stat().st_dev == private.stat().st_dev:
            raise RuntimeError("base image did not mount on a distinct device")
        if (mount / "sentinel").read_text(encoding="utf-8") != "base":
            raise RuntimeError("base image was changed through the fork")
        command(["hdiutil", "detach", str(mount)], timings)
        mounted = False
        print(json.dumps({"result": "pass", "timings": timings, "image_allocated_bytes": [base.stat().st_blocks * 512, fork.stat().st_blocks * 512]}))
    finally:
        if mounted or mount.stat().st_dev != private.stat().st_dev:
            print(f"probe image may still be mounted; inspect and detach before cleanup: {private}", file=sys.stderr)
        else:
            shutil.rmtree(private)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"probe failed: {exc}", file=sys.stderr)
        sys.exit(1)
