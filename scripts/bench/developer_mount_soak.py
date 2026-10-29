#!/usr/bin/env python3
"""Exercise real Git and compiler workloads on native and mounted filesystems.

This records comparative workload timings; it does not establish filesystem
correctness or prove that a supplied mount is backed by a particular driver.
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import traceback
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mounted_fs import create_workspace, inside, percentile, run_git, safe_cleanup


def run_command(argv, *, cwd=None, timeout=600, capture=True):
    """Run one fixed executable with an independent wall-clock deadline."""
    return subprocess.run(argv, cwd=cwd, check=True, timeout=timeout,
                          stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                          stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
                          text=True)


def atomic_edit(path, content):
    fd, temp_name = tempfile.mkstemp(prefix=".soak-save-", dir=path.parent)
    temp = Path(temp_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            os.fchmod(stream.fileno(), path.stat().st_mode & 0o777)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        dirfd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        if temp.exists():
            temp.unlink()


def project_source(index, value):
    return (f'#include "unit.h"\nint unit_{index}(void) {{ return {value} + SOAK_BIAS; }}\n').encode("ascii")


MAIN_SOURCE = b'''#include <stdio.h>\n#include "unit.h"\nint main(void) { printf("%d\\n", unit_0()+unit_1()+unit_2()); return 0; }\n'''
HEADER_SOURCE = b"#ifndef SOAK_UNIT_H\n#define SOAK_UNIT_H\n#define SOAK_BIAS 0\nint unit_0(void);\nint unit_1(void);\nint unit_2(void);\n#endif\n"


def ensure_program(binary, expected, timeout):
    result = run_command([str(binary)], timeout=timeout)
    actual = result.stdout.strip()
    if actual != str(expected):
        raise RuntimeError(f"program output mismatch: expected {expected}, got {actual!r}")


GIT_ENV = {"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null",
           "GIT_TEMPLATE_DIR": "/dev/null", "GIT_AUTHOR_NAME": "forkfs benchmark",
           "GIT_AUTHOR_EMAIL": "bench@example.invalid", "GIT_COMMITTER_NAME": "forkfs benchmark",
           "GIT_COMMITTER_EMAIL": "bench@example.invalid"}


def compiler_command(compiler, sysroot, *args):
    argv = [compiler]
    if sysroot is not None:
        argv.extend(["-isysroot", str(sysroot)])
    argv.extend(str(arg) for arg in args)
    return argv


def project_setup(root, compiler, sysroot, remaining):
    source = root / "src"
    build = root / "build"
    source.mkdir()
    build.mkdir()
    (source / "main.c").write_bytes(MAIN_SOURCE)
    (source / "unit.h").write_bytes(HEADER_SOURCE)
    values = [1, 2, 3]
    for i, value in enumerate(values):
        (source / f"unit{i}.c").write_bytes(project_source(i, value))
    objs = [build / f"unit{i}.o" for i in range(3)]
    for i, obj in enumerate(objs):
        run_command(compiler_command(compiler, sysroot, "-c", "-I", source, source / f"unit{i}.c", "-o", obj), timeout=remaining())
    main_obj = build / "main.o"
    run_command(compiler_command(compiler, sysroot, "-c", "-I", source, source / "main.c", "-o", main_obj), timeout=remaining())
    binary = build / "soak-program"
    run_command(compiler_command(compiler, sysroot, *[main_obj, *objs, "-o", binary]), timeout=remaining())
    ensure_program(binary, sum(values), remaining())
    return source, build, objs, binary, values


def git_call(root, remaining, *args, capture_output=False):
    return run_git(root, *args, env=GIT_ENV, timeout=remaining(), capture_output=capture_output)


def git_setup(root, remaining):
    (root / ".gitignore").write_text("/build/\n/.forkfs-bench-owner\n", encoding="ascii")
    git_call(root, remaining, "init", "-q", "--template=/dev/null")
    git_call(root, remaining, "add", "src", ".gitignore")
    git_call(root, remaining, "commit", "-q", "-m", "soak fixture")


def usage_measure(root, paths):
    apparent = allocated = 0
    for path in paths:
        if path.is_file():
            stat = path.stat()
            apparent += stat.st_size
            allocated += getattr(stat, "st_blocks", (stat.st_size + 511) // 512) * 512
    return {"apparent_bytes": apparent, "allocated_bytes": allocated}


def summarize_raw(raw):
    entries = []
    for op in sorted({item["op"] for item in raw}):
        for side in ("native", "world"):
            vals = [r["elapsed_ns"] for r in raw if r["op"] == op and r["side"] == side and r["outcome"] == "ok"]
            if vals:
                entry = {"op": op, "side": side, "count": len(vals), "p50_ns": percentile(vals, .50),
                         "p95_ns": None, "p99_ns": None, "tail_min_samples": 1000,
                         "tail_status": "reported" if len(vals) >= 1000 else "insufficient_samples"}
                if len(vals) >= 1000:
                    entry.update({"p95_ns": percentile(vals, .95), "p99_ns": percentile(vals, .99)})
                entries.append(entry)
    return entries


def exception_text(exc):
    rendered = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    stderr = getattr(exc, "stderr", None)
    if stderr:
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        rendered += "\nCaptured stderr:\n" + str(stderr)
    return rendered


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, type=Path, help="existing native fixture root")
    parser.add_argument("--world", type=Path, help="existing mounted World root (optional)")
    parser.add_argument("--output", required=True, type=Path, help="existing output directory")
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--files", type=int, default=32, help="tracked Git fixture files")
    parser.add_argument("--compiler", default="cc", help="compiler executable or PATH-resolved name")
    parser.add_argument("--sysroot", type=Path, help="SDK/sysroot directory passed to compiler and linker")
    parser.add_argument("--deadline", type=int, default=600, help="seconds allowed per external build/run command")
    args = parser.parse_args()
    if args.rounds < 1 or not 1 <= args.files <= 50000 or args.deadline < 1:
        parser.error("rounds/deadline must be positive; files must be 1..50000")
    try:
        native = args.native.expanduser().resolve(strict=True)
        output = args.output.expanduser().resolve(strict=True)
        world = args.world.expanduser().resolve(strict=True) if args.world else None
    except OSError as exc:
        parser.error(str(exc))
    if not native.is_dir() or not output.is_dir() or (world is not None and not world.is_dir()):
        parser.error("native, world, and output must be existing directories")
    try:
        sysroot = args.sysroot.expanduser().resolve(strict=True) if args.sysroot else None
    except OSError as exc:
        parser.error(f"invalid --sysroot: {exc}")
    if sysroot is not None and not sysroot.is_dir():
        parser.error("--sysroot must be an existing directory")
    roots = [native] + ([world] if world is not None else [])
    if world is not None:
        if not os.path.ismount(world):
            parser.error("--world must be an existing mount point")
        if native == world or inside(native, world) or inside(world, native):
            parser.error("native and world roots must be separate and non-nested")
        if os.path.samefile(native, world) or native.stat().st_dev == world.stat().st_dev:
            parser.error("native and world must have distinct filesystem devices")
    if any(inside(output, root) for root in roots):
        parser.error("output must be outside fixture roots")

    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"compiler not found: {args.compiler}")
    compiler = str(Path(compiler).resolve())
    started_at = time.monotonic()
    def remaining():
        value = args.deadline - (time.monotonic() - started_at)
        if value <= 0:
            raise subprocess.TimeoutExpired("developer_mount_soak overall deadline", args.deadline)
        return value

    version = None
    git_version = None
    token = uuid.uuid4().hex
    result_dir = output / f"forkfs-developer-mount-soak-{token}"
    result_dir.mkdir(mode=0o700)
    print(f"Artifacts: {result_dir}", flush=True)
    raw_path = result_dir / "samples.jsonl"
    summary_path = result_dir / "summary.json"
    paths = []
    raw = []
    failure = None
    cleanup_errors = []
    rng = random.Random(0xD3E10 + args.rounds + args.files)
    try:
        version = run_command([compiler, "--version"], timeout=remaining()).stdout.splitlines()[0]
        git_version = git_call(native, remaining, "--version", capture_output=True).stdout.strip()
        for root in roots:
            paths.append(create_workspace(root, token, args.files, command_timeout=remaining))
        side_values = []
        for root in paths:
            _, _, _, _, initial_values = project_setup(root, compiler, sysroot, remaining)
            side_values.append(initial_values)
            git_setup(root, remaining)
        max_space = {"native": {"apparent_bytes": 0, "allocated_bytes": 0}}
        if world is not None:
            max_space["world"] = {"apparent_bytes": 0, "allocated_bytes": 0}

        def record(op, side, round_no, fn):
            started = time.perf_counter_ns()
            outcome = "ok"
            detail = None
            try:
                fn()
            except Exception as exc:
                outcome = "error"
                detail = exception_text(exc)
                raise
            finally:
                row = {"op": op, "side": side, "round": round_no,
                       "elapsed_ns": time.perf_counter_ns() - started,
                       "outcome": outcome}
                if detail:
                    row["error"] = detail
                raw.append(row)
                with raw_path.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(row, separators=(",", ":")) + "\n")
                    stream.flush()
                    os.fsync(stream.fileno())

        for round_no in range(args.rounds):
            order = [0, 1] if len(paths) == 2 else [0]
            if rng.randrange(2):
                order.reverse()
            # Same logical edits on both sides, regardless of run order.
            for side in order:
                root = paths[side]
                label = "native" if side == 0 else "world"
                unit = round_no % 3
                source = root / "src" / f"unit{unit}.c"
                next_value = 10 + round_no
                values = side_values[side]
                def git_work(source=source, root=root, unit=unit, next_value=next_value):
                    atomic_edit(source, project_source(unit, next_value))
                    status = git_call(root, remaining, "status", "--porcelain", "--untracked-files=normal", capture_output=True)
                    diff = git_call(root, remaining, "diff", "--", f"src/unit{unit}.c", capture_output=True)
                    if f"src/unit{unit}.c" not in status.stdout or not diff.stdout:
                        raise RuntimeError("Git status did not observe edited translation unit")
                    git_call(root, remaining, "add", str(source.relative_to(root)))
                    git_call(root, remaining, "commit", "-q", "-m", f"soak round {round_no}")
                    if git_call(root, remaining, "status", "--porcelain", capture_output=True).stdout.strip():
                        raise RuntimeError("Git commit left unexpected worktree changes")
                    values[unit] = next_value
                record("git_edit_status_add_commit", label, round_no, git_work)

                build = root / "build"
                obj = build / f"unit{unit}.o"
                src = root / "src" / f"unit{unit}.c"
                binary = build / "soak-program"
                def full_build(root=root, unit=unit, next_value=next_value, obj=obj, src=src, build=build, binary=binary, values=values):
                    for i in range(3):
                        run_command(compiler_command(compiler, sysroot, "-c", "-I", root / "src", root / "src" / f"unit{i}.c", "-o", build / f"unit{i}.o"), timeout=remaining())
                    run_command(compiler_command(compiler, sysroot, "-c", "-I", root / "src", root / "src" / "main.c", "-o", build / "main.o"), timeout=remaining())
                    run_command(compiler_command(compiler, sysroot, build / "main.o", *(build / f"unit{i}.o" for i in range(3)), "-o", binary), timeout=remaining())
                    expected = sum((next_value if i == unit else values[i]) for i in range(3))
                    ensure_program(binary, expected, remaining())
                record("compile_full_four_objects_link_run", label, round_no, full_build)

                inc_value = next_value + 1
                def incremental(root=root, unit=unit, next_value=inc_value, obj=obj, src=src, build=build, binary=binary, values=values):
                    atomic_edit(src, project_source(unit, next_value))
                    run_command(compiler_command(compiler, sysroot, "-c", "-I", root / "src", src, "-o", obj), timeout=remaining())
                    run_command(compiler_command(compiler, sysroot, build / "main.o", *(build / f"unit{i}.o" for i in range(3)), "-o", binary), timeout=remaining())
                    expected = sum((next_value if i == unit else values[i]) for i in range(3))
                    ensure_program(binary, expected, remaining())
                    values[unit] = next_value
                record("compile_incremental_affected_object_link_run", label, round_no, incremental)
                # Keep the next round's Git baseline clean; this housekeeping is
                # outside the measured compiler operation.
                git_call(root, remaining, "add", str(src.relative_to(root)))
                git_call(root, remaining, "commit", "-q", "-m", f"incremental round {round_no}")
                space = usage_measure(root, list(root.rglob("*")))
                max_space[label] = {k: max(max_space[label][k], v) for k, v in space.items()}
                with (result_dir / "storage.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps({'round': round_no, 'side': label, 'time_ns': time.time_ns(), **space}) + "\n")

        info = {"schema_version": 1, "created_unix_ns": time.time_ns(),
                "mode": "mounted_compare" if world else "native_only_no_mounted_acceptance",
                "native_root": str(native), "world_root": str(world) if world else None,
                "world_identity_note": "os.path.ismount and distinct st_dev checks only; caller identity cannot prove the mount is the target filesystem",
                "compiler": {"requested": args.compiler, "resolved_path": compiler, "version": version,
                             "sysroot": str(sysroot) if sysroot else None,
                             "argv_flags": ["-isysroot", str(sysroot)] if sysroot else [],
                             "cache_scope": "OS and compiler caches are left in their natural state; no cache dropping"},
                "rounds": args.rounds, "git_fixture_files": args.files, "command_deadline_seconds": args.deadline,
                "workspace_peak_usage": max_space, "sampled_peaks_exact": False,
                "git_version": git_version, "deadline_scope": "whole run; cleanup may take additional time",
                "workspace_usage_scope": "regular-file samples in the fixture including Git; not backing-store or device allocation",
                "results": summarize_raw(raw),
                "workspaces_cleaned": False, "outcome": "ok", "error": None}
        summary = info
    except BaseException as exc:
        failure = exc
        summary = {"schema_version": 1, "created_unix_ns": time.time_ns(),
                   "mode": "mounted_compare" if world else "native_only_no_mounted_acceptance",
                   "native_root": str(native), "world_root": str(world) if world else None,
                   "compiler": {"requested": args.compiler, "resolved_path": compiler,
                                "version": version, "sysroot": str(sysroot) if sysroot else None,
                                "argv_flags": ["-isysroot", str(sysroot)] if sysroot else []},
                   "rounds": args.rounds, "git_fixture_files": args.files,
                   "command_deadline_seconds": args.deadline, "workspace_peak_usage": max_space if 'max_space' in locals() else {},
                   "results": summarize_raw(raw), "workspaces_cleaned": False, "outcome": "error",
                   "error": exception_text(exc)}
    finally:
        for path, root in zip(paths, roots):
            if path.exists() or path.is_symlink():
                try:
                    safe_cleanup(path, root, token)
                except BaseException as exc:
                    cleanup_errors.append(f"{path}: {type(exc).__name__}: {exc}")
        summary["workspaces_cleaned"] = not cleanup_errors and len(paths) == len(roots)
        summary["cleanup_errors"] = cleanup_errors
        if cleanup_errors:
            summary['outcome'] = 'error'
            summary['error'] = (summary.get('error') or '') + "\nCleanup errors: " + "; ".join(cleanup_errors)
        with summary_path.open("x", encoding="utf-8") as stream:
            json.dump(summary, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    if failure is not None:
        raise failure
    if cleanup_errors:
        raise RuntimeError("workspace cleanup failed: " + "; ".join(cleanup_errors))
    print(result_dir)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
        print(f"developer_mount_soak: {exception_text(exc)}", file=sys.stderr)
        sys.exit(1)
