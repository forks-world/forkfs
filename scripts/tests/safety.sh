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
    # The undeletable-trash-entry case below denies the owner delete_child with an ACL, which no
    # chmod of the mode bits and no chflags can undo.
    chmod -R -N "$SCRATCH" 2>/dev/null
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
if echo "$out" | grep -qi "hardlink"; then ok P9 "init reports the hardlinks it found in the tree"
else bad P9 "init reports the hardlinks it found in the tree"; fi

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
# --now is the synchronous gc: without it the trash is handed to the background collector (T2.1).
"$WORLD" fs gc --retention 0 --now > /dev/null
if "$WORLD" fs list | grep -q "^W2"; then bad P4 "gc past the retention window deletes it"
else ok P4 "gc past the retention window deletes it"; fi

# ---- P8 + PR #1 review (3rd round): `.wfs-tmp` in a user's directory is the user's ------------
# The fork used to build its clone at `<target>.wfs-tmp` and remove whatever was there first, and
# gc used to sweep every `*.wfs-tmp` out of the parent directory of every world. Both of those
# directories belong to the user. Here `wtmp.wfs-tmp` is a file that is exactly the old temporary
# name of the fork below, and `notes.wfs-tmp` is an ordinary directory in the same place.
mkdir -p "$SCRATCH/pr3"
echo "mine, not a temporary" > "$SCRATCH/pr3/wtmp.wfs-tmp"
mkdir -p "$SCRATCH/pr3/notes.wfs-tmp/sub"
echo keep > "$SCRATCH/pr3/notes.wfs-tmp/sub/keep"
"$WORLD" fs fork --from S1 --to "$SCRATCH/pr3/wtmp" > /dev/null 2>&1
if [ -e "$SCRATCH/pr3/wtmp/hello.txt" ] &&
   [ "$(cat "$SCRATCH/pr3/wtmp.wfs-tmp" 2>/dev/null)" = "mine, not a temporary" ] &&
   [ "$(cat "$SCRATCH/pr3/notes.wfs-tmp/sub/keep" 2>/dev/null)" = keep ]; then
    ok PR3 "fork --to X leaves the user's own X.wfs-tmp alone"
else
    bad PR3 "fork --to X leaves the user's own X.wfs-tmp alone"
fi
# The fork's own temporary is drawn, not derived, and it is gone by the time the fork returns.
if [ -z "$(find "$SCRATCH/pr3" -maxdepth 1 -name '.wfs-fork-*' -print -quit)" ]; then
    ok PR3 "the fork leaves no temporary of its own behind"
else
    bad PR3 "the fork leaves no temporary of its own behind"
fi
"$WORLD" fs gc > /dev/null
if [ "$(cat "$SCRATCH/pr3/wtmp.wfs-tmp" 2>/dev/null)" = "mine, not a temporary" ] &&
   [ "$(cat "$SCRATCH/pr3/notes.wfs-tmp/sub/keep" 2>/dev/null)" = keep ]; then
    ok PR3 "gc does not sweep *.wfs-tmp out of a user's directory"
else
    bad PR3 "gc does not sweep *.wfs-tmp out of a user's directory"
fi
[ -e "$SCRATCH/w1-moved/hello.txt" ] && ok P8 "gc left the live world alone" \
                                     || bad P8 "gc left the live world alone"
# A stray `*.wfs-tmp` directory is no longer something gc acts on at all: what it collects is the
# path a CREATING row recorded (core_test drives that crash through the test seam).
mkdir -p "$SCRATCH/pr3/interrupted.wfs-tmp/sub"
touch "$SCRATCH/pr3/interrupted.wfs-tmp/sub/half"
"$WORLD" fs gc > /dev/null
[ -e "$SCRATCH/pr3/interrupted.wfs-tmp/sub/half" ] \
    && ok PR3 "gc does not guess that a *.wfs-tmp tree is ours" \
    || bad PR3 "gc does not guess that a *.wfs-tmp tree is ours"

# ---- PR #1 review (3rd round): an orphan in the trash starts a worker ---------------------------
# A discard killed between the rename into <store>/trash and the commit of its row leaves an
# ordinary `W<n>-<t>` directory that no row claims. The collector calls it due immediately; the
# check that decides whether to spawn a collector at all used to miss it, so the orphan stayed
# there through every later fork and discard.
mkdir -p "$WORLD_STORE/trash/W9999-1/sub"
touch "$WORLD_STORE/trash/W9999-1/sub/leftover"
"$WORLD" fs fork --from S1 --to "$SCRATCH/pr3orphan" > "$SCRATCH/pr3orphan.log" 2>&1
for _ in $(seq 60); do [ -e "$WORLD_STORE/trash/W9999-1" ] || break; sleep 0.25; done
if [ ! -e "$WORLD_STORE/trash/W9999-1" ]; then
    ok PR3 "a fork starts a worker for a row-less trash orphan"
else
    bad PR3 "a fork starts a worker for a row-less trash orphan"
    ls "$WORLD_STORE/trash" | sed 's/^/        /'
fi

# ---- PR #1 review (3rd round): gc leaves a fork that is still running alone ---------------------
# A CREATING row means "somebody is building this tree". gc read every one of them as "somebody
# WAS building this tree", so a collector spawned by one command and a clone started by another
# overlapped badly: the worker deleted the live fork's tree and its row, and the fork then
# "succeeded" with an UPDATE that matched nothing, leaving a directory at --to that no row knew
# about. The producer's pid is on the row now. The row here is written directly, because the only
# other way to hold a fork open for the length of a gc is to race it.
if command -v sqlite3 > /dev/null 2>&1; then
    mkdir -p "$SCRATCH/pr3live/.wfs-fork-live/sub"
    touch "$SCRATCH/pr3live/.wfs-fork-live/sub/half"
    sqlite3 "$WORLD_STORE/metadata.db" "INSERT INTO worlds(kind,parent_world,snapshot_id,name,path,state,created_at,tmp_path,owner_pid,owner_start) VALUES(1,0,1,'pr3live','$SCRATCH/pr3live/w',0,0,'$SCRATCH/pr3live/.wfs-fork-live',$$,0);" 2>/dev/null
    # created_at 0 and no minimum age: the producer being alive is the only thing protecting it.
    WORLD_GC_CREATING_MIN_AGE=0 "$WORLD" fs gc > /dev/null 2>&1
    if [ -e "$SCRATCH/pr3live/.wfs-fork-live/sub/half" ] &&
       [ "$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM worlds WHERE name='pr3live';")" = 0 ]; then
        ok PR3 "gc leaves a CREATING row alone while its producer is alive"
    else
        bad PR3 "gc leaves a CREATING row alone while its producer is alive"
    fi
    # And once the producer is gone, that same row and that same tree are collected.
    sqlite3 "$WORLD_STORE/metadata.db" "UPDATE worlds SET owner_pid=2147480000 WHERE name='pr3live';"
    WORLD_GC_CREATING_MIN_AGE=0 "$WORLD" fs gc > /dev/null 2>&1
    if [ ! -e "$SCRATCH/pr3live/.wfs-fork-live" ] &&
       [ "$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM worlds WHERE name='pr3live';")" = 3 ]; then
        ok PR3 "gc collects it once the producer is gone"
    else
        bad PR3 "gc collects it once the producer is gone"
    fi
fi

# ---- PR #1 review (5th round): a fork tree gc cannot remove keeps its row ----------------------
# A fork's temporary lives in the user's own target directory under a name drawn at random, and is
# deliberately never found by a suffix sweep -- the CREATING row is the only record of it. gc used
# to mark that row DEAD and clear tmp_path whether or not the tree actually went, so one EPERM
# stranded the whole clone permanently, with nothing left that knew its name. Same ACL as the
# undeletable-trash-entry case below: it survives chmod and chflags, so the tree really cannot go.
if command -v sqlite3 > /dev/null 2>&1; then
    STUCKTMP="$SCRATCH/pr5stuck/.wfs-fork-stuck"
    mkdir -p "$STUCKTMP/keep"
    echo x > "$STUCKTMP/keep/f.txt"
    sqlite3 "$WORLD_STORE/metadata.db" "INSERT INTO worlds(kind,parent_world,snapshot_id,name,path,state,created_at,tmp_path,owner_pid,owner_start) VALUES(1,0,1,'pr5stuck','$SCRATCH/pr5stuck/w',0,0,'$STUCKTMP',2147480000,0);" 2>/dev/null
    chmod +a "$(id -un) deny delete,delete_child,add_file" "$STUCKTMP/keep"
    out=$(WORLD_GC_CREATING_MIN_AGE=0 "$WORLD" fs gc 2>&1)
    STUCKSTATE=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM worlds WHERE name='pr5stuck';")
    STUCKPATH=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT tmp_path FROM worlds WHERE name='pr5stuck';")
    if [ -e "$STUCKTMP/keep/f.txt" ] && [ "$STUCKSTATE" = 0 ] && [ "$STUCKPATH" = "$STUCKTMP" ]; then
        ok PR5 "a fork tree gc cannot remove keeps its row and its tmp_path"
    else
        bad PR5 "a fork tree gc cannot remove keeps its row and its tmp_path (state $STUCKSTATE)"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "could not be removed" && ok PR5 "gc says so instead of reporting it collected" \
                                                 || { bad PR5 "gc says so instead of reporting it collected"; echo "$out" | sed 's/^/        /'; }
    if "$WORLD" fs gc --status | grep -q "^abandoned: *1 half-built fork tree"; then
        ok PR5 "gc --status counts the abandoned tree"
    else
        bad PR5 "gc --status counts the abandoned tree"; "$WORLD" fs gc --status | sed 's/^/        /'
    fi
    # Take the ACL away and the next wake finishes what it started: tree gone, row dead.
    chmod -N "$STUCKTMP/keep"
    WORLD_GC_CREATING_MIN_AGE=0 "$WORLD" fs gc > /dev/null 2>&1
    STUCKSTATE=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM worlds WHERE name='pr5stuck';")
    if [ ! -e "$STUCKTMP" ] && [ "$STUCKSTATE" = 3 ]; then
        ok PR5 "once it can be removed the next gc removes it and buries the row"
    else
        bad PR5 "once it can be removed the next gc removes it and buries the row (state $STUCKSTATE)"
    fi
    "$WORLD" fs gc --status | grep -q "^abandoned:" && bad PR5 "and stops counting it" \
                                                    || ok PR5 "and stops counting it"
fi

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

# ---- T1.5: the pre-clone pool ------------------------------------------------------------------
# An entry is a finished clone of S1 waiting under the store with no marker and no world row.
# A fork that finds one renames it into place; one that does not, clones as before.
"$WORLD" fs pool drain --all > /dev/null
"$WORLD" fs pool status | grep -q "^pool: empty" && ok T1.5 "pool status says when the pool is empty" \
                                                 || bad T1.5 "pool status says when the pool is empty"

# P7: the entries live inside the store, which is not a legal fork target.
check    P7 "forking into the pool directory is refused" 3 -- \
         "$WORLD" fs fork --from S1 --to "$WORLD_STORE/pool/S1/stolen"
has_hint P7 "that refusal explains why" "plain project directory" -- \
         "$WORLD" fs fork --from S1 --to "$WORLD_STORE/pool/S1/stolen"

# The miss path: an empty pool is not an error.
"$WORLD" fs fork --from S1 --to "$SCRATCH/w-miss" > "$SCRATCH/miss.log" 2>&1
if [ "$(cat "$SCRATCH/w-miss/hello.txt" 2>/dev/null)" = hello ] && ! grep -q "(pool)" "$SCRATCH/miss.log"; then
    ok T1.5 "a fork with an empty pool clones as before"
else
    bad T1.5 "a fork with an empty pool clones as before"; sed 's/^/        /' "$SCRATCH/miss.log"
fi

ready_S1() { "$WORLD" fs pool status | awk '$1=="S1"{print $3}'; }
"$WORLD" fs pool fill S1 --count 2 > "$SCRATCH/fill.log" 2>&1
[ "$(ready_S1)" = 2 ] && ok T1.5 "pool fill S1 --count 2 leaves 2 ready" \
                      || { bad T1.5 "pool fill S1 --count 2 leaves 2 ready"; sed 's/^/        /' "$SCRATCH/fill.log"; }
# fill is a top-up, not an addition
"$WORLD" fs pool fill S1 --count 2 > /dev/null 2>&1
[ "$(ready_S1)" = 2 ] && ok T1.5 "filling again to the same count makes nothing" \
                      || bad T1.5 "filling again to the same count makes nothing"

# The hit: the world IS one of those trees, renamed into place.
for d in "$WORLD_STORE"/pool/S1/*; do printf '%s %s\n' "$(basename "$d")" "$(stat -f %i "$d")"; done > "$SCRATCH/pool-before.txt"
"$WORLD" fs fork --from S1 --to "$SCRATCH/w-hit" > "$SCRATCH/hit.log" 2>&1
WINO=$(stat -f %i "$SCRATCH/w-hit" 2>/dev/null || echo 0)
HITDIR=$(awk -v i="$WINO" '$2==i{print $1}' "$SCRATCH/pool-before.txt")
if grep -q "(pool)" "$SCRATCH/hit.log" && [ -n "$HITDIR" ]; then
    ok T1.5 "a fork with a filled pool is served from the pool (same inode)"
else
    bad T1.5 "a fork with a filled pool is served from the pool (same inode)"; sed 's/^/        /' "$SCRATCH/hit.log"
fi
[ -n "$HITDIR" ] && [ ! -e "$WORLD_STORE/pool/S1/$HITDIR" ] \
    && ok T1.5 "the pool entry's directory is gone afterwards" \
    || bad T1.5 "the pool entry's directory is gone afterwards"
[ "$(cat "$SCRATCH/w-hit/hello.txt" 2>/dev/null)" = hello ] && [ -f "$SCRATCH/w-hit/.world" ] \
    && ok T1.5 "the handed-out world has the content and a marker" \
    || bad T1.5 "the handed-out world has the content and a marker"
"$WORLD" fs verify "$SCRATCH/w-hit" > /dev/null 2>&1 && ok T1.5 "it is a registered world (P1/P2)" \
                                                     || bad T1.5 "it is a registered world (P1/P2)"

# The hit re-fills the pool in a detached background process.
for _ in $(seq 40); do [ "$(ready_S1)" = 2 ] && break; sleep 0.25; done
[ "$(ready_S1)" = 2 ] && ok T1.5 "a hit triggers a background top-up back to 2" \
                      || bad T1.5 "a hit triggers a background top-up back to 2 (got $(ready_S1))"
[ -s "$WORLD_STORE/logs/pool.log" ] && ok T1.5 "the background filler logs to <store>/logs/pool.log" \
                                    || bad T1.5 "the background filler logs to <store>/logs/pool.log"
# --no-pool ignores a warm pool
"$WORLD" fs fork --from S1 --to "$SCRATCH/w-nopool" --no-pool > "$SCRATCH/nopool.log" 2>&1
grep -q "(pool)" "$SCRATCH/nopool.log" && bad T1.5 "--no-pool clones here and now" \
                                       || ok T1.5 "--no-pool clones here and now"

# verify S1 also checks what is waiting, and gc cleans up after a killed filler.
"$WORLD" fs verify S1 | grep -q "pool entries (0 touched)" && ok T1.5 "verify S1 reports the pool entries" \
                                                           || bad T1.5 "verify S1 reports the pool entries"
mkdir -p "$WORLD_STORE/pool/S1/dead.wfs-tmp" "$WORLD_STORE/pool/S1/0000000000000000"
before=$(ready_S1)
"$WORLD" fs gc --retention 7 > "$SCRATCH/gc-pool.log" 2>&1
if [ ! -e "$WORLD_STORE/pool/S1/dead.wfs-tmp" ] && [ ! -e "$WORLD_STORE/pool/S1/0000000000000000" ] &&
   [ "$(ready_S1)" = "$before" ]; then
    ok T1.5 "gc removes half-built and orphaned entries, keeps the ready ones"
else
    bad T1.5 "gc removes half-built and orphaned entries, keeps the ready ones"; sed 's/^/        /' "$SCRATCH/gc-pool.log"
fi
"$WORLD" fs pool drain S1 > /dev/null
"$WORLD" fs pool status | grep -q "^pool: empty" && ok T1.5 "pool drain empties it" \
                                                 || bad T1.5 "pool drain empties it"

# ---- M2 ----------------------------------------------------------------------------------------
# A store of its own, so the T2 cases cannot be confused by the nine worlds above.

S2="$SCRATCH/store2"
w2() { "$WORLD" --store "$S2" "$@"; }
P2DIR="$SCRATCH/proj2"
mkdir -p "$P2DIR/src"
for i in $(seq 1 40); do echo "line $i" > "$P2DIR/src/f$i.txt"; done
echo hello > "$P2DIR/hello.txt"
w2 fs init "$P2DIR" --name p2 > /dev/null || { echo "store2 init failed"; exit 1; }
w2 fs fork --from S1 --to "$SCRATCH/t-a" --no-pool > /dev/null
w2 fs fork --from S1 --to "$SCRATCH/t-b" --no-pool > /dev/null
w2 fs fork --from S1 --to "$SCRATCH/t-c" --no-pool > /dev/null

# ---- T2.1: discard is O(1), the deleting is somebody else's problem --------------------------
t0=$(python3 -c 'import time;print(int(time.time()*1000))')
w2 fs discard W1 > /dev/null
t1=$(python3 -c 'import time;print(int(time.time()*1000))')
if [ "$((t1 - t0))" -lt 500 ]; then ok T2.1 "discard of a 42-entry world is O(1) ($((t1-t0)) ms)"
else bad T2.1 "discard of a 42-entry world is O(1) ($((t1-t0)) ms)"; fi
# Nothing was unlinked: the tree is still whole, in the trash.
TRASHED=$(ls -d "$S2"/trash/W1-* 2>/dev/null | head -1)
if [ -n "$TRASHED" ] && [ -f "$TRASHED/hello.txt" ]; then ok T2.1 "the tree is intact in <store>/trash"
else bad T2.1 "the tree is intact in <store>/trash"; fi
w2 fs gc --status | grep -q "^trash: *1 entries" && ok T2.1 "gc --status counts the trash" \
                                                 || { bad T2.1 "gc --status counts the trash"; w2 fs gc --status | sed 's/^/        /'; }
w2 fs gc --status | grep -qE "contents: *42 tree entries, ~" && ok T2.1 "gc --status reports entries and an estimated size" \
                                                             || { bad T2.1 "gc --status reports entries and an estimated size"; w2 fs gc --status | sed 's/^/        /'; }
w2 fs gc --status | grep -q "^worker: *none" && ok T2.1 "gc --status says when no worker is running" \
                                             || bad T2.1 "gc --status says when no worker is running"
# Inside the retention window nothing is due and no worker is started.
w2 fs gc > "$SCRATCH/gc1.log" 2>&1
grep -q "background" "$SCRATCH/gc1.log" && bad T2.1 "gc starts no worker when nothing is due" \
                                        || ok T2.1 "gc starts no worker when nothing is due"
w2 fs restore W1 > /dev/null && [ -f "$SCRATCH/t-a/hello.txt" ] \
    && ok T2.1 "a world inside the retention window still restores" \
    || bad T2.1 "a world inside the retention window still restores"

# The background collector: `gc` hands the trash over and returns, the worker empties it.
w2 fs discard W1 > /dev/null
w2 fs gc --retention 0 > "$SCRATCH/gc2.log" 2>&1
grep -q "emptied in the background" "$SCRATCH/gc2.log" && ok T2.1 "gc hands due trash to the background worker" \
                                                       || { bad T2.1 "gc hands due trash to the background worker"; sed 's/^/        /' "$SCRATCH/gc2.log"; }
for _ in $(seq 60); do [ -z "$(ls "$S2"/trash 2>/dev/null)" ] && break; sleep 0.25; done
[ -z "$(ls "$S2"/trash 2>/dev/null)" ] && ok T2.1 "the worker empties the trash on its own" \
                                       || { bad T2.1 "the worker empties the trash on its own"; ls "$S2/trash" | sed 's/^/        /'; }
w2 fs list | grep -q "^W1 .*trashed" && bad T2.1 "the collected world is marked dead" \
                                     || ok T2.1 "the collected world is marked dead"
[ -s "$S2/logs/gc.log" ] && ok T2.1 "the worker logs to <store>/logs/gc.log" \
                         || bad T2.1 "the worker logs to <store>/logs/gc.log"
grep -q "entries unlinked" "$S2/logs/gc.log" && ok T2.1 "the log says how many entries it unlinked" \
                                             || { bad T2.1 "the log says how many entries it unlinked"; sed 's/^/        /' "$S2/logs/gc.log"; }

# Crash safety: a tree renamed to *.deleting is not a world any more, whatever the row says.
w2 fs discard W2 > /dev/null
TRASHED=$(ls -d "$S2"/trash/W2-* 2>/dev/null | head -1)
mv "$TRASHED" "$TRASHED$( : ).deleting"
check    T2.1 "restore refuses a trash entry that is being deleted" 3 -- w2 fs restore W2
has_hint T2.1 "that refusal says what to do instead" "fork" -- w2 fs restore W2
w2 fs gc --now --retention 0 > /dev/null 2>&1
[ -z "$(ls "$S2"/trash 2>/dev/null)" ] && ok T2.1 "the next gc finishes the interrupted deletion" \
                                       || { bad T2.1 "the next gc finishes the interrupted deletion"; ls "$S2/trash" | sed 's/^/        /'; }

# One collector per store: a worker that cannot take <store>/locks/gc.lock does nothing at all.
w2 fs fork --from S1 --to "$SCRATCH/t-d" --no-pool > /dev/null
w2 fs discard W4 > /dev/null
python3 -c 'import fcntl,sys,time; f=open(sys.argv[1],"a+"); fcntl.flock(f,fcntl.LOCK_EX); time.sleep(4)' \
    "$S2/locks/gc.lock" &
HOLDER=$!
sleep 0.5
w2 fs gc --worker --retention 0 > "$SCRATCH/gcw.log" 2>&1
if [ -n "$(ls "$S2"/trash 2>/dev/null)" ] && [ ! -s "$SCRATCH/gcw.log" ]; then
    ok T2.1 "a second worker exits on the store's gc lock without touching the trash"
else
    bad T2.1 "a second worker exits on the store's gc lock without touching the trash"
    ls "$S2/trash" | sed 's/^/        /'; sed 's/^/        /' "$SCRATCH/gcw.log"
fi
kill "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null
w2 fs gc --now --retention 0 > /dev/null 2>&1

# ---- T2.2: discarding a snapshot ---------------------------------------------------------------
check    T2.2 "discard S<n> is refused while an active world needs it" 3 -- w2 fs discard S1
out=$(w2 fs discard S1 2>&1)
echo "$out" | grep -qE "source of [0-9]+ active world\(s\) \(W[0-9]+" && ok T2.2 "that refusal names the worlds holding it" \
                                                                       || { bad T2.2 "that refusal names the worlds holding it"; echo "$out" | sed 's/^/        /'; }
has_hint  T2.2 "the refusal offers checkpoint as the way out" "checkpoint" -- w2 fs discard S1
# --force must NOT be a way to orphan a live world's source.
check    T2.2 "--force does not override a live world" 3 -- w2 fs discard S1 --force
# Take the worlds out of the way, keeping one in the trash: a trashed world is not a reason to refuse.
w2 fs discard W5 > /dev/null 2>&1
w2 fs discard W3 > /dev/null 2>&1   # stays in the trash, inside the retention window
w2 fs pool fill S1 --count 1 > /dev/null 2>&1
check    T2.2 "a pool entry alone still refuses" 3 -- w2 fs discard S1
has_hint T2.2 "that refusal offers --force" "--force" -- w2 fs discard S1
check    T2.2 "--force drains the pool and discards" 0 -- w2 fs discard S1 --force
w2 fs status | grep -qE "^snapshots: .*1 trashed" && ok T2.2 "fs status counts the trashed snapshot" \
                                                  || { bad T2.2 "fs status counts the trashed snapshot"; w2 fs status | sed 's/^/        /'; }
w2 fs pool status | grep -q "^pool: empty" && ok T2.2 "--force drained its pool entries" \
                                           || bad T2.2 "--force drained its pool entries"
# A world in the trash whose source is gone cannot come back.
check    T2.2 "restoring a world whose snapshot was discarded is refused" 3 -- w2 fs restore W3
out=$(w2 fs restore W3 2>&1)
echo "$out" | grep -q "no baseline" && ok T2.2 "that refusal explains why (no baseline to diff against)" \
                                   || { bad T2.2 "that refusal explains why"; echo "$out" | sed 's/^/        /'; }
# The gate is 0000; the deleter has to reopen it or nothing below it can be unlinked.
SNAPTRASH=$(ls -d "$S2"/trash/S1-* 2>/dev/null | head -1)
[ -n "$SNAPTRASH" ] && [ "$(stat -f '%Lp' "$SNAPTRASH/root")" = "0" ] \
    && ok T2.2 "the discarded snapshot is still gated in the trash" \
    || bad T2.2 "the discarded snapshot is still gated in the trash"
w2 fs gc --now --retention 0 > "$SCRATCH/gc3.log" 2>&1
[ ! -e "$SNAPTRASH" ] && ok T2.2 "gc deletes the trashed snapshot through the closed gate" \
                      || { bad T2.2 "gc deletes the trashed snapshot through the closed gate"; sed 's/^/        /' "$SCRATCH/gc3.log"; }

# ---- T2.2: reconciliation ------------------------------------------------------------------------
w2 fs init "$P2DIR" --name recon > /dev/null           # S2
w2 fs fork --from S2 --to "$SCRATCH/t-e" --no-pool > /dev/null
SNAP2=$(w2 fs inspect S2 | awk '/^path:/{print $2}')
chmod 0700 "$SNAP2"; rm -rf "${SNAP2%/root}"           # the tree vanishes behind the store's back
rm -rf "$SCRATCH/t-e"                                  # and so does a world
w2 fs gc --retention 7 > "$SCRATCH/rec1.log" 2>&1
grep -q "1 snapshot(s) and 1 world(s)" "$SCRATCH/rec1.log" && ok T2.2 "gc reports dangling rows without touching them" \
                                                           || { bad T2.2 "gc reports dangling rows without touching them"; sed 's/^/        /' "$SCRATCH/rec1.log"; }
w2 fs status | grep -q "^dangling:" && ok T2.2 "fs status reports them too" \
                                    || { bad T2.2 "fs status reports them too"; w2 fs status | sed 's/^/        /'; }
w2 fs list | grep -q "^S2" && ok T2.2 "a plain gc leaves the dangling rows alone" \
                           || bad T2.2 "a plain gc leaves the dangling rows alone"
w2 fs gc --reconcile --retention 7 > "$SCRATCH/rec2.log" 2>&1
grep -q "reconciled: 1 snapshots and 1 worlds" "$SCRATCH/rec2.log" && ok T2.2 "gc --reconcile marks them dead" \
                                                                   || { bad T2.2 "gc --reconcile marks them dead"; sed 's/^/        /' "$SCRATCH/rec2.log"; }
w2 fs list | grep -q "^S2" && bad T2.2 "the dead snapshot is gone from the listing" \
                           || ok T2.2 "the dead snapshot is gone from the listing"
w2 fs status | grep -q "^dangling:" && bad T2.2 "nothing dangles afterwards" \
                                    || ok T2.2 "nothing dangles afterwards"

# ---- T2.3: the world marker carries its store's path ---------------------------------------------
# `-o` options do not reach the FSKit extension on macOS 27, so this is how a mount finds the
# store that owns the world it is being pointed at.
w2 fs init "$P2DIR" --name t23 > /dev/null             # S3
w2 fs fork --from S3 --to "$SCRATCH/t-f" --no-pool > /dev/null
python3 - "$SCRATCH/t-f/.world" "$S2" <<'EOF' && ok T2.3 "a forked world's .world names its store path" \
                                              || bad T2.3 "a forked world's .world names its store path"
import json,sys,os
m=json.load(open(sys.argv[1]))
sys.exit(0 if os.path.realpath(m.get("store_path","")) == os.path.realpath(sys.argv[2]) else 1)
EOF
# ... and so does one handed out by the pool, which takes a different code path.
w2 fs pool fill S3 --count 1 > /dev/null 2>&1
WORLD_POOL_TOPUP=0 w2 fs fork --from S3 --to "$SCRATCH/t-g" > "$SCRATCH/t23.log" 2>&1
if grep -q "(pool)" "$SCRATCH/t23.log" && python3 - "$SCRATCH/t-g/.world" "$S2" <<'EOF'
import json,sys,os
m=json.load(open(sys.argv[1]))
sys.exit(0 if os.path.realpath(m.get("store_path","")) == os.path.realpath(sys.argv[2]) else 1)
EOF
then ok T2.3 "a pool-served world's .world names its store path too"
else bad T2.3 "a pool-served world's .world names its store path too"; sed 's/^/        /' "$SCRATCH/t23.log"; fi

# ---- T2.5 (P9): hardlinks inside a tree are rebuilt inside every clone of it --------------------
# $PROJ/src/a.c and $PROJ/src/a-link.c are one file with two names (line 56). clonefile breaks
# that (CLONE_MODEL_MACOS27 §11); the snapshot's manifest carries the group and every clone --
# fork, pool entry, checkpoint -- gets it back.
"$WORLD" fs fork --from S1 --to "$SCRATCH/w-hl" --no-pool > /dev/null 2>&1
hi_a=$(stat -f %i "$SCRATCH/w-hl/src/a.c" 2>/dev/null)
hi_b=$(stat -f %i "$SCRATCH/w-hl/src/a-link.c" 2>/dev/null)
hl_n=$(stat -f %l "$SCRATCH/w-hl/src/a.c" 2>/dev/null)
if [ -n "$hi_a" ] && [ "$hi_a" = "$hi_b" ] && [ "$hl_n" = 2 ]; then
    ok T2.5 "fork: the two names are one file again (same inode, 2 links)"
else
    bad T2.5 "fork: the two names are one file again (inodes $hi_a/$hi_b, $hl_n links)"
fi
echo relinked > "$SCRATCH/w-hl/src/a-link.c"
[ "$(cat "$SCRATCH/w-hl/src/a.c")" = relinked ] && ok T2.5 "a write through one name is visible through the other" \
                                                || bad T2.5 "a write through one name is visible through the other"
# The pool hands out a clone that was made minutes ago: the rebuild happens when it is filled,
# so the hand-out stays a marker plus a rename.
"$WORLD" fs pool fill S1 --count 1 > /dev/null 2>&1
WORLD_POOL_TOPUP=0 "$WORLD" fs fork --from S1 --to "$SCRATCH/w-hl-pool" > "$SCRATCH/hlpool.log" 2>&1
pi_a=$(stat -f %i "$SCRATCH/w-hl-pool/src/a.c" 2>/dev/null)
pi_b=$(stat -f %i "$SCRATCH/w-hl-pool/src/a-link.c" 2>/dev/null)
if grep -q "(pool)" "$SCRATCH/hlpool.log" && [ -n "$pi_a" ] && [ "$pi_a" = "$pi_b" ]; then
    ok T2.5 "a pool-served world has its hardlinks too"
else
    bad T2.5 "a pool-served world has its hardlinks too"; sed 's/^/        /' "$SCRATCH/hlpool.log"
fi
# A group whose other name lives outside the tree cannot be rebuilt -- there is nothing inside
# the clone to link to -- so init says so and the fork gets independent copies.
mkdir -p "$SCRATCH/extproj"
echo shared > "$SCRATCH/extproj/shared.txt"
ln "$SCRATCH/extproj/shared.txt" "$SCRATCH/outside.txt"
out=$("$WORLD" fs init "$SCRATCH/extproj" --name extproj 2>&1)
EXTSNAP=$(echo "$out" | awk '/^S[0-9]+ /{print $1}')
if echo "$out" | grep -q "outside this tree"; then ok T2.5 "init warns about the links it cannot rebuild"
else bad T2.5 "init warns about the links it cannot rebuild"; echo "$out" | sed 's/^/        /'; fi
"$WORLD" fs fork --from "$EXTSNAP" --to "$SCRATCH/w-ext" --no-pool > /dev/null 2>&1
if [ "$(stat -f %l "$SCRATCH/w-ext/shared.txt" 2>/dev/null)" = 1 ] &&
   [ "$(cat "$SCRATCH/w-ext/shared.txt")" = shared ]; then
    ok T2.5 "a group reaching outside the tree stays an independent copy"
else
    bad T2.5 "a group reaching outside the tree stays an independent copy"
fi

# ---- PR #1 review (P1): `.wfs-tmp` is a perfectly legal file name in somebody's workspace -----
# The hardlink replay used to build its temporary link at `<name>.wfs-tmp` and, on EEXIST,
# unlink whatever was already there. A tree with `a` and `b` (one file, two names) and an
# ordinary `b.wfs-tmp` beside them therefore lost that third file on every fork. The replay now
# draws a name of its own and never unlinks anything that is not its own.
mkdir -p "$SCRATCH/tmpname"
echo linked > "$SCRATCH/tmpname/a"
ln "$SCRATCH/tmpname/a" "$SCRATCH/tmpname/b"
echo "mine, not a temporary" > "$SCRATCH/tmpname/b.wfs-tmp"
echo "nor is this one" > "$SCRATCH/tmpname/a.wfs-tmp"
out=$("$WORLD" fs init "$SCRATCH/tmpname" --name tmpname 2>&1)
TNSNAP=$(echo "$out" | awk '/^S[0-9]+ /{print $1}')
"$WORLD" fs fork --from "$TNSNAP" --to "$SCRATCH/w-tmpname" --no-pool > /dev/null 2>&1
ti_a=$(stat -f %i "$SCRATCH/w-tmpname/a" 2>/dev/null)
ti_b=$(stat -f %i "$SCRATCH/w-tmpname/b" 2>/dev/null)
ti_n=$(stat -f %l "$SCRATCH/w-tmpname/a" 2>/dev/null)
if [ -n "$ti_a" ] && [ "$ti_a" = "$ti_b" ] && [ "$ti_n" = 2 ]; then
    ok PR1 "a and b are one file again beside a user's own .wfs-tmp"
else
    bad PR1 "a and b are one file again beside a user's own .wfs-tmp (inodes $ti_a/$ti_b, $ti_n links)"
fi
if [ "$(cat "$SCRATCH/w-tmpname/b.wfs-tmp" 2>/dev/null)" = "mine, not a temporary" ] &&
   [ "$(cat "$SCRATCH/w-tmpname/a.wfs-tmp" 2>/dev/null)" = "nor is this one" ]; then
    ok PR1 "the user's own *.wfs-tmp files come through the fork intact"
else
    bad PR1 "the user's own *.wfs-tmp files come through the fork intact"
    ls -la "$SCRATCH/w-tmpname" | sed 's/^/        /'
fi
if [ -z "$(ls -a "$SCRATCH/w-tmpname" 2>/dev/null | grep '^\.wfs-hl-')" ]; then
    ok PR1 "and the replay leaves no temporary of its own behind"
else
    bad PR1 "and the replay leaves no temporary of its own behind"
    ls -a "$SCRATCH/w-tmpname" | sed 's/^/        /'
fi

# ---- PR #1 review (4th round, P2): a hardlink group under a read-only directory ----------------
# 0555 is an ordinary mode for a vendored tree, a generated fixture, a `chmod -R a-w` release
# directory. The clone wears it too, so the replay's link(2)/rename(2) inside it came back EACCES
# -- and the result was ignored, so the snapshot was published with a manifest and a row
# advertising a group its tree did not have, and every fork and pool entry inherited it. The
# directory is now lent owner write for exactly those two calls and given its exact mode back.
mkdir -p "$SCRATCH/roproj/ro"
echo "read only" > "$SCRATCH/roproj/ro/x"
ln "$SCRATCH/roproj/ro/x" "$SCRATCH/roproj/ro/y"
chmod 0555 "$SCRATCH/roproj/ro"
out=$("$WORLD" fs init "$SCRATCH/roproj" --name roproj 2>&1)
ROSNAP=$(echo "$out" | awk '/^S[0-9]+ /{print $1}')
ROPATH=$("$WORLD" fs inspect "$ROSNAP" | awk '/^path:/{print $2}')
# The snapshot's own tree first, behind the gate.
chmod 0700 "$ROPATH" 2>/dev/null
si_x=$(stat -f %i "$ROPATH/ro/x" 2>/dev/null)
si_y=$(stat -f %i "$ROPATH/ro/y" 2>/dev/null)
si_n=$(stat -f %l "$ROPATH/ro/x" 2>/dev/null)
si_m=$(stat -f %Lp "$ROPATH/ro" 2>/dev/null)
chmod 0 "$ROPATH" 2>/dev/null
if [ -n "$si_x" ] && [ "$si_x" = "$si_y" ] && [ "$si_n" = 2 ]; then
    ok PR1 "the snapshot rebuilds a hardlink pair inside a 0555 directory"
else
    bad PR1 "the snapshot rebuilds a hardlink pair inside a 0555 directory (inodes $si_x/$si_y, $si_n links)"
fi
[ "$si_m" = 555 ] && ok PR1 "and the snapshot's directory is 0555 again afterwards" \
                  || bad PR1 "and the snapshot's directory is 0555 again afterwards (mode $si_m)"
"$WORLD" fs fork --from "$ROSNAP" --to "$SCRATCH/w-ro" --no-pool > /dev/null 2>&1
ri_x=$(stat -f %i "$SCRATCH/w-ro/ro/x" 2>/dev/null)
ri_y=$(stat -f %i "$SCRATCH/w-ro/ro/y" 2>/dev/null)
ri_n=$(stat -f %l "$SCRATCH/w-ro/ro/x" 2>/dev/null)
ri_m=$(stat -f %Lp "$SCRATCH/w-ro/ro" 2>/dev/null)
if [ -n "$ri_x" ] && [ "$ri_x" = "$ri_y" ] && [ "$ri_n" = 2 ]; then
    ok PR1 "the fork rebuilds it too"
else
    bad PR1 "the fork rebuilds it too (inodes $ri_x/$ri_y, $ri_n links)"
fi
[ "$ri_m" = 555 ] && ok PR1 "and the world's directory is 0555 again afterwards" \
                  || bad PR1 "and the world's directory is 0555 again afterwards (mode $ri_m)"
[ -z "$(ls -a "$SCRATCH/w-ro/ro" 2>/dev/null | grep '^\.wfs-hl-')" ] \
    && ok PR1 "and no temporary of the replay's own is left in it" \
    || { bad PR1 "and no temporary of the replay's own is left in it"; ls -a "$SCRATCH/w-ro/ro" | sed 's/^/        /'; }
# And through the pool, whose background filler does the same replay on the entry it clones.
"$WORLD" fs pool fill "$ROSNAP" --count 1 > /dev/null 2>&1
WORLD_POOL_TOPUP=0 "$WORLD" fs fork --from "$ROSNAP" --to "$SCRATCH/w-ro-pool" > "$SCRATCH/ropool.log" 2>&1
pri_x=$(stat -f %i "$SCRATCH/w-ro-pool/ro/x" 2>/dev/null)
pri_y=$(stat -f %i "$SCRATCH/w-ro-pool/ro/y" 2>/dev/null)
pri_m=$(stat -f %Lp "$SCRATCH/w-ro-pool/ro" 2>/dev/null)
if grep -q "(pool)" "$SCRATCH/ropool.log" && [ -n "$pri_x" ] && [ "$pri_x" = "$pri_y" ] && [ "$pri_m" = 555 ]; then
    ok PR1 "a pool-served world has it as well, 0555 and all"
else
    bad PR1 "a pool-served world has it as well, 0555 and all (inodes $pri_x/$pri_y, mode $pri_m)"
    sed 's/^/        /' "$SCRATCH/ropool.log"
fi

# ---- P17: trees in the store but no database -> refuse, never rebuild -----------------------
# Its own store, because the point of the rule is that the store is left exactly as it was
# found. Snapshot and world ids live in metadata.db; a fresh one hands out 1 again and the next
# `init` writes S1 on top of the snapshots/S1 that is still on disk.
P17STORE="$SCRATCH/p17-store"
mkdir -p "$SCRATCH/p17-src"
echo one > "$SCRATCH/p17-src/a.txt"
"$WORLD" --store "$P17STORE" fs init "$SCRATCH/p17-src" --name p17 > /dev/null 2>&1
# The gate is opened first so that the scratch cleanup can get rid of the tree afterwards.
# Finding the tree does not need it: a readdir of snapshots/ never steps inside S1.
chmod 0700 "$P17STORE/snapshots/S1/root" 2>/dev/null
rm -f "$P17STORE"/metadata.db "$P17STORE"/metadata.db-wal "$P17STORE"/metadata.db-shm
check    P17 "a store with snapshots but no metadata.db is refused" 3 -- \
         "$WORLD" --store "$P17STORE" fs status
has_hint P17 "the refusal says restore the db or move the store aside" "restore metadata.db" -- \
         "$WORLD" --store "$P17STORE" fs list
[ ! -e "$P17STORE/metadata.db" ] && ok P17 "the refused open left no new database behind" \
                                || bad P17 "the refused open left no new database behind"
out=$("$WORLD" --store "$P17STORE" fs gc --reconcile 2>&1)
if echo "$out" | grep -q "gc --reconcile.*cannot help"; then
    ok P17 "the refusal explains that gc --reconcile cannot repair this"
else
    bad P17 "the refusal explains that gc --reconcile cannot repair this"; echo "$out" | sed 's/^/        /'
fi
echo "this is not a database" > "$P17STORE/metadata.db"
check P17 "a metadata.db that is not a database is refused too" 3 -- \
      "$WORLD" --store "$P17STORE" fs status
rm -f "$P17STORE/metadata.db"
[ -f "$P17STORE/snapshots/S1/root/a.txt" ] && ok P17 "the snapshot tree is left exactly as it was" \
                                          || bad P17 "the snapshot tree is left exactly as it was"
check P17 "an empty store directory is new, not damaged" 0 -- \
      "$WORLD" --store "$SCRATCH/p17-fresh" fs status

# ---- PR #1 review (P2): `discard S<n> --now` deletes, it does not just move -------------------
# A store of its own: the case is a snapshot nobody references, and the stores above are full of
# worlds holding theirs.
RSTORE="$SCRATCH/review-store"
rv() { "$WORLD" --store "$RSTORE" "$@"; }
mkdir -p "$SCRATCH/review-src/sub"
echo one > "$SCRATCH/review-src/a.txt"
echo two > "$SCRATCH/review-src/sub/b.txt"
rv fs init "$SCRATCH/review-src" --name rv1 > /dev/null 2>&1
out=$(rv fs discard S1 --now 2>&1)
if [ ! -e "$RSTORE/snapshots/S1" ] && [ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ]; then
    ok PR1 "discard S<n> --now deletes the snapshot tree before it returns"
else
    bad PR1 "discard S<n> --now deletes the snapshot tree before it returns"
    echo "$out" | sed 's/^/        /'; ls "$RSTORE/trash" 2>/dev/null | sed 's/^/        /'
fi
echo "$out" | grep -q "^S1 deleted" && ok PR1 "it says the snapshot is deleted, not collected later" \
                                    || { bad PR1 "it says the snapshot is deleted, not collected later"; echo "$out" | sed 's/^/        /'; }
rv fs gc --status | grep -q "^trash: *0 entries" && ok PR1 "and leaves nothing in the trash" \
                                                 || { bad PR1 "and leaves nothing in the trash"; rv fs gc --status | sed 's/^/        /'; }
# Without --now it still only moves the tree into the trash.
rv fs init "$SCRATCH/review-src" --name rv2 > /dev/null 2>&1
rv fs discard S2 > /dev/null 2>&1
[ -n "$(ls -d "$RSTORE"/trash/S2-* 2>/dev/null)" ] && ok PR1 "without --now the snapshot only moves to the trash" \
                                                   || bad PR1 "without --now the snapshot only moves to the trash"
# ---- PR #1 review (5th round, P2): --now finishes a snapshot that is already in the trash -----
# `discard S<n>` then `discard S<n> --now` used to be a refusal, so the only way to get the space
# back before the retention period was a store-wide gc. A world has always accepted exactly this.
out=$(rv fs discard S2 --now 2>&1)
if [ -z "$(ls -d "$RSTORE"/trash/S2-* 2>/dev/null)" ] && echo "$out" | grep -q "^S2 deleted"; then
    ok PR5 "discard S<n> --now finishes a snapshot that was already trashed"
else
    bad PR5 "discard S<n> --now finishes a snapshot that was already trashed"
    echo "$out" | sed 's/^/        /'; ls "$RSTORE/trash" 2>/dev/null | sed 's/^/        /'
fi
rv fs inspect S2 | grep -q "^state: *dead" && ok PR5 "and the row is dead, not still trashed" \
                                           || { bad PR5 "and the row is dead, not still trashed"; rv fs inspect S2 | sed 's/^/        /'; }
rv fs gc --status | grep -q "^trash: *0 entries" && ok PR5 "and the trash is empty again" \
                                                 || { bad PR5 "and the trash is empty again"; rv fs gc --status | sed 's/^/        /'; }
# Without --now it is still a refusal, and the refusal now says what --now is for.
rv fs init "$SCRATCH/review-src" --name rv3 > /dev/null 2>&1
rv fs discard S3 > /dev/null 2>&1
check    PR5 "a second discard of a trashed snapshot is still refused" 3 -- rv fs discard S3
has_hint PR5 "that refusal offers --now" "discard S<n> --now" -- rv fs discard S3
rv fs discard S3 --now > /dev/null 2>&1

# ---- PR #1 review (6th round, P1): `adopt` may not resurrect a discarded baseline -------------
# An unregistered copy carries the marker of the world it was copied from, snapshot id included,
# and `adopt` used to write that id onto a new ACTIVE row whatever had become of the snapshot.
# Discard the only world, discard the snapshot (nothing references it any more), adopt the copy:
# the result was a world whose every `diff` answers "source gone" and a store that had forgotten
# the snapshot was ever anybody's source.
rv fs init "$SCRATCH/review-src" --name rv4 > /dev/null 2>&1
RW=$(rv fs fork --from S4 --to "$SCRATCH/rv4-world" 2>/dev/null | awk '{print $1}')
cp -R "$SCRATCH/rv4-world" "$SCRATCH/rv4-copy"
rv fs discard "$RW" --now > /dev/null 2>&1
rv fs discard S4 --now > /dev/null 2>&1
check    PR6 "adopting a copy whose snapshot has been discarded is refused" 3 -- rv fs adopt "$SCRATCH/rv4-copy"
has_hint PR6 "the refusal offers the marker-less way to keep it" "world fs init" -- rv fs adopt "$SCRATCH/rv4-copy"
rv fs status | grep -q "^worlds: *0 active" && ok PR6 "and no world row was written for it" \
                                             || { bad PR6 "and no world row was written for it"; rv fs status | sed 's/^/        /'; }

# ---- PR #1 review (6th round, P2): a damaged hardlink manifest is not "no hardlinks" ----------
# `hl_groups` on the row says how many groups of names share an inode; the manifest's own section
# says which names. clonefile(2) breaks every one of those links, so the replay of that section
# is the only thing that puts them back -- and a manifest that will not read, or that holds fewer
# groups than the row claims, used to be read as "nothing to replay". The fork then published a
# tree with independent files where the snapshot records one inode under two names.
mkdir -p "$SCRATCH/hl-src"
echo linked > "$SCRATCH/hl-src/a.txt"
ln "$SCRATCH/hl-src/a.txt" "$SCRATCH/hl-src/b.txt"
rv fs init "$SCRATCH/hl-src" --name hl > /dev/null 2>&1
HMAN="$RSTORE/snapshots/S5/manifest"
[ "$(grep -c '^hl ' "$HMAN" 2>/dev/null)" = 2 ] && ok PR6 "the snapshot manifest records the hardlink group" \
                                                 || { bad PR6 "the snapshot manifest records the hardlink group"; cat "$HMAN" | sed 's/^/        /'; }
grep -v '^hl ' "$HMAN" > "$SCRATCH/hl-man.stripped" && cat "$SCRATCH/hl-man.stripped" > "$HMAN"
check    PR6 "a fork from a snapshot whose manifest lost its groups is refused" 3 -- rv fs fork --from S5 --to "$SCRATCH/hl-w"
has_hint PR6 "the refusal sends you to verify" "world fs verify S5" -- rv fs fork --from S5 --to "$SCRATCH/hl-w"
[ ! -e "$SCRATCH/hl-w" ] && ok PR6 "and nothing was published at --to" \
                          || bad PR6 "and nothing was published at --to"
check    PR6 "verify reports the damaged manifest" 3 -- rv fs verify S5
check    PR6 "pool fill refuses it too" 3 -- rv fs pool fill S5 --count 1
rv fs pool status | grep -q "^pool: empty" && ok PR6 "and leaves no ready entry behind" \
                                            || { bad PR6 "and leaves no ready entry behind"; rv fs pool status | sed 's/^/        /'; }

rv fs gc --now --retention 0 > /dev/null 2>&1

# ---- PR #1 review (6th round, P2): --now follows a collector that has already renamed ---------
# The collector's first step is one rename, `W<n>-<t>` -> `W<n>-<t>.deleting`, and it records the
# new name in a second step. In between -- and after any interruption in that window -- the row
# still names the tree by the name it no longer has. `discard W<n> --now` read the resulting
# -ENOENT as "already gone", marked the row DEAD and returned: the space it promised back was
# still on disk, under a name the row no longer mentioned. Here that window is reproduced by
# doing the collector's rename by hand.
rv fs init "$SCRATCH/review-src" --name rv6 > /dev/null 2>&1
RW6=$(rv fs fork --from S6 --to "$SCRATCH/rv6-world" 2>/dev/null | awk '{print $1}')
rv fs discard "$RW6" > /dev/null 2>&1
TE=$(ls -d "$RSTORE"/trash/W*-* 2>/dev/null | head -1)
mv "$TE" "$TE.deleting"
out=$(rv fs discard "$RW6" --now 2>&1); rc=$?
[ "$rc" = 0 ] && ok PR6 "--now on an entry the collector has renamed still returns 0" \
               || { bad PR6 "--now on an entry the collector has renamed still returns 0 (exit $rc)"; echo "$out" | sed 's/^/        /'; }
[ ! -e "$TE.deleting" ] && ok PR6 "and the tree is really gone when it returns" \
                         || { bad PR6 "and the tree is really gone when it returns"; ls "$RSTORE/trash" | sed 's/^/        /'; }
[ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && ok PR6 "and the trash is empty" \
                                           || { bad PR6 "and the trash is empty"; ls "$RSTORE/trash" | sed 's/^/        /'; }
rv fs inspect "$RW6" | grep -q "^state: *dead" && ok PR6 "and the row is dead" \
                                               || { bad PR6 "and the row is dead"; rv fs inspect "$RW6" | sed 's/^/        /'; }

# ---- PR #1 review (P2): the batch limit bites inside one tree, not only between trees ----------
# One trash entry of 120k entries -- more than a second of unlinking on this machine -- and a
# one-second batch. The wake has to come back on time with the tree half deleted, hand over to a
# successor, and the chain has to finish the job on its own.
BIG="$RSTORE/trash/big-1"
mkdir -p "$BIG"
python3 - "$BIG" <<'EOF'
import os, sys
base = sys.argv[1]
for d in range(400):
    p = os.path.join(base, 'd%03d' % d)
    os.makedirs(p, exist_ok=True)
    for i in range(300):
        os.close(os.open(os.path.join(p, 'f%03d' % i), os.O_CREAT | os.O_WRONLY, 0o644))
EOF
t0=$(python3 -c 'import time;print(int(time.time()*1000))')
WORLD_GC_PAUSE_MS=0 WORLD_GC_BATCH_SECS=1 rv fs gc --worker --retention 0 > "$SCRATCH/gcbig.log" 2>&1
t1=$(python3 -c 'import time;print(int(time.time()*1000))')
if [ "$((t1 - t0))" -lt 4000 ]; then ok PR1 "a one-second gc wake returns on time mid-tree ($((t1-t0)) ms)"
else bad PR1 "a one-second gc wake returns on time mid-tree ($((t1-t0)) ms)"; sed 's/^/        /' "$SCRATCH/gcbig.log"; fi
grep -q "work remains, handing over" "$SCRATCH/gcbig.log" && ok PR1 "it reports the work it did not get to" \
                                                          || { bad PR1 "it reports the work it did not get to"; sed 's/^/        /' "$SCRATCH/gcbig.log"; }
LEFT=$(find "$RSTORE/trash" 2>/dev/null | wc -l | tr -d ' ')
if [ -d "$BIG.deleting" ] && [ "$LEFT" -gt 1 ] && [ "$LEFT" -lt 120401 ]; then
    ok PR1 "the half-deleted tree keeps its .deleting name ($LEFT entries left)"
else
    bad PR1 "the half-deleted tree keeps its .deleting name ($LEFT entries left)"
fi
for _ in $(seq 120); do [ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && break; sleep 0.25; done
[ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && ok PR1 "the successor chain finishes the tree" \
                                            || { bad PR1 "the successor chain finishes the tree"; ls "$RSTORE/trash" | sed 's/^/        /'; }

# ---- PR #1 review (P2): a trash entry that cannot be deleted never reads as an empty trash ----
# An ACL that denies the owner delete_child survives both chmod and chflags, which is exactly what
# the deleter tries when unlink(2) comes back EPERM -- so this entry really cannot be removed.
STUCK="$RSTORE/trash/stuck-1"
mkdir -p "$STUCK/keep"
echo x > "$STUCK/keep/f.txt"
echo y > "$STUCK/other.txt"
chmod +a "$(id -un) deny delete,delete_child,add_file" "$STUCK/keep"
out=$(rv fs gc --now --retention 0 2>&1)
[ -d "$STUCK.deleting/keep" ] && ok PR1 "an undeletable trash entry stays in the trash" \
                              || { bad PR1 "an undeletable trash entry stays in the trash"; echo "$out" | sed 's/^/        /'; }
echo "$out" | grep -q "could not be deleted" && ok PR1 "gc says so instead of reporting a clean run" \
                                              || { bad PR1 "gc says so instead of reporting a clean run"; echo "$out" | sed 's/^/        /'; }
echo "$out" | grep -q "will try again" && ok PR1 "the first failures are retried" \
                                        || { bad PR1 "the first failures are retried"; echo "$out" | sed 's/^/        /'; }
rv fs gc --status | grep -q "^trash: *1 entries" && ok PR1 "gc --status still counts it" \
                                                 || { bad PR1 "gc --status still counts it"; rv fs gc --status | sed 's/^/        /'; }
# The retries are capped: after a few wakes the collector stops waking itself for it, but it
# still says what is in there -- the trash is never reported as empty while it is not.
for _ in 1 2 3 4; do out=$(rv fs gc --now --retention 0 2>&1); done
echo "$out" | grep -q "too often" && ok PR1 "the retries are capped, with a reason" \
                                   || { bad PR1 "the retries are capped, with a reason"; echo "$out" | sed 's/^/        /'; }
WORLD_GC_PAUSE_MS=0 rv fs gc --worker --retention 0 > "$SCRATCH/gcstuck.log" 2>&1
grep -q "trash empty" "$SCRATCH/gcstuck.log" && bad PR1 "the worker never calls the trash empty while it is not" \
                                             || ok PR1 "the worker never calls the trash empty while it is not"
grep -q "could not be deleted" "$SCRATCH/gcstuck.log" && ok PR1 "the worker log names the failure" \
                                                      || { bad PR1 "the worker log names the failure"; sed 's/^/        /' "$SCRATCH/gcstuck.log"; }
# Take the ACL away and the very next gc finishes the job -- the entry was never forgotten.
chmod -N "$STUCK.deleting/keep"
rv fs gc --now --retention 0 > /dev/null 2>&1
[ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && ok PR1 "once it can be deleted the next gc removes it" \
                                            || { bad PR1 "once it can be deleted the next gc removes it"; ls "$RSTORE/trash" | sed 's/^/        /'; }

# ---- PR #1 review (4th round, P2): the batch limit survives the deleter's fallback -------------
# The parallel deleter falls back to the single-threaded one on any error that is not the
# deadline, and that fallback used to be handed no deadline at all: one unreadable directory in a
# big trash tree turned a nominal one-second wake into an unbounded one, which is exactly the
# foreground contention max_secs exists to bound. Here the whole tree sits under a 0000 directory
# -- the parallel walker's opendir fails with EACCES before it has removed a single entry, so the
# fallback is taken every time, deterministically. It must still stop at the deadline and leave a
# resumable `.deleting` tree. (The fallback chmods the obstacle out of the way on its way in --
# it is deleting the thing -- so the successors take the parallel path and finish the job.)
BIG2="$RSTORE/trash/big-2"
mkdir -p "$BIG2/blocked"
python3 - "$BIG2/blocked" <<'EOF'
import os, sys
base = sys.argv[1]
for d in range(400):
    p = os.path.join(base, 'd%03d' % d)
    os.makedirs(p, exist_ok=True)
    for i in range(300):
        os.close(os.open(os.path.join(p, 'f%03d' % i), os.O_CREAT | os.O_WRONLY, 0o644))
EOF
chmod 0000 "$BIG2/blocked"
t0=$(python3 -c 'import time;print(int(time.time()*1000))')
WORLD_GC_PAUSE_MS=0 WORLD_GC_BATCH_SECS=1 rv fs gc --worker --retention 0 > "$SCRATCH/gcfb.log" 2>&1
t1=$(python3 -c 'import time;print(int(time.time()*1000))')
if [ "$((t1 - t0))" -lt 2500 ]; then ok PR1 "a one-second wake that falls back still returns on time ($((t1-t0)) ms)"
else bad PR1 "a one-second wake that falls back still returns on time ($((t1-t0)) ms)"; sed 's/^/        /' "$SCRATCH/gcfb.log"; fi
grep -q "work remains, handing over" "$SCRATCH/gcfb.log" && ok PR1 "it hands the rest of the tree over" \
                                                         || { bad PR1 "it hands the rest of the tree over"; sed 's/^/        /' "$SCRATCH/gcfb.log"; }
LEFT2=$(find "$RSTORE/trash" 2>/dev/null | wc -l | tr -d ' ')
if [ -d "$BIG2.deleting/blocked" ] && [ "$LEFT2" -gt 1 ] && [ "$LEFT2" -lt 120403 ]; then
    ok PR1 "the fallback stopped mid-tree and kept the .deleting name ($LEFT2 entries left)"
else
    bad PR1 "the fallback stopped mid-tree and kept the .deleting name ($LEFT2 entries left)"
fi
for _ in $(seq 160); do [ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && break; sleep 0.25; done
[ -z "$(ls "$RSTORE"/trash 2>/dev/null)" ] && ok PR1 "the successor chain finishes it once the obstacle is gone" \
                                            || { bad PR1 "the successor chain finishes it once the obstacle is gone"; ls "$RSTORE/trash" | sed 's/^/        /'; }

echo
"$WORLD" fs status | sed 's/^/      /'
echo
echo "safety: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
