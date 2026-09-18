#!/bin/bash
# Shared helpers for the M0 benchmark harness.
# Every benchmark takes a directory and prints one CSV line: name,dir,wall_seconds

bench_time() {
    # bench_time <label> <cmd...>   → prints label,wall
    local label=$1; shift
    local t0 t1
    t0=$(python3 -c 'import time; print(time.perf_counter())')
    "$@" >/dev/null 2>&1
    t1=$(python3 -c 'import time; print(time.perf_counter())')
    python3 -c "print(f'{'$label'},{$t1-$t0:.4f}')"
}

drop_caches() {
    # macOS has no drop_caches; `purge` needs root. Best effort, silent if not permitted.
    sudo -n purge 2>/dev/null || true
}

repeat() {
    # repeat <n> <label> <cmd...>  → median of n runs
    local n=$1 label=$2; shift 2
    local vals=()
    for _ in $(seq "$n"); do
        vals+=("$(bench_time "$label" "$@" | cut -d, -f2)")
    done
    python3 - "$label" "${vals[@]}" <<'PY'
import sys, statistics
label, *vals = sys.argv[1:]
vals = [float(v) for v in vals]
print(f"{label},{statistics.median(vals):.4f},{min(vals):.4f},{max(vals):.4f}")
PY
}
