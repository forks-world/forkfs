"""Owned-image cleanup must handle a partial attach and fail closed."""
import plistlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import revision_disk_full as disk


class CleanupTests(unittest.TestCase):
    def exercise(self, responses):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / 'mount'
            mount.mkdir()
            image = root / 'test.sparseimage'
            with patch.object(disk, 'run', side_effect=responses) as run:
                disk.detach_owned_image(image, mount)
                return run.call_args_list

    def attached(self, image):
        return plistlib.dumps({'images': [{'image-path': str(image),
            'system-entities': [{'dev-entry': '/dev/disk42'}]}]})

    def test_partial_attach_discovered_and_detached(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / 'mount'
            mount.mkdir()
            image = root / 'test.sparseimage'
            absent = plistlib.dumps({'images': []})
            with patch.object(disk, 'run', side_effect=[self.attached(image), b'', absent]) as run:
                disk.detach_owned_image(image, mount)
                self.assertEqual(run.call_args_list[1].args,
                                 ('/usr/bin/hdiutil', 'detach', '/dev/disk42'))

    def test_absent_attachment_needs_no_detach(self):
        self.assertEqual(len(self.exercise([plistlib.dumps({'images': []})])), 1)

    def test_unknown_state_preserves_image(self):
        for reply in [b'not a plist', plistlib.dumps({'images': [{}]})]:
            with self.subTest(reply=reply), self.assertRaises(Exception):
                self.exercise([reply])

    def test_detach_failure_preserves_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mount = root / 'mount'
            mount.mkdir()
            image = root / 'test.sparseimage'
            with patch.object(disk, 'run', side_effect=[self.attached(image), RuntimeError('busy')]):
                with self.assertRaisesRegex(RuntimeError, 'busy'):
                    disk.detach_owned_image(image, mount)


if __name__ == '__main__':
    unittest.main()
