#!/bin/bash
# Micro workloads that stress the FSKit frontend: metadata traversal + read + write.
# Usage: scripts/bench/fsops.sh <dir> [runs]
# Output CSV: workload,median_s,min_s,max_s
set -euo pipefail
. "$(dirname "$0")/common.sh"
DIR=$1; RUNS=${2:-3}
cd "$DIR"

echo "workload,median_s,min_s,max_s"
repeat "$RUNS" find_all        find . -type f
repeat "$RUNS" stat_all        bash -c 'find . -type f -print0 | xargs -0 stat -f "%z" >/dev/null'
repeat "$RUNS" cat_all         bash -c 'find . -type f -print0 | xargs -0 cat >/dev/null'
if command -v rg >/dev/null; then
repeat "$RUNS" rg_literal      rg -c --no-messages "TODO" .
fi
if [ -d .git ]; then
repeat "$RUNS" git_status      git status --porcelain
fi
repeat "$RUNS" write_4k_x1000  bash -c 'mkdir -p .bench && for i in $(seq 1000); do head -c 4096 /dev/zero > .bench/f$i; done && rm -rf .bench'
repeat "$RUNS" write_64m       bash -c 'dd if=/dev/zero of=.bench64m bs=1m count=64 2>/dev/null && rm -f .bench64m'
