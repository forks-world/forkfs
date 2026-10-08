#!/usr/bin/env python3
"""Compare APFS data containers using isolated, verified disk images."""

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

TMP_ROOT = Path("/private/tmp")
IMAGE_MIB = 160
FORMATS = ("udif_udrw", "sparse", "sparse_shadow")
SMALL_COUNT = 64
SEQUENTIAL_BYTES = 4 * 1024 * 1024
SMALL_PAYLOAD = b"forkfs APFS container probe small file\n" * 4
EDIT_BASE = (b"forkfs APFS image edit target\n" * 160)[:4096]
WARM_DATA = (b"forkfs identical cache warmup fixture\n" * 16384)[:1024 * 1024]
EDIT_SAVED = (b"forkfs 4 KiB atomic save sample\n" * 128)[:4096]
SEQUENTIAL_CHUNK = bytes(range(256)) * 256


def command(argv, *, plist=False, timeout=180):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout)
    if result.returncode:
        err = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"command failed ({result.returncode}): {argv!r}: {err}")
    return plistlib.loads(result.stdout) if plist else result.stdout


def attached_images():
    return command(["/usr/bin/hdiutil", "info", "-plist"], plist=True).get("images", [])


def same_image(record, image_path):
    candidate = record.get("image-path")
    return bool(candidate) and Path(candidate).resolve() == image_path.resolve()


def verified_record(image_path, mountpoint=None):
    records = [r for r in attached_images() if same_image(r, image_path)]
    if len(records) != 1:
        raise RuntimeError(f"expected one hdiutil info record for {image_path}; found {len(records)}")
    entities = records[0].get("system-entities", [])
    devices = [e.get("dev-entry") for e in entities if e.get("dev-entry")]
    if not devices:
        raise RuntimeError(f"hdiutil info has no device nodes for {image_path}")
    if mountpoint is not None and not any(
            e.get("mount-point") == str(mountpoint) and e.get("dev-entry") for e in entities):
        raise RuntimeError(f"hdiutil info did not verify device and mountpoint for {image_path}: {mountpoint}")
    return records[0]


def mount_image(image_path, mountpoint, shadow_path=None, readonly=False):
    if mountpoint.exists():
        if mountpoint.is_symlink() or not mountpoint.is_dir() or any(mountpoint.iterdir()):
            raise RuntimeError(f"refusing to reuse non-empty or replaced mountpoint {mountpoint}")
    else:
        mountpoint.mkdir(mode=0o700)
    started = time.perf_counter_ns()
    args = ["/usr/bin/hdiutil", "attach", "-plist", "-nobrowse", "-noverify",
            "-mountpoint", str(mountpoint)]
    if readonly:
        args.append("-readonly")
    if shadow_path is not None:
        args.extend(["-shadow", str(shadow_path)])
    args.append(str(image_path))
    command(args, timeout=180)
    record = verified_record(image_path, mountpoint)
    mount_ns = time.perf_counter_ns() - started
    device = next(e["dev-entry"] for e in record.get("system-entities", [])
                  if e.get("mount-point") == str(mountpoint) and e.get("dev-entry"))
    return mount_ns, device


def unmount_image(image_path, mountpoint):
    record = verified_record(image_path, mountpoint)
    entity = next(e for e in record.get("system-entities", [])
                  if e.get("mount-point") == str(mountpoint) and e.get("dev-entry"))
    started = time.perf_counter_ns()
    command(["/usr/bin/hdiutil", "detach", entity["dev-entry"]], timeout=180)
    if any(same_image(r, image_path) for r in attached_images()):
        raise RuntimeError(f"image still has attached device entries after detach: {image_path}")
    return time.perf_counter_ns() - started


def image_format(path):
    return command(["/usr/bin/hdiutil", "imageinfo", str(path), "-format"]).decode().strip()


def allocated(path):
    stat = path.stat()
    return {"image_file_size_bytes": stat.st_size, "allocated_bytes": stat.st_blocks * 512}


def host_available_bytes(path):
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_frsize


def hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb", buffering=0) as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def fixture_manifest(root):
    result = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            result.append({"path": path.relative_to(root).as_posix(),
        "size": path.stat().st_size, "sha256": hash_file(path)})
    return result


def create_fixture(root):
    fixture = root / "fixture"
    fixture.mkdir()
    (fixture / "micro").mkdir()
    (fixture / "edit-target.bin").write_bytes(EDIT_BASE)
    (fixture / "warm.bin").write_bytes(WARM_DATA)
    payload = (b"small fixture file contents\n" * 16)[:512]
    for i in range(64):
        (fixture / "micro" / f"base-{i:03d}.dat").write_bytes(payload)
    return fixture


def prepare_base(work, key, kind, volume_name):
    if kind == "udif_udrw":
        image = work / f"base-{key}.dmg"
        command(["/usr/bin/hdiutil", "create", "-quiet", "-size", f"{IMAGE_MIB}m",
                 "-type", "UDIF", "-fs", "APFS", "-volname", volume_name, str(image)])
        expected = "UDRW"
    else:
        image = work / f"base-{key}.sparseimage"
        command(["/usr/bin/hdiutil", "create", "-quiet", "-size", f"{IMAGE_MIB}m",
                 "-type", "SPARSE", "-fs", "APFS", "-volname", volume_name, str(image)])
        expected = "SPRS"
    actual = image_format(image)
    if actual != expected:
        raise RuntimeError(f"created {kind} base has actual format {actual!r}, expected {expected!r}")
    mountpoint = work / f"prepare-{key}"
    mount_image(image, mountpoint)
    fixture = create_fixture(mountpoint)
    manifest = fixture_manifest(fixture)
    unmount_image(image, mountpoint)
    return {"kind": kind, "image": image, "format": actual, "manifest": manifest,
            "requested_container": "UDIF" if kind == "udif_udrw" else "SPARSE",
            "initial_allocation": allocated(image), "initial_hash": hash_file(image)}


def fsync_write(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            view = view[written:]
        os.fsync(fd)
    finally:
        os.close(fd)


def warm_fixture(fixture):
    # Warm all candidate images with the same namespace and read pattern before data timing.
    st = 0
    for path in sorted(fixture.rglob("*")):
        os.stat(path)
        if path.is_file():
            with path.open("rb") as source:
                while source.read(64 * 1024):
                    pass
            st += 1
    return st


def workload_edit_save(fixture):
    target = fixture / "edit-target.bin"
    started = time.perf_counter_ns()
    with tempfile.NamedTemporaryFile(dir=fixture, prefix=".edit-", delete=False) as out:
        temporary = Path(out.name)
        out.write(EDIT_SAVED)
        os.fchmod(out.fileno(), target.stat().st_mode & 0o777)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, target)
    return time.perf_counter_ns() - started


def workload_small_create_unlink(fixture, iteration):
    directory = fixture / "micro"
    started = time.perf_counter_ns()
    for i in range(SMALL_COUNT):
        path = directory / f"run-{iteration:03d}-{i:03d}.dat"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            os.write(fd, SMALL_PAYLOAD)
        finally:
            os.close(fd)
        path.unlink()
    elapsed = time.perf_counter_ns() - started
    return elapsed, elapsed // SMALL_COUNT


def workload_sequential_write(fixture, iteration):
    path = fixture / f"sequential-{iteration:03d}.bin"
    chunk = SEQUENTIAL_CHUNK
    started = time.perf_counter_ns()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        remaining = SEQUENTIAL_BYTES
        while remaining:
            written = os.write(fd, chunk[:min(len(chunk), remaining)])
            remaining -= written
        os.fsync(fd)
    finally:
        os.close(fd)
    return time.perf_counter_ns() - started


def verify_persisted_workload(fixture, iteration):
    edit = (fixture / "edit-target.bin").read_bytes()
    if edit != EDIT_SAVED:
        raise RuntimeError(f"iteration {iteration}: atomic edit contents did not persist")
    seq_path = fixture / f"sequential-{iteration:03d}.bin"
    seq_stat = seq_path.stat()
    if seq_stat.st_size != SEQUENTIAL_BYTES:
        raise RuntimeError(f"iteration {iteration}: sequential file length mismatch: {seq_stat.st_size}")
    seq_hash = hash_file(seq_path)
    expected = hashlib.sha256(SEQUENTIAL_CHUNK * (SEQUENTIAL_BYTES // len(SEQUENTIAL_CHUNK))).hexdigest()
    if seq_hash != expected:
        raise RuntimeError(f"iteration {iteration}: sequential file content hash mismatch")
    leftovers = list((fixture / "micro").glob(f"run-{iteration:03d}-*.dat"))
    if leftovers:
        raise RuntimeError(f"iteration {iteration}: small files remain after create/unlink: {leftovers[:3]}")
    return {"edit_contents_match": True, "edit_bytes": len(edit),
            "sequential_length_matches": True, "sequential_bytes": seq_stat.st_size,
            "sequential_sha256": seq_hash, "sequential_hash_matches": True,
            "small_files_all_unlinked": True}


def one_trial(work, variant, bases, iteration, order_index):
    base = bases["udif_udrw"] if variant == "udif_udrw" else bases["sparse"]
    copy_image = None
    shadow = None
    if variant == "udif_udrw":
        copy_image = work / f"trial-udrw-{iteration:02d}.dmg"
        command(["/bin/cp", "-c", str(base["image"]), str(copy_image)])
        expected_format = "UDRW"
        attach_image = copy_image
    elif variant == "sparse":
        copy_image = work / f"trial-sparse-{iteration:02d}.sparseimage"
        command(["/bin/cp", "-c", str(base["image"]), str(copy_image)])
        expected_format = "SPRS"
        attach_image = copy_image
    else:
        shadow = work / f"overlay-{iteration:02d}.shadow"
        expected_format = "SPRS"
        attach_image = base["image"]
    if image_format(attach_image) != expected_format:
        raise RuntimeError(f"trial image format changed for {variant}: {attach_image}")

    base_hash_before = hash_file(base["image"])
    if base_hash_before != base["initial_hash"]:
        raise RuntimeError(f"pristine base changed before {variant} iteration {iteration}")
    base_allocation_before = allocated(base["image"])
    host_free_before = host_available_bytes(work)
    working_before = allocated(copy_image) if copy_image is not None else None
    mountpoint = work / f"mount-{iteration:02d}-{variant}"
    attach_ns, _ = mount_image(attach_image, mountpoint, shadow_path=shadow)
    fixture = mountpoint / "fixture"
    if variant == "sparse_shadow":
        if shadow is None or not shadow.is_file():
            raise RuntimeError("hdiutil -shadow did not create its shadow file before data writes")
        shadow_after_attach = allocated(shadow)
    else:
        shadow_after_attach = None

    # Read and stat identical baseline paths so data timing begins in a matched warm state.
    warm_file_count = warm_fixture(fixture)
    edit_ns = workload_edit_save(fixture)
    small_total_ns, small_each_ns = workload_small_create_unlink(fixture, iteration)
    seq_ns = workload_sequential_write(fixture, iteration)
    if shadow is not None and not shadow.is_file():
        raise RuntimeError("shadow file disappeared while mounted")
    detach_ns = unmount_image(attach_image, mountpoint)

    base_hash_after_primary = hash_file(base["image"])
    base_allocation_after_primary = allocated(base["image"])
    base_unchanged = base_hash_after_primary == base["initial_hash"]
    if not base_unchanged or base_allocation_after_primary["allocated_bytes"] != base_allocation_before["allocated_bytes"]:
        raise RuntimeError(f"base image changed during {variant} iteration {iteration}")
    working_after = allocated(copy_image) if copy_image is not None else None
    shadow_after = allocated(shadow) if shadow is not None else None
    host_free_after = host_available_bytes(work)
    if copy_image is not None:
        physical_growth = working_after["allocated_bytes"] - working_before["allocated_bytes"]
    else:
        physical_growth = shadow_after["allocated_bytes"] - shadow_after_attach["allocated_bytes"]

    verify_mountpoint = work / f"verify-{iteration:02d}-{variant}"
    verify_attach_ns, _ = mount_image(attach_image, verify_mountpoint, shadow_path=shadow)
    verification = verify_persisted_workload(verify_mountpoint / "fixture", iteration)
    verify_detach_ns = unmount_image(attach_image, verify_mountpoint)
    base_hash_after_verify = hash_file(base["image"])
    base_allocation_after_verify = allocated(base["image"])
    if (base_hash_after_verify != base["initial_hash"] or
            base_allocation_after_verify["allocated_bytes"] != base_allocation_before["allocated_bytes"]):
        raise RuntimeError(f"base image changed during verification for {variant} iteration {iteration}")
    working_after_verify = allocated(copy_image) if copy_image is not None else None
    shadow_after_verify = allocated(shadow) if shadow is not None else None
    sample = {
        "iteration": iteration,
        "interleave_order_index": order_index,
        "variant": variant,
        "container_label": {"udif_udrw": "UDIF/UDRW", "sparse": "SPARSE",
                             "sparse_shadow": "SPARSE+shadow"}[variant],
        "imageinfo_format_code": expected_format,
        "virtual_capacity_bytes": IMAGE_MIB * 1024 * 1024,
        "base_image": base["image"].name,
        "attach_ns": attach_ns,
        "detach_ns": detach_ns,
        "verify_remount_attach_ns": verify_attach_ns,
        "verify_remount_detach_ns": verify_detach_ns,
        "edit_save_4k_fsync_rename_ns": edit_ns,
        "small_create_unlink_64_total_ns": small_total_ns,
        "small_create_unlink_ns_per_op": small_each_ns,
        "sequential_write_4m_fsync_ns": seq_ns,
        "sequential_bytes": SEQUENTIAL_BYTES,
        "warm_fixture_files": warm_file_count,
        "base_sha256_before": base_hash_before,
        "base_sha256_after_primary_detach": base_hash_after_primary,
        "base_sha256_after_verify_detach": base_hash_after_verify,
        "base_unchanged": base_unchanged,
        "base_allocation_before": base_allocation_before,
        "base_allocation_after_primary_detach": base_allocation_after_primary,
        "base_allocation_after_verify_detach": base_allocation_after_verify,
        "base_physical_growth_bytes": base_allocation_after_verify["allocated_bytes"] - base_allocation_before["allocated_bytes"],
        "copy_image_before": working_before,
        "copy_image_after_primary_detach": working_after,
        "copy_image_after_verify_detach": working_after_verify,
        "shadow_after_attach_before_data": shadow_after_attach,
        "shadow_after_primary_detach": shadow_after,
        "shadow_after_verify_detach": shadow_after_verify,
        "working_object_physical_growth_bytes": physical_growth,
        "host_apfs_available_before_bytes": host_free_before,
        "host_apfs_available_after_primary_detach_bytes": host_free_after,
        "host_apfs_available_decrease_bytes": host_free_before - host_free_after,
        "persisted_data_verification": verification,
    }
    # The attachment has been verified detached before these owned image files are removed.
    if copy_image is not None:
        copy_image.unlink()
    if shadow is not None:
        shadow.unlink()
    return sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="new JSON result file; parent directory must already exist")
    parser.add_argument("--repeat", type=int, default=5, help="cycles per variant, 5..10 (default 5)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this probe requires macOS")
    if not 5 <= args.repeat <= 10:
        parser.error("--repeat must be between 5 and 10")
    output = args.output.expanduser().resolve()
    if not output.parent.is_dir():
        parser.error("the output parent directory must already exist")
    if output.exists():
        parser.error("output file already exists; refusing to overwrite")
    tmp_root = TMP_ROOT.resolve(strict=True)
    if not tmp_root.is_dir():
        parser.error("/private/tmp is not an existing directory")
    if output == tmp_root or tmp_root in output.parents:
        parser.error("JSON output must be outside /private/tmp")

    work = Path(tempfile.mkdtemp(prefix="forkfs-container-compare-", dir=tmp_root))
    if work.parent != tmp_root or work.is_symlink() or work.stat().st_dev != tmp_root.stat().st_dev:
        raise RuntimeError(f"temporary directory failed /private/tmp identity checks: {work}")
    token = uuid.uuid4().hex[:8]
    report = {"schema_version": 1, "probe": "apfs_container_compare",
              "created_unix_ns": time.time_ns(), "macos_version": __import__("platform").mac_ver()[0],
              "image_size_mib": IMAGE_MIB, "maximum_simultaneous_logical_image_capacity_mib": 3 * IMAGE_MIB,
              "repeat_requested_per_variant": args.repeat, "variants": list(FORMATS),
              "fixture": {"files": 66, "edit_bytes": 4096, "warm_bytes": len(WARM_DATA),
                          "small_operations_per_sample": SMALL_COUNT,
                          "sequential_write_bytes": SEQUENTIAL_BYTES},
              "cache_policy": "No cache purge. Every mounted fixture is read/stat warmed identically before data timing; attach timing reflects OS state and is not claimed cold.",
              "samples": [], "errors": [], "cleanup": None,
              "p95_reported": False, "temporary_directory": str(work)}
    bases = {}
    abnormal = False
    cleanup_warning = None
    try:
        bases["udif_udrw"] = prepare_base(work, "udif", "udif_udrw", f"ForkUDIF{token}")
        bases["sparse"] = prepare_base(work, "sparse", "sparse", f"ForkSparse{token}")
        if bases["udif_udrw"]["manifest"] != bases["sparse"]["manifest"]:
            raise RuntimeError("fixture manifests differ between UDRW and SPARSE base images")
        report["base_images"] = {
            key: {"path": base["image"].name, "requested_container": base["requested_container"],
                  "actual_hdiutil_imageinfo_format_code": base["format"],
                  "virtual_capacity_bytes": IMAGE_MIB * 1024 * 1024,
                  "image_file_size_bytes": base["initial_allocation"]["image_file_size_bytes"],
                  "initial_allocated_bytes": base["initial_allocation"]["allocated_bytes"],
                  "sha256": base["initial_hash"], "fixture_manifest_sha256": hashlib.sha256(
                      json.dumps(base["manifest"], sort_keys=True).encode()).hexdigest()}
            for key, base in bases.items()}
        order = []
        for i in range(args.repeat):
            rotation = i % len(FORMATS)
            order.append([*FORMATS[rotation:], *FORMATS[:rotation]])
        for iteration, variants in enumerate(order, start=1):
            for order_index, variant in enumerate(variants):
                sample = one_trial(work, variant, bases, iteration, order_index)
                report["samples"].append(sample)
        for base in bases.values():
            if hash_file(base["image"]) != base["initial_hash"]:
                raise RuntimeError(f"base image changed after all trials: {base['image']}")
        grouped = {}
        for variant in FORMATS:
            samples = [s for s in report["samples"] if s["variant"] == variant]
            grouped[variant] = {"samples": len(samples),
                                "median_attach_ns": statistics.median(s["attach_ns"] for s in samples),
                                "median_detach_ns": statistics.median(s["detach_ns"] for s in samples),
                                "median_edit_save_ns": statistics.median(s["edit_save_4k_fsync_rename_ns"] for s in samples),
                                "median_small_create_unlink_ns_per_op": statistics.median(s["small_create_unlink_ns_per_op"] for s in samples),
                                "median_sequential_write_fsync_ns": statistics.median(s["sequential_write_4m_fsync_ns"] for s in samples),
                                "median_working_object_physical_growth_bytes": statistics.median(
                                    s["working_object_physical_growth_bytes"] for s in samples),
                                "median_host_apfs_available_decrease_bytes": statistics.median(
                                    s["host_apfs_available_decrease_bytes"] for s in samples)}
        report["summary"] = grouped
        report["complete"] = len(report["samples"]) == args.repeat * len(FORMATS)
        report["p95_reported"] = False
    except Exception as exc:
        report["errors"].append({"type": type(exc).__name__, "message": str(exc)})
        report["complete"] = False
    finally:
        try:
            records = attached_images()
            work_images = [p for p in work.iterdir() if p.is_file() and
                           p.suffix in (".dmg", ".sparseimage", ".shadow")]
            still_attached = [r for r in records if any(same_image(r, p) for p in work_images)]
            if still_attached:
                for record in still_attached:
                    image_path = Path(record.get("image-path", "<unknown>"))
                    entities = record.get("system-entities", [])
                    dev = next((e.get("dev-entry") for e in entities
                                if e.get("content-hint") == "GUID_partition_scheme" and e.get("dev-entry")), None)
                    if dev is None:
                        abnormal = True
                        cleanup_warning = f"attached private image has no verified device node: {image_path}"
                        continue
                    try:
                        command(["/usr/bin/hdiutil", "detach", dev], timeout=180)
                    except Exception as exc:
                        abnormal = True
                        cleanup_warning = f"could not detach private image {image_path} ({dev}): {exc}"
            remaining = attached_images()
            still_attached = [r.get("image-path", "<unknown>") for r in remaining
                              if any(same_image(r, p) for p in work_images)]
            if still_attached:
                abnormal = True
                cleanup_warning = f"private image device entries remain attached: {still_attached}"
        except Exception as exc:
            abnormal = True
            cleanup_warning = f"cannot verify hdiutil attachment state: {exc}"
        if abnormal:
            report["complete"] = False
            report["errors"].append({"type": "CleanupError", "message": cleanup_warning})
            report["cleanup"] = {"temporary_directory_preserved": True, "mounted_image": True,
                                  "warning": cleanup_warning}
            print(f"WARNING: preserving {work}; {cleanup_warning}", file=sys.stderr)
        else:
            try:
                shutil.rmtree(work)
                report["cleanup"] = {"temporary_directory_preserved": False, "mounted_image": False}
                report["temporary_directory"] = None
            except Exception as exc:
                report["complete"] = False
                cleanup_warning = f"could not remove private work directory: {exc}"
                report["errors"].append({"type": "CleanupError", "message": cleanup_warning})
                report["cleanup"] = {"temporary_directory_preserved": True, "mounted_image": False,
                                      "warning": cleanup_warning}
                print(f"WARNING: preserving {work}; {cleanup_warning}", file=sys.stderr)
        if not abnormal and report.get("temporary_directory") == str(work):
            report["temporary_directory"] = None
        with output.open("x", encoding="utf-8") as result:
            json.dump(report, result, indent=2)
            result.write("\n")
            result.flush()
            os.fsync(result.fileno())
        print(output)
    if not report.get("complete"):
        sys.exit(1)


if __name__ == "__main__":
    main()
