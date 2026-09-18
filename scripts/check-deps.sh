#!/bin/bash
# Fail if any built binary links a non-system library (arch.md §39: no third-party runtimes).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/Release}
BINS=("$BUILD/cli/world" "$BUILD/core/core_test")
[ -f "$BUILD/macos/fskit/WorldFSExtension" ] && BINS+=("$BUILD/macos/fskit/WorldFSExtension")
rc=0
for b in "${BINS[@]}"; do
    if [ "$(uname)" = Darwin ]; then
        deps=$(otool -L "$b" | tail -n +2 | awk '{print $1}')
        bad=$(echo "$deps" | grep -vE '^(/usr/lib/|/System/Library/)' || true)
    else
        deps=$(ldd "$b" | awk '/=>/{print $3}')
        bad=$(echo "$deps" | grep -vE '^(/lib|/usr/lib|/lib64|/usr/lib64)' || true)
    fi
    if [ -n "$bad" ]; then echo "FAIL $b links non-system libraries:"; echo "$bad"; rc=1; else echo "ok   $b ($(echo "$deps" | wc -l | tr -d ' ') system libs)"; fi
done
exit $rc
