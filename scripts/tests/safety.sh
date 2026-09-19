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
    # u+rwX, not u+w: a gate-protected snapshot root is 0000 and has to be traversable again
    # before anything below it can be removed.
    chmod -R u+rwX "$SCRATCH" 2>/dev/null
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

# ---- P3 (T1.1b): the default protection is the gate directory ---------------------------------
# The snapshot root is 0000, so the kernel refuses to resolve any name below it. Nothing inside
# was modified to achieve that, which is why a fork needs no unprotect walk.
[ "$(stat -f '%Lp' "$SNAP")" = "0" ] && ok P3 "the snapshot root is mode 0000 after init" \
                                     || bad P3 "the snapshot root is mode 0000 after init (got $(stat -f '%Lp' "$SNAP"))"
check P3 "the snapshot cannot be listed"                  1 -- ls "$SNAP"
check P3 "a file inside the snapshot cannot be read"      1 -- bash -c "cat '$SNAP/hello.txt'"
check P3 "a file inside the snapshot cannot be stat'ed"   1 -- stat "$SNAP/hello.txt"
check P3 "a file inside the snapshot cannot be written"   1 -- bash -c "echo x > '$SNAP/hello.txt'"
check P3 "a file cannot be created inside the snapshot"   1 -- bash -c "echo x > '$SNAP/new.txt'"
check P3 "a file inside the snapshot cannot be deleted"   1 -- rm -f "$SNAP/hello.txt"
check P3 "verify S1 is clean" 0 -- "$WORLD" fs verify S1
# The gate is the whole of the protection, so verify must notice it being left open.
chmod 0755 "$SNAP"
check P3 "verify reports a gate left open" 3 -- "$WORLD" fs verify S1
chmod 0000 "$SNAP"
check P3 "verify is clean again once the gate is closed" 0 -- "$WORLD" fs verify S1

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
# Look behind the gate on purpose (the owner always can; the gate stops accidents and every
# tool that does not go out of its way, exactly like UF_IMMUTABLE stops them for --hard).
chmod 0700 "$SNAP"
[ "$(cat "$SNAP/hello.txt")" = hello ] && ok P3 "writing in the world did not touch the snapshot" \
                                       || bad P3 "writing in the world did not touch the snapshot"
chmod 0000 "$SNAP"

# T1.1b: nothing was cloned into the world that has to be undone -- no immutable flags, and the
# world root has the project's own mode, not the 0500 of the open gate or the 0000 of the gate.
w1mode=$(stat -f '%Lp' "$SCRATCH/w1")
if [ "$w1mode" != 0 ] && [ "$w1mode" != 500 ] && [ -w "$SCRATCH/w1" ] && [ -x "$SCRATCH/w1" ]; then
    ok "--" "fork: the world root has a normal mode ($w1mode)"
else
    bad "--" "fork: the world root has a normal mode (got $w1mode)"
fi
if [ -z "$(find "$SCRATCH/w1" -flags +uchg -print -quit)" ]; then
    ok "--" "fork: no entry in the world carries UF_IMMUTABLE"
else
    bad "--" "fork: no entry in the world carries UF_IMMUTABLE"
fi

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
# --hard: the old per-entry UF_IMMUTABLE protection, still available by request.
if "$WORLD" fs checkpoint W1 --name after-edit --hard | grep -q "from W1"; then
    ok "--" "checkpoint records which world it came from"
else
    bad "--" "checkpoint records which world it came from"
fi
[ -e "$SCRATCH/w1-moved/hello.txt" ] && ok "--" "the world stays writable after a checkpoint" \
                                     || bad "--" "the world stays writable after a checkpoint"

# ---- P3 again: --hard protection and the old tamper test -------------------------------------------
S2=$("$WORLD" fs inspect S2 | awk '/^path:/{print $2}')
"$WORLD" fs inspect S2 | grep -q "protection: hard" && ok P3 "S2 is recorded as a hard snapshot" \
                                                    || bad P3 "S2 is recorded as a hard snapshot"
[ "$(cat "$S2/hello.txt")" = edited ] && ok P3 "a hard snapshot stays readable (no gate)" \
                                      || bad P3 "a hard snapshot stays readable (no gate)"
check P3 "a file in a hard snapshot cannot be written" 1 -- bash -c "echo x > '$S2/hello.txt'"
check P3 "a file in a hard snapshot cannot be deleted" 1 -- rm -f "$S2/hello.txt"
check P3 "verify S2 is clean" 0 -- "$WORLD" fs verify S2
"$WORLD" fs fork --from S2 --to "$SCRATCH/w-hard" > /dev/null
if [ -z "$(find "$SCRATCH/w-hard" -flags +uchg -print -quit)" ] && echo x > "$SCRATCH/w-hard/hello.txt"; then
    ok P3 "a fork from a hard snapshot is unprotected again"
else
    bad P3 "a fork from a hard snapshot is unprotected again"
fi
chflags nouchg "$S2/hello.txt"
echo tampered > "$S2/hello.txt"
check P3 "verify reports a tampered snapshot" 3 -- "$WORLD" fs verify S2

# ---- P5: the exec lock --------------------------------------------------------------------------
W1PATH=$("$WORLD" fs inspect W1 | awk '/^path:/{print $2}')
OTHER=$("$WORLD" fs inspect W3 | awk '/^path:/{print $2}')
LOCK="$WORLD_STORE/locks/W1.lock"

"$WORLD" exec W1 -- /bin/sleep 30 &
EXEC_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$LOCK" ] && break; sleep 0.2; done
if [ -f "$LOCK" ]; then ok P5 "world exec writes $WORLD_STORE/locks/W1.lock"
else bad P5 "world exec writes the lock file"; fi
check    P5 "discard is refused while a world is in use"    3 -- "$WORLD" fs discard W1
has_hint P5 "the refusal offers --force"             "--force" -- "$WORLD" fs discard W1
check    P5 "checkpoint is refused while a world is in use" 3 -- "$WORLD" fs checkpoint W1
check    P5 "fork from a world in use is refused"           3 -- "$WORLD" fs fork --from W1 --to "$SCRATCH/busy"
check    P5 "a second exec in the same world is refused"    3 -- "$WORLD" exec W1 -- /usr/bin/true
check    P5 "--force overrides the lock"                    0 -- "$WORLD" fs fork --from W1 --to "$SCRATCH/forced" --force
check    P5 "an unrelated world is not affected"            0 -- "$WORLD" fs checkpoint W3
kill "$EXEC_PID" 2>/dev/null
wait "$EXEC_PID" 2>/dev/null
[ ! -f "$LOCK" ] && ok P5 "the lock is released when the command ends" \
                 || bad P5 "the lock is released when the command ends"
check P5 "discard works again afterwards" 0 -- "$WORLD" fs discard W1
"$WORLD" fs restore W1 > /dev/null

# A lock left behind by a dead process is cleaned silently, not obeyed.
printf 'pid 2147480000\nstart 1\ncmd ghost\n' > "$LOCK"
check P5 "a stale lock does not block anything" 0 -- "$WORLD" fs checkpoint W1 --name stale
[ ! -f "$LOCK" ] && ok P5 "a stale lock is removed" || bad P5 "a stale lock is removed"

# ---- P14: the seatbelt profile ---------------------------------------------------------------------
check P14 "the child's exit code is this command's exit code" 42 -- "$WORLD" exec W1 -- /bin/sh -c 'exit 42'
check P14 "WORLD_ROOT is the world and cwd is its root"        0 -- "$WORLD" exec W1 -- /bin/sh -c '[ "$PWD" = "$WORLD_ROOT" ] && [ "$WORLD_ID" = W1 ] && [ -n "$WORLD_STORE" ]'
check P14 "the sandboxed child can write its own world"        0 -- "$WORLD" exec W1 -- /bin/sh -c 'touch sandbox-ok'
check P14 "the sandboxed child can write \$TMPDIR"             0 -- "$WORLD" exec W1 -- /bin/sh -c 'touch "$TMPDIR/wfs-sandbox-ok" && touch /private/tmp/wfs-sandbox-ok'
check P14 "the sandboxed child cannot write another world"     1 -- "$WORLD" exec W1 -- /bin/sh -c "touch '$OTHER/evil'"
check P14 "the sandboxed child cannot write the store"         1 -- "$WORLD" exec W1 -- /bin/sh -c "touch '$WORLD_STORE/evil'"
check P14 "the sandboxed child cannot read the snapshots"      1 -- "$WORLD" exec W1 -- /bin/sh -c "ls '$WORLD_STORE/snapshots'"
check P14 "--no-sandbox runs the command unrestricted"         0 -- "$WORLD" exec W1 --no-sandbox -- /bin/sh -c "touch '$OTHER/allowed'"
rm -f "$OTHER/allowed" "$SCRATCH/w1-moved/sandbox-ok" /private/tmp/wfs-sandbox-ok
[ ! -e "$OTHER/evil" ] && ok P14 "nothing the sandbox refused actually landed" \
                       || bad P14 "nothing the sandbox refused actually landed"
[ -z "$(ls "$WORLD_STORE/tmp" 2>/dev/null)" ] && ok P14 "the generated profile is removed afterwards" \
                                              || bad P14 "the generated profile is removed afterwards"

# ---- P10: diff never trusts the event stream on its own (T1.3) -----------------------------------
# A fresh world, a change of every kind, and then the two paths have to agree line for line.
"$WORLD" fs fork --from S1 --to "$SCRATCH/wdiff" --name wdiff > "$SCRATCH/wd.log" 2>&1
WD=$(awk '{print $1}' "$SCRATCH/wd.log")
if ! echo "$WD" | grep -qE '^W[0-9]+$' || [ ! -d "$SCRATCH/wdiff" ]; then
    bad P10 "fork a world to diff"; cat "$SCRATCH/wd.log" | sed 's/^/        /'
else
    [ -z "$("$WORLD" fs diff "$WD")" ] && ok P10 "an untouched world diffs to nothing" \
                                       || bad P10 "an untouched world diffs to nothing"
    echo changed > "$SCRATCH/wdiff/hello.txt"     # M
    echo new > "$SCRATCH/wdiff/src/new.c"         # A
    rm "$SCRATCH/wdiff/src/a.c"                   # D
    chmod 0600 "$SCRATCH/wdiff/src/a-link.c"      # T
    mkdir "$SCRATCH/wdiff/emptydir"               # A (a directory with nothing to speak for it)
    sleep 1                                       # fseventsd journals on a timer; see TASKS.md T1.3
    # --events asks for the FSEvents path explicitly: the default on a tree this small is the
    # full scan (TASKS.md T1.3, "which path is the default"), and the two must agree line for line.
    "$WORLD" fs diff "$WD" --events > "$SCRATCH/d-ev.txt" 2> "$SCRATCH/d-ev.err"
    evrc=$?
    "$WORLD" fs diff "$WD" --full > "$SCRATCH/d-full.txt" 2>/dev/null
    # Sorted by path, so the changes are interleaved rather than grouped by kind.
    want=$'A emptydir\nM hello.txt\nT src/a-link.c\nD src/a.c\nA src/new.c'
    if [ "$evrc" = 0 ] && [ "$(cat "$SCRATCH/d-ev.txt")" = "$want" ]; then
        ok P10 "diff via FSEvents reports exactly A/M/D/T"
    else
        bad P10 "diff via FSEvents reports exactly A/M/D/T (exit $evrc)"
        cat "$SCRATCH/d-ev.txt" "$SCRATCH/d-ev.err" | sed 's/^/        /'
    fi
    [ "$(cat "$SCRATCH/d-full.txt")" = "$want" ] && ok P10 "diff --full agrees line for line" \
                                                 || { bad P10 "diff --full agrees line for line"; sed 's/^/        /' "$SCRATCH/d-full.txt"; }
    "$WORLD" fs diff "$WD" --stat | grep -q '^2 added, 1 modified, 1 deleted, 1 metadata-only$' \
        && ok P10 "diff --stat counts them (default path)" || bad P10 "diff --stat counts them (default path)"
    # The source snapshot going away is a refusal with a reason, not a wrong answer.
    SP=$("$WORLD" fs inspect S1 | awk '/^path:/{print $2}')
    mv "${SP%/root}" "$SCRATCH/s1-away"
    check    P10 "diff refuses when the source snapshot is gone" 3 -- "$WORLD" fs diff "$WD"
    has_hint P10 "the refusal says what to do instead" "world fs list" -- "$WORLD" fs diff "$WD"
    mv "$SCRATCH/s1-away" "${SP%/root}"
fi

echo
"$WORLD" fs status | sed 's/^/      /'
echo
echo "safety: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
