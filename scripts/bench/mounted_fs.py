#!/usr/bin/env python3
"""Compare native and mounted World metadata-heavy file operations safely."""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path


def percentile(values, q):
    ordered = sorted(values)
    # Nearest-rank percentile, stable and easy to reproduce from raw samples.
    return ordered[max(0, min(len(ordered) - 1, (len(ordered) * q + 0.999999).__floor__() - 1))]


def run_git(path, *args, env=None):
    clean_env = dict(env if env is not None else os.environ)
    identity = {key: clean_env[key] for key in (
        "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL"
    ) if key in clean_env}
    clean_env = {key: value for key, value in clean_env.items() if not key.startswith("GIT_")}
    clean_env.update(identity)
    clean_env.update({"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
                      "GIT_TEMPLATE_DIR": "/dev/null"})
    subprocess.run(["git", "-C", str(path), *args], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=clean_env)


def inside(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def create_workspace(root, token, files):
    name = f".forkfs-perf-{token}"
    path = root / name
    path.mkdir(mode=0o700)  # exclusive: never reuse or overwrite an existing workspace
    marker = path / ".forkfs-bench-owner"
    marker.write_text(token + "\n", encoding="ascii")
    try:
        (path / "tracked").mkdir()
        payload = (b"forkfs mounted filesystem benchmark fixture\n" * 16)[:512]
        for i in range(files):
            (path / "tracked" / f"file-{i:05d}.txt").write_bytes(payload)
        git_env = os.environ.copy()
        git_env.update({"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
                        "GIT_AUTHOR_NAME": "forkfs benchmark", "GIT_AUTHOR_EMAIL": "bench@example.invalid",
                        "GIT_COMMITTER_NAME": "forkfs benchmark", "GIT_COMMITTER_EMAIL": "bench@example.invalid"})
        run_git(path, "init", "-q", "--template=/dev/null", env=git_env)
        run_git(path, "add", "tracked", env=git_env)
        run_git(path, "commit", "-q", "-m", "benchmark fixture", env=git_env)
    except BaseException:
        safe_cleanup(path, root, token)
        raise
    return path


def safe_cleanup(path, root, token):
    # Only remove the exact direct child created by this invocation, with its private marker.
    if path.parent != root or not path.name.endswith(token):
        raise RuntimeError(f"refusing cleanup outside owned workspace: {path}")
    if path.is_symlink() or not path.is_dir():
        raise RuntimeError(f"refusing cleanup of replaced workspace: {path}")
    marker = path / ".forkfs-bench-owner"
    if marker.is_symlink() or not marker.is_file() or marker.read_text(encoding="ascii") != token + "\n":
        raise RuntimeError(f"workspace ownership marker changed; left in place: {path}")
    shutil.rmtree(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path, help="existing native comparison directory")
    parser.add_argument("world", type=Path, help="existing mounted World directory")
    parser.add_argument("--output", type=Path, required=True, help="existing output directory for a new results folder")
    parser.add_argument("--samples", type=int, default=100, help="samples per workload and side (1..1000; default 100)")
    parser.add_argument("--files", type=int, default=1000, help="tracked fixture files per side (samples..50000; default 1000)")
    args = parser.parse_args()
    if not 1 <= args.samples <= 1000:
        parser.error("--samples must be between 1 and 1000")
    if not args.samples <= args.files <= 50000:
        parser.error("--files must be between --samples and 50000")
    try:
        roots = [p.expanduser().resolve(strict=True) for p in (args.native, args.world)]
        output = args.output.expanduser().resolve(strict=True)
    except OSError as exc:
        parser.error(str(exc))
    if any(not p.is_dir() for p in (*roots, output)):
        parser.error("native, world, and output must be existing directories")
    if roots[0] == roots[1] or inside(roots[0], roots[1]) or inside(roots[1], roots[0]):
        parser.error("native and world must be separate, non-nested directories")
    try:
        if os.path.samefile(roots[0], roots[1]):
            parser.error("native and world resolve to the same directory")
    except OSError as exc:
        parser.error(f"cannot verify comparison directory identity: {exc}")
    if roots[0].stat().st_dev == roots[1].stat().st_dev:
        parser.error("native and world are on the same device; refusing because mounted World identity cannot be verified")
    if any(inside(output, p) for p in roots):
        parser.error("output must be outside the compared directories")

    token = uuid.uuid4().hex
    result_dir = output / f"forkfs-mounted-perf-{token}"
    result_dir.mkdir(mode=0o700)  # fail if any collision; never overwrite prior results
    paths = []
    raw_path = result_dir / "samples.jsonl"
    summary_path = result_dir / "summary.json"
    try:
        for root in roots:
            paths.append(create_workspace(root, token, args.files))
        # Make one tracked file dirty so Git measures a real changed-worktree status.
        (paths[0] / "tracked" / "file-00000.txt").write_bytes(b"native changed\n")
        (paths[1] / "tracked" / "file-00000.txt").write_bytes(b"world changed\n")
        stat_files = [[p / "tracked" / f"file-{i:05d}.txt" for i in range(args.samples)] for p in paths]
        # Cold means first stat of distinct paths in this process; OS caches are not dropped.
        workloads = ("stat_cold_first_path", "edit_save", "git_status", "metadata_create_delete", "stat_hot")
        raw = []
        rng = random.Random(0xF05F5)

        def measure(side, workload, sample):
            root = paths[side]
            started = time.perf_counter_ns()
            if workload == "edit_save":
                target = root / "tracked" / "file-00000.txt"
                with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".save-", delete=False) as f:
                    temp = Path(f.name)
                    line = f"save revision {sample} side {side}\n".encode()
                    f.write((line * ((4096 + len(line) - 1) // len(line)))[:4096])
                    os.fchmod(f.fileno(), target.stat().st_mode & 0o777)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(temp, target)
            elif workload == "git_status":
                run_git(root, "status", "--porcelain", "--untracked-files=normal")
            elif workload == "metadata_create_delete":
                item = root / "tracked" / f".meta-{sample:05d}"
                item.mkdir()
                (item / "entry").touch()
                (item / "entry").unlink()
                item.rmdir()
            elif workload == "stat_cold_first_path":
                os.stat(stat_files[side][sample])
            elif workload == "stat_hot":
                os.stat(root / "tracked" / "file-00000.txt")
            elapsed = time.perf_counter_ns() - started
            return elapsed

        with raw_path.open("x", encoding="utf-8") as out:
            for workload in workloads:
                for sample in range(args.samples):
                    sides = [0, 1]
                    if rng.randrange(2):
                        sides.reverse()
                    for side in sides:
                        ns = measure(side, workload, sample)
                        row = {"workload": workload, "side": "native" if side == 0 else "world",
                               "sample": sample, "elapsed_ns": ns}
                        raw.append(row)
                        out.write(json.dumps(row, separators=(",", ":")) + "\n")
            out.flush()
            os.fsync(out.fileno())

        summaries = []
        for workload in workloads:
            for side in ("native", "world"):
                values = [r["elapsed_ns"] for r in raw if r["workload"] == workload and r["side"] == side]
                summaries.append({"workload": workload, "side": side, "samples": len(values),
                                  "p50_ns": percentile(values, .50), "p95_ns": percentile(values, .95)})
        info = {"schema_version": 1, "created_unix_ns": time.time_ns(), "native_root": str(roots[0]),
                "world_root": str(roots[1]), "fixture_files": args.files, "samples_per_workload_side": args.samples,
                "stat_cold_definition": "first stat of distinct paths in process; OS cache is not dropped",
                "workspaces_cleaned": False, "results": summaries}
        with summary_path.open("x", encoding="utf-8") as out:
            json.dump(info, out, indent=2)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        print(result_dir)
    finally:
        for path, root in zip(paths, roots):
            if path.exists() or path.is_symlink():
                safe_cleanup(path, root, token)
        if summary_path.exists():
            data = json.loads(summary_path.read_text(encoding="utf-8"))
            data["workspaces_cleaned"] = True
            summary_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"mounted_fs benchmark: {exc}", file=sys.stderr)
        sys.exit(1)
