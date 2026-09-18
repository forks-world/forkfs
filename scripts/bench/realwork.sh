#!/bin/bash
# Realistic mixed workloads, native vs worldfs mount. Usage: scripts/bench/realwork.sh <base-dir> <mountpoint>
# Uses third_party/fmt (a real C++ project with a git checkout) as the source tree.
# By design the "native" side IS the mount's backing directory, so run this with the mount idle (no other writer, no open files under it).
set -euo pipefail
export PATH=/opt/homebrew/bin:$HOME/.cargo/bin:/usr/local/bin:$PATH
RG=$(command -v rg || true)
# This machine's Command Line Tools ship an incomplete c++/v1 include dir; point at the SDK copy.
SDK=$(xcrun --show-sdk-path); export CXXFLAGS="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
HERE=$(cd "$(dirname "$0")/../.." && pwd)
B=$1; M=$2
SRC=$HERE/third_party/fmt
t() { local label=$1; shift; local t0 t1; t0=$(python3 -c 'import time;print(time.perf_counter())'); "$@" >/dev/null 2>&1; t1=$(python3 -c 'import time;print(time.perf_counter())'); python3 -c "print(f'{'$label':<34}{$t1-$t0:8.3f}s')"; }
for side in native worldfs; do
    if [ $side = native ]; then D=$B/rw; else D=$M/rw; fi
    rm -rf "$B/rw"; mkdir -p "$D"
    echo "== $side ($D)"
    t "tar extract fmt tree"          bash -c "cd '$SRC' && tar cf - --exclude=.git . | (cd '$D' && tar xf -)"
    t "rm -rf extracted tree"         bash -c "rm -rf '$D'/* '$D'/.[!.]*"
    t "git clone (local, objects+checkout)" git clone -q --no-hardlinks "$SRC" "$D"
    t "git status (1st, racy)"        git -C "$D" status --porcelain
    t "git status (2nd)"              git -C "$D" status --porcelain
    [ -n "$RG" ] && t "rg -c 'format' tree"           "$RG" -c format "$D"
    t "find | wc"                     bash -c "find '$D' | wc -l"
    t "cmake configure"               cmake -S "$D" -B "$D/build" -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_MODULE=OFF -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$CXXFLAGS"
    t "cmake build libfmt (-j8)"      cmake --build "$D/build" -j8
    t "touch header + rebuild"        bash -c "touch '$D/include/fmt/format.h' && cmake --build '$D/build' -j8"
    t "rm -rf tree"                   rm -rf "$D"
done
