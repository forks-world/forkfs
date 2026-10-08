#!/usr/bin/env python3
"""Measure a quiesced APFS sparse-image clone while a writer is active."""

import argparse
import hashlib
import json
import os
import plistlib
import select
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

IMAGE_MIB = 160
TMP_ROOT = Path("/private/tmp")
WRITER_CODE = r'''
import hashlib, json, os, signal, sys, time
path, start = sys.argv[1], int(sys.argv[2])
running = True
def stop(_sig, _frame):
    global running
    running = False
signal.signal(signal.SIGINT, stop)
fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
seq = start
try:
    while running:
        payload = f"active-apfs-probe-{seq}".encode()
        row = {"seq": seq, "payload": payload.decode(), "sha256": hashlib.sha256(payload).hexdigest()}
        data = (json.dumps(row, separators=(",", ":")) + "\n").encode()
        offset = 0
        while offset < len(data):
            offset += os.write(fd, data[offset:])
        seq += 1
        if seq == start + 1:
            os.fsync(fd)
            print("READY", seq, flush=True)
        time.sleep(0.002)
finally:
    os.fsync(fd)
    os.close(fd)
print(seq, flush=True)
'''


def run(command, *, plist=False, timeout=120):
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout)
    if plist:
        return plistlib.loads(result.stdout)
    return result.stdout


def attached_images():
    data = run(["/usr/bin/hdiutil", "info", "-plist"], plist=True)
    return data.get("images", [])


def same_image(record, image_path):
    candidate = record.get("image-path")
    return bool(candidate) and Path(candidate).resolve() == image_path.resolve()


def attach(image, mountpoint):
    if mountpoint.exists():
        if mountpoint.is_symlink() or not mountpoint.is_dir() or any(mountpoint.iterdir()):
            raise RuntimeError(f"refusing to reuse a non-empty or replaced mountpoint: {mountpoint}")
    else:
        mountpoint.mkdir(mode=0o700)
    run(["/usr/bin/hdiutil", "attach", "-plist", "-nobrowse", "-noverify",
         "-mountpoint", str(mountpoint), str(image)])
    records = [r for r in attached_images() if same_image(r, image)]
    mounted = [e for r in records for e in r.get("system-entities", [])
               if e.get("mount-point") == str(mountpoint) and e.get("dev-entry")]
    if len(records) != 1 or not mounted:
        raise RuntimeError(f"hdiutil attach returned without a verified mount at {mountpoint}")
    return mounted[0]["dev-entry"]


def detach_mount(mountpoint, image_path):
    records = [r for r in attached_images() if same_image(r, image_path)]
    if len(records) > 1:
        raise RuntimeError(f"multiple attached image records for private image {image_path}")
    if records:
        entities = records[0].get("system-entities", [])
        entity = next((e for e in entities if e.get("mount-point") == str(mountpoint)), None)
        if entity is None:
            raise RuntimeError(f"private image remains attached without expected mountpoint: {image_path}")
        run(["/usr/bin/hdiutil", "detach", entity["dev-entry"]], timeout=120)
    if any(same_image(r, image_path) for r in attached_images()):
        raise RuntimeError(f"image remains attached after detach: {image_path}")


def start_writer(log_path, next_seq):
    process = subprocess.Popen([sys.executable, "-c", WRITER_CODE, str(log_path), str(next_seq)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 10)
        if not ready:
            raise RuntimeError("writer did not report a completed first write within 10 seconds")
        line = process.stdout.readline().strip().split()
        if len(line) != 2 or line[0] != "READY" or int(line[1]) != next_seq + 1:
            raise RuntimeError(f"writer startup did not confirm first sequence {next_seq + 1}: {line!r}")
    except Exception:
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=10)
        raise
    return process


def stop_writer(process):
    if process is None:
        return 0
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
    stdout, stderr = process.communicate(timeout=20)
    if process.returncode != 0:
        raise RuntimeError(f"writer stopped with status {process.returncode}: {stderr.strip()}")
    return int(stdout.strip())


def validate_log(data):
    rows = []
    for expected, line in enumerate(data.splitlines()):
        row = json.loads(line)
        payload = row["payload"].encode()
        if row["seq"] != expected or row["sha256"] != hashlib.sha256(payload).hexdigest():
            raise RuntimeError(f"invalid or non-contiguous writer record at sequence {expected}")
        rows.append(row)
    if not rows:
        raise RuntimeError("writer produced no records")
    return rows


def write_durable(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)


def under(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def write_result(path, report):
    with path.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="new JSON result file; its parent directory must already exist")
    parser.add_argument("--repeat", type=int, default=5, help="cycles, from 5 through 20 (default 5)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this probe requires macOS")
    if not 5 <= args.repeat <= 20:
        parser.error("--repeat must be between 5 and 20")
    output = args.output.expanduser().resolve()
    if not output.parent.is_dir():
        parser.error("the output parent directory must already exist")
    if output.exists():
        parser.error("output file already exists; refusing to overwrite")
    tmp_root = TMP_ROOT.resolve(strict=True)
    if not tmp_root.is_dir():
        parser.error("/private/tmp is not an existing directory")
    if under(output, tmp_root):
        parser.error("JSON output must be outside /private/tmp; only the sparse image belongs there")

    token = uuid.uuid4().hex[:10]
    work = Path(tempfile.mkdtemp(prefix="forkfs-apfs-probe-", dir=tmp_root))
    if work.parent != tmp_root or work.is_symlink():
        raise RuntimeError(f"unexpected temporary directory identity: {work}")
    src_image = work / "source.sparseimage"
    src_mount = work / "source-mount"
    log_path = "probe-log.jsonl"
    report = {"schema_version": 1, "probe": "apfs_active_image_fork", "image_mib": IMAGE_MIB,
              "repeat_requested": args.repeat, "source_image": str(src_image),
              "temporary_directory": str(work), "samples": [], "errors": [],
              "claims": {"image_is_sparse": True, "clone_requires_detached_source": True,
                         "cold_cache": False, "p95_reported": False}}
    writer = None
    abnormal = False
    cleanup_warning = None
    try:
        if work.stat().st_dev != tmp_root.stat().st_dev:
            raise RuntimeError("probe directory is not on the same host filesystem as /private/tmp")
        run(["/usr/bin/hdiutil", "create", "-quiet", "-size", f"{IMAGE_MIB}m", "-type", "SPARSE",
             "-fs", "APFS", "-volname", f"ForkProbe{token}", str(src_image)], timeout=120)
        if not src_image.is_file() or src_image.stat().st_size > IMAGE_MIB * 1024 * 1024:
            raise RuntimeError("created image is missing or exceeds the sparse-image size bound")
        attach(src_image, src_mount)
        data_file = src_mount / log_path
        writer = start_writer(data_file, 0)
        time.sleep(0.15)

        for iteration in range(args.repeat):
            child_image = work / f"child-{iteration + 1}.sparseimage"
            child_mount = work / f"child-mount-{iteration + 1}"
            pause_start = time.perf_counter_ns()
            stop_writer(writer)
            writer = None
            run(["/bin/sync"], timeout=120)
            baseline = data_file.read_bytes()
            baseline_rows = validate_log(baseline)
            detach_mount(src_mount, src_image)
            copy_start = time.perf_counter_ns()
            run(["/bin/cp", "-c", str(src_image), str(child_image)], timeout=120)
            copy_end = time.perf_counter_ns()
            attach(src_image, src_mount)
            pause_end = time.perf_counter_ns()
            writer = start_writer(src_mount / log_path, len(baseline_rows))
            attach(child_image, child_mount)
            cycle_end = time.perf_counter_ns()

            child_data = (child_mount / log_path).read_bytes()
            source_after_attach = (src_mount / log_path).read_bytes()
            if child_data != baseline or not source_after_attach.startswith(baseline):
                raise RuntimeError(f"iteration {iteration + 1}: fork data differs from the quiesced source")
            validate_log(child_data)
            child_marker = child_mount / f"child-only-{iteration + 1}"
            write_durable(child_marker, b"child-side write\n")
            if (src_mount / child_marker.name).exists():
                raise RuntimeError(f"iteration {iteration + 1}: child write appeared in source")
            deadline = time.monotonic() + 2.0
            source_grew = False
            while time.monotonic() < deadline:
                current = (src_mount / log_path).read_bytes()
                if len(current) > len(baseline):
                    source_grew = True
                    break
                time.sleep(0.005)
            if not source_grew or (child_mount / log_path).read_bytes() != baseline:
                raise RuntimeError(f"iteration {iteration + 1}: source continuation was not independent")
            stop_seq = stop_writer(writer)
            writer = None
            rows = validate_log((src_mount / log_path).read_bytes())
            if stop_seq != len(rows):
                raise RuntimeError(f"iteration {iteration + 1}: stopped writer sequence does not match the log")
            detach_mount(child_mount, child_image)
            report["samples"].append({
                "iteration": iteration + 1,
                "baseline_records": len(baseline_rows),
                "source_records_after_resume": len(rows),
                "records_added_after_resume": len(rows) - len(baseline_rows),
                "copy_seconds": (copy_end - copy_start) / 1e9,
                "source_write_pause_seconds": (pause_end - pause_start) / 1e9,
                "full_stop_to_both_mounts_seconds": (cycle_end - pause_start) / 1e9,
                "checks": {"baseline_log_hashes_and_sequence": True,
                           "fork_matches_quiesced_source": True,
                           "child_write_absent_from_source": True,
                           "source_resume_absent_from_child": True}})
            if iteration + 1 < args.repeat:
                writer = start_writer(src_mount / log_path, len(rows))

        # Leave source mounted only while stopping its writer, then detach cleanly.
        stop_writer(writer)
        writer = None
        detach_mount(src_mount, src_image)
        pause_values = [s["source_write_pause_seconds"] for s in report["samples"]]
        full_values = [s["full_stop_to_both_mounts_seconds"] for s in report["samples"]]
        report["complete"] = True
        report["summary"] = {
            "completed_samples": len(report["samples"]),
            "source_write_pause_median_seconds": statistics.median(pause_values),
            "full_cycle_median_seconds": statistics.median(full_values),
            "p95_reported": False}
    except Exception as exc:
        report["errors"].append({"type": type(exc).__name__, "message": str(exc)})
        report["complete"] = False
    finally:
        if writer is not None:
            if writer.poll() is None:
                try:
                    stop_writer(writer)
                    writer = None
                except Exception as exc:
                    abnormal = True
                    cleanup_warning = f"could not stop writer: {exc}"
            else:
                writer.communicate()
                writer = None
        # Only inspect and detach exact mountpoints below this private temp directory.
        mounts_remaining = None
        try:
            private_images = [src_image, *work.glob("child-*.sparseimage")]
            records = attached_images()
            owned_live = [r for r in records if any(same_image(r, image) for image in private_images)]
            if writer is None:
                for record in owned_live:
                    image_path = Path(record["image-path"]).resolve()
                    entities = record.get("system-entities", [])
                    entity = next((e for e in entities if e.get("mount-point")), None)
                    if entity is None:
                        abnormal = True
                        cleanup_warning = f"attached private image has no verified mountpoint: {image_path}"
                        continue
                    try:
                        run(["/usr/bin/hdiutil", "detach", entity["dev-entry"]], timeout=120)
                    except Exception as exc:
                        abnormal = True
                        cleanup_warning = f"mount cleanup failed for image {image_path}: {exc}"
            else:
                abnormal = True
                cleanup_warning = "writer may still own an open file; leaving attached images untouched"
            remaining = attached_images()
            owned_live = [r.get("image-path", "<unknown>") for r in remaining
                          if any(same_image(r, image) for image in private_images)]
            mounts_remaining = len(owned_live)
            if owned_live:
                abnormal = True
                cleanup_warning = f"private images still have attached devices: {owned_live}"
        except Exception as exc:
            abnormal = True
            cleanup_warning = f"cannot verify attached image state: {exc}"
        if abnormal:
            report["complete"] = False
            report["errors"].append({"type": "CleanupError", "message": cleanup_warning})
            report["cleanup"] = {"temporary_directory_preserved": True,
                                  "warning": cleanup_warning,
                                  "mounted": None if mounts_remaining is None else mounts_remaining > 0,
                                  "mount_state_verified": mounts_remaining is not None}
            print(f"WARNING: preserving {work}; {cleanup_warning}", file=sys.stderr)
        else:
            try:
                shutil.rmtree(work)
                report["cleanup"] = {"temporary_directory_preserved": False, "mounted": False,
                                      "mount_state_verified": True}
            except Exception as exc:
                report["complete"] = False
                cleanup_warning = f"could not remove private work directory: {exc}"
                report["errors"].append({"type": "CleanupError", "message": cleanup_warning})
                report["cleanup"] = {"temporary_directory_preserved": True, "warning": cleanup_warning,
                                      "mounted": False, "mount_state_verified": True}
                abnormal = True
                print(f"WARNING: preserving {work}; {cleanup_warning}", file=sys.stderr)
        report["temporary_directory"] = str(work) if abnormal else None
        write_result(output, report)
        print(output)

    if report.get("complete") is False or len(report.get("samples", [])) != args.repeat:
        sys.exit(1)


if __name__ == "__main__":
    main()
