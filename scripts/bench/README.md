# Mounted filesystem benchmark

Compare real file operations in an existing native directory and mounted World. The script
creates a uniquely named, private fixture inside each directory, initializes an isolated Git
repository before timing, and removes only those fixture directories after checking their
ownership markers. It never mounts or formats a volume.

```bash
mkdir -p build/bench-results
python3 scripts/bench/mounted_fs.py /path/to/native /path/to/mounted-world \
  --output build/bench-results --files 1000 --samples 100
```

`--output` must already exist. Each invocation creates a new results directory containing
`samples.jsonl` (one raw nanosecond sample per workload, side, and iteration) and
`summary.json` (p50/p95 plus fixture and cache definitions). Workloads are first stat of distinct
paths, atomic 4 KiB save (fsync + rename), Git status on a dirty tracked file, create/delete of a
directory and child, and repeated stat of one path. Git fixture creation and initial commit are
outside the measured samples. Native and World operations are interleaved. The roots must have
different device IDs; otherwise the script cannot verify that the World is a separate mount.
Different device IDs alone do not authenticate a forkfs World; check the mount identity separately.

“Cold” means first stat of a distinct path in this process; OS caches are not flushed, so it
does not mean a physical cold cache or a newly mounted volume. The results measure the mounted
data path only. They exclude mount setup, isolation, broker, and history costs, and cannot by
themselves establish the redesign's performance budgets. Use `--files 50000` for the 50k-file
Git workload; sample count is capped at 1000 and may not exceed fixture size.

`apfs_active_fork_probe.py` is a separate macOS-only experiment for the active-image clone
question. It creates a 160 MiB APFS sparse image under `/private/tmp`, runs a continuous append
writer, then measures writer stop + sync/close + detach + `cp -c` + remount of the source and
clone. It checks the quiesced log byte-for-byte and verifies writes diverge after remount. The
default is five repetitions; it reports each raw cycle, the number of resumed writes, and medians,
never p95. Give it a new JSON
output path whose parent already exists:

```bash
python3 scripts/bench/apfs_active_fork_probe.py --output build/bench-results/apfs-active.json
```

The probe never clones a mounted image. If it cannot verify that all of its image device entries
detached, it preserves the `/private/tmp` directory and prints a warning. Results include source
write-pause time, complete stop-to-both-mounts time, copy time, validation flags, and errors. This
small synthetic writer does not model a large project, VM workload, power loss, or crash-consistency
semantics beyond the explicit clean stop/sync/detach sequence.

`apfs_container_compare.py` compares three outer image containers with the same deterministic
APFS fixture: UDIF read/write (`UDRW`), sparse single-file (`SPARSE`), and that same
pristine sparse base attached with `hdiutil -shadow`. It creates 160 MiB base images under
`/private/tmp` and allows at most one 160 MiB trial clone or shadow at once (480 MiB maximum
simultaneous logical image capacity). Five or more rounds use a rotating order. Each attach is
followed by identical fixture warming before measuring 4 KiB fsync+rename, 64 small create/unlink
operations, and 4 MiB sequential write+fsync. The JSON reports each timing, attach/detach costs,
virtual capacity, image-file length, initial allocated bytes, and per-sample clone/overlay
allocation growth. It also records host APFS available-space delta as a noisy volume-level
cross-check. Per-file `st_blocks` is an allocation count, not exclusive extent accounting for
APFS clones. It does not flush host caches; attach timing is not a cold-mount claim.

For the shadow case, direct SPARSE and SPARSE+shadow use the same pristine base image. The script
checks that `hdiutil` created the specified shadow before writing and hashes the entire base image
before/after each trial. After each measured detach, it remounts the working image and validates
the exact 4 KiB edit, the 4 MiB sequential file length and hash, and that all create/unlink files
remain absent. Any changed base hash or failed read-back fails the sample. Mount and cleanup decisions use
the exact image path, device node, and mountpoint from `hdiutil info -plist`; if a private image
device remains attached or its state cannot be verified, the script preserves its `/private/tmp`
directory and prints a warning. It never attaches or detaches a physical volume. Results include
medians only; five samples are not used to claim p95.

```bash
python3 scripts/bench/apfs_container_compare.py \
  --output build/bench-results/apfs-container-compare.json
```
