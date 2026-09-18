#!/bin/bash
# Compare native APFS vs a worldfs passthrough mount of the same tree.
# Usage: scripts/bench/compare.sh <backing-dir> <mountpoint> [runs]
# Assumes the mount is already active (scripts/mount.sh). Prints ratio table.
# By design the "native" side IS the mount's backing directory, so run this with the mount idle (no other writer, no open files under it).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BACKING=$1; MNT=$2; RUNS=${3:-3}
OUT=${BENCH_OUT:-/tmp/worldfs-bench}
mkdir -p "$OUT"
"$HERE/fsops.sh" "$BACKING" "$RUNS" > "$OUT/native.csv"
"$HERE/fsops.sh" "$MNT" "$RUNS"     > "$OUT/passthrough.csv"
python3 - "$OUT/native.csv" "$OUT/passthrough.csv" <<'PY'
import csv, sys
def load(p):
    with open(p) as f:
        return {r['workload']: float(r['median_s']) for r in csv.DictReader(f)}
nat, pt = load(sys.argv[1]), load(sys.argv[2])
print(f"{'workload':<16}{'native_s':>10}{'worldfs_s':>11}{'native%':>9}")
for k in nat:
    if k in pt:
        pct = 100.0 * nat[k] / pt[k] if pt[k] else float('nan')
        print(f"{k:<16}{nat[k]:>10.3f}{pt[k]:>11.3f}{pct:>8.0f}%")
PY
