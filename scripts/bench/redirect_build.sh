#!/bin/bash
# E2: a real cmake build (third_party/fmt) with the build/ directory (1) on the worldfs mount,
# (2) redirected to native APFS via a symlink, plus a fully native baseline.
# Same CXXFLAGS / -DFMT_MODULE=OFF trick as scripts/bench/realwork.sh.
#
# Usage: scripts/bench/redirect_build.sh <native-dir> <mount-dir> <mount-redirect-dir> <redirect-target-root> [reps=2]
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
SDK=$(xcrun --show-sdk-path); export CXXFLAGS="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$HERE/third_party/fmt
NAT=$1; MNT=$2; RED=$3; RT=$4; REPS=${5:-2}
PY=python3
now() { $PY -c 'import time;print(time.perf_counter())'; }

run_cfg() { # run_cfg <config> <basedir>  -> prints "<config> <cfg_s> <build_s> <rebuild_s> <rmbuild_s>"
    local cfg=$1 base=$2 d="$2/rw" tgt="$RT/build"
    rm -rf "$d" "$tgt"; mkdir -p "$d"
    (cd "$SRC" && tar cf - --exclude=.git . | (cd "$d" && tar xf -))
    if [ "$cfg" = redirect ]; then mkdir -p "$tgt"; ln -s "$tgt" "$d/build"; else mkdir -p "$d/build"; fi
    local a b c e t0
    t0=$(now); cmake -S "$d" -B "$d/build" -DFMT_TEST=${FMT_TEST:-OFF} -DFMT_DOC=OFF -DFMT_MODULE=OFF \
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$CXXFLAGS" >/dev/null 2>&1; a=$($PY -c "print(f'{$(now)-$t0:.3f}')")
    t0=$(now); cmake --build "$d/build" -j8 >/dev/null 2>&1;                      b=$($PY -c "print(f'{$(now)-$t0:.3f}')")
    t0=$(now); touch "$d/include/fmt/format.h"; cmake --build "$d/build" -j8 >/dev/null 2>&1; c=$($PY -c "print(f'{$(now)-$t0:.3f}')")
    # remove the CONTENTS of build/ so all three configs delete the same files (rm -rf on a
    # symlink would only drop the link)
    t0=$(now); rm -rf "$d"/build/* "$d"/build/.[!.]* 2>/dev/null;                 e=$($PY -c "print(f'{$(now)-$t0:.3f}')")
    echo "$cfg $a $b $c $e"
    rm -rf "$d" "$tgt"
}

echo "config      configure   build-j8    rebuild   rm-build"
for cfg in native mount redirect; do
    case $cfg in native) base=$NAT;; mount) base=$MNT;; redirect) base=$RED;; esac
    mkdir -p "$base"
    for r in $(seq "$REPS"); do
        run_cfg "$cfg" "$base" | awk '{printf "%-11s %9s %10s %10s %10s\n", $1, $2, $3, $4, $5}'
    done
done
