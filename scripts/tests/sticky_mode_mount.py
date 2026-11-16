#!/usr/bin/env python3
"""Check sticky mode behavior on an owned forkfs mount or APFS reference directory."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from filesystem_compat import check, filesystem


WORKER = r'''
import json, os, pathlib, stat, sys
root=pathlib.Path(sys.argv[1])
def mode(path): return stat.S_IMODE(path.lstat().st_mode)

# Kernel creation may strip sticky from regular files/directories. Verify
# chmod/fchmod persistence after creation instead.
regular=root/'regular'
fd=os.open(regular,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
os.fchmod(fd,0o1777)
assert stat.S_IMODE(os.fstat(fd).st_mode)==0o1777
os.write(fd,b'kept');assert stat.S_IMODE(os.fstat(fd).st_mode)==0o1777
os.close(fd);assert mode(regular)==0o1777

directory=root/'directory';directory.mkdir(mode=0o700);os.chmod(directory,0o1777)
assert mode(directory)==0o1777

fifo=root/'fifo';os.mkfifo(fifo,0o600);os.chmod(fifo,0o1777)
assert mode(fifo)==0o1777

link=root/'link';os.symlink('regular',link);os.lchmod(link,0o1777)
assert mode(link)==0o1777 and link.resolve().name=='regular'

# An open-unlinked inode keeps its own mode when the pathname is reused.
orphan=root/'orphan';fd=os.open(orphan,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
os.fchmod(fd,0o1777);os.unlink(orphan)
replacement=os.open(orphan,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600);os.close(replacement)
os.fchmod(fd,0o1000)
assert stat.S_IMODE(os.fstat(fd).st_mode)==0o1000
assert mode(orphan)==0o600
os.close(fd)
print(json.dumps({'regular_chmod_fchmod_and_write':True,'directory_chmod':True,
                  'fifo_chmod':True,'symlink_lchmod':True,'unlinked_handle_path_reuse':True}))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mountpoint', type=Path)
    parser.add_argument('--reference-apfs', action='store_true',
                        help='use a caller-provided ordinary APFS directory as the reference')
    args = parser.parse_args()
    check(platform.system() == 'Darwin', 'requires macOS')
    target = args.mountpoint.resolve(strict=True)
    mounted, kind = filesystem(target)
    if args.reference_apfs:
        check(kind == 'apfs', 'reference path must be on APFS')
    else:
        check(kind == 'forkfs' and target == mounted, 'specify the exact forkfs mount root')
        check(target.stat().st_mode & 0o777 == 0o700, 'forkfs root must remain private')

    root = Path(tempfile.mkdtemp(prefix='forkfs-sticky-', dir=target))
    try:
        result = subprocess.run([sys.executable, '-c', WORKER, str(root)],
                                check=True, capture_output=True, text=True, timeout=30)
        print(json.dumps({'filesystem': kind, 'scratch': root.name,
                          'checks': json.loads(result.stdout)}, sort_keys=True))
    finally:
        shutil.rmtree(root)
    return 0


if __name__ == '__main__':
    sys.exit(main())
