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
# PR #1 review (7th round): a 120k-entry tree, made once and then cloned. clonefile(2) on a
# directory takes the whole subtree in about a second, where `cp -Rc` copies it entry by entry
# and takes fifteen -- and what the deadline cases below need is a big tree, not a slow test.
clone_tree() {
    python3 -c 'import ctypes, sys
sys.exit(0 if ctypes.CDLL("/usr/lib/libSystem.B.dylib").clonefile(sys.argv[1].encode(), sys.argv[2].encode(), 0) == 0 else 1)' "$1" "$2"
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
    if "$WORLD" fs gc --status | grep -q "^abandoned: *1 half-built tree"; then
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

# ---- PR #1 review (11th round): a *.wfs-tmp the sweep cannot remove is reported and retried ----
# `<store>/snapshots/S<n>.wfs-tmp` whose id no row has is the suffix sweep's to remove -- being
# named by nothing is exactly what makes it the sweep's. The removal's result was dropped: one
# that would not go (an ACL, an EPERM, a transient EIO) was counted in no report, set no
# work_remains, and `wfs_gc_pending()` only ever classifies the trash -- so nothing ever came
# back for it and the tree sat in the store until somebody ran gc by hand. Same ACL as the two
# undeletable cases above: it survives chmod and chflags.
SWEEPSTUCK="$WORLD_STORE/snapshots/S999998.wfs-tmp"
mkdir -p "$SWEEPSTUCK/keep"
echo x > "$SWEEPSTUCK/keep/f.txt"
chmod +a "$(id -un) deny delete,delete_child,add_file" "$SWEEPSTUCK/keep"
out=$("$WORLD" fs gc 2>&1)
if [ -e "$SWEEPSTUCK/keep/f.txt" ] && echo "$out" | grep -q "could not be removed"; then
    ok PR11 "gc reports the *.wfs-tmp it could not sweep"
else
    bad PR11 "gc reports the *.wfs-tmp it could not sweep"; echo "$out" | sed 's/^/        /'
fi
echo "$out" | grep -q "the collector will try again" \
    && ok PR11 "and keeps work_remains set, so the worker chain comes back for it" \
    || { bad PR11 "and keeps work_remains set, so the worker chain comes back for it"; echo "$out" | sed 's/^/        /'; }
if "$WORLD" fs gc --status | grep -q "^abandoned: *1 half-built tree"; then
    ok PR11 "gc --status counts it"
else
    bad PR11 "gc --status counts it"; "$WORLD" fs gc --status | sed 's/^/        /'
fi
# Take the ACL away and the next sweep finishes it -- and stops counting it.
chmod -N "$SWEEPSTUCK/keep"
"$WORLD" fs gc > /dev/null 2>&1
if [ ! -e "$SWEEPSTUCK" ] && ! "$WORLD" fs gc --status | grep -q "^abandoned:"; then
    ok PR11 "once it can be removed the next gc removes it and stops counting it"
else
    bad PR11 "once it can be removed the next gc removes it and stops counting it"
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
# PR #1 review (27th round, P2): a pool the collector cannot READ is not an empty pool. An
# opendir/readdir that failed with anything but ENOENT used to be skipped in silence and the
# scan reported as complete, so a row-less tree left by a crashed fork or filler was missed,
# neither the run nor --status said anything, and since gc's "is there work?" probe only looks
# at the trash the worker chain stopped there as well.
mkdir -p "$WORLD_STORE/pool/S1/0000000000000001"
chmod 000 "$WORLD_STORE/pool"
"$WORLD" fs gc --retention 7 > "$SCRATCH/gc-blindpool.log" 2>&1
grep -q "under <store>/pool could not be read" "$SCRATCH/gc-blindpool.log" \
    && ok T1.5 "gc reports a pool directory it could not read" \
    || { bad T1.5 "gc reports a pool directory it could not read"; sed 's/^/        /' "$SCRATCH/gc-blindpool.log"; }
grep -q "collector will try again" "$SCRATCH/gc-blindpool.log" \
    && ok T1.5 "and says it will come back for it (work remains)" \
    || bad T1.5 "and says it will come back for it (work remains)"
"$WORLD" fs gc --status > "$SCRATCH/gc-blindpool-status.log" 2>&1
grep -q "under <store>/pool could not be read" "$SCRATCH/gc-blindpool-status.log" \
    && ok T1.5 "gc --status does not call a pool it cannot read a clean one" \
    || { bad T1.5 "gc --status does not call a pool it cannot read a clean one"; sed 's/^/        /' "$SCRATCH/gc-blindpool-status.log"; }
chmod 700 "$WORLD_STORE/pool"
"$WORLD" fs gc --retention 7 > "$SCRATCH/gc-blindpool2.log" 2>&1
[ ! -e "$WORLD_STORE/pool/S1/0000000000000001" ] \
    && ok T1.5 "and the orphan goes as soon as the pool can be read again" \
    || { bad T1.5 "and the orphan goes as soon as the pool can be read again"; sed 's/^/        /' "$SCRATCH/gc-blindpool2.log"; }

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
grep -q "collected in the background" "$SCRATCH/gc2.log" && ok T2.1 "gc hands due trash to the background worker" \
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

# ---- PR #1 review (17th round, P2): a marker that names another store is a refusal -------------
# The extension opens the path it finds in the marker and falls back to its own container default
# only when it obtained no path at all (WorldVolume.mm). So a store that has moved since the world
# was forked, or a world copied out of somebody else's store, would make a mount serve a different
# store from the one the command is talking to -- and the CLI used to print a note promising a
# fallback that does not happen. `fs verify` is where that is said now, and where it is fixed.
# (`fs mount` itself only exists in an -DWFS_FSKIT=ON build and is not exercised here: nothing in
# this script ever mounts anything. It asks the same question of the same core call.)
w2 fs fork --from S3 --to "$SCRATCH/t-h" --no-pool > /dev/null
mkdir -p "$SCRATCH/store3"
python3 - "$SCRATCH/t-h/.world" "$SCRATCH/store3" <<'EOF'
import json,sys
m=json.load(open(sys.argv[1]))
m["store_path"]=sys.argv[2]
open(sys.argv[1],"w").write(json.dumps(m,indent=2))
EOF
check T2.3 "a world whose .world names another store path is refused" 3 -- w2 fs verify "$SCRATCH/t-h"
w2 fs verify "$SCRATCH/t-h" > "$SCRATCH/t23b.log" 2>&1
if grep -q "store3" "$SCRATCH/t23b.log" && grep -q "$S2" "$SCRATCH/t23b.log"; then
    ok T2.3 "the refusal names both stores"
else
    bad T2.3 "the refusal names both stores"; sed 's/^/        /' "$SCRATCH/t23b.log"
fi
has_hint T2.3 "and says how to fix it" "refresh-marker" -- w2 fs verify "$SCRATCH/t-h"
# The way out, for the same store under a new path: the store id still matches, so only the path
# is rewritten -- the world id, the name, the origin snapshot and created_at stay as they were.
before=$(python3 -c 'import json,sys;m=json.load(open(sys.argv[1]));print(m["world"],m["name"],m["snapshot"],m["created_at"])' "$SCRATCH/t-h/.world")
w2 fs verify "$SCRATCH/t-h" --refresh-marker > "$SCRATCH/t23c.log" 2>&1
grep -q "store path refreshed" "$SCRATCH/t23c.log" && ok T2.3 "--refresh-marker rewrites the path" \
                                                   || { bad T2.3 "--refresh-marker rewrites the path"; sed 's/^/        /' "$SCRATCH/t23c.log"; }
check T2.3 "and the world verifies clean afterwards" 0 -- w2 fs verify "$SCRATCH/t-h"
after=$(python3 -c 'import json,sys;m=json.load(open(sys.argv[1]));print(m["world"],m["name"],m["snapshot"],m["created_at"])' "$SCRATCH/t-h/.world")
if [ "$before" = "$after" ] && python3 - "$SCRATCH/t-h/.world" "$S2" <<'EOF'
import json,sys,os
m=json.load(open(sys.argv[1]))
sys.exit(0 if os.path.realpath(m.get("store_path","")) == os.path.realpath(sys.argv[2]) else 1)
EOF
then ok T2.3 "the refresh changes the store path and nothing else"
else bad T2.3 "the refresh changes the store path and nothing else"; fi
# A marker whose store ID is somebody else's is not a relocation: P1/P2 say that is another
# store's world, and --refresh-marker will not take it over (that is what `adopt` is for).
python3 - "$SCRATCH/t-h/.world" <<'EOF'
import json,sys
m=json.load(open(sys.argv[1]))
m["store"]="0123456789abcdef0123456789abcdef"
open(sys.argv[1],"w").write(json.dumps(m,indent=2))
EOF
check T2.3 "a marker from a foreign store is refused" 3 -- w2 fs verify "$SCRATCH/t-h"
check T2.3 "and --refresh-marker does not take it over" 3 -- w2 fs verify "$SCRATCH/t-h" --refresh-marker
python3 -c 'import json,sys;sys.exit(0 if json.load(open(sys.argv[1]))["store"]=="0123456789abcdef0123456789abcdef" else 1)' "$SCRATCH/t-h/.world" \
    && ok T2.3 "the foreign marker is left exactly as it was" \
    || bad T2.3 "the foreign marker is left exactly as it was"

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

# ---- PR #1 review (6th round, P2): the gc deadline reaches the pool, too ----------------------
# A pool entry is a whole clone of a snapshot. Collecting stale ones ran in front of the
# deadline-controlled trash loop and never looked at the clock, so a two-second worker wake -- or
# an interactive `gc` -- could spend minutes unlinking full-size clones. Here: one 120k-entry
# entry whose snapshot is then taken away behind the store's back (which is what `--reconcile`
# is for), and a one-second budget (the same entry takes ~5 s to unlink in one go). The wake has to come back on time, leave the entry where a
# successor finds it, say so, and the chain has to finish the job on its own.
POOLSRC="$SCRATCH/pool-src"
mkdir -p "$POOLSRC"
python3 - "$POOLSRC" <<'EOF'
import os, sys
base = sys.argv[1]
for d in range(400):
    p = os.path.join(base, 'd%03d' % d)
    os.makedirs(p, exist_ok=True)
    for i in range(300):
        os.close(os.open(os.path.join(p, 'f%03d' % i), os.O_CREAT | os.O_WRONLY, 0o644))
EOF
PS=$(rv fs init "$POOLSRC" --name poolbig 2>/dev/null | awk '/^S[0-9]/{print $1}')
rv fs pool fill "$PS" --count 1 > /dev/null 2>&1
PDIR="$RSTORE/pool/$PS"
[ -n "$(ls "$PDIR" 2>/dev/null)" ] && ok PR6 "the pool holds a pre-cloned entry for $PS" \
                                    || bad PR6 "the pool holds a pre-cloned entry for $PS"
# The snapshot's tree, gone from under the store: its row is then dangling and --reconcile buries
# it, which is what makes the pool entry stale.
chmod 0700 "$RSTORE/snapshots/$PS/root"
rm -rf "${RSTORE:?}/snapshots/${PS:?}"
t0=$(python3 -c 'import time;print(int(time.time()*1000))')
out=$(WORLD_GC_BATCH_SECS=1 rv fs gc --reconcile --retention 0 2>&1)
t1=$(python3 -c 'import time;print(int(time.time()*1000))')
if [ "$((t1 - t0))" -lt 2500 ]; then ok PR6 "a one-second gc does not unlink a whole pool clone ($((t1-t0)) ms)"
else bad PR6 "a one-second gc does not unlink a whole pool clone ($((t1-t0)) ms)"; echo "$out" | sed 's/^/        /'; fi
[ -n "$(ls "$PDIR" 2>/dev/null)" ] && ok PR6 "what it did not finish is still where a successor finds it" \
                                    || { bad PR6 "what it did not finish is still where a successor finds it"; echo "$out" | sed 's/^/        /'; }
echo "$out" | grep -q "being collected in the background" && ok PR6 "and it hands the rest over" \
                                                           || { bad PR6 "and it hands the rest over"; echo "$out" | sed 's/^/        /'; }
rv fs gc --status | grep -q "stale pre-clone" && ok PR6 "gc --status counts the stale entry" \
                                               || { bad PR6 "gc --status counts the stale entry"; rv fs gc --status | sed 's/^/        /'; }
for _ in $(seq 240); do [ -z "$(ls "$PDIR" 2>/dev/null)" ] && break; sleep 0.25; done
[ -z "$(ls "$PDIR" 2>/dev/null)" ] && ok PR6 "the successor chain finishes the pool entry" \
                                    || { bad PR6 "the successor chain finishes the pool entry"; ls -d "$PDIR"/* | sed 's/^/        /'; }


# ---- PR #1 review (7th round, P2): a half-built snapshot gc cannot remove keeps its row -------
# The mirror of the abandoned-fork-tree case above, and it had the opposite bug: gc called
# fs_remove_tree() on S<n>.wfs-tmp and on S<n>, threw both results away, and deleted the row. The
# suffix sweep only ever looks at `*.wfs-tmp`, so an S<n> that would not go was left with nothing
# in the store that knew it was rubbish -- leaked for ever, and invisible to `gc --status`. A
# store of its own, because the row is written straight into the database and would otherwise
# take a snapshot id the tests above name by hand. Same ACL as the undeletable-trash case: it
# survives both chmod and chflags, so the tree really cannot go.
if command -v sqlite3 > /dev/null 2>&1; then
    H7STORE="$SCRATCH/half-store"
    h7() { "$WORLD" --store "$H7STORE" "$@"; }
    h7 fs status > /dev/null 2>&1
    sqlite3 "$H7STORE/metadata.db" "INSERT INTO snapshots(name,path,src_path,from_world,created_at,state,owner_pid,owner_start) VALUES('pr7half','','',0,0,0,2147480000,0);" 2>/dev/null
    H7S=$(sqlite3 "$H7STORE/metadata.db" "SELECT id FROM snapshots WHERE name='pr7half';")
    H7DIR="$H7STORE/snapshots/S$H7S"
    mkdir -p "$H7DIR/keep"
    echo x > "$H7DIR/keep/f.txt"
    chmod +a "$(id -un) deny delete,delete_child,add_file" "$H7DIR/keep"
    out=$(WORLD_GC_CREATING_MIN_AGE=0 h7 fs gc 2>&1)
    H7ROWS=$(sqlite3 "$H7STORE/metadata.db" "SELECT count(*) FROM snapshots WHERE id=$H7S;")
    if [ -e "$H7DIR/keep/f.txt" ] && [ "$H7ROWS" = 1 ]; then
        ok PR7 "a half-built snapshot gc cannot remove keeps its row"
    else
        bad PR7 "a half-built snapshot gc cannot remove keeps its row (rows $H7ROWS)"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "could not be removed" && ok PR7 "gc says so instead of reporting it collected" \
                                                 || { bad PR7 "gc says so instead of reporting it collected"; echo "$out" | sed 's/^/        /'; }
    if h7 fs gc --status | grep -q "^abandoned: *1 half-built tree"; then
        ok PR7 "gc --status counts the half-built snapshot"
    else
        bad PR7 "gc --status counts the half-built snapshot"; h7 fs gc --status | sed 's/^/        /'
    fi
    # Take the ACL away and the next wake finishes what it started: tree gone, row gone.
    chmod -N "$H7DIR/keep"
    WORLD_GC_CREATING_MIN_AGE=0 h7 fs gc > /dev/null 2>&1
    H7ROWS=$(sqlite3 "$H7STORE/metadata.db" "SELECT count(*) FROM snapshots WHERE id=$H7S;")
    if [ ! -e "$H7DIR" ] && [ "$H7ROWS" = 0 ]; then
        ok PR7 "once it can be removed the next gc removes the tree and the row"
    else
        bad PR7 "once it can be removed the next gc removes the tree and the row (rows $H7ROWS)"
    fi
    h7 fs gc --status | grep -q "^abandoned:" && bad PR7 "and stops counting it" \
                                              || ok PR7 "and stops counting it"
fi


# ---- PR #1 review (8th round, P2): a stale pool entry gc cannot remove keeps its row ----------
# The third of the same shape. pool_collect() unlinked a stale pre-clone entry and then deleted
# its row whatever the removal had actually done, so one EPERM (an ACL, a transient EIO) left a
# whole clone of a snapshot on disk with nothing in the store that knew it was rubbish: the
# row-less-orphan sweep would find it, but it had no retry counter and set no work_remains, so
# nothing came back for it until somebody ran `world fs gc` by hand. The row is kept while its
# tree is now, counted, reported, and retried under the same cap a trash entry gets. A store of
# its own, and the same ACL as the undeletable-trash case: it survives chmod and chflags.
if command -v sqlite3 > /dev/null 2>&1; then
    P8STORE="$SCRATCH/pool8-store"
    p8() { "$WORLD" --store "$P8STORE" "$@"; }
    P8SRC="$SCRATCH/pool8-src"
    mkdir -p "$P8SRC/keep"
    echo x > "$P8SRC/keep/f.txt"
    P8S=$(p8 fs init "$P8SRC" --name pool8 2>/dev/null | awk '/^S[0-9]/{print $1}')
    p8 fs pool fill "$P8S" --count 1 > /dev/null 2>&1
    P8ENTRY=$(ls -d "$P8STORE/pool/$P8S"/* 2>/dev/null | head -1)
    if [ -n "$P8ENTRY" ]; then
        # snapshot rows are immutable, so a snap_created_at that no longer matches means the id
        # belongs to a different snapshot now -- the entry is stale and must never be handed out.
        sqlite3 "$P8STORE/metadata.db" "UPDATE pool SET snap_created_at=snap_created_at+1;"
        chmod +a "$(id -un) deny delete,delete_child,add_file" "$P8ENTRY/keep"
        out=$(p8 fs gc 2>&1)
        P8ROWS=$(sqlite3 "$P8STORE/metadata.db" "SELECT count(*) FROM pool;")
        if [ -e "$P8ENTRY/keep/f.txt" ] && [ "$P8ROWS" = 1 ]; then
            ok PR8 "a stale pool entry gc cannot remove keeps its row"
        else
            bad PR8 "a stale pool entry gc cannot remove keeps its row (rows $P8ROWS)"
            echo "$out" | sed 's/^/        /'
        fi
        echo "$out" | grep -q "stale pre-clone entry could not be removed" \
            && ok PR8 "gc says so instead of reporting it collected" \
            || { bad PR8 "gc says so instead of reporting it collected"; echo "$out" | sed 's/^/        /'; }
        p8 fs gc --status | grep -q "stale pre-clone" && ok PR8 "gc --status counts the stale entry" \
            || { bad PR8 "gc --status counts the stale entry"; p8 fs gc --status | sed 's/^/        /'; }
        # Take the ACL away and the next wake finishes what it started: tree gone, row gone.
        chmod -N "$P8ENTRY/keep"
        p8 fs gc > /dev/null 2>&1
        for _ in $(seq 40); do [ ! -e "$P8ENTRY" ] && break; sleep 0.25; done
        P8ROWS=$(sqlite3 "$P8STORE/metadata.db" "SELECT count(*) FROM pool;")
        if [ ! -e "$P8ENTRY" ] && [ "$P8ROWS" = 0 ]; then
            ok PR8 "once it can be removed the next gc removes the tree and the row"
        else
            bad PR8 "once it can be removed the next gc removes the tree and the row (rows $P8ROWS)"
        fi
    else
        bad PR8 "the pool holds a pre-cloned entry for $P8S"
    fi
fi


# ---- PR #1 review (7th round, P2): the gc deadline reaches the cheap half's own trees ---------
# gc's cheap half removes three kinds of whole tree before the deadline-controlled trash loop is
# reached: an abandoned fork's half-built clone, a half-built snapshot's S<n> and S<n>.wfs-tmp,
# and the `*.wfs-tmp` suffix sweep under <store>/snapshots. None of them looked at the clock, so
# a two-second worker wake -- or an interactive `gc` -- could spend minutes there on
# workspace-sized clones, which is the foreground contention max_secs exists to bound (P16).
# Same shape as the pool case above: a 120k-entry tree (about five seconds to unlink on this
# machine) and a one-second budget. A store of its own for each, so the two cases cannot spend
# each other's budget and neither can be finished by the other's worker.
if command -v sqlite3 > /dev/null 2>&1; then
    D7STORE="$SCRATCH/deadline-fork-store"
    d7() { "$WORLD" --store "$D7STORE" "$@"; }
    d7 fs status > /dev/null 2>&1
    mkdir -p "$SCRATCH/d7fork"
    D7TMP="$SCRATCH/d7fork/.wfs-fork-abandoned"
    clone_tree "$POOLSRC" "$D7TMP"
    sqlite3 "$D7STORE/metadata.db" "INSERT INTO worlds(kind,parent_world,snapshot_id,name,path,state,created_at,tmp_path,owner_pid,owner_start) VALUES(1,0,0,'d7fork','$SCRATCH/d7fork/w',0,0,'$D7TMP',2147480000,0);" 2>/dev/null
    t0=$(python3 -c 'import time;print(int(time.time()*1000))')
    out=$(WORLD_GC_BATCH_SECS=1 d7 fs gc 2>&1)
    t1=$(python3 -c 'import time;print(int(time.time()*1000))')
    if [ "$((t1 - t0))" -lt 2500 ]; then ok PR7 "a one-second gc does not unlink a whole abandoned fork tree ($((t1-t0)) ms)"
    else bad PR7 "a one-second gc does not unlink a whole abandoned fork tree ($((t1-t0)) ms)"; echo "$out" | sed 's/^/        /'; fi
    D7LEFT=$(find "$D7TMP" 2>/dev/null | wc -l | tr -d ' ')
    D7STATE=$(sqlite3 "$D7STORE/metadata.db" "SELECT state FROM worlds WHERE name='d7fork';")
    if [ "$D7STATE" = 0 ] && [ "$D7LEFT" -gt 0 ] && [ "$D7LEFT" -lt 120401 ]; then
        ok PR7 "what it did not finish keeps its CREATING row and its tmp_path ($D7LEFT entries left)"
    else
        bad PR7 "what it did not finish keeps its CREATING row and its tmp_path (state $D7STATE, $D7LEFT entries left)"
    fi
    echo "$out" | grep -q "being collected in the background" && ok PR7 "and it hands the rest over" \
                                                               || { bad PR7 "and it hands the rest over"; echo "$out" | sed 's/^/        /'; }
    # Running out of time is not a failure: nothing is reported as undeletable, and a gc with no
    # budget at all finishes the job.
    echo "$out" | grep -q "could not be removed" && bad PR7 "out of time is not reported as a failure" \
                                                  || ok PR7 "out of time is not reported as a failure"
    WORLD_GC_CREATING_MIN_AGE=0 d7 fs gc --now > /dev/null 2>&1
    for _ in $(seq 120); do [ ! -e "$D7TMP" ] && break; sleep 0.25; done
    D7STATE=$(sqlite3 "$D7STORE/metadata.db" "SELECT state FROM worlds WHERE name='d7fork';")
    if [ ! -e "$D7TMP" ] && [ "$D7STATE" = 3 ]; then
        ok PR7 "a gc without the budget finishes the tree and buries the row"
    else
        bad PR7 "a gc without the budget finishes the tree and buries the row (state $D7STATE)"
    fi

    # The same for a half-built snapshot: its S<n>.wfs-tmp is a clone of the source tree, and it
    # is removed twice over -- once by the CREATING loop, once by the suffix sweep right after it.
    D8STORE="$SCRATCH/deadline-snap-store"
    d8() { "$WORLD" --store "$D8STORE" "$@"; }
    d8 fs status > /dev/null 2>&1
    sqlite3 "$D8STORE/metadata.db" "INSERT INTO snapshots(name,path,src_path,from_world,created_at,state,owner_pid,owner_start) VALUES('d8snap','','',0,0,0,2147480000,0);" 2>/dev/null
    D8S=$(sqlite3 "$D8STORE/metadata.db" "SELECT id FROM snapshots WHERE name='d8snap';")
    D8TMP="$D8STORE/snapshots/S$D8S.wfs-tmp"
    clone_tree "$POOLSRC" "$D8TMP"
    t0=$(python3 -c 'import time;print(int(time.time()*1000))')
    out=$(WORLD_GC_BATCH_SECS=1 d8 fs gc 2>&1)
    t1=$(python3 -c 'import time;print(int(time.time()*1000))')
    if [ "$((t1 - t0))" -lt 2500 ]; then ok PR7 "a one-second gc does not unlink a whole half-built snapshot ($((t1-t0)) ms)"
    else bad PR7 "a one-second gc does not unlink a whole half-built snapshot ($((t1-t0)) ms)"; echo "$out" | sed 's/^/        /'; fi
    D8LEFT=$(find "$D8TMP" 2>/dev/null | wc -l | tr -d ' ')
    D8ROWS=$(sqlite3 "$D8STORE/metadata.db" "SELECT count(*) FROM snapshots WHERE id=$D8S;")
    if [ "$D8ROWS" = 1 ] && [ "$D8LEFT" -gt 0 ] && [ "$D8LEFT" -lt 120401 ]; then
        ok PR7 "what it did not finish keeps its CREATING row ($D8LEFT entries left)"
    else
        bad PR7 "what it did not finish keeps its CREATING row (rows $D8ROWS, $D8LEFT entries left)"
    fi
    echo "$out" | grep -q "being collected in the background" && ok PR7 "and that one hands the rest over too" \
                                                               || { bad PR7 "and that one hands the rest over too"; echo "$out" | sed 's/^/        /'; }
    WORLD_GC_CREATING_MIN_AGE=0 d8 fs gc --now > /dev/null 2>&1
    # The tree and the row, not the tree alone: a background worker collecting the same tree in
    # parallel removes the two a few milliseconds apart, and reading the row in between made this
    # assertion flaky (PR #1 review, 8th round -- seen once while the pool case below was added).
    for _ in $(seq 120); do
        [ ! -e "$D8TMP" ] && [ "$(sqlite3 "$D8STORE/metadata.db" "SELECT count(*) FROM snapshots WHERE id=$D8S;")" = 0 ] && break
        sleep 0.25
    done
    D8ROWS=$(sqlite3 "$D8STORE/metadata.db" "SELECT count(*) FROM snapshots WHERE id=$D8S;")
    if [ ! -e "$D8TMP" ] && [ "$D8ROWS" = 0 ]; then
        ok PR7 "a gc without the budget finishes the snapshot tree and its row"
    else
        bad PR7 "a gc without the budget finishes the snapshot tree and its row (rows $D8ROWS)"
    fi
fi

# ---- PR #1 review (9th round) -----------------------------------------------------------------
#
# Three of this round's rules, driven through the CLI. The two races themselves need a seam
# inside the library (core_test has them); what is checked here is the classification and the
# refusal an operator actually meets.
if command -v sqlite3 > /dev/null 2>&1; then
    N9STORE="$SCRATCH/round9-store"
    n9() { "$WORLD" --store "$N9STORE" "$@"; }
    N9SRC="$SCRATCH/round9-src"
    mkdir -p "$N9SRC"
    echo one > "$N9SRC/a.txt"
    N9S=$(n9 fs init "$N9SRC" --name r9 2>/dev/null | awk '/^S[0-9]/{print $1}')

    # (1) P18, the orphan rule: a row's trash_path claims the tree under BOTH of its names. A
    # collector renames a trash entry to `<name>.deleting` first and records that second, so a
    # tree wearing the `.deleting` name while its row still says the plain one is that row's
    # entry -- deleted with the row buried, never counted as a row-less orphan.
    N9W=$(n9 fs fork --from "$N9S" --to "$PROJ/r9w" --name r9w 2>/dev/null | awk '/^W[0-9]/{print $1}')
    n9 fs discard "$N9W" > /dev/null 2>&1
    N9ID=${N9W#W}
    N9TP=$(sqlite3 "$N9STORE/metadata.db" "SELECT trash_path FROM worlds WHERE id=$N9ID;")
    if [ -n "$N9TP" ] && [ -d "$N9TP" ]; then
        mv "$N9TP" "$N9TP.deleting"
        out=$(n9 fs gc --retention 0 --now 2>&1)
        N9STATE=$(sqlite3 "$N9STORE/metadata.db" "SELECT state FROM worlds WHERE id=$N9ID;")
        if [ ! -e "$N9TP.deleting" ] && [ "$N9STATE" = 3 ]; then
            ok PR9 "a tree wearing .deleting is still its row's entry, not an orphan"
        else
            bad PR9 "a tree wearing .deleting is still its row's entry, not an orphan (state $N9STATE)"
            echo "$out" | sed 's/^/        /'
        fi
        # gc's summary line always names the class; what must be zero is the count.
        echo "$out" | grep -q "0 orphan trash dirs" && ok PR9 "and gc does not report it as one" \
            || { bad PR9 "and gc does not report it as one"; echo "$out" | sed 's/^/        /'; }
    else
        bad PR9 "the discarded world has a tree in the trash"
    fi
    # ... and a directory in the trash that really is row-less still goes immediately.
    mkdir -p "$N9STORE/trash/W9999-1/sub"
    echo junk > "$N9STORE/trash/W9999-1/sub/j.txt"
    out=$(n9 fs gc --retention 0 --now 2>&1)
    if [ ! -e "$N9STORE/trash/W9999-1" ]; then
        ok PR9 "a directory in the trash that no row names still goes at once"
    else
        bad PR9 "a directory in the trash that no row names still goes at once"
        echo "$out" | sed 's/^/        /'
    fi

    # (2) P18, reconciliation: a world that was merely moved and then verified keeps its row.
    # `gc --reconcile` reads "no tree at the recorded path" and used to write DEAD with no
    # predicate at all, so a verify that relocated the row by inode had its work buried.
    N9W2=$(n9 fs fork --from "$N9S" --to "$PROJ/r9move" --name r9move 2>/dev/null | awk '/^W[0-9]/{print $1}')
    N9ID2=${N9W2#W}
    mv "$PROJ/r9move" "$PROJ/r9moved"
    n9 fs verify "$PROJ/r9moved" > /dev/null 2>&1
    out=$(n9 fs gc --reconcile 2>&1)
    N9STATE2=$(sqlite3 "$N9STORE/metadata.db" "SELECT state FROM worlds WHERE id=$N9ID2;")
    N9PATH2=$(sqlite3 "$N9STORE/metadata.db" "SELECT path FROM worlds WHERE id=$N9ID2;")
    if [ "$N9STATE2" = 1 ] && [ "$N9PATH2" = "$PROJ/r9moved" ]; then
        ok PR9 "a moved world that has been verified survives --reconcile"
    else
        bad PR9 "a moved world that has been verified survives --reconcile (state $N9STATE2, path $N9PATH2)"
        echo "$out" | sed 's/^/        /'
    fi
    # ... and one whose tree really is gone is still reconciled.
    rm -rf "$PROJ/r9moved"
    n9 fs gc --reconcile > /dev/null 2>&1
    N9STATE2=$(sqlite3 "$N9STORE/metadata.db" "SELECT state FROM worlds WHERE id=$N9ID2;")
    if [ "$N9STATE2" = 3 ]; then ok PR9 "a world whose tree really is gone is still reconciled"
    else bad PR9 "a world whose tree really is gone is still reconciled (state $N9STATE2)"; fi
fi

# ---- PR #1 review (9th round, P2): a hardlink group that is not the size it declares ----------
# The manifest's `hl <group> <nlink> <path>` lines only ever describe groups every one of whose
# links is inside the tree, so a group's member count IS its nlink. Moving one member's group id
# to its neighbour keeps the #hl header's group and name totals intact -- which is all the 8th
# round's check looked at -- and the replay then links a name belonging to one inode onto another
# group's canonical file.
H9STORE="$SCRATCH/hl9-store"
h9() { "$WORLD" --store "$H9STORE" "$@"; }
H9SRC="$SCRATCH/hl9-src"
mkdir -p "$H9SRC"
echo aaa > "$H9SRC/a1"; ln "$H9SRC/a1" "$H9SRC/a2"; ln "$H9SRC/a1" "$H9SRC/a3"
echo bbb > "$H9SRC/b1"; ln "$H9SRC/b1" "$H9SRC/b2"; ln "$H9SRC/b1" "$H9SRC/b3"
H9S=$(h9 fs init "$H9SRC" --name hl9 2>/dev/null | awk '/^S[0-9]/{print $1}')
H9MAN="$H9STORE/snapshots/$H9S/manifest"
if [ -f "$H9MAN" ]; then
    check PR9 "the snapshot verifies as written" 0 -- h9 fs verify "$H9S"
    cp "$H9MAN" "$H9MAN.bak"
    # the third `hl` line -- the last member of group 0 -- re-tagged into group 1
    python3 - "$H9MAN" <<'EOF'
import sys
p = sys.argv[1]
lines = open(p).read().splitlines(True)
seen = 0
for i, l in enumerate(lines):
    if l.startswith("hl "):
        seen += 1
        if seen == 3:
            f = l.split(" ", 3)
            lines[i] = " ".join(["hl", "1", f[2], f[3]])
            break
open(p, "w").writelines(lines)
EOF
    check PR9 "a group that is not the size it declares is damage" 3 -- h9 fs verify "$H9S"
    check PR9 "and a fork from it is refused" 3 -- h9 fs fork --from "$H9S" --to "$PROJ/hl9w" --name hl9w
    [ -e "$PROJ/hl9w" ] && bad PR9 "and nothing is published at --to" || ok PR9 "and nothing is published at --to"
    cp "$H9MAN.bak" "$H9MAN"
    check PR9 "the undamaged manifest forks again" 0 -- h9 fs fork --from "$H9S" --to "$PROJ/hl9w" --name hl9w
    if [ "$(cat "$PROJ/hl9w/b1" 2>/dev/null)" = bbb ] && \
       [ "$(stat -f %l "$PROJ/hl9w/b1" 2>/dev/null)" = 3 ]; then
        ok PR9 "and the groups come out whole, with nobody's content from the other group"
    else
        bad PR9 "and the groups come out whole, with nobody's content from the other group"
    fi
else
    bad PR9 "the hardlinked snapshot has a manifest"
fi


# ---- PR #1 review (12th round): a reconciled snapshot's stump that will not go keeps its row ---
# `gc --reconcile` marked the row DEAD first and then removed <store>/snapshots/S<n> with the
# result thrown away. An S<n> that would not go (an ACL, an EPERM, a transient EIO) leaked for
# ever after that: reconciliation only ever scans ACTIVE rows and the suffix sweep only
# `*.wfs-tmp`, so nothing left in the store knew that directory was rubbish, nothing counted it
# and nothing came back for it. Same rule as the 5th, 7th and 8th rounds: a tree gc could not
# remove is not a tree gc removed, so the removal comes first and the row is buried only when
# the directory is confirmed gone.
if command -v sqlite3 > /dev/null 2>&1; then
    R12SRC="$SCRATCH/r12src"
    mkdir -p "$R12SRC"
    echo x > "$R12SRC/f.txt"
    "$WORLD" fs init "$R12SRC" --name r12 > /dev/null 2>&1
    R12ID=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT id FROM snapshots WHERE name='r12';")
    R12DIR="$WORLD_STORE/snapshots/S$R12ID"
    chmod 0700 "$R12DIR/root"          # the gate, so this test can take the root away
    rm -rf "$R12DIR/root"              # the path the row records is gone: the row is dangling
    mkdir -p "$R12DIR/keep"
    echo x > "$R12DIR/keep/f.txt"
    chmod +a "$(id -un) deny delete,delete_child,add_file" "$R12DIR/keep"
    out=$("$WORLD" fs gc --reconcile 2>&1)
    R12STATE=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM snapshots WHERE id=$R12ID;")
    if [ -e "$R12DIR/keep/f.txt" ] && [ "$R12STATE" = 1 ]; then
        ok PR12 "a stump reconcile cannot remove keeps its row"
    else
        bad PR12 "a stump reconcile cannot remove keeps its row (state $R12STATE)"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "could not be removed" \
        && ok PR12 "and gc says so instead of reporting the snapshot reconciled" \
        || { bad PR12 "and gc says so instead of reporting the snapshot reconciled"; echo "$out" | sed 's/^/        /'; }
    # Take the ACL away and the next reconcile finishes what it started: stump gone, row dead.
    chmod -N "$R12DIR/keep"
    "$WORLD" fs gc --reconcile > /dev/null 2>&1
    R12STATE=$(sqlite3 "$WORLD_STORE/metadata.db" "SELECT state FROM snapshots WHERE id=$R12ID;")
    if [ ! -e "$R12DIR" ] && [ "$R12STATE" = 3 ]; then
        ok PR12 "once it can be removed the next reconcile removes it and buries the row"
    else
        bad PR12 "once it can be removed the next reconcile removes it and buries the row (state $R12STATE)"
    fi
fi

# ---- PR #1 review (16th round, P2): a forced discard cannot drop a pool entry it cannot remove -
# wfs_pool_drain() deleted each pool row first and then removed its trees with the result thrown
# away, and returned 0 whatever had happened. One EPERM (an ACL, a transient EIO) therefore left
# a whole pre-cloned world under <store>/pool with no row at all -- and `discard S<n> --force`
# went on to trash the snapshot on the strength of that 0, so nothing ever came back for the
# clone: gc's pending check only scans the trash, and the snapshot's own trash entry is not due
# for days. The same rule as the 5th, 7th, 8th and 12th rounds, one command further on: the tree
# first, the row only when it and its `.wfs-tmp` name are both proven gone, and a removal that
# failed fails the discard with the errno the operator can act on.
if command -v sqlite3 > /dev/null 2>&1; then
    D16STORE="$SCRATCH/drain16-store"
    d16() { "$WORLD" --store "$D16STORE" "$@"; }
    D16SRC="$SCRATCH/drain16-src"
    mkdir -p "$D16SRC/keep"
    echo x > "$D16SRC/keep/f.txt"
    D16S=$(d16 fs init "$D16SRC" --name drain16 2>/dev/null | awk '/^S[0-9]/{print $1}')
    d16 fs pool fill "$D16S" --count 1 > /dev/null 2>&1
    D16ENTRY=$(ls -d "$D16STORE/pool/$D16S"/* 2>/dev/null | head -1)
    if [ -n "$D16ENTRY" ]; then
        # The same ACL the undeletable-trash and stale-entry cases use: it survives chmod and
        # chflags, so the entry's tree genuinely will not go.
        chmod +a "$(id -un) deny delete,delete_child,add_file" "$D16ENTRY/keep"
        out=$(d16 fs discard "$D16S" --force 2>&1); rc=$?
        D16STATE=$(sqlite3 "$D16STORE/metadata.db" "SELECT state FROM snapshots WHERE name='drain16';")
        D16ROWS=$(sqlite3 "$D16STORE/metadata.db" "SELECT count(*) FROM pool;")
        if [ "$rc" != 0 ] && [ "$D16STATE" = 1 ] && [ "$D16ROWS" = 1 ] && [ -e "$D16ENTRY/keep/f.txt" ]; then
            ok PR16 "a forced discard whose pool entry will not go leaves the snapshot and the row alone"
        else
            bad PR16 "a forced discard whose pool entry will not go leaves the snapshot and the row alone (exit $rc, state $D16STATE, rows $D16ROWS)"
            echo "$out" | sed 's/^/        /'
        fi
        echo "$out" | grep -q "could not be removed" \
            && ok PR16 "and the refusal names the entry and the errno" \
            || { bad PR16 "and the refusal names the entry and the errno"; echo "$out" | sed 's/^/        /'; }
        # PR #1 review (21st round, P1): the row the drain keeps through the removal is not an
        # ordinary READY entry of an ACTIVE snapshot any more -- if it were, a fork could claim
        # the tree the drain is in the middle of unlinking and publish a world around what is
        # left of it. It is DRAINING: out of the hand-out set for good, and waiting for the
        # collector exactly like every other tree that would not go. So the row's state says 2,
        # `gc --status` counts it, and a fork of that snapshot clones instead of taking it.
        D16PSTATE=$(sqlite3 "$D16STORE/metadata.db" "SELECT state FROM pool;")
        [ "$D16PSTATE" = 2 ] \
            && ok PR21 "the row the drain could not remove is DRAINING, which no claim matches" \
            || bad PR21 "the row the drain could not remove is DRAINING, which no claim matches (state $D16PSTATE)"
        d16 fs gc --status | grep -q "stale pre-clone" \
            && ok PR21 "and gc --status counts it, because it is waiting for the collector" \
            || { bad PR21 "and gc --status counts it, because it is waiting for the collector"; d16 fs gc --status | sed 's/^/        /'; }
        out=$(d16 fs fork --from "$D16S" --to "$SCRATCH/drain16-w" 2>&1); rc=$?
        if [ "$rc" = 0 ] && ! echo "$out" | grep -q "(pool)" && [ -e "$SCRATCH/drain16-w/keep/f.txt" ]; then
            ok PR21 "and a fork clones its own tree rather than the one being removed"
        else
            bad PR21 "and a fork clones its own tree rather than the one being removed (exit $rc)"
            echo "$out" | sed 's/^/        /'
        fi
        # ... and that world goes again, so the --force below is about the pool entry alone.
        D16W=$(echo "$out" | awk '/^W[0-9]/{print $1}')
        d16 fs discard "$D16W" --now > /dev/null 2>&1
        # Take the ACL away and the very same command goes through: pool empty, no tree left.
        chmod -N "$D16ENTRY/keep"
        out=$(d16 fs discard "$D16S" --force 2>&1); rc=$?
        D16STATE=$(sqlite3 "$D16STORE/metadata.db" "SELECT state FROM snapshots WHERE name='drain16';")
        D16ROWS=$(sqlite3 "$D16STORE/metadata.db" "SELECT count(*) FROM pool;")
        if [ "$rc" = 0 ] && [ ! -e "$D16ENTRY" ] && [ "$D16ROWS" = 0 ] && [ "$D16STATE" = 2 ]; then
            ok PR16 "and once it can be removed the same --force drains the pool and trashes it"
        else
            bad PR16 "and once it can be removed the same --force drains the pool and trashes it (exit $rc, state $D16STATE, rows $D16ROWS)"
            echo "$out" | sed 's/^/        /'
        fi
        [ -z "$(ls -A "$D16STORE/pool" 2>/dev/null)" ] \
            && ok PR16 "with nothing left under <store>/pool" \
            || { bad PR16 "with nothing left under <store>/pool"; ls -R "$D16STORE/pool" | sed 's/^/        /'; }
    else
        bad PR16 "the pool holds a pre-cloned entry for $D16S"
    fi
fi

# ---- PR #1 review (20th round, P1): a directory at the collector's working name is not ours ---
# Deleting a trash entry starts with one rename: `<entry>` -> `<entry>.deleting`. A
# `trash_fold_leftover()` used to run before it and remove whatever was already at that name,
# recursively, on the theory that it could only be an interrupted attempt of ours. Since the 15th
# round it cannot be: the rename and the row update that records it commit in ONE transaction, so
# by our own doing exactly one of the two names exists at any instant (a crash in between leaves
# the tree at `.deleting` and the entry gone, which is the case trash_follow_deleting() resolves).
# Both names there at once therefore means somebody else made the second one -- and for a world
# discarded from another volume the trash is `<parent>/.wfs-trash`, the user's own directory,
# under the entirely predictable name `W<id>-<timestamp>`. The fold deleted the user's directory.
# Now nothing folds anything: the entry is skipped, counted and named, and `--now` refuses.
B20STORE="$SCRATCH/blocked20-store"
b20() { "$WORLD" --store "$B20STORE" "$@"; }
B20SRC="$SCRATCH/blocked20-src"
mkdir -p "$B20SRC"
echo x > "$B20SRC/f.txt"
B20S=$(b20 fs init "$B20SRC" --name blocked20 2>/dev/null | awk '/^S[0-9]/{print $1}')
B20W=$(b20 fs fork --from "$B20S" --to "$SCRATCH/blocked20-w" --name b20w 2>/dev/null | awk '/^W[0-9]/{print $1}')
b20 fs discard "$B20W" > /dev/null 2>&1
B20ENTRY=$(ls -d "$B20STORE/trash/$B20W"-* 2>/dev/null | head -1)
if [ -n "$B20ENTRY" ]; then
    # The stray: a directory of the user's, with a file in it, at exactly the name the collector
    # is about to want. Nothing in the store ever wrote this name.
    mkdir -p "$B20ENTRY.deleting/keep"
    echo "not the collector's" > "$B20ENTRY.deleting/keep/user.txt"
    out=$(b20 fs discard "$B20W" --now 2>&1); rc=$?
    if [ "$rc" = 3 ] && [ -f "$B20ENTRY.deleting/keep/user.txt" ] && [ -d "$B20ENTRY" ]; then
        ok PR20 "--now refuses rather than remove a directory it did not name"
    else
        bad PR20 "--now refuses rather than remove a directory it did not name (exit $rc)"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "$B20ENTRY.deleting" \
        && ok PR20 "and the refusal names the directory that is in the way" \
        || { bad PR20 "and the refusal names the directory that is in the way"; echo "$out" | sed 's/^/        /'; }
    out=$(b20 fs gc --now --retention 0 2>&1)
    if [ -f "$B20ENTRY.deleting/keep/user.txt" ] && [ -d "$B20ENTRY" ]; then
        ok PR20 "the collector leaves both the stray and the entry alone"
    else
        bad PR20 "the collector leaves both the stray and the entry alone"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "could not be collected" \
        && ok PR20 "and gc says so instead of reporting a clean run" \
        || { bad PR20 "and gc says so instead of reporting a clean run"; echo "$out" | sed 's/^/        /'; }
    b20 fs gc --status --retention 0 | grep -q "^blocked: .*$B20ENTRY.deleting" \
        && ok PR20 "and gc --status names it for whoever has to clear it" \
        || { bad PR20 "and gc --status names it for whoever has to clear it"; b20 fs gc --status --retention 0 | sed 's/^/        /'; }
    # The entry was never renamed, so it is not `.deleting` to anybody: `restore` still works.
    # (Checked here rather than after the collection below, which is where it stops being true.)
    b20 fs restore "$B20W" > /dev/null 2>&1 \
        && ok PR20 "and the world can still be restored out of the trash" \
        || { bad PR20 "and the world can still be restored out of the trash"; b20 fs list | sed 's/^/        /'; }
    # Take the stray away and discard the world again: the entry the collector refused to touch
    # is collected exactly as it always would have been. Nothing about it was ever broken.
    rm -rf "$B20ENTRY.deleting"
    b20 fs discard "$B20W" > /dev/null 2>&1
    b20 fs gc --now --retention 0 > /dev/null 2>&1
    if [ -z "$(ls -A "$B20STORE/trash" 2>/dev/null)" ]; then
        ok PR20 "and once the stray is out of the way the entry is collected as usual"
    else
        bad PR20 "and once the stray is out of the way the entry is collected as usual"
        ls -A "$B20STORE/trash" | sed 's/^/        /'
    fi
else
    bad PR20 "the discarded world has a trash entry"
fi

# ---- PR #1 review (25th round, P1): a trash entry is ours only if it IS our tree ---------------
# The collector asked one question before renaming an entry to `.deleting` and deleting it: does
# the row still NAME this path? For a world discarded across volumes the entry lives at
# `<parent>/.wfs-trash/W<id>-<timestamp>` -- the user's own directory, under a name they can
# work out -- so during the retention period they can move the real tree away and leave something
# else at exactly that path, and the collector would delete THAT, contents and all. `discard
# --now` did the same and reported success; `restore` renamed the stranger home, re-stat'ed it
# and wrote its dev/ino into the row, registering somebody else's directory as the world.
# The row carries dir_dev/dir_ino from the moment the world was published, and every rename on
# this path is same-volume (the store's trash, or a `.wfs-trash` beside the world), so the
# inode is the world's identity all the way through the trash. Nothing is renamed or removed
# until it matches. (The EXDEV side-trash cannot be exercised without a second volume; the entry
# below is in <store>/trash, which is the same code path -- the check runs off the row, not off
# where the entry happens to live.)
F25STORE="$SCRATCH/foreign25-store"
f25() { "$WORLD" --store "$F25STORE" "$@"; }
F25SRC="$SCRATCH/foreign25-src"
mkdir -p "$F25SRC"
echo x > "$F25SRC/f.txt"
F25S=$(f25 fs init "$F25SRC" --name foreign25 2>/dev/null | awk '/^S[0-9]/{print $1}')
F25W=$(f25 fs fork --from "$F25S" --to "$SCRATCH/foreign25-w" --name f25w 2>/dev/null | awk '/^W[0-9]/{print $1}')
f25 fs discard "$F25W" > /dev/null 2>&1
F25ENTRY=$(ls -d "$F25STORE/trash/$F25W"-* 2>/dev/null | head -1)
if [ -n "$F25ENTRY" ]; then
    # The substitution: the world's tree moved aside, and a directory of the user's -- with a
    # file in it -- put at the exact path the row names.
    mv "$F25ENTRY" "$SCRATCH/foreign25-real"
    mkdir -p "$F25ENTRY/keep"
    echo "not the world" > "$F25ENTRY/keep/user.txt"
    out=$(f25 fs gc --now --retention 0 2>&1)
    if [ -f "$F25ENTRY/keep/user.txt" ]; then
        ok PR25 "the collector does not delete a directory that is not the world's tree"
    else
        bad PR25 "the collector does not delete a directory that is not the world's tree"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "could not be collected" \
        && ok PR25 "and gc says so instead of reporting a clean run" \
        || { bad PR25 "and gc says so instead of reporting a clean run"; echo "$out" | sed 's/^/        /'; }
    f25 fs gc --status --retention 0 | grep -q "^foreign: .*$F25ENTRY" \
        && ok PR25 "and gc --status names the path it refused to touch" \
        || { bad PR25 "and gc --status names the path it refused to touch"; f25 fs gc --status --retention 0 | sed 's/^/        /'; }
    out=$(f25 fs discard "$F25W" --now 2>&1); rc=$?
    if [ "$rc" != 0 ] && [ -f "$F25ENTRY/keep/user.txt" ]; then
        ok PR25 "--now refuses rather than delete it"
    else
        bad PR25 "--now refuses rather than delete it (exit $rc)"
        echo "$out" | sed 's/^/        /'
    fi
    echo "$out" | grep -q "$F25ENTRY" \
        && ok PR25 "and the refusal names the path" \
        || { bad PR25 "and the refusal names the path"; echo "$out" | sed 's/^/        /'; }
    out=$(f25 fs restore "$F25W" 2>&1); rc=$?
    if [ "$rc" != 0 ] && [ -f "$F25ENTRY/keep/user.txt" ] && [ ! -e "$SCRATCH/foreign25-w" ]; then
        ok PR25 "and restore refuses to bring a stranger home as the world"
    else
        bad PR25 "and restore refuses to bring a stranger home as the world (exit $rc)"
        echo "$out" | sed 's/^/        /'
        ls -A "$SCRATCH/foreign25-w" 2>/dev/null | sed 's/^/        /'
    fi
    # Put the world's own tree back where the row says it is: everything works exactly as it
    # always did. Nothing about the entry was ever broken -- it just was not there.
    rm -rf "$F25ENTRY"
    mv "$SCRATCH/foreign25-real" "$F25ENTRY"
    if f25 fs restore "$F25W" > /dev/null 2>&1 && [ -f "$SCRATCH/foreign25-w/f.txt" ]; then
        ok PR25 "and with the world's own tree back at that path restore works"
    else
        bad PR25 "and with the world's own tree back at that path restore works"
        f25 fs list | sed 's/^/        /'
    fi
    f25 fs discard "$F25W" > /dev/null 2>&1
    f25 fs gc --now --retention 0 > /dev/null 2>&1
    if [ -z "$(ls -A "$F25STORE/trash" 2>/dev/null)" ]; then
        ok PR25 "and the collector takes it as usual"
    else
        bad PR25 "and the collector takes it as usual"
        ls -A "$F25STORE/trash" | sed 's/^/        /'
    fi
else
    bad PR25 "the discarded world has a trash entry"
fi


echo
"$WORLD" fs status | sed 's/^/      /'
echo
echo "safety: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
