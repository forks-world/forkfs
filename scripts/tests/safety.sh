#!/bin/bash
# End-to-end check of the fool-proofing rules of docs/M1_DESIGN.md §3, driven through the CLI.
# One line of PASS/FAIL per rule; exit 1 if any rule failed.
#
# Everything happens inside $WORLD_TEST_DIR (default: a scratch directory whose last component
# must be `m1test`, which is the only thing this script ever deletes).
#
#   scripts/tests/safety.sh [build-dir] [scratch-dir]
set -uo pipefail

BUILD=${1:-build/Release}
cd "$(dirname "$0")/../.."
BUILD=$(cd "$BUILD" && pwd)
WORLD="$BUILD/cli/world"
[ -x "$WORLD" ] || { echo "no world CLI at $WORLD; build first"; exit 2; }

SCRATCH=${2:-${WORLD_TEST_DIR:-/private/tmp/claude-501/-Users-hurricane-private-code-forks-world-forkfs/5b03ff32-c328-4b45-93c8-d8077b5207cc/scratchpad/m1test}}
case "$SCRATCH" in
    */m1test) ;;
    *) echo "refusing to use $SCRATCH: the scratch directory must be named m1test"; exit 2 ;;
esac

cleanup_scratch() {
    [ -e "$SCRATCH" ] || return 0
    chflags -R nouchg "$SCRATCH" 2>/dev/null
    chmod -R u+w "$SCRATCH" 2>/dev/null
    rm -rf "$SCRATCH"
}
cleanup_scratch
mkdir -p "$SCRATCH"
export WORLD_STORE="$SCRATCH/store"
PROJ="$SCRATCH/proj"

pass=0; fail=0
ok()   { printf 'PASS  %-4s %s\n' "$1" "$2"; pass=$((pass+1)); }
bad()  { printf 'FAIL  %-4s %s\n' "$1" "$2"; fail=$((fail+1)); }
# check <rule> <description> <expected-exit> -- command...
check() {
    local rule=$1 desc=$2 want=$3; shift 4
    local out; out=$("$@" 2>&1); local rc=$?
    if [ "$rc" = "$want" ]; then ok "$rule" "$desc"; else bad "$rule" "$desc (exit $rc, wanted $want)"; echo "$out" | sed 's/^/        /'; fi
}
# The refusal has to say what to do instead, not just complain.
has_hint() {
    local rule=$1 desc=$2 needle=$3; shift 4
    local out; out=$("$@" 2>&1)
    if echo "$out" | grep -q "try: .*$needle"; then ok "$rule" "$desc"
    else bad "$rule" "$desc (no 'try: ...$needle' in output)"; echo "$out" | sed 's/^/        /'; fi
}

mkdir -p "$PROJ/src"
echo hello > "$PROJ/hello.txt"
echo 'int main(){}' > "$PROJ/src/a.c"
ln "$PROJ/src/a.c" "$PROJ/src/a-link.c"      # P9

echo "== store: $WORLD_STORE"

# ---- P6: cross-volume clonefile is detected by cloning, not by comparing st_dev -------------
check   P6 "init from another volume is refused" 3 -- "$WORLD" fs init /System/Library/CoreServices
has_hint P6 "the refusal names --store / --copy" "--store" -- "$WORLD" fs init /System/Library/CoreServices

# ---- init: a protected snapshot -------------------------------------------------------------
out=$("$WORLD" fs init "$PROJ" --name proj 2>&1) || { echo "$out"; echo "init failed"; exit 1; }
echo "$out" | sed 's/^/      /'
if echo "$out" | grep -qi "hardlink"; then ok P9 "init warns that the tree has hardlinks"
else bad P9 "init warns that the tree has hardlinks"; fi

SNAP=$("$WORLD" fs inspect S1 | awk '/^path:/{print $2}')
check P3 "a file inside the snapshot cannot be written" 1 -- bash -c "echo x > '$SNAP/hello.txt'"
check P3 "a file cannot be created inside the snapshot" 1 -- bash -c "echo x > '$SNAP/new.txt'"
check P3 "a file inside the snapshot cannot be deleted" 1 -- rm -f "$SNAP/hello.txt"
check P3 "verify S1 is clean" 0 -- "$WORLD" fs verify S1

# ---- P7: dangerous init / fork paths --------------------------------------------------------
check P7 "init / is refused" 3 -- "$WORLD" fs init /
check P7 "init \$HOME is refused" 3 -- "$WORLD" fs init "$HOME"
check P7 "init inside the store is refused" 3 -- "$WORLD" fs init "$WORLD_STORE"
check P7 "init on a snapshot root is refused" 3 -- "$WORLD" fs init "$SNAP"

"$WORLD" fs fork --from S1 --to "$SCRATCH/w1" > /dev/null || { echo "fork failed"; exit 1; }
check P7 "init on a world root is refused" 3 -- "$WORLD" fs init "$SCRATCH/w1"
has_hint P7 "that refusal points at checkpoint" "checkpoint" -- "$WORLD" fs init "$SCRATCH/w1"
check P7 "init inside a world is refused" 3 -- "$WORLD" fs init "$SCRATCH/w1/src"
check P7 "forking into a world is refused" 3 -- "$WORLD" fs fork --from S1 --to "$SCRATCH/w1/inner"

# fork produced a writable copy with the same content
if [ "$(cat "$SCRATCH/w1/hello.txt")" = hello ] && echo edited > "$SCRATCH/w1/hello.txt"; then
    ok "--" "fork: content identical and writable"
else
    bad "--" "fork: content identical and writable"
fi
[ "$(cat "$SNAP/hello.txt")" = hello ] && ok P3 "writing in the world did not touch the snapshot" \
                                       || bad P3 "writing in the world did not touch the snapshot"

# ---- P1: identity survives a move -----------------------------------------------------------
mv "$SCRATCH/w1" "$SCRATCH/w1-moved"
check P1 "a world moved away is not silently used at the old path" 3 -- "$WORLD" fs verify W1
if "$WORLD" fs verify "$SCRATCH/w1-moved" | grep -q "path repaired" &&
   "$WORLD" fs inspect W1 | grep -q "w1-moved"; then
    ok P1 "verify <new path> re-registers the moved world"
else
    bad P1 "verify <new path> re-registers the moved world"
fi

# ---- P2: a copy is not a world --------------------------------------------------------------
cp -R "$SCRATCH/w1-moved" "$SCRATCH/w1-copy"
check    P2 "a cp -R copy is refused" 3 -- "$WORLD" fs verify "$SCRATCH/w1-copy"
has_hint P2 "the refusal points at adopt" "adopt" -- "$WORLD" fs verify "$SCRATCH/w1-copy"
check    P2 "checkpointing a copy is refused" 3 -- bash -c "'$WORLD' fs init '$SCRATCH/w1-copy'"
if "$WORLD" fs adopt "$SCRATCH/w1-copy" --name adopted > /dev/null &&
   "$WORLD" fs verify "$SCRATCH/w1-copy" > /dev/null; then
    ok P2 "adopt turns the copy into a world of its own"
else
    bad P2 "adopt turns the copy into a world of its own"
fi
check P2 "adopting twice is refused" 3 -- "$WORLD" fs adopt "$SCRATCH/w1-copy"

# ---- P4: discard is reversible ----------------------------------------------------------------
"$WORLD" fs discard W2 > /dev/null
if [ ! -e "$SCRATCH/w1-copy" ] && "$WORLD" fs list | grep -q "W2.*trashed"; then
    ok P4 "discard moves the world to the trash"
else
    bad P4 "discard moves the world to the trash"
fi
"$WORLD" fs gc --retention 7 > /dev/null
"$WORLD" fs list | grep -q "W2.*trashed" && ok P4 "gc keeps trash inside the retention window" \
                                         || bad P4 "gc keeps trash inside the retention window"
if "$WORLD" fs restore W2 > /dev/null && [ "$(cat "$SCRATCH/w1-copy/hello.txt")" = edited ]; then
    ok P4 "restore brings it back with its content"
else
    bad P4 "restore brings it back with its content"
fi
"$WORLD" fs discard W2 > /dev/null
"$WORLD" fs gc --retention 0 > /dev/null
if "$WORLD" fs list | grep -q "^W2"; then bad P4 "gc past the retention window deletes it"
else ok P4 "gc past the retention window deletes it"; fi

# ---- P8: a half-built tree is collected --------------------------------------------------------
mkdir -p "$SCRATCH/interrupted.wfs-tmp/sub"
touch "$SCRATCH/interrupted.wfs-tmp/sub/half"
"$WORLD" fs gc > /dev/null
if [ -e "$SCRATCH/interrupted.wfs-tmp" ]; then bad P8 "gc removes a stray *.wfs-tmp tree"
else ok P8 "gc removes a stray *.wfs-tmp tree"; fi
[ -e "$SCRATCH/w1-moved/hello.txt" ] && ok P8 "gc left the live world alone" \
                                     || bad P8 "gc left the live world alone"

# ---- P12: two commands at once do not corrupt the store -----------------------------------------
"$WORLD" fs fork --from S1 --to "$SCRATCH/par-a" > "$SCRATCH/pa.log" 2>&1 &
"$WORLD" fs fork --from S1 --to "$SCRATCH/par-b" > "$SCRATCH/pb.log" 2>&1 &
wait
if [ -e "$SCRATCH/par-a/hello.txt" ] && [ -e "$SCRATCH/par-b/hello.txt" ] &&
   [ "$("$WORLD" fs list | grep -c '^W')" -ge 3 ]; then
    ok P12 "two concurrent forks both complete"
else
    bad P12 "two concurrent forks both complete"; cat "$SCRATCH/pa.log" "$SCRATCH/pb.log" | sed 's/^/        /'
fi

# ---- P13: a store from another schema is refused -------------------------------------------------
mkdir -p "$SCRATCH/oldstore"
echo 99 > "$SCRATCH/oldstore/VERSION"
check P13 "a store with a different schema is refused" 3 -- "$WORLD" --store "$SCRATCH/oldstore" fs list

# ---- checkpoint and the snapshot DAG --------------------------------------------------------------
if "$WORLD" fs checkpoint W1 --name after-edit | grep -q "from W1"; then
    ok "--" "checkpoint records which world it came from"
else
    bad "--" "checkpoint records which world it came from"
fi
[ -e "$SCRATCH/w1-moved/hello.txt" ] && ok "--" "the world stays writable after a checkpoint" \
                                     || bad "--" "the world stays writable after a checkpoint"

# ---- P3 again: tampering with a snapshot is reported ----------------------------------------------
S2=$("$WORLD" fs inspect S2 | awk '/^path:/{print $2}')
chflags nouchg "$S2/hello.txt"
echo tampered > "$S2/hello.txt"
check P3 "verify reports a tampered snapshot" 3 -- "$WORLD" fs verify S2

echo
"$WORLD" fs status | sed 's/^/      /'
echo
echo "safety: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
