#!/bin/bash
# E1: does redirecting write-heavy directories off the FSKit mount recover the lost throughput?
#
# Three configurations per scenario:
#   native   : everything on real APFS
#   mount    : everything on the worldfs mount
#   redirect : the scenario tree lives on the mount, but its write-heavy artifact directory
#              (target/ , node_modules/ , tmp/) is a symlink to a native APFS directory,
#              i.e. the kernel leaves the file system at that name and never issues XPC for it.
#              This is the measurable stand-in for arch.md §12 entries.operation = REDIRECT.
#
# Scenario bodies are copied from scripts/bench/agentstress.sh, with one change: deletion removes
# the *contents* of the artifact directory instead of the directory itself, so that all three
# configurations delete exactly the same files (rm -rf on a symlink would only drop the link).
#
# Usage: scripts/bench/redirect_exp.sh <native-dir> <mount-dir> <mount-redirect-dir> <redirect-target-root> [reps=2]
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
NAT=$1; MNT=$2; RED=$3; RT=$4; REPS=${5:-2}
PY=python3
mkdir -p "$NAT" "$MNT" "$RED" "$RT"

# Guard: the native side must not be the mount or the mount's backing store (see agentstress.sh).
guard() {
    local nat mnt nat_mp mnt_mp back
    nat=$(cd "$NAT" && pwd -P); mnt=$(cd "$MNT" && pwd -P)
    mountpoint_of() { df "$1" | tail -1 | awk '{for(i=9;i<=NF;i++) printf "%s%s", $i, (i<NF?" ":"")}'; }
    nat_mp=$(mountpoint_of "$nat"); mnt_mp=$(mountpoint_of "$mnt")
    [ "$nat_mp" = "$mnt_mp" ] && { echo "refusing: native dir is on the mount" >&2; exit 2; }
    back=$(mount | sed -n "s|^file://\(.*\)/ on $mnt_mp (.*|\1|p" | head -1)
    [ -n "$back" ] && back=$(cd "$back" 2>/dev/null && pwd -P)
    case "$nat/" in "$back"/*) echo "refusing: native dir is inside backing $back" >&2; exit 2;; esac
    local rt; rt=$(cd "$RT" && pwd -P)
    case "$rt/" in "$back"/*|"$mnt_mp"/*) echo "refusing: redirect target inside mount/backing" >&2; exit 2;; esac
}
guard

now() { $PY -c 'import time;print(time.perf_counter())'; }

# ---------------------------------------------------------------- scenario bodies (from agentstress.sh)

gen_tree() { # gen_tree <dir> <dirs> <files> <kb>
    $PY - "$@" <<'PY'
import os, sys
d, dirs, files, kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
line = "int f(int x) { return x * 3 + 1; } // TODO refactor\n"
want = kb * 1024
body = (line * (want // len(line) + 1))[:want]
for i in range(dirs):
    p = os.path.join(d, f"pkg{i:04d}", "src"); os.makedirs(p, exist_ok=True)
    for j in range(files):
        with open(os.path.join(p, f"file{j:03d}.c"), "w") as f: f.write(body)
PY
}

# S4 build-artifacts: 5000 .o-style files (4-64KB) under target/, then delete them.
s4_setup() { mkdir -p "$1/target"; }
s4_run() { $PY - <<'PY'
import os, random, shutil
random.seed(2)
for i in range(5000):
    d = f"target/debug/deps/crate{i%100:03d}/objs"; os.makedirs(d, exist_ok=True)
    with open(f"{d}/o{i}.o", "wb") as f: f.write(os.urandom(random.choice([4096, 16384, 65536])))
shutil.rmtree("target/debug")
PY
}
s4_check() { [ ! -d "$1/target/debug" ]; }
S4_ART=target

# S5 install-tree: tar-extract a node_modules-style tree, hardlink half / cp -c half, remove.
s5_setup() { gen_tree "$1/pkgsrc" 100 40 2; (cd "$1" && tar cf pkg.tar pkgsrc && rm -rf pkgsrc); mkdir -p "$1/node_modules"; }
s5_run() { bash -c 'tar xf pkg.tar -C node_modules && i=0; for f in $(find node_modules/ -name "file00*.c" | head -2000); do if [ $((i%2)) = 0 ]; then ln "$f" "$f.lnk"; else cp -c "$f" "$f.clone" 2>/dev/null || cp "$f" "$f.clone"; fi; i=$((i+1)); done; rm -rf node_modules/pkgsrc'; }
s5_check() { [ ! -d "$1/node_modules/pkgsrc" ]; }
S5_ART=node_modules

# S7 test-churn: 200 process spawns, each writing/reading/deleting 20 temp files under tmp/.
s7_setup() { mkdir -p "$1/tmp"; }
s7_run() { $PY - <<'PY'
import subprocess, sys
for i in range(200):
    subprocess.run([sys.executable, "-c", "import os,tempfile\nd=tempfile.mkdtemp(dir='tmp')\nfor k in range(20):\n p=os.path.join(d,f't{k}')\n open(p,'w').write('x'*1024)\n open(p).read()\n os.unlink(p)\nos.rmdir(d)"], check=True)
PY
}
s7_check() { [ -d "$1/tmp" ]; }
S7_ART=tmp

# S2 edit-loop: CONTROL. Edits source files, which are never redirected, so it must not improve.
s2_setup() { gen_tree "$1/tree" 10 50 8; }
s2_run() { $PY - <<'PY'
import os
for i in range(500):
    p = f"tree/pkg{i%10:04d}/src/file{i%50:03d}.c"
    s = open(p).read().replace("x * 3", f"x * {i}", 1)
    t = p + ".tmp"
    with open(t, "w") as f: f.write(s); f.flush(); os.fsync(f.fileno())
    os.rename(t, p)
PY
}
s2_check() { grep -q "x \* 499" "$1/tree/pkg0009/src/file049.c"; }
S2_ART=

# ---------------------------------------------------------------- driver

one_run() { # one_run <scenario> <config> <basedir> <artifact-name> -> seconds on stdout
    local scen=$1 cfg=$2 base=$3 art=$4
    local d="$base/$scen" tgt="$RT/$scen"
    rm -rf "$d" "$tgt"; mkdir -p "$d"
    if [ "$cfg" = redirect ] && [ -n "$art" ]; then
        mkdir -p "$tgt/$art"
        ln -s "$tgt/$art" "$d/$art"
    fi
    ${scen}_setup "$d"
    local t0 t1; t0=$(now); (cd "$d" && ${scen}_run) >/dev/null 2>"$d/../$scen.err"; local rc=$?; t1=$(now)
    local ck=FAIL; ${scen}_check "$d" && ck=ok
    $PY -c "print(f'{$t1-$t0:.4f} $ck rc=$rc')"
    rm -rf "$d" "$tgt"
}

printf '%-22s %-10s %10s %10s %s\n' scenario config best_s all_s check
for scen in ${SCENARIOS:-s4 s5 s7 s2}; do
    art_var=$(echo "$scen" | tr a-z A-Z)_ART; art=${!art_var}
    B_native=""; B_mount=""; B_redirect=""
    for cfg in native mount redirect; do
        case $cfg in native) base=$NAT;; mount) base=$MNT;; redirect) base=$RED;; esac
        all=""; best=""; check=""
        for r in $(seq "$REPS"); do
            out=$(one_run "$scen" "$cfg" "$base" "$art")
            v=$(echo "$out" | awk '{print $1}'); check=$(echo "$out" | awk '{print $2" "$3}')
            all="$all $v"
            if [ -z "$best" ] || $PY -c "import sys; sys.exit(0 if $v < $best else 1)"; then best=$v; fi
        done
        eval "B_$cfg=$best"
        printf '%-22s %-10s %10s %10s %s\n' "$scen" "$cfg" "$best" "$all" "$check"
    done
    $PY -c "
n=$B_native; m=$B_mount; r=$B_redirect
print(f'  -> $scen  native%: mount={100*n/m:.0f}%  redirect={100*n/r:.0f}%  speedup(redirect/mount)={m/r:.2f}x')"
done
