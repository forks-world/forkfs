"""Failure-path tests for image cleanup; no disk images or child processes are run."""

import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import disk_full


class ImageCleanupTests(unittest.TestCase):
    def exercise(self, *, attach_timeout=False, detach_ok=False, unreadable=False):
        with tempfile.TemporaryDirectory(prefix="forkfs-wrapper-unit-") as root:
            root = Path(root)
            private = root / "image"
            private.mkdir()
            mount = private / "mount"
            binary = root / "disk_full_test"
            binary.touch()
            mounted = False
            calls = []
            original_lstat = Path.lstat

            def lstat(path):
                result = original_lstat(path)
                if path == mount and mounted:
                    if unreadable:
                        raise PermissionError("cannot inspect test mount")
                    fields = list(result)
                    fields[2] += 1  # st_dev: emulate a mounted volume, not the host directory.
                    return os.stat_result(fields)
                return result

            def run(command, **kwargs):
                nonlocal mounted
                calls.append(command[1])
                if command[1] == "attach":
                    # An attach can mount successfully and still fail or time out afterward.
                    mounted = True
                    if attach_timeout:
                        raise subprocess.TimeoutExpired(command, 120)
                    return subprocess.CompletedProcess(command, 7, stdout="attach failed\n")
                if command[1] == "detach":
                    if detach_ok:
                        mounted = False
                    return subprocess.CompletedProcess(command, 0 if detach_ok else 8, stdout="")
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
                cleanup = stack.enter_context(mock.patch.object(disk_full.shutil, "rmtree"))
                child = stack.enter_context(mock.patch.object(disk_full.subprocess, "run"))
                output = io.StringIO()
                stack.enter_context(contextlib.redirect_stdout(output))
                stack.enter_context(contextlib.redirect_stderr(output))
                rc = disk_full.main()
                child.assert_not_called()
                if detach_ok and not unreadable:
                    cleanup.assert_called_once_with(private)
                else:
                    cleanup.assert_not_called()
                    self.assertIn("preserving private image", output.getvalue())
                return rc, calls

    def test_partial_attach_and_failed_detach_preserve_image(self):
        rc, calls = self.exercise()
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "detach"])

    def test_attach_timeout_still_detaches_before_cleanup(self):
        rc, calls = self.exercise(attach_timeout=True, detach_ok=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach", "detach"])

    def test_unknown_mount_state_never_removes_image(self):
        rc, calls = self.exercise(unreadable=True)
        self.assertNotEqual(rc, 0)
        self.assertEqual(calls, ["create", "attach"])

    def test_cleanup_does_not_hide_attach_failure(self):
        rc, calls = self.exercise(detach_ok=True)
        self.assertEqual(rc, 7)
        self.assertEqual(calls, ["create", "attach", "detach"])


if __name__ == "__main__":
    unittest.main()
