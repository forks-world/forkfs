#!/usr/bin/env python3
"""Independent ext4 lifecycle/security entry point. Requires a real ext4 scratch mount."""
from pathlib import Path
import subprocess
import sys

here = Path(__file__).parent
subprocess.run([sys.executable, str(here / 'linux_reflink.py'), *sys.argv[1:],
                '--filesystem', 'ext4'], check=True)
subprocess.run([sys.executable, str(here / 'linux_ext4_mounts.py'), *sys.argv[1:]], check=True)
