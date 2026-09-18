#!/bin/bash
# Coding-agent IO stress suite: native dir vs worldfs mount. No network; all data is generated locally.
# Usage: scripts/bench/agentstress.sh <native-dir> <mount-dir> [scale=1]
# Each scenario prints: name  native_s  worldfs_s  native%  [check]
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
N=$1; W=$2; SCALE=${3:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
PY=python3
mkdir -p "$N" "$W"

# The native side must be real APFS, not the mount and not the mount's backing directory: a previous
# run aliased the two, so both "sides" wrote the same bytes and the correctness checks went bogus.
guard_native_not_aliased() {
    local nat mnt nat_mp mnt_mp back
    nat=$(cd "$N" && pwd -P) || exit 2
    mnt=$(cd "$W" && pwd -P) || exit 2
    mountpoint_of() { df "$1" | tail -1 | awk '{for(i=9;i<=NF;i++) printf "%s%s", $i, (i<NF?" ":"")}'; }
    inside() { case "$1/" in "$2/"*) return 0;; *) return 1;; esac; }
    nat_mp=$(mountpoint_of "$nat"); mnt_mp=$(mountpoint_of "$mnt")
    if [ -n "$mnt_mp" ] && [ "$nat_mp" = "$mnt_mp" ]; then
        echo "refusing to run: native dir $nat is inside the worldfs mount $mnt_mp" >&2
        echo "(both sides would be the same file system; pick a native dir on real APFS)" >&2
        exit 2
    fi
    # `mount` prints "file:///backing/path/ on /mountpoint (worldfs, ...)" for a worldfs mount
    back=$(mount | sed -n "s|^file://\(.*\)/ on $mnt_mp (.*|\1|p" | head -1)
    [ -n "$back" ] && back=$(cd "$back" 2>/dev/null && pwd -P)
    if [ -n "$back" ] && inside "$nat" "$back"; then
        echo "refusing to run: native dir $nat is inside the backing directory $back of mount $mnt_mp" >&2
        echo "(both sides would write the same files; pick a native dir outside the backing store)" >&2
        exit 2
    fi
}
guard_native_not_aliased

now() { $PY -c 'import time;print(time.perf_counter())'; }
run_in() { # run_in <dir> <cmd...> → seconds (stdout), exit code in RC
    local d=$1; shift; local t0 t1; t0=$(now); (cd "$d" && "$@") >/dev/null 2>"$d/.stress.err"; RC=$?; t1=$(now); $PY -c "print($t1-$t0)"; }
report() { # report <name> <native_s> <mount_s> <check>
    $PY -c "n=float('$2'); m=float('$3'); print(f'{\"$1\":<24}{n:9.3f}s {m:9.3f}s {100*n/m if m else 0:6.0f}%  $4')"; }
scenario() { # scenario <name> <setup-fn> <run-fn> <check-fn>
    local name=$1 setup=$2 run=$3 check=$4 tn tm cn cm
    for side in N W; do
        local d=${!side}/s; rm -rf "$d"; mkdir -p "$d"; $setup "$d"
    done
    tn=$(run_in "$N/s" $run); cn=$($check "$N/s" && echo ok || echo FAIL)
    tm=$(run_in "$W/s" $run); cm=$($check "$W/s" && echo ok || echo FAIL)
    report "$name" "$tn" "$tm" "native:$cn worldfs:$cm"
    [ "$cn" = FAIL ] && tail -5 "$N/s/.stress.err" 2>/dev/null | sed "s/^/    native  $name: /" >&2
    [ "$cm" = FAIL ] && tail -5 "$W/s/.stress.err" 2>/dev/null | sed "s/^/    worldfs $name: /" >&2
    rm -rf "$N/s"
}
gen_tree() { # gen_tree <dir> <dirs> <files> <kb>
    $PY - "$@" <<'PY'
import os, sys
d, dirs, files, kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
line = "int f(int x) { return x * 3 + 1; } // TODO refactor\n"
want = kb * 1024
body = (line * (want // len(line) + 1))[:want]   # exactly kb KiB, not 64 lines
for i in range(dirs):
    p = os.path.join(d, f"pkg{i:04d}", "src"); os.makedirs(p, exist_ok=True)
    for j in range(files):
        with open(os.path.join(p, f"file{j:03d}.c"), "w") as f: f.write(body)
PY
}
noop() { :; }
ok() { :; }
echo "scenario                  native    worldfs  native%  correctness   (scale=$SCALE)"

# S1 context-read: grep + find + random reads over a 10k-file tree
s1_setup() { gen_tree "$1/tree" $((200*SCALE)) 50 4; }
s1_run() { $PY - <<'PY'
import os, random, subprocess
random.seed(1)
subprocess.run(["grep","-rl","TODO","tree"], stdout=subprocess.DEVNULL)
subprocess.run(["find","tree","-name","*.c"], stdout=subprocess.DEVNULL)
files=[os.path.join(r,f) for r,_,fs in os.walk("tree") for f in fs]
for p in random.sample(files, 2000): open(p,"rb").read()
subprocess.run(["grep","-rl","refactor","tree"], stdout=subprocess.DEVNULL)
PY
}
scenario "S1 context-read" s1_setup s1_run ok

# S2 edit-loop: 500 × (read file, change one line, write temp, rename over)
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
scenario "S2 edit-loop" s2_setup s2_run s2_check

# S3 edit-loop-parallel: 8 processes × 200 edits on disjoint files
s3_setup() { gen_tree "$1/tree" 8 200 8; }
s3_run() { for w in 0 1 2 3 4 5 6 7; do $PY - $w <<'PY' &
import os, sys
w = int(sys.argv[1])
for i in range(200):
    p = f"tree/pkg{w:04d}/src/file{i%200:03d}.c"
    s = open(p).read() + f"// edit {i}\n"
    t = p + ".tmp"
    with open(t, "w") as f: f.write(s)
    os.rename(t, p)
PY
done; wait; }
s3_check() { [ "$(grep -l 'edit 199' "$1"/tree/pkg000*/src/file199.c | wc -l | tr -d ' ')" = 8 ]; }
scenario "S3 edit-parallel-8" s3_setup s3_run s3_check

# S4 build-artifacts: write 5000 .o-style files (4–64KB) into a deep tree, then delete
s4_run() { $PY - <<'PY'
import os, random, shutil
random.seed(2)
for i in range(5000):
    d = f"target/debug/deps/crate{i%100:03d}/objs"; os.makedirs(d, exist_ok=True)
    with open(f"{d}/o{i}.o", "wb") as f: f.write(os.urandom(random.choice([4096, 16384, 65536])))
shutil.rmtree("target")
PY
}
scenario "S4 build-artifacts" noop s4_run ok

# S5 install-tree: node_modules-style tree via tar, hardlink half, clone (cp -c) half, remove
s5_setup() { gen_tree "$1/pkgsrc" $((100*SCALE)) 40 2; (cd "$1" && tar cf pkg.tar pkgsrc && rm -rf pkgsrc); }
s5_run() { bash -c 'mkdir -p node_modules && tar xf pkg.tar -C node_modules && i=0; for f in $(find node_modules -name "file00*.c" | head -2000); do if [ $((i%2)) = 0 ]; then ln "$f" "$f.lnk"; else cp -c "$f" "$f.clone" 2>/dev/null || cp "$f" "$f.clone"; fi; i=$((i+1)); done; rm -rf node_modules'; }
scenario "S5 install-tree" s5_setup s5_run ok

# S6 git-cycle: init/add/commit/status/branch/edit/diff/stash/checkout
s6_setup() { gen_tree "$1/tree" 40 50 4; }
s6_run() { bash -c 'git init -q . && git add -A && git -c user.email=a@b -c user.name=a commit -qm init && git status --porcelain >/dev/null && git checkout -qb work && for i in $(seq 50); do echo "// change $i" >> tree/pkg00$(printf %02d $((i%40)))/src/file00$((i%10)).c; done && git diff --stat >/dev/null && git stash -q && git status --porcelain >/dev/null && git stash pop -q && git add -A && git -c user.email=a@b -c user.name=a commit -qm work && git checkout -q master 2>/dev/null || git checkout -q main; git log --oneline | wc -l'; }
# `[ -d .git ]` passed even when every git write failed (it hid a real EACCES bug), so check the commits.
s6_check() { [ "$(git -C "$1" log --oneline work 2>/dev/null | wc -l | tr -d ' ')" = 2 ]; }
scenario "S6 git-cycle" s6_setup s6_run s6_check

# S7 test-run-churn: 200 × spawn a process that writes+reads+deletes temp files (pytest/jest style)
s7_run() { $PY - <<'PY'
import subprocess, sys
for i in range(200):
    subprocess.run([sys.executable, "-c", "import os,tempfile\nd=tempfile.mkdtemp(dir='.')\nfor k in range(20):\n p=os.path.join(d,f't{k}')\n open(p,'w').write('x'*1024)\n open(p).read()\n os.unlink(p)\nos.rmdir(d)"], check=True)
PY
}
scenario "S7 test-churn" noop s7_run ok

# S8 watch-latency: kqueue/fs.watch event delivery for 50 writes (needs node)
if command -v node >/dev/null; then
s8_run() { node -e '
const fs=require("fs"); let got=0; fs.writeFileSync("w.txt","0");
const w=fs.watch(".", ()=>{got++;});
let i=0; const iv=setInterval(()=>{ fs.writeFileSync("w.txt", String(i)); if(++i>=50){clearInterval(iv); setTimeout(()=>{w.close(); fs.writeFileSync("events.txt", String(got)); process.exit(0)},500);} }, 20);'; }
s8_check() { [ "$(cat "$1/events.txt" 2>/dev/null || echo 0)" -gt 0 ]; }
scenario "S8 watch-events" noop s8_run s8_check
fi

# S9 big-file: 1000 random 8KB writes into a 256MB file + mmap read of 64MB
s9_run() { $PY - <<'PY'
import os, random, mmap
random.seed(3)
with open("big.bin", "wb") as f: f.truncate(256<<20)
fd = os.open("big.bin", os.O_RDWR)
for _ in range(1000): os.pwrite(fd, b"z"*8192, random.randrange(0, (256<<20)-8192))
os.fsync(fd)
m = mmap.mmap(fd, 64<<20, prot=mmap.PROT_READ); s = sum(m[i] for i in range(0, 64<<20, 4096)); m.close(); os.close(fd)
PY
}
scenario "S9 big-file-8K-writes" noop s9_run ok

# S12 exec-artifacts: compile an executable and a dylib in place, run + dlopen, edit, rebuild, run again.
# Regression test for the quarantine/AMFI kernel wedge (see docs/TASKS.md).
s12_run() { bash -c '
set -e
printf "#include <stdio.h>\nint main(){puts(\"v1\");return 0;}\n" > p.c && /usr/bin/clang p.c -o p && [ "$(./p)" = v1 ]
printf "int f(void){return 7;}\n" > l.c && /usr/bin/clang -dynamiclib l.c -o l.dylib && python3 -c "import ctypes; assert ctypes.CDLL(\"./l.dylib\").f()==7"
printf "#include <stdio.h>\nint main(){puts(\"v2\");return 0;}\n" > p.c && /usr/bin/clang p.c -o p && [ "$(./p)" = v2 ]
cp p p2 && [ "$(./p2)" = v2 ]
echo ok > result.txt'; }
s12_check() { [ "$(cat "$1/result.txt" 2>/dev/null)" = ok ]; }
scenario "S12 exec-artifacts" noop s12_run s12_check

rm -rf "$N/s" "$W/s"
