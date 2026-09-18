#!/bin/bash
# Passthrough semantics smoke test. Usage: scripts/smoke.sh <backing-dir> <mountpoint>
# Requires the worldfs mount to be active. Exits non-zero on the first failure.
set -euo pipefail
B=$1; M=$2
fail() { echo "FAIL: $*"; exit 1; }
ok()   { echo "ok   $*"; }

[ -d "$M" ] || fail "mountpoint missing"
mount | grep -q " $M " || fail "nothing mounted at $M"

# lookup / readdir / read
echo "smoke-$$" > "$B/smoke.txt"
[ "$(cat "$M/smoke.txt")" = "smoke-$$" ] || fail "read through mount"
ok read
ls "$M" | grep -q smoke.txt || fail "readdir"
ok readdir
# stat parity
[ "$(stat -f '%z %p' "$B/smoke.txt")" = "$(stat -f '%z %p' "$M/smoke.txt")" ] || fail "stat parity"
ok stat
# write / truncate / append
echo "via-mount" > "$M/w.txt"; [ "$(cat "$B/w.txt")" = "via-mount" ] || fail "write"
echo "more" >> "$M/w.txt"; [ "$(wc -l < "$B/w.txt")" -eq 2 ] || fail "append"
: > "$M/w.txt"; [ ! -s "$B/w.txt" ] || fail "truncate"
ok write/append/truncate
# mkdir / rename / rm
mkdir "$M/d1"; mv "$M/d1" "$M/d2"; [ -d "$B/d2" ] || fail "mkdir+rename dir"
touch "$M/d2/f"; mv "$M/d2/f" "$M/d2/g"; [ -e "$B/d2/g" ] || fail "rename file"
rm "$M/d2/g"; rmdir "$M/d2"; [ ! -e "$B/d2" ] || fail "unlink/rmdir"
ok mkdir/rename/unlink/rmdir
# symlink / hardlink
ln -s smoke.txt "$M/s"; [ "$(readlink "$M/s")" = "smoke.txt" ] || fail "symlink"
ln "$M/smoke.txt" "$M/h"; [ "$(stat -f %l "$B/smoke.txt")" -eq 2 ] || fail "hardlink"
rm "$M/s" "$M/h"
ok symlink/hardlink
# chmod / xattr
chmod 600 "$M/smoke.txt"; [ "$(stat -f %p "$B/smoke.txt")" = "100600" ] || fail "chmod"
xattr -w user.k v "$M/smoke.txt"; [ "$(xattr -p user.k "$B/smoke.txt")" = "v" ] || fail "setxattr"
xattr -d user.k "$M/smoke.txt"
ok chmod/xattr
# mmap read + write correctness
python3 - "$M/smoke.txt" <<'PY'
import mmap, sys
p = sys.argv[1]
with open(p, "r+b") as f:
    f.truncate(8192)
    mm = mmap.mmap(f.fileno(), 8192)
    mm[4096:4100] = b"MMAP"
    mm.flush(); mm.close()
with open(p, "rb") as f:
    f.seek(4096); assert f.read(4) == b"MMAP", "mmap write not visible"
PY
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); assert d[4096:4100]==b'MMAP'" "$B/smoke.txt" || fail "mmap write reached backing"
ok mmap
# fsync
python3 -c "import os,sys; fd=os.open(sys.argv[1],os.O_RDWR); os.write(fd,b'x'); os.fsync(fd); os.close(fd)" "$M/smoke.txt" || fail "fsync"
ok fsync
rm -f "$B/smoke.txt" "$B/w.txt"
echo "ALL OK"
