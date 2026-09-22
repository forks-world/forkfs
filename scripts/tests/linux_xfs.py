#!/usr/bin/env python3
"""XFS entry point for the shared Linux reflink lifecycle/security regression."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).with_name('linux_reflink.py')), run_name='__main__')
