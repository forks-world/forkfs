#!/bin/bash
# Generate a synthetic source-like tree: <dirs> dirs × <files> files of ~<kb> KB.
# Usage: scripts/bench/mktree.sh <dest> [dirs=200] [files=50] [kb=4]
set -euo pipefail
DEST=$1; DIRS=${2:-200}; FILES=${3:-50}; KB=${4:-4}
mkdir -p "$DEST"
python3 - "$DEST" "$DIRS" "$FILES" "$KB" <<'PY'
import os, sys, random
dest, dirs, files, kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
random.seed(7)
body = ("// TODO: generated line for benchmark purposes\n" * 64)[: kb * 1024]
for d in range(dirs):
    p = os.path.join(dest, f"pkg{d:04d}", "src")
    os.makedirs(p, exist_ok=True)
    for f in range(files):
        with open(os.path.join(p, f"file{f:03d}.c"), "w") as fh:
            fh.write(body)
print(f"created {dirs*files} files in {dest}")
PY
