#!/usr/bin/env python3
"""Independent ext4 lifecycle/security entry point. Requires a real ext4 scratch mount."""
from pathlib import Path
import runpy
import sys

sys.argv.extend(['--filesystem', 'ext4'])
runpy.run_path(str(Path(__file__).with_name('linux_reflink.py')), run_name='__main__')
