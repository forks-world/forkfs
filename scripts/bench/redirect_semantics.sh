#!/bin/bash
# E3: correctness of real toolchains when a write-heavy directory inside a worldfs workspace is a
# symlink to a native APFS directory (the stand-in for arch.md §12 entries.operation = REDIRECT).
# No network. Every claim is checked by an actual command, not by inspection.
#
# Usage: scripts/bench/redirect_semantics.sh <workspace-on-mount> <native-redirect-root>
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
W=$1; RT=$2
PASS=0; FAIL=0
rm -rf "$W" "$RT"; mkdir -p "$W" "$RT"

ok()   { PASS=$((PASS+1)); printf 'PASS  %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf 'FAIL  %s\n' "$*"; }
note() { printf 'NOTE  %s\n' "$*"; }
check() { if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

echo "=== T1 go build + go test, GOCACHE and output inside a symlinked target/ ==="
mkdir -p "$RT/t1-target/gocache" "$RT/t1-target/bin" "$W/t1"
ln -s "$RT/t1-target" "$W/t1/target"
cat > "$W/t1/go.mod" <<'EOF'
module example.com/t1

go 1.21
EOF
cat > "$W/t1/main.go" <<'EOF'
package main

import "fmt"

func Add(a, b int) int { return a + b }

func main() { fmt.Println("sum", Add(2, 3)) }
EOF
cat > "$W/t1/main_test.go" <<'EOF'
package main

import "testing"

func TestAdd(t *testing.T) {
	if Add(2, 3) != 5 {
		t.Fatal("bad")
	}
}
EOF
export GOFLAGS=-mod=mod GOPROXY=off GOCACHE="$RT/t1-target/gocache" GOTMPDIR="$RT/t1-target/gocache" GOMODCACHE="$RT/t1-target/gomod"
( cd "$W/t1" && go build -o target/bin/t1 ./... ) >"$W/t1.build.log" 2>&1 \
  && [ -x "$RT/t1-target/bin/t1" ] && ok "go build output lands in the native target/ (binary exists at $RT/t1-target/bin/t1)" \
  || { bad "go build into symlinked target/"; sed 's/^/      /' "$W/t1.build.log" | head -5; }
out=$( "$W/t1/target/bin/t1" 2>&1 ); [ "$out" = "sum 5" ] && ok "the built binary runs through the symlink (stdout='$out')" || bad "run built binary: '$out'"
( cd "$W/t1" && go test ./... ) >"$W/t1.test.log" 2>&1 && ok "go test passes with GOCACHE inside the symlinked target/" \
  || { bad "go test"; sed 's/^/      /' "$W/t1.test.log" | head -5; }
( cd "$W/t1" && go test -count=1 ./... ) >>"$W/t1.test.log" 2>&1 && ok "go test -count=1 (second, cache warm) passes" || bad "go test second run"
[ "$(find "$RT/t1-target/gocache" -type f | wc -l | tr -d ' ')" -gt 0 ] && ok "GOCACHE really populated on the native side ($(find "$RT/t1-target/gocache" -type f | wc -l | tr -d ' ') files)" || bad "GOCACHE empty"
unset GOFLAGS GOPROXY GOCACHE GOTMPDIR GOMODCACHE

echo
echo "=== T2 python -m venv into a symlinked .venv/, then run its python + compileall ==="
mkdir -p "$RT/t2-venv" "$W/t2"
ln -s "$RT/t2-venv" "$W/t2/.venv"
if ( cd "$W/t2" && python3 -m venv .venv ) >"$W/t2.log" 2>&1; then
  bad "python -m venv into a PRE-EXISTING symlinked .venv/ unexpectedly succeeded"
else
  ok "BLOCKER CONFIRMED: python -m venv REFUSES a .venv/ that is a symlink: $(tr -d '\n' < "$W/t2.log" | head -c 160)"
  note "this is a CPython rule, not a file-system bug: venv.EnvBuilder.ensure_directories raises"
  note "  ValueError('Unable to create directory %r') when os.path.islink(env_dir); it reproduces on plain APFS"
fi
rm -f "$W/t2/.venv"
# workaround A: build the venv AT the native path, then link it into the workspace
python3 -m venv "$RT/t2-venv-real" >"$W/t2a.log" 2>&1 && ok "workaround: python -m venv at the native path succeeds" || bad "native venv create"
ln -s "$RT/t2-venv-real" "$W/t2/.venv"
check "venv python runs through the symlink (.venv/bin/python -V)"  "'$W/t2/.venv/bin/python' -V"
printf 'def f(x):\n    return x * 2\n' > "$W/t2/mod.py"
check "venv python imports a module that lives on the mount"        "cd '$W/t2' && ./.venv/bin/python -c 'import mod; assert mod.f(3)==6'"
check "venv pip --version works"                                    "'$W/t2/.venv/bin/python' -m pip --version"
note "sys.prefix inside the venv is the NATIVE path: $( "$W/t2/.venv/bin/python" -c 'import sys;print(sys.prefix)' )"
mkdir -p "$W/t2/src"; for i in 1 2 3; do printf 'def g%s():\n    return %s\n' "$i" "$i" > "$W/t2/src/m$i.py"; done
check "compileall of mount sources into __pycache__ on the mount"   "'$W/t2/.venv/bin/python' -m compileall -q '$W/t2/src'"
mkdir -p "$RT/t2-pyc"; rm -rf "$W/t2/src/__pycache__"; ln -s "$RT/t2-pyc" "$W/t2/src/__pycache__"
check "compileall with __pycache__ symlinked to native"             "'$W/t2/.venv/bin/python' -m compileall -q -f '$W/t2/src'"
[ "$(find "$RT/t2-pyc" -name '*.pyc' | wc -l | tr -d ' ')" = 3 ] && ok "3 .pyc files landed natively via the symlinked __pycache__" || bad "pyc count: $(find "$RT/t2-pyc" -name '*.pyc' | wc -l | tr -d ' ')"
check "importing those modules uses the redirected __pycache__"     "cd '$W/t2/src' && '$W/t2/.venv/bin/python' -c 'import m1,m2,m3; assert m1.g1()==1 and m3.g3()==3'"

echo "=== T3 node require() of a package under a symlinked node_modules/ ==="
mkdir -p "$RT/t3-nm/leftpad" "$W/t3"
ln -s "$RT/t3-nm" "$W/t3/node_modules"
cat > "$RT/t3-nm/leftpad/package.json" <<'EOF'
{ "name": "leftpad", "version": "1.0.0", "main": "index.js" }
EOF
cat > "$RT/t3-nm/leftpad/index.js" <<'EOF'
module.exports = (s, n) => String(s).padStart(n, "0");
EOF
cat > "$W/t3/app.js" <<'EOF'
const lp = require("leftpad");
if (lp(7, 3) !== "007") { console.error("bad", lp(7, 3)); process.exit(1); }
console.log("ok");
EOF
out=$( cd "$W/t3" && node app.js 2>&1 ); [ "$out" = ok ] && ok "node require() resolves through the symlinked node_modules/" || bad "node require: $out"
cat > "$RT/t3-nm/leftpad/index.mjs" <<'EOF'
export default (s, n) => String(s).padStart(n, "0");
EOF
python3 - "$RT/t3-nm/leftpad/package.json" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["exports"]={".":{"import":"./index.mjs","require":"./index.js"}}
json.dump(d,open(p,"w"))
PY
cat > "$W/t3/app.mjs" <<'EOF'
import lp from "leftpad";
if (lp(7, 3) !== "007") process.exit(1);
console.log("ok");
EOF
out=$( cd "$W/t3" && node app.mjs 2>&1 ); [ "$out" = ok ] && ok "node ESM import resolves through the symlinked node_modules/" || bad "node ESM: $out"
out=$( cd "$W/t3" && node -e 'console.log(require.resolve("leftpad"))' 2>&1 )
note "require.resolve returns the REAL native path, not the workspace path: $out"

echo
echo "=== T4a git with .git as a symlink to a native directory ==="
mkdir -p "$RT/t4a-git" "$W/t4a/src"
ln -s "$RT/t4a-git" "$W/t4a/.git"
for i in 1 2 3; do echo "line $i" > "$W/t4a/src/f$i.txt"; done
( cd "$W/t4a" && git init -q . \
  && git add -A && git -c user.email=a@b -c user.name=a commit -qm init \
  && git status --porcelain \
  && git checkout -qb work \
  && echo change >> src/f1.txt && git diff --stat \
  && git stash -q && git status --porcelain && git stash pop -q \
  && git add -A && git -c user.email=a@b -c user.name=a commit -qm work ) >"$W/t4a.log" 2>&1 \
  && ok "git init/add/commit/status/checkout -b/diff/stash/commit with .git symlinked to native" \
  || { bad "git via .git symlink"; tail -6 "$W/t4a.log" | sed 's/^/      /'; }
n=$( git -C "$W/t4a" log --oneline work 2>/dev/null | wc -l | tr -d ' ' ); [ "$n" = 2 ] && ok "2 commits on branch work" || bad "commit count=$n"
[ -f "$RT/t4a-git/HEAD" ] && ok "object store really lives natively ($(find "$RT/t4a-git/objects" -type f | wc -l | tr -d ' ') object files under $RT/t4a-git/objects)" || bad "no HEAD in native git dir"
check "git fsck clean"                 "git -C '$W/t4a' fsck --no-progress"
check "git gc packs objects natively"  "git -C '$W/t4a' gc -q --aggressive"
check "git log after gc"               "git -C '$W/t4a' log --oneline work"
note "git rev-parse --git-dir = $( git -C "$W/t4a" rev-parse --git-dir )"
note "git rev-parse --show-toplevel = $( git -C "$W/t4a" rev-parse --show-toplevel )"

echo
echo "=== T4b git init --separate-git-dir (gitfile, no symlink) ==="
mkdir -p "$W/t4b/src"
for i in 1 2 3; do echo "line $i" > "$W/t4b/src/f$i.txt"; done
( cd "$W/t4b" && git init -q --separate-git-dir "$RT/t4b-git" . \
  && git add -A && git -c user.email=a@b -c user.name=a commit -qm init \
  && git status --porcelain \
  && git checkout -qb work \
  && echo change >> src/f1.txt && git diff --stat \
  && git stash -q && git status --porcelain && git stash pop -q \
  && git add -A && git -c user.email=a@b -c user.name=a commit -qm work ) >"$W/t4b.log" 2>&1 \
  && ok "git with --separate-git-dir gitfile: full add/commit/status/branch/diff/stash cycle" \
  || { bad "git via gitfile"; tail -6 "$W/t4b.log" | sed 's/^/      /'; }
n=$( git -C "$W/t4b" log --oneline work 2>/dev/null | wc -l | tr -d ' ' ); [ "$n" = 2 ] && ok "2 commits on branch work (gitfile)" || bad "commit count=$n"
note ".git is a regular file: $(file -b "$W/t4b/.git"), content: $(cat "$W/t4b/.git")"
check "git fsck clean (gitfile)"       "git -C '$W/t4b' fsck --no-progress"
check "git worktree list works"        "git -C '$W/t4b' worktree list"
note "index lives natively: $(ls -l "$RT/t4b-git/index" 2>/dev/null | awk '{print $5" bytes"}')"

echo
echo "=== T5 clang build with output in a symlinked build/ ==="
mkdir -p "$RT/t5-build" "$W/t5"
ln -s "$RT/t5-build" "$W/t5/build"
printf '#include <stdio.h>\nint lib(void);\nint main(){printf("v=%%d\\n", lib());return 0;}\n' > "$W/t5/main.c"
printf 'int lib(void){return 42;}\n' > "$W/t5/lib.c"
( cd "$W/t5" && /usr/bin/clang -c lib.c -o build/lib.o && /usr/bin/clang -c main.c -o build/main.o \
  && /usr/bin/clang build/main.o build/lib.o -o build/prog ) >"$W/t5.log" 2>&1 \
  && ok "clang compiles and links with all outputs in the symlinked build/" || { bad "clang build"; tail -5 "$W/t5.log" | sed 's/^/      /'; }
out=$( "$W/t5/build/prog" 2>&1 ); [ "$out" = "v=42" ] && ok "the binary built into the native build/ executes (stdout='$out')" || bad "exec built prog: $out"
( cd "$W/t5" && /usr/bin/clang -dynamiclib lib.c -o build/liblib.dylib ) >>"$W/t5.log" 2>&1 \
  && python3 -c "import ctypes,sys; sys.exit(0 if ctypes.CDLL('$RT/t5-build/liblib.dylib').lib()==42 else 1)" \
  && ok "dylib built into the native build/ dlopen()s (no quarantine path involved)" || bad "dylib dlopen"
q=$( xattr -p com.apple.quarantine "$RT/t5-build/prog" 2>&1 || true )
note "quarantine xattr on the redirected binary: ${q:-<none>}"

echo
echo "=== T6 do walkers descend into the symlinked directory? ==="
mkdir -p "$W/t6/src" "$RT/t6-target/deps"
ln -s "$RT/t6-target" "$W/t6/target"
echo "NEEDLE in source" > "$W/t6/src/a.c"
for i in $(seq 1 50); do echo "NEEDLE in artifact $i" > "$RT/t6-target/deps/o$i.c"; done
nf=$( cd "$W/t6" && find . | wc -l | tr -d ' ' )
nfL=$( cd "$W/t6" && find -L . 2>/dev/null | wc -l | tr -d ' ' )
note "find .        entries = $nf   (symlink counted as ONE entry, not descended)"
note "find -L .     entries = $nfL  (follows the symlink)"
[ "$nf" -lt 10 ] && ok "find . does NOT descend into the redirected directory" || bad "find . descended ($nf entries)"
[ "$nfL" -gt 50 ] && ok "find -L . does descend (opt-in)" || bad "find -L did not descend"
ng=$( cd "$W/t6" && grep -rl NEEDLE . 2>/dev/null | wc -l | tr -d ' ' )
ngR=$( cd "$W/t6" && grep -Rl NEEDLE . 2>/dev/null | wc -l | tr -d ' ' )
note "grep -r  matches = $ng   grep -R (follow) matches = $ngR"
[ "$ng" = 1 ] && ok "grep -r does NOT descend into the redirected directory" || bad "grep -r matched $ng files"
[ "$ngR" -gt 1 ] && ok "grep -R (capital, follow symlinks) does descend" || note "grep -R matched $ngR"
if command -v rg >/dev/null; then
  nr=$( cd "$W/t6" && rg -l NEEDLE 2>/dev/null | wc -l | tr -d ' ' )
  note "rg -l matches = $nr (ripgrep does not follow symlinks unless -L)"
else note "ripgrep not installed; find/grep results above are the proxy"; fi
nd=$( cd "$W/t6" && python3 -c "
import os
print(sum(len(f) for _,_,f in os.walk('.')))" )
note "python os.walk (followlinks=False, the default) file count = $nd"
nD=$( cd "$W/t6" && python3 -c "
import os
print(sum(len(f) for _,_,f in os.walk('.', followlinks=True)))" )
note "python os.walk(followlinks=True) file count = $nD"
ndu=$( du -sk "$W/t6" 2>/dev/null | awk '{print $1}' )
note "du -sk of the workspace = ${ndu}k (du does not follow the symlink either)"

echo
echo "=== T7 safety: rm -rf of the workspace must not delete the native target ==="
mkdir -p "$W/t7/proj" "$RT/t7-target"
ln -s "$RT/t7-target" "$W/t7/proj/target"
for i in 1 2 3; do echo keep > "$RT/t7-target/keep$i"; done
before=$( ls "$RT/t7-target" | wc -l | tr -d ' ' )
( cd "$W/t7" && rm -rf proj/ ) 2>/dev/null
after=$( ls "$RT/t7-target" 2>/dev/null | wc -l | tr -d ' ' )
[ ! -e "$W/t7/proj" ] && ok "rm -rf proj/ removed the workspace directory" || bad "proj/ still exists"
[ "$after" = "$before" ] && ok "native redirect target untouched by rm -rf ($after/$before files kept)" || bad "native target lost files: $before -> $after"
mkdir -p "$W/t7/proj2"; ln -s "$RT/t7-target" "$W/t7/proj2/target"
( cd "$W/t7/proj2" && rm -rf target ) 2>/dev/null
[ "$(ls "$RT/t7-target" | wc -l | tr -d ' ')" = "$before" ] && ok "rm -rf target (the symlink itself) drops only the link, native files survive" || bad "rm -rf symlink deleted native content"
note "=> the artifact tree OUTLIVES rm -rf: a real REDIRECT implementation must garbage-collect it, or the user leaks a node_modules per delete"

echo
echo "=== T8 misc semantics an agent will hit ==="
mkdir -p "$W/t8" "$RT/t8-target"
ln -s "$RT/t8-target" "$W/t8/target"
echo a > "$W/t8/src.txt"
mv "$W/t8/src.txt" "$W/t8/target/moved.txt" 2>"$W/t8.err" && ok "mv from mount into the redirected dir works (copy+unlink across file systems)" || { bad "mv across the boundary: $(cat "$W/t8.err")"; }
echo b > "$W/t8/h.txt"
if ln "$W/t8/h.txt" "$W/t8/target/h.lnk" 2>"$W/t8.err"; then bad "hardlink ACROSS the boundary unexpectedly succeeded"; else ok "hardlink across the boundary fails as expected: $(tr -d '\n' < "$W/t8.err")"; fi
if cp -c "$W/t8/h.txt" "$W/t8/target/h.clone" 2>"$W/t8.err"; then note "cp -c across the boundary succeeded (fell back to a copy?)"; else ok "cp -c (clonefile) across the boundary fails: $(tr -d '\n' < "$W/t8.err")"; fi
check "cp (plain) across the boundary works" "cp '$W/t8/h.txt' '$W/t8/target/h.copy'"
check "rename WITHIN the redirected dir is atomic/native" "mv '$W/t8/target/h.copy' '$W/t8/target/h.copy2'"
d1=$( stat -f '%d' "$W/t8" ); d2=$( stat -f '%d' "$W/t8/target/" )
note "st_dev differs across the boundary: workspace=$d1 redirected=$d2 (tools that check 'same device' will see two file systems)"
note "df on the redirected dir: $( df -h "$W/t8/target/" | tail -1 | awk '{print $1, $9}' )"

echo
printf 'E3 summary: %d passed, %d failed\n' "$PASS" "$FAIL"
