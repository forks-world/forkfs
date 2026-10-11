"""Configure metadata CI without shell-specific argument expansion."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=Path('build/metadata'))
parser.add_argument('--fskit', choices=['on', 'off'], default='off')
args = parser.parse_args()
if args.fskit == 'on' and sys.platform != 'darwin':
    parser.error('--fskit on requires macOS')
source = Path(__file__).resolve().parents[2]
scratch = Path(os.environ.get('RUNNER_TEMP', tempfile.gettempdir())) / 'forkfs-metadata-ci-tmp'
scratch.mkdir(parents=True, exist_ok=True)
tmpdir = str(scratch.resolve()) + os.sep
if '\n' in tmpdir or '\r' in tmpdir:
    raise ValueError('temporary directory cannot contain a newline')
environment = dict(os.environ, TMPDIR=tmpdir)
command = ['cmake', '-S', str(source), '-B', str(args.build_dir.resolve()),
           '-DWFS_BUILD_LEGACY=OFF', '-DCMAKE_BUILD_TYPE=Release']
command.append('-DWFS_COW_FSKIT=' + args.fskit.upper())
if sys.platform.startswith('linux'):
    command.append('-DCMAKE_CXX_COMPILER=g++-14')
subprocess.run(command, env=environment, check=True)
if os.environ.get('GITHUB_ENV'):
    with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as stream:
        stream.write(f'TMPDIR={tmpdir}\n')
