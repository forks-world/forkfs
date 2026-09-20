#!/usr/bin/env python3
"""Run the bounded APFS disk-full regression on a private disk image.

The C++ test deliberately fills the image itself.  Keeping image creation and teardown here
means a test failure can never turn an ordinary runner volume into the fill target.
"""

import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


MIN_HOST_FREE = 2 * 1024 * 1024 * 1024
MIN_IMAGE_BYTES = 300 * 1024 * 1024
MAX_IMAGE_BYTES = 1024 * 1024 * 1024
TEST_TIMEOUT = 600


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def run(command, **kwargs):
    print("+", " ".join(str(x) for x in command), flush=True)
    kwargs.setdefault("timeout", 120)
    return subprocess.run(command, check=False, **kwargs)


def diskutil_is_apfs(mount: Path) -> bool:
    result = subprocess.run(
        ["diskutil", "info", "-plist", str(mount)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    if result.returncode:
        return False
    try:
        info = plistlib.loads(result.stdout)
    except Exception:
        return False
    filesystem = str(info.get("FilesystemType", "")).lower()
    personality = str(info.get("FilesystemPersonality", "")).lower()
    return filesystem == "apfs" or personality == "apfs"


def main() -> int:
    if sys.platform != "darwin":
        print("disk-full regression requires macOS", file=sys.stderr)
        return 77
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PATH/TO/build-dir", file=sys.stderr)
        return 2

    binary_arg = Path(sys.argv[1]).resolve()
    binary = binary_arg / "core" / "disk_full_test" if binary_arg.is_dir() else binary_arg
    if binary.name != "disk_full_test" or not binary.is_file() or not os.access(binary, os.X_OK):
        print("refusing a non-disk_full_test executable", file=sys.stderr)
        return 2
    for tool in ("hdiutil", "diskutil"):
        if shutil.which(tool) is None:
            print(f"missing required macOS tool: {tool}", file=sys.stderr)
            return 2

    private = Path(tempfile.mkdtemp(prefix="forkfs-disk-full-"))
    image = private / "test.dmg"
    mount = private / "mount"
    attach_attempted = False
    detached = False

    def mount_state():
        """True/False when known, None if inspecting the mount itself failed."""
        try:
            mount_lstat = mount.lstat()
            if not stat.S_ISDIR(mount_lstat.st_mode) or mount.is_symlink():
                return None
            parent_stat = mount.parent.stat()
            # A freshly-created mountpoint has the parent's device.  A mounted APFS image has
            # its own device.  lstat/stat errors intentionally remain unknown: never remove a
            # private directory if we cannot establish that it is no longer a mountpoint.
            return mount_lstat.st_dev != parent_stat.st_dev
        except OSError:
            return None

    result_code = 0
    try:
        # Keep every post-mkdtemp operation inside the guarded cleanup path.  A failed mkdir must
        # not strand the private directory on the host filesystem.
        mount.mkdir()
        usage = shutil.disk_usage(private)
        if usage.free < MIN_HOST_FREE:
            fail(f"host filesystem has only {usage.free} free bytes; need {MIN_HOST_FREE}")

        created = run(
            [
                "hdiutil",
                "create",
                "-size",
                "512m",
                "-fs",
                "APFS",
                "-volname",
                "forkfs-test",
                str(image),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        print(created.stdout, end="")
        if created.returncode:
            result_code = created.returncode
        else:
            attach_attempted = True
            attached_result = run(
                [
                    "hdiutil",
                    "attach",
                    "-nobrowse",
                    "-owners",
                    "on",
                    "-mountpoint",
                    str(mount),
                    str(image),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            print(attached_result.stdout, end="")
            mounted = mount_state()
            if attached_result.returncode:
                if mounted is True:
                    print("hdiutil attach returned failure but left a mounted volume", file=sys.stderr)
                elif mounted is None:
                    fail("could not determine whether a failed hdiutil attach left a mount")
                result_code = attached_result.returncode
            else:
                if mounted is not True:
                    fail("hdiutil attach succeeded but mountpoint is not mounted")
                if not mount.is_dir() or not diskutil_is_apfs(mount):
                    fail("attached test volume is not APFS")
                mount_dev = mount.stat().st_dev
                host_dev = private.parent.stat().st_dev
                if mount_dev == host_dev:
                    fail("test mount is on the host device, refusing to fill it")
                statvfs = os.statvfs(mount)
                capacity = statvfs.f_blocks * statvfs.f_frsize
                if not MIN_IMAGE_BYTES <= capacity <= MAX_IMAGE_BYTES:
                    fail(f"test image capacity is unreasonable: {capacity} bytes")
                print(f"private APFS image: capacity={capacity} st_dev={mount_dev}", flush=True)

                try:
                    test_result = subprocess.run(
                        [str(binary), str(mount)],
                        check=False,
                        timeout=TEST_TIMEOUT,
                    )
                    result_code = test_result.returncode
                except subprocess.TimeoutExpired:
                    print(f"disk-full test exceeded {TEST_TIMEOUT}s", file=sys.stderr)
                    result_code = 124
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"disk-full wrapper: {exc}", file=sys.stderr)
        result_code = 1
    finally:
        mounted = mount_state() if attach_attempted else False
        if mounted is None:
            print(
                f"disk-full wrapper: cannot determine mount state; preserving private image at {private}",
                file=sys.stderr,
            )
            result_code = 1
        elif mounted:
            try:
                detached_result = run(
                    ["hdiutil", "detach", str(mount)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                print(
                    f"disk-full wrapper: detach failed; preserving private image at {private}: {exc}",
                    file=sys.stderr,
                )
                result_code = 1
            else:
                print(detached_result.stdout, end="")
                detached = detached_result.returncode == 0 and mount_state() is False
                if not detached:
                    # Never recursively remove a path that may still be a mounted volume.  Keep
                    # the private directory so a CI failure can be inspected and report its path.
                    print(
                        f"disk-full wrapper: detach failed; preserving private image at {private}",
                        file=sys.stderr,
                    )
                    result_code = 1
        if (not attach_attempted) or detached or mount_state() is False:
            try:
                shutil.rmtree(private)
            except OSError as exc:
                print(f"disk-full wrapper: cleanup failed for {private}: {exc}", file=sys.stderr)
                result_code = 1
    return result_code


if __name__ == "__main__":
    raise SystemExit(main())
