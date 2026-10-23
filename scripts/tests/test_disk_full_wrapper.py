"""Failure-path tests for image cleanup; no disk images or child processes are run."""

import contextlib
import io
import os
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import disk_full


class ImageCleanupTests(unittest.TestCase):
    def exercise(self, *, attach_timeout=False, detach_ok=False, unreadable=False,
                malformed_discovery=False, unmounted=False, absent=False,
                detach_timeout=False, persistent=False, busy_attempts=0,
                detach_error_clears=False, discovery_lost_after_detach=False,
                replacement_device=False):
        with tempfile.TemporaryDirectory(prefix="forkfs-wrapper-unit-") as root:
            root = Path(root)
            private = root / "image"
            private.mkdir()
            mount = private / "mount"
            binary = root / "disk_full_test"
            binary.touch()
            mounted = False
            info_attached = False
            detach_count = 0
            calls = []
            original_lstat = Path.lstat

            def lstat(path):
                result = original_lstat(path)
                if path == mount and unreadable:
                    raise PermissionError("cannot inspect test mount")
                if path == mount and mounted:
                    fields = list(result)
                    fields[2] += 1  # st_dev: emulate a mounted volume, not the host directory.
                    return os.stat_result(fields)
                return result

            def run(command, **kwargs):
                nonlocal mounted, info_attached, detach_count
                calls.append(command[1])
                if command[1] == "attach":
                    # An attach can mount successfully and still fail or time out afterward.
                    mounted = not unmounted and not absent
                    info_attached = not absent
                    if attach_timeout:
                        raise subprocess.TimeoutExpired(command, 120)
                    return subprocess.CompletedProcess(command, 7, stdout="attach failed\n")
                if command[1] == "info":
                    if malformed_discovery or (discovery_lost_after_detach and detach_count):
                        return subprocess.CompletedProcess(command, 0, stdout=b"not a plist")
                    images = [] if not info_attached else [{
                        "image-path": str(private / "test.dmg"),
                        "system-entities": [{"dev-entry": "/dev/disk43s1" if replacement_device and detach_count else "/dev/disk42s1", "whole-disk": False}],
                    }]
                    return subprocess.CompletedProcess(
                        command, 0, stdout=plistlib.dumps({"images": images})
                    )
                if command[1] == "detach":
                    self.assertEqual(command[2], "/dev/disk43s1" if replacement_device and detach_count else "/dev/disk42s1")
                    detach_count += 1
                    if detach_timeout:
                        raise subprocess.TimeoutExpired(command, 120)
                    if (detach_ok and not persistent and detach_count > busy_attempts) or detach_error_clears:
                        mounted = False
                        info_attached = False
                    return subprocess.CompletedProcess(command, 0 if detach_ok and detach_count > busy_attempts else 8, stdout="")
                return subprocess.CompletedProcess(command, 0, stdout="")

            with contextlib.ExitStack() as stack:
                stack.enter_context(mock.patch.object(sys, "platform", "darwin"))
                stack.enter_context(mock.patch.object(sys, "argv", ["disk_full.py", str(binary)]))
                stack.enter_context(mock.patch.object(disk_full.tempfile, "mkdtemp", return_value=str(private)))
                stack.enter_context(mock.patch.object(disk_full.shutil, "which", return_value="/usr/bin/tool"))
                stack.enter_context(mock.patch.object(disk_full.os, "access", return_value=True))
                stack.enter_context(mock.patch.object(disk_full.shutil, "disk_usage",
                                                       return_value=mock.Mock(free=3 * 1024**3)))
                stack.enter_context(mock.patch.object(Path, "lstat", lstat))
                stack.enter_context(mock.patch.object(disk_full, "run", side_effect=run))
                stack.enter_context(mock.patch.object(disk_full.time, "sleep"))
                cleanup = stack.enter_context(mock.patch.object(disk_full.shutil, "rmtree"))
                child = stack.enter_context(mock.patch.object(disk_full.subprocess, "run"))
                output = io.StringIO()
                stack.enter_context(contextlib.redirect_stdout(output))
                stack.enter_context(contextlib.redirect_stderr(output))
                rc = disk_full.main()
                child.assert_not_called()
                if (detach_ok or absent or detach_error_clears) and not any((unreadable, malformed_discovery,
                                                       detach_timeout, persistent, discovery_lost_after_detach)):
                    self.assertLessEqual(detach_count, disk_full.MAX_DETACH_ATTEMPTS)
                    cleanup.assert_called_once_with(private)
                else:
                    cleanup.assert_not_called()
                    self.assertIn("preserving private image", output.getvalue())
                return rc, calls

    def test_partial_attach_and_failed_detach_preserve_image(self):
        rc, calls = self.exercise()
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info"] +
                         ["detach", "info"] * disk_full.MAX_DETACH_ATTEMPTS)

    def test_transient_busy_detach_retries_and_preserves_test_status(self):
        rc, calls = self.exercise(detach_ok=True, busy_attempts=2)
        self.assertEqual(rc, 7)  # successful cleanup must not hide the original failure
        self.assertEqual(calls, ['create', 'attach', 'info'] + ['detach', 'info'] * 3)

    def test_failed_detach_that_removed_attachment_does_not_retry_stale_device(self):
        rc, calls = self.exercise(detach_error_clears=True)
        self.assertEqual(rc, 7)
        self.assertEqual(calls, ['create', 'attach', 'info', 'detach', 'info'])

    def test_unknown_discovery_after_busy_detach_preserves_image(self):
        rc, calls = self.exercise(detach_ok=True, busy_attempts=1, discovery_lost_after_detach=True)
        self.assertEqual(rc, 1)
        self.assertEqual(calls, ['create', 'attach', 'info', 'detach', 'info'])

    def test_retry_uses_rediscovered_device_not_old_name(self):
        rc, calls = self.exercise(detach_ok=True, busy_attempts=1, replacement_device=True)
        self.assertEqual(rc, 7)
        self.assertEqual(calls.count('detach'), 2)

    def test_attach_timeout_still_detaches_before_cleanup(self):
        rc, calls = self.exercise(attach_timeout=True, detach_ok=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info", "detach", "info"])

    def test_unknown_mount_state_never_removes_image(self):
        rc, calls = self.exercise(unreadable=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info"] +
                         ["detach", "info"] * disk_full.MAX_DETACH_ATTEMPTS)

    def test_unknown_mount_state_preserves_after_successful_detach(self):
        rc, calls = self.exercise(unreadable=True, detach_ok=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info", "detach", "info"])

    def test_cleanup_does_not_hide_attach_failure(self):
        rc, calls = self.exercise(detach_ok=True)
        self.assertEqual(rc, 7)
        self.assertEqual(calls, ["create", "attach", "info", "detach", "info"])

    def test_unmounted_attachment_is_detached_before_cleanup(self):
        rc, calls = self.exercise(unmounted=True, detach_ok=True)
        self.assertEqual(rc, 7)
        self.assertEqual(calls, ["create", "attach", "info", "detach", "info"])

    def test_timeout_with_unmounted_attachment_is_detached(self):
        rc, calls = self.exercise(unmounted=True, attach_timeout=True, detach_ok=True)
        self.assertNotEqual(rc, 0)
        self.assertIn("detach", calls)

    def test_unmounted_attachment_failed_detach_preserves_image(self):
        rc, calls = self.exercise(unmounted=True)
        self.assertNotEqual(rc, 0)
        self.assertIn("detach", calls)

    def test_detach_timeout_preserves_image(self):
        rc, calls = self.exercise(unmounted=True, detach_timeout=True)
        self.assertNotEqual(rc, 0)
        self.assertIn("detach", calls)

    def test_persistent_attachment_has_bounded_cleanup(self):
        rc, calls = self.exercise(unmounted=True, detach_ok=True, persistent=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls.count("detach"), disk_full.MAX_DETACH_ATTEMPTS)

    def test_confirmed_absent_image_is_removed_without_detach(self):
        rc, calls = self.exercise(absent=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info"])

    def test_malformed_discovery_preserves_image(self):
        rc, calls = self.exercise(malformed_discovery=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "info"])


if __name__ == "__main__":
    unittest.main()
