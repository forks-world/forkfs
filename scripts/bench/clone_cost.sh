#!/bin/bash
# E4 / E6: what does an APFS clonefile()-based World fork actually cost?
#
# E4  fork cost model for the REDIRECT design: clone one node_modules-sized native directory.
# E6a fork latency vs tree size for a "native-root World" (the whole base tree is cloned).
# E6b storage: du of clone vs original, growth after modifying 1%, COW isolation check.
# E6d changed-set computation without a namespace layer (stat walk / git status).
# E6e volume constraints of clonefile().
#
# Everything runs on plain APFS; nothing touches the worldfs mount.
# Usage: scripts/bench/clone_cost.sh <native-work-root>
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
R=$1
HERE=$(cd "$(dirname "$0")/../.." && pwd)
PY=python3
mkdir -p "$R"

CLONE_PY="$R/_clone.py"
cat > "$CLONE_PY" <<'PY'
# clonefile(2) via ctypes. On a directory APFS clones the whole subtree in one call.
import ctypes, ctypes.util, os, sys, time
libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
libc.clonefile.restype = ctypes.c_int
CLONE_NOFOLLOW, CLONE_NOOWNERCOPY, CLONE_ACL = 0x1, 0x2, 0x4

def clone(src, dst, flags=0):
    rc = libc.clonefile(src.encode(), dst.encode(), flags)
    if rc != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e), src, None, dst)
    return rc

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    t0 = time.perf_counter()
    try:
        clone(src, dst)
    except OSError as e:
        print(f"ERRNO {e.errno} {e.strerror}")
        sys.exit(3)
    print(f"{time.perf_counter()-t0:.4f}")
PY

MK_PY="$R/_mktree.py"
cat > "$MK_PY" <<'PY'
import os, random, sys
d, n, perdir = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
random.seed(7)
blob = os.urandom(8192)
made = 0; k = 0
while made < n:
    p = os.path.join(d, f"pkg{k:04d}", "lib"); os.makedirs(p, exist_ok=True)
    for j in range(min(perdir, n - made)):
        sz = random.randrange(1024, 8193)
        with open(os.path.join(p, f"f{j:04d}.js"), "wb") as f: f.write(blob[:sz])
        made += 1
    k += 1
print(made)
PY

t() { $PY -c "import time;print(f'{time.perf_counter():.6f}')"; }
el() { $PY -c "print(f'{$2-$1:.4f}')"; }
dus() { du -sk "$1" | awk '{print $1}'; }

echo "############ E6e volume constraints of clonefile()"
$PY -c "import os;print('  work root device :', os.stat('$R').st_dev)"
$PY -c "import os,pathlib;p=pathlib.Path.home()/'Library/Containers';print('  ~/Library/Containers device :', os.stat(p).st_dev)"
df -h "$R" | tail -1 | awk '{print "  df(work root) :", $1, "->", $9}'
df -h "$HOME/Library/Containers" | tail -1 | awk '{print "  df(containers):", $1, "->", $9}'
diskutil info "$(df "$R" | tail -1 | awk '{print $1}')" 2>/dev/null | grep -E 'Volume Name|File System Personality|APFS Container' | sed 's/^ */  /'
# cross-volume attempt: a RAM disk is a different APFS container
RD=""
if DEV=$(hdiutil attach -nomount ram://131072 2>/dev/null); then
    DEV=$(echo "$DEV" | tr -d ' ')
    if newfs_apfs -v CLONEX "$DEV" >/dev/null 2>&1 && mkdir -p "$R/xvol" && mount -t apfs "$DEV" "$R/xvol" 2>/dev/null; then
        RD=$DEV
        echo hello > "$R/xsrc.txt"
        echo -n "  clonefile across volumes (APFS ram disk): "
        $PY "$CLONE_PY" "$R/xsrc.txt" "$R/xvol/xdst.txt" || true
        umount "$R/xvol" 2>/dev/null
    fi
    [ -n "$RD" ] && hdiutil detach "$RD" >/dev/null 2>&1
fi
[ -z "$RD" ] && echo "  (could not build a second APFS volume without privileges; clonefile(2) documents EXDEV when src and dst are not on the same volume)"

echo
echo "############ E4 fork cost for one redirected node_modules-sized dir (20000 files, 1-8KB)"
rm -rf "$R/big" "$R/big-clone" "$R/big-cpc" "$R/big-cp"
mkdir -p "$R/big"; $PY "$MK_PY" "$R/big" 20000 200 >/dev/null
echo "  files=$(find "$R/big" -type f | wc -l | tr -d ' ')  dirs=$(find "$R/big" -type d | wc -l | tr -d ' ')  du=$(dus "$R/big")k"
for rep in 1 2; do
    rm -rf "$R/big-clone"; s=$($PY "$CLONE_PY" "$R/big" "$R/big-clone"); echo "  [$rep] clonefile(dir, recursive)  ${s}s   ($($PY -c "print(f'{1e6*$s/20000:.1f}')") us/file)"
done
for rep in 1 2; do
    rm -rf "$R/big-cpc"; a=$(t); cp -c -R "$R/big" "$R/big-cpc"; b=$(t); echo "  [$rep] cp -c -R                   $(el $a $b)s   ($($PY -c "print(f'{1e6*$(el $a $b)/20000:.1f}')") us/file)"
done
for rep in 1 2; do
    rm -rf "$R/big-cp"; a=$(t); cp -R "$R/big" "$R/big-cp"; b=$(t); echo "  [$rep] cp -R (real copy)           $(el $a $b)s   ($($PY -c "print(f'{1e6*$(el $a $b)/20000:.1f}')") us/file)"
done
echo "  du -sk  original=$(dus "$R/big")k  clonefile=$(dus "$R/big-clone")k  cp-c-R=$(dus "$R/big-cpc")k  cp-R=$(dus "$R/big-cp")k"
echo "  free-space delta check: df avail before/after is the honest measure ->"
df -k "$R" | tail -1 | awk '{print "    df avail now:", $4"k"}'
rm -rf "$R/big-cpc" "$R/big-cp"

echo
echo "############ E4(v) clonefile of a single 2GB file"
rm -f "$R/big2g.bin" "$R/big2g.clone"
$PY - "$R/big2g.bin" <<'PY'
import os, sys
buf = os.urandom(8 << 20)
with open(sys.argv[1], "wb") as f:
    for _ in range(256): f.write(buf)
    f.flush(); os.fsync(f.fileno())
PY
ls -lh "$R/big2g.bin" | awk '{print "  size:", $5}'
for rep in 1 2; do
    rm -f "$R/big2g.clone"; s=$($PY "$CLONE_PY" "$R/big2g.bin" "$R/big2g.clone"); echo "  [$rep] clonefile(2GB file)  ${s}s"
done
a=$(t); cp "$R/big2g.bin" "$R/big2g.copy"; b=$(t); echo "  cp (real copy 2GB)   $(el $a $b)s"
echo "  du: orig=$(dus "$R/big2g.bin")k clone=$(dus "$R/big2g.clone")k copy=$(dus "$R/big2g.copy")k"
rm -f "$R/big2g.copy"

echo
echo "############ E6a fork latency vs tree size (native-root World = clone the whole base)"
printf '  %-14s %8s %10s %10s %10s %10s\n' tree files clone_s us/file cp-c-R_s us/file
for n in 1000 10000 50000; do
    rm -rf "$R/t$n" "$R/t$n-clone" "$R/t$n-cpc"
    mkdir -p "$R/t$n"; $PY "$MK_PY" "$R/t$n" $n 200 >/dev/null
    best=""; for rep in 1 2; do rm -rf "$R/t$n-clone"; s=$($PY "$CLONE_PY" "$R/t$n" "$R/t$n-clone"); best=$($PY -c "print(min([x for x in ['$best','$s'] if x] , key=float))"); done
    bc=""; for rep in 1 2; do rm -rf "$R/t$n-cpc"; a=$(t); cp -c -R "$R/t$n" "$R/t$n-cpc"; b=$(t); s=$(el $a $b); bc=$($PY -c "print(min([x for x in ['$bc','$s'] if x], key=float))"); done
    printf '  %-14s %8s %10s %10s %10s %10s\n' "synthetic-$n" "$n" "$best" "$($PY -c "print(f'{1e6*$best/$n:.1f}')")" "$bc" "$($PY -c "print(f'{1e6*$bc/$n:.1f}')")"
    [ "$n" != 50000 ] && rm -rf "$R/t$n" "$R/t$n-clone"
    rm -rf "$R/t$n-cpc"
done
# real repo including .git
rm -rf "$R/fmt" "$R/fmt-clone" "$R/fmt-cpc"
cp -R "$HERE/third_party/fmt" "$R/fmt" 2>/dev/null
nf=$(find "$R/fmt" | wc -l | tr -d ' ')
best=""; for rep in 1 2; do rm -rf "$R/fmt-clone"; s=$($PY "$CLONE_PY" "$R/fmt" "$R/fmt-clone"); best=$($PY -c "print(min([x for x in ['$best','$s'] if x], key=float))"); done
bc=""; for rep in 1 2; do rm -rf "$R/fmt-cpc"; a=$(t); cp -c -R "$R/fmt" "$R/fmt-cpc"; b=$(t); s=$(el $a $b); bc=$($PY -c "print(min([x for x in ['$bc','$s'] if x], key=float))"); done
printf '  %-14s %8s %10s %10s %10s %10s\n' "fmt(+.git)" "$nf" "$best" "$($PY -c "print(f'{1e6*$best/$nf:.1f}')")" "$bc" "$($PY -c "print(f'{1e6*$bc/$nf:.1f}')")"
rm -rf "$R/fmt-cpc"

echo
echo "############ E6b storage: du after clone, growth after touching 1%, COW isolation"
O=$R/t50000; C=$R/t50000-clone
echo "  original du=$(dus "$O")k   clone du=$(dus "$C")k   (du counts logical blocks; clones share them)"
BEFORE=$(df -k "$R" | tail -1 | awk '{print $4}')
$PY - "$C" <<'PY'
import os, sys
root = sys.argv[1]
files = sorted(os.path.join(r, f) for r, _, fs in os.walk(root) for f in fs)
sel = files[::100]            # 1%
for p in sel:
    with open(p, "ab") as f: f.write(b"x" * 100)
print(f"  modified {len(sel)} files (1%), {len(sel)*100} bytes appended = {len(sel)*100/1024:.1f} KiB of new data")
PY
sync
AFTER=$(df -k "$R" | tail -1 | awk '{print $4}')
echo "  df avail  before=${BEFORE}k after=${AFTER}k  -> consumed $((BEFORE-AFTER))k"
echo "  du clone after edits = $(dus "$C")k (was $(dus "$O")k for the original)"
$PY - "$O" "$C" <<'PY'
import os, sys, filecmp, random
o, c = sys.argv[1], sys.argv[2]
files = sorted(os.path.relpath(os.path.join(r, f), c) for r, _, fs in os.walk(c) for f in fs)
mod = set(files[::100])
random.seed(11)
sample = random.sample([f for f in files if f not in mod], 200)
bad = [f for f in sample if not filecmp.cmp(os.path.join(o, f), os.path.join(c, f), shallow=False)]
print(f"  COW isolation: 200 untouched files sampled, byte-identical mismatches = {len(bad)}")
o_mod = [f for f in list(mod)[:200] if os.path.getsize(os.path.join(o, f)) == os.path.getsize(os.path.join(c, f))]
print(f"  of the modified files, originals that ALSO changed size = {len(o_mod)} (must be 0)")
print(f"  inode of a shared file: original={os.stat(os.path.join(o, files[1])).st_ino} clone={os.stat(os.path.join(c, files[1])).st_ino} (distinct inodes, shared extents)")
PY

echo
echo "############ E6d changed-set without a namespace layer (50k files)"
$PY - "$C" <<'PY'
import os, sys, random
root = sys.argv[1]
random.seed(13)
files = sorted(os.path.join(r, f) for r, _, fs in os.walk(root) for f in fs)
# 500 modified (on top of the 1% already appended), 200 added, 100 deleted
for p in random.sample(files, 500):
    with open(p, "ab") as f: f.write(b"m")
d = os.path.join(root, "pkg0000", "lib")
for i in range(200):
    open(os.path.join(d, f"added{i}.js"), "wb").write(b"new")
for p in random.sample(files, 100):
    try: os.unlink(p)
    except FileNotFoundError: pass
print("  applied: 500 modified, 200 added, 100 deleted")
PY
DIFF_PY=$R/_diff.py
cat > "$DIFF_PY" <<'PY'
import os, sys, time
a, b = sys.argv[1], sys.argv[2]
def snap(root):
    out = {}
    stack = [root]
    while stack:
        d = stack.pop()
        with os.scandir(d) as it:
            for e in it:
                if e.is_dir(follow_symlinks=False): stack.append(e.path)
                else:
                    st = e.stat(follow_symlinks=False)
                    out[os.path.relpath(e.path, root)] = (st.st_size, st.st_mtime_ns, st.st_ino, st.st_ctime_ns)
    return out
t0 = time.perf_counter()
A = snap(a); B = snap(b)
added = B.keys() - A.keys(); deleted = A.keys() - B.keys()
mod = {k for k in A.keys() & B.keys() if A[k][:2] != B[k][:2]}
t1 = time.perf_counter()
print(f"  stat-walk diff over {len(A)}+{len(B)} entries: {t1-t0:.3f}s -> +{len(added)} -{len(deleted)} ~{len(mod)}")
t0 = time.perf_counter()
B2 = snap(b)
print(f"  (one-sided scandir walk of the clone only: {time.perf_counter()-t0:.3f}s, {len(B2)} entries)")
PY
for rep in 1 2; do $PY "$DIFF_PY" "$O" "$C"; done
# git status on a real repo of comparable shape
rm -rf "$R/fmtgit"; cp -c -R "$HERE/third_party/fmt" "$R/fmtgit" 2>/dev/null
if [ -d "$R/fmtgit/.git" ]; then
    git -C "$R/fmtgit" status --porcelain >/dev/null 2>&1
    $PY - "$R/fmtgit" <<'PY'
import os, random, sys
root = sys.argv[1]
random.seed(5)
files = [os.path.join(r,f) for r,_,fs in os.walk(root) for f in fs if '.git' not in r]
for p in random.sample(files, min(500, len(files))):
    with open(p, "ab") as f: f.write(b"\n// x\n")
print(f"  fmt repo: {len(files)} tracked-ish files, 500 modified")
PY
    for rep in 1 2; do
        a=$(t); git -C "$R/fmtgit" status --porcelain >/dev/null 2>&1; b=$(t)
        echo "  git status --porcelain on fmt (~$(find "$R/fmtgit" -type f | wc -l | tr -d ' ') files): $(el $a $b)s"
    done
    rm -rf "$R/fmtgit"
fi
echo "  FSEvents note: fseventsd journals every APFS volume ( /.fseventsd ); a native-root World could"
echo "  subscribe with FSEventStreamCreate(sinceWhen=<fork event id>) and get the changed paths without any"
echo "  walk. Not implemented here (fs_usage needs root); recorded as the option that removes the O(tree) scan."
ls -d /.fseventsd 2>/dev/null | sed 's/^/  fseventsd journal present: /'

echo
echo "############ done"
