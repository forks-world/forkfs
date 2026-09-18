#!/bin/bash
# E6a(real repo) / E6c / E6d(git): the "native-root World" model, where a World is a recursive
# APFS clonefile() of the base tree into a plain native directory and there is no FSKit at all.
# Usage: scripts/bench/clone_world.sh <native-work-root>
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
SDK=$(xcrun --show-sdk-path); export CXXFLAGS="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
R=$1; HERE=$(cd "$(dirname "$0")/../.." && pwd); PY=python3
mkdir -p "$R"
CLONE_PY="$R/_clone.py"
[ -f "$CLONE_PY" ] || cat > "$CLONE_PY" <<'PY'
import ctypes, ctypes.util, os, sys, time
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
libc.clonefile.restype = ctypes.c_int
src, dst = sys.argv[1], sys.argv[2]
t0 = time.perf_counter()
if libc.clonefile(src.encode(), dst.encode(), 0) != 0:
    e = ctypes.get_errno(); print(f"ERRNO {e} {os.strerror(e)}"); sys.exit(3)
print(f"{time.perf_counter()-t0:.4f}")
PY
t() { $PY -c "import time;print(f'{time.perf_counter():.6f}')"; }
el() { $PY -c "print(f'{$2-$1:.4f}')"; }

echo "############ E6a(real repo) clone a working tree WITH a populated .git"
rm -rf "$R/repo" "$R/repo-clone"
git clone -q --no-hardlinks "$HERE" "$R/repo" 2>/dev/null
( cd "$R/repo" && tar cf - -C "$HERE/third_party/fmt" --exclude=.git . | tar xf - -C . ) 2>/dev/null
( cd "$R/repo" && git add -A >/dev/null 2>&1 && git -c user.email=a@b -c user.name=a commit -qm bulk >/dev/null 2>&1 )
nf=$(find "$R/repo" | wc -l | tr -d ' '); ng=$(find "$R/repo/.git" | wc -l | tr -d ' ')
echo "  repo entries=$nf (of which .git=$ng)  du=$(du -sk "$R/repo" | awk '{print $1}')k"
best=""; for rep in 1 2; do rm -rf "$R/repo-clone"; s=$($PY "$CLONE_PY" "$R/repo" "$R/repo-clone"); echo "  [$rep] clonefile(repo incl .git) ${s}s"; best=$($PY -c "print(min([x for x in ['$best','$s'] if x], key=float))"); done
echo "  -> $($PY -c "print(f'{1e6*$best/$nf:.1f}')") us/entry"
echo -n "  git status in the clone works: "; git -C "$R/repo-clone" status --porcelain >/dev/null 2>&1 && echo yes || echo NO
echo -n "  git log in the clone works   : "; git -C "$R/repo-clone" log --oneline 2>/dev/null | wc -l | tr -d ' '

echo
echo "############ E6d(git) changed-set on a 50k-file GIT repo: git status vs stat-walk"
rm -rf "$R/g50" "$R/g50-clone"
mkdir -p "$R/g50"
$PY - "$R/g50" <<'PY'
import os, sys, random
d = sys.argv[1]; random.seed(7); blob = os.urandom(8192)
made = 0; k = 0
while made < 50000:
    p = os.path.join(d, f"pkg{k:04d}", "lib"); os.makedirs(p, exist_ok=True)
    for j in range(200):
        with open(os.path.join(p, f"f{j:04d}.js"), "wb") as f: f.write(blob[:random.randrange(1024, 8193)])
        made += 1
    k += 1
PY
( cd "$R/g50" && git init -q . && git add -A && git -c user.email=a@b -c user.name=a commit -qm init ) >/dev/null 2>&1
a=$(t); $PY "$CLONE_PY" "$R/g50" "$R/g50-clone" >/dev/null; b=$(t); echo "  clonefile of the 50k git repo: $(el $a $b)s"
git -C "$R/g50-clone" status --porcelain >/dev/null 2>&1   # warm the index/lstat cache
$PY - "$R/g50-clone" <<'PY'
import os, sys, random
root = sys.argv[1]; random.seed(13)
files = sorted(os.path.join(r,f) for r,_,fs in os.walk(root) for f in fs if '.git' not in r)
for p in random.sample(files, 500): open(p, "ab").write(b"m")
d = os.path.join(root, "pkg0000", "lib")
for i in range(200): open(os.path.join(d, f"added{i}.js"), "wb").write(b"new")
for p in random.sample(files, 100):
    try: os.unlink(p)
    except FileNotFoundError: pass
print("  applied: 500 modified, 200 added, 100 deleted")
PY
for rep in 1 2; do
    a=$(t); n=$(git -C "$R/g50-clone" status --porcelain 2>/dev/null | wc -l | tr -d ' '); b=$(t)
    echo "  [$rep] git status --porcelain (50k files, 800 changes): $(el $a $b)s  -> $n paths"
done
for rep in 1 2; do
    a=$(t); n=$(git -C "$R/g50-clone" status --porcelain --untracked-files=no 2>/dev/null | wc -l | tr -d ' '); b=$(t)
    echo "  [$rep] git status -uno                                 : $(el $a $b)s  -> $n paths"
done

echo
echo "############ E6c agent workloads INSIDE a clone vs a plain native copy (expect ~= each other)"
rm -rf "$R/wsbase" "$R/ws" "$R/wsnat"
mkdir -p "$R/wsbase"
( cd "$HERE/third_party/fmt" && tar cf - --exclude=.git . | tar xf - -C "$R/wsbase" )
$PY - "$R/wsbase/tree" <<'PY2'
import os, sys
d = sys.argv[1]
line = "int f(int x) { return x * 3 + 1; } // TODO refactor\n"
body = (line * (8*1024 // len(line) + 1))[:8*1024]
for i in range(10):
    p = os.path.join(d, f"pkg{i:04d}", "src"); os.makedirs(p, exist_ok=True)
    for j in range(50):
        open(os.path.join(p, f"file{j:03d}.c"), "w").write(body)
PY2
NENT=$(find "$R/wsbase" | wc -l | tr -d ' ')

work() { # work <label> <dir>
    local lab=$1 d=$2 a b
    a=$(t); ( cd "$d" && $PY - <<'PY2'
import os
for i in range(500):
    p = f"tree/pkg{i%10:04d}/src/file{i%50:03d}.c"
    s = open(p).read().replace("x * 3", f"x * {i}", 1)
    t = p + ".tmp"
    with open(t, "w") as f: f.write(s); f.flush(); os.fsync(f.fileno())
    os.rename(t, p)
PY2
); b=$(t); local s2=$(el $a $b)
    a=$(t); ( cd "$d" && $PY - <<'PY2'
import os, random, shutil
random.seed(2)
for i in range(5000):
    dd = f"target/debug/deps/crate{i%100:03d}/objs"; os.makedirs(dd, exist_ok=True)
    open(f"{dd}/o{i}.o", "wb").write(os.urandom(random.choice([4096, 16384, 65536])))
shutil.rmtree("target")
PY2
); b=$(t); local s4=$(el $a $b)
    a=$(t); cmake -S "$d" -B "$d/build" -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_MODULE=OFF -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$CXXFLAGS" >/dev/null 2>&1; b=$(t); local cf=$(el $a $b)
    a=$(t); cmake --build "$d/build" -j8 >/dev/null 2>&1; b=$(t); local bd=$(el $a $b)
    a=$(t); touch "$d/include/fmt/format.h"; cmake --build "$d/build" -j8 >/dev/null 2>&1; b=$(t); local rb=$(el $a $b)
    a=$(t); rm -rf "$d/build"; b=$(t); local rm=$(el $a $b)
    printf '  %-26s %8s %8s %8s %8s %8s %8s\n' "$lab" "$s2" "$s4" "$cf" "$bd" "$rb" "$rm"
}

printf '  %-26s %8s %8s %8s %8s %8s %8s\n' config S2-edit S4-artif configure build-j8 rebuild rm-build
for rep in 1 2; do
    rm -rf "$R/ws"; a=$(t); $PY "$CLONE_PY" "$R/wsbase" "$R/ws" >/dev/null; b=$(t)
    fk=$(el $a $b)
    rm -rf "$R/wsnat"; a=$(t); cp -R "$R/wsbase" "$R/wsnat"; b=$(t); cpr=$(el $a $b)
    echo "  [rep $rep] fork by clonefile ($NENT entries) = ${fk}s   |  plain cp -R = ${cpr}s"
    work "clone-root World" "$R/ws"
    work "plain native copy" "$R/wsnat"
done
echo "  sanity: base workspace still intact? $(find "$R/wsbase" -name 'o*.o' | wc -l | tr -d ' ') stray .o in base (must be 0), $(grep -c 'x \* 3' "$R/wsbase/tree/pkg0009/src/file049.c" | tr -d ' ') unedited marker lines in base (must be >0)"
rm -rf "$R/ws" "$R/wsnat"
echo "############ done"
