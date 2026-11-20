"""Execute the immutable filesystem contract corpus against isolated adapters."""
from __future__ import annotations

import errno
import hashlib
import json
import os
from collections import Counter
from pathlib import Path
import platform
import signal
import shutil
import subprocess
import tempfile
import time
import sys
import uuid

from .adapters import PosixAdapter, RpcAdapter, Unsupported, UntypedRpcFailure
from .model import FILE_SIZE_LIMIT, SchemaError, load_json, resolve_refs, validate_case_file, validate_coverage


BYTE_LIMIT = 64 * 1024 * 1024
ENTRY_LIMIT = 4096
STEP_LIMIT = 256


def _json_hash(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    return hashlib.sha256(encoded).hexdigest()


def _errno_value(expected):
    if type(expected) is int:
        return expected
    if isinstance(expected, dict):
        osname = "darwin" if platform.system() == "Darwin" else "linux" if platform.system() == "Linux" else None
        if osname is None:
            raise Unsupported(f"errno map has no supported platform for {platform.system()}")
        expected = expected[osname]
    return getattr(errno, expected, None)


def _actual_errno(exc):
    if isinstance(exc, UntypedRpcFailure): return None
    if isinstance(exc, OSError): return exc.errno
    return None


def _compare(result, expected, saved):
    failures = []
    for key, value in expected.items():
        if key in ("errno", "unchanged"):
            continue
        wanted = resolve_refs(value, saved)
        actual_key = "inode" if key == "inode_not" else "names" if key == "ordered_names" else key
        if actual_key not in result:
            failures.append(f"result omitted required field {actual_key}")
            continue
        actual = result[actual_key]
        if key == "inode_not":
            if actual == wanted: failures.append(f"inode unexpectedly equals {wanted!r}")
        elif key == "data_hex" or key == "value_hex":
            if actual.lower() != wanted.lower(): failures.append(f"{key} differs")
        elif key == "names":
            if not isinstance(actual, list) or Counter(actual) != Counter(wanted): failures.append("directory names differ")
        elif key == "ordered_names":
            if actual != wanted: failures.append(f"ordered directory names: expected {wanted!r}, got {actual!r}")
        elif actual != wanted:
            failures.append(f"{key}: expected {wanted!r}, got {actual!r}")
    return failures


def _case_bytes(case):
    total = 0
    for fixture in case["fixture"]:
        if fixture["type"] == "file": total += len(bytes.fromhex(fixture["data_hex"]))
    for step in case["steps"]:
        args = step.get("args", {})
        for key in ("data_hex", "value_hex"):
            value = args.get(key)
            if isinstance(value, str): total += len(bytes.fromhex(value))
        for key in ("size", "offset", "count"):
            value = args.get(key)
            if isinstance(value, int) and value > FILE_SIZE_LIMIT:
                raise Unsupported(f"case {key} exceeds {FILE_SIZE_LIMIT} byte bound")
        offset = args.get("offset", 0)
        data = args.get("data_hex")
        if isinstance(offset, int) and isinstance(data, str) and offset + len(bytes.fromhex(data)) > FILE_SIZE_LIMIT:
            raise Unsupported(f"case write extent exceeds {FILE_SIZE_LIMIT} byte bound")
    if total > BYTE_LIMIT: raise Unsupported(f"case byte budget exceeds {BYTE_LIMIT}")
    if len(case["fixture"]) > ENTRY_LIMIT or len(case["steps"]) > STEP_LIMIT:
        raise Unsupported("case exceeds fixture/step count bound")


def _mount_info(path):
    if platform.system() != "Darwin": raise Unsupported("native POSIX profile requires macOS")
    result = subprocess.run(["/sbin/mount"], capture_output=True, text=True, timeout=5, check=True)
    matches = []
    for row in result.stdout.splitlines():
        if " on " not in row or " (" not in row: continue
        source, rest = row.rsplit(" on ", 1)
        mount_text, options = rest.rsplit(" (", 1)
        mount_path = Path(mount_text)
        try:
            if path == mount_path or mount_path in path.parents:
                matches.append((len(mount_path.parts), source, mount_path, options.rstrip(")")))
        except OSError:
            continue
    if not matches: raise Unsupported(f"cannot identify mount containing {path}")
    _, source, root, options = max(matches, key=lambda item: item[0])
    tags = options.rstrip(")").split(", ")
    kind = tags[0].strip().lower()
    return {"source": source, "root": root, "kind": kind, "options": options, "tags": tags}


def _verify_native(profile, reference_root, mountpoint):
    info = {}
    if profile in ("reference", "mounted"):
        if reference_root is None: raise Unsupported("--reference-root is required for POSIX reference evidence")
        root = Path(reference_root).resolve(strict=True)
        apfs = _mount_info(root)
        if apfs["kind"] != "apfs": raise Unsupported(f"reference root is not on APFS: {apfs}")
        info["reference"] = {"fs_type": apfs["kind"], "mount_root": str(apfs["root"])}
    if profile == "mounted":
        if mountpoint is None: raise Unsupported("--mountpoint is required for mounted profile")
        requested = Path(mountpoint).absolute()
        target = requested.resolve(strict=True)
        if requested != target: raise Unsupported("mountpoint must be an actual path, not a symlink")
        mounted = _mount_info(target)
        if mounted["root"] != target or mounted["kind"] != "forkfs":
            raise Unsupported(f"candidate is not the exact real forkfs mount root: {mounted}")
        if os.stat(target).st_mode & 0o777 != 0o700:
            raise Unsupported("forkfs mount root must have mode 0700")
        info["candidate"] = {"fs_type": "forkfs", "mount_root": str(target), "verified": True}
    return info


def _preflight_main(manifest_path, result_path):
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    try:
        value = _verify_native(manifest["profile"], manifest.get("reference_root"), manifest.get("mountpoint"))
        result = {"ok": True, "value": value}
    except BaseException as exc:
        result = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
    Path(result_path).write_text(json.dumps(result, sort_keys=True) + "\n", encoding="utf-8")
    return 0


def _native_preflight(profile, reference_root, mountpoint, timeout):
    with tempfile.TemporaryDirectory(prefix="ffpre-", dir=tempfile.gettempdir()) as control:
        manifest = Path(control) / "preflight.json"
        output = Path(control) / "result.json"
        manifest.write_text(json.dumps({"profile": profile,
                                        "reference_root": str(reference_root) if reference_root else None,
                                        "mountpoint": str(mountpoint) if mountpoint else None}), encoding="utf-8")
        entry = Path(__file__).resolve().parents[2] / "scripts/tests/fs_contract.py"
        proc = subprocess.Popen([sys.executable, str(entry), "--preflight", str(manifest), str(output)],
                                cwd=str(entry.parents[2]), stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                start_new_session=True)
        try:
            proc.wait(timeout=min(timeout, 15.0))
        except subprocess.TimeoutExpired:
            _terminate_group(proc, 0.5)
            raise Unsupported("native mount preflight exceeded its hard deadline")
        try:
            result = json.loads(output.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise Unsupported(f"native mount preflight returned no result (exit {proc.returncode})") from exc
        if not result.get("ok"):
            raise Unsupported(result.get("error", "native mount preflight failed"))
        return result["value"]


def _step_errno_match(expected, actual):
    wanted = _errno_value(expected)
    return wanted is not None and actual == wanted


def _execute_case_inner(case, role, scratch_root, daemon, timeout):
    result = {"case_id": case["id"], "family": case["families"], "profile_role": role,
              "layer": "rpc" if role == "daemon" else "posix", "status": "blocked",
              "steps": [], "case_known_gap": case.get("known_gap")}
    adapter = None
    scratch = Path(scratch_root)
    saved = {}
    prior_umask = os.umask(0)
    result["umask"] = 0
    try:
        _case_bytes(case)
        os.mkdir(scratch, 0o700)
        if role == "daemon":
            adapter = RpcAdapter(daemon, scratch, timeout=timeout)
        else:
            adapter = PosixAdapter(scratch, expected_fs="apfs" if role == "reference" else "forkfs")
        for item in case["fixture"]:
            adapter.fixture(item)
        _verify_fixture(adapter, case["fixture"])
        bad = False
        for index, step in enumerate(case["steps"]):
            record = {"index": index, "op": step["op"], "status": "blocked"}
            result["steps"].append(record)
            try:
                args = {key: resolve_refs(value, saved) for key, value in step["args"].items()}
            except SchemaError as exc:
                record.update(status="blocked", detail=f"dependency unavailable: {exc}")
                bad = True
                continue
            expected_errno = step["expect"]["errno"]
            expected_success = type(expected_errno) is int and expected_errno == 0
            before = None
            if not expected_success:
                try:
                    before = adapter.fingerprint(args.get("revision"))
                    record["fingerprint_before_sha256"] = _json_hash(before)
                except Unsupported as exc:
                    record.update(status="unsupported", detail=str(exc)); bad = True; continue
            try:
                observed = adapter.execute(step["op"], args)
                actual_errno = 0
                if not isinstance(observed, dict):
                    raise RuntimeError("adapter returned a non-object result")
                record["observed"] = observed
            except UntypedRpcFailure as exc:
                actual_errno = None
                record["wire_error"] = {"code": exc.code, "payload_hex": exc.payload[:256].hex()}
                record["error"] = str(exc)
                observed = {}
            except Unsupported as exc:
                record.update(status="unsupported", detail=str(exc)); bad = True; continue
            except TimeoutError as exc:
                record.update(status="blocked", detail=f"timeout: {exc}"); bad = True; continue
            except OSError as exc:
                actual_errno = exc.errno
                record["error"] = f"{type(exc).__name__}: {exc}"
                observed = {}
            except SchemaError as exc:
                record.update(status="blocked", detail=f"unsafe or invalid operation: {exc}"); bad = True; continue
            except Exception as exc:
                record["error"] = f"{type(exc).__name__}: {exc}"
                actual_errno = None; observed = {}
            if not _step_errno_match(expected_errno, actual_errno):
                record["status"] = "fail"
                record["errno_expected"] = expected_errno
                record["errno_actual"] = actual_errno
                bad = True
            else:
                mismatches = _compare(observed, step["expect"], saved)
                if mismatches:
                    record.update(status="fail", mismatches=mismatches); bad = True
                else:
                    record["status"] = "pass"
            if step.get("save") and actual_errno == 0 and record["status"] == "pass":
                saved[step["save"]] = observed
            if before is not None:
                try:
                    after = adapter.fingerprint(args.get("revision"))
                    record["fingerprint_after_sha256"] = _json_hash(after)
                    if before != after:
                        record["status"] = "fail"
                        record["atomicity_diff"] = _fingerprint_diff(before, after)
                        bad = True
                except Unsupported as exc:
                    record["fingerprint_after_error"] = f"cannot prove unchanged state: {exc}"
                    if record["status"] != "fail":
                        record["status"] = "unsupported"
                    bad = True
        statuses = [step["status"] for step in result["steps"]]
        # Preserve observed contract violations even if a later dependent step
        # cannot run or the fingerprint becomes unavailable.
        if "fail" in statuses: result["status"] = "known_gap" if "known_gap" in case else "fail"
        elif "blocked" in statuses: result["status"] = "blocked"
        elif "unsupported" in statuses: result["status"] = "unsupported"
        elif any(status != "pass" for status in statuses): result["status"] = "known_gap" if "known_gap" in case else "fail"
        else: result["status"] = "pass"
    except Unsupported as exc:
        result.update(status="unsupported", detail=str(exc))
    except TimeoutError as exc:
        result.update(status="blocked", detail=f"timeout: {exc}")
    except Exception as exc:
        result.update(status="blocked", detail=f"setup/runner error: {type(exc).__name__}: {exc}")
    finally:
        if adapter is not None:
            try: adapter.close_all()
            except Exception as exc: result.setdefault("cleanup_errors", []).append(str(exc))
        if scratch.exists():
            try: shutil.rmtree(scratch)
            except Exception as exc: result.setdefault("cleanup_errors", []).append(str(exc))
        os.umask(prior_umask)
    if result.get("cleanup_errors"):
        result["status"] = "blocked"
    return result


def _worker_main(manifest_path, result_path):
    try:
        manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        result = _execute_case_inner(manifest["case"], manifest["role"], manifest["scratch_root"],
                                     Path(manifest["daemon"]) if manifest.get("daemon") else None,
                                     manifest["timeout"])
    except BaseException as exc:
        result = {"case_id": "unknown", "profile_role": "unknown", "layer": "unknown",
                  "status": "blocked", "steps": [],
                  "detail": f"worker failure: {type(exc).__name__}: {exc}"}
    Path(result_path).write_text(json.dumps(result, sort_keys=True) + "\n", encoding="utf-8")
    return 0


def _execute_case(case, role, root_parent, daemon, timeout):
    """Run all adapter syscalls and cleanup in an owned, deadline-supervised worker."""
    if role == "daemon":
        root_parent = Path(tempfile.gettempdir())
    elif root_parent is None:
        return {"case_id": case["id"], "family": case["families"], "profile_role": role,
                "layer": "posix", "status": "blocked", "steps": [], "detail": "scratch parent missing"}
    scratch_root = Path(root_parent) / f"ff-{case['id'][:8]}-{uuid.uuid4().hex[:12]}"
    with tempfile.TemporaryDirectory(prefix="ffctl-", dir=tempfile.gettempdir()) as control:
        manifest_path = Path(control) / "case.json"
        result_path = Path(control) / "result.json"
        manifest_path.write_text(json.dumps({"case": case, "role": role, "scratch_root": str(scratch_root),
                                             "daemon": str(daemon) if daemon else None, "timeout": timeout}),
                                 encoding="utf-8")
        entry = Path(__file__).resolve().parents[2] / "scripts/tests/fs_contract.py"
        error_path = Path(control) / "worker.stderr"
        with error_path.open("wb") as errors:
            process = subprocess.Popen([sys.executable, str(entry), "--worker", str(manifest_path), str(result_path)],
                                        cwd=str(entry.parents[2]), stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=errors, start_new_session=True)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                _terminate_group(process, 1)
                return {"case_id": case["id"], "family": case["families"], "profile_role": role,
                        "layer": "rpc" if role == "daemon" else "posix", "status": "blocked",
                        "steps": [{"index": index, "op": step["op"], "status": "blocked",
                                   "detail": "worker case deadline exceeded"}
                                  for index, step in enumerate(case["steps"])],
                        "detail": f"case exceeded {timeout}s deadline", "worker_pid": process.pid,
                        "cleanup_pending": str(scratch_root)}
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            detail = error_path.read_text(encoding="utf-8", errors="replace")[-2000:]
            result = {"case_id": case["id"], "family": case["families"], "profile_role": role,
                      "layer": "rpc" if role == "daemon" else "posix", "status": "blocked", "steps": [],
                      "detail": f"worker exited {process.returncode} without a result: {detail}"}
        result["worker_pid"] = process.pid
        return result


def _terminate_group(process, grace):
    try: os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    try: process.wait(timeout=grace)
    except subprocess.TimeoutExpired: pass
    # The worker may have exited while its daemon child stayed in the group.
    try: os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError: pass
    try: process.wait(timeout=1)
    except subprocess.TimeoutExpired: pass


def _fingerprint_diff(before, after):
    def ident(index, row): return row.get("path", row.get("symbol", row.get("handle", f"#{index}")))
    def rows(value):
        flattened = list(value.get("entries", [])) + list(value.get("handles", []))
        for view, entries in value.get("view_trees", {}).items():
            flattened.extend({**row, "path": f"{view}:{row.get('path', '')}"} for row in entries)
        return flattened
    left = rows(before); right = rows(after)
    a = {ident(index, row): row for index, row in enumerate(left)}
    b = {ident(index, row): row for index, row in enumerate(right)}
    changed = []
    for key in sorted(set(a) | set(b)):
        if a.get(key) != b.get(key): changed.append(key)
    view_hashes_before = {view: _json_hash(rows) for view, rows in before.get("view_trees", {}).items()}
    view_hashes_after = {view: _json_hash(rows) for view, rows in after.get("view_trees", {}).items()}
    return {"changed": changed[:100], "before_keys": sorted(a)[:100], "after_keys": sorted(b)[:100],
            "view_hashes_before": view_hashes_before, "view_hashes_after": view_hashes_after,
            "publication_before": {k: before.get(k) for k in ("seq", "indexed_objects", "object_roots", "revisions") if k in before},
            "publication_after": {k: after.get(k) for k in ("seq", "indexed_objects", "object_roots", "revisions") if k in after}}


def _verify_fixture(adapter, fixture):
    for item in fixture:
        path = item["path"]
        op = "lstat" if item["type"] == "symlink" else "stat"
        info = adapter.execute(op, {"path": path})
        if info.get("type") != item["type"]:
            raise RuntimeError(f"fixture type mismatch at {path!r}: {info.get('type')!r}")
        if item["type"] == "file":
            expected_data = item["data_hex"].lower()
            actual = adapter.execute("read", {"path": path})["data_hex"].lower()
            if actual != expected_data:
                raise RuntimeError(f"fixture bytes mismatch at {path!r}")
        if item["type"] == "symlink":
            target = adapter.execute("readlink", {"path": path}).get("target")
            if target != item["target"]:
                raise RuntimeError(f"fixture symlink target mismatch at {path!r}")
        if item["type"] in {"file", "directory"}:
            expected_mode = item.get("mode", 0o644 if item["type"] == "file" else 0o755)
            if info.get("mode") != expected_mode:
                raise RuntimeError(f"fixture mode mismatch at {path!r}: expected {expected_mode:o}, got {info.get('mode')!r}")


def _coverage_frontier(families, executions):
    by_case = {}
    for record in executions:
        by_case.setdefault(record["case_id"], []).append({"profile_role": record["profile_role"], "layer": record["layer"], "status": record["status"]})
    frontier = {}
    for family, item in families.items():
        frontier[family] = {
            "cases": {cid: {"executions": by_case.get(cid, []), "layers": sorted({x["layer"] for x in by_case.get(cid, [])})}
                      for cid in item.get("case_ids", [])},
            "existing_external": [{**entry, "status": "not_run"} for entry in item.get("existing_external", [])],
            "missing_contracts": [{**entry, "status": "not_run"} for entry in item.get("missing_contracts", [])],
        }
    return frontier


def run_contract(case_file: Path, coverage_file: Path, profile: str, report_path: Path,
                 daemon: Path | None = None, reference_root: Path | None = None,
                 mountpoint: Path | None = None, timeout=5):
    case_doc = load_json(case_file)
    coverage_doc = load_json(coverage_file)
    cases = validate_case_file(case_doc)
    families = validate_coverage(coverage_doc, cases)
    if profile not in {"reference", "daemon", "mounted"}:
        raise SchemaError(f"unknown profile {profile}")
    if profile == "daemon" and daemon is None: raise SchemaError("daemon profile requires --daemon")
    # Workers run from the repository root. Keep paths anchored to the caller
    # without resolving symlinks before the native mount checks.
    daemon = Path(daemon).absolute() if daemon is not None else None
    reference_root = Path(reference_root).absolute() if reference_root is not None else None
    mountpoint = Path(mountpoint).absolute() if mountpoint is not None else None
    native_error = None
    try:
        native_info = _native_preflight(profile, reference_root, mountpoint, timeout) if profile != "daemon" else {}
    except Unsupported as exc:
        native_info = {}
        native_error = str(exc)
    records = []
    for case in cases.values():
        if profile not in case["profiles"]: continue
        if native_error:
            roles = ["reference", "mounted"] if profile == "mounted" else ["reference"]
            for role in roles:
                records.append({"case_id": case["id"], "family": case["families"],
                                "profile_role": role, "layer": "posix", "status": "blocked",
                                "steps": [], "detail": f"native preflight blocked: {native_error}"})
        elif profile == "daemon":
            records.append(_execute_case(case, "daemon", None, daemon, timeout))
        elif profile == "reference":
            records.append(_execute_case(case, "reference", reference_root, None, timeout))
        else:
            # Fixed expectations are first checked against APFS, then independently
            # against the verified mount using the same immutable case definition.
            records.append(_execute_case(case, "reference", reference_root, None, timeout))
            records.append(_execute_case(case, "mounted", mountpoint, None, timeout))
    if not records:
        raise SchemaError(f"profile {profile} selected no contract cases")
    missing_mandatory = [{"family": family, **entry} for family, details in families.items()
                         for entry in details.get("missing_contracts", [])
                         if entry["mandatory"] and profile in entry["profiles"]]
    profile_passed = all(record["status"] == "pass" for record in records) and not missing_mandatory
    frontier = _coverage_frontier(families, records)
    frontier_open = any(entry["status"] == "not_run"
                        for family in frontier.values()
                        for key in ("existing_external", "missing_contracts")
                        for entry in family[key])
    report = {
        "schema_version": 1,
        "profile": profile,
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "native_verified": bool(profile == "mounted" and native_info.get("candidate", {}).get("verified") and "reference" in native_info),
        "native": native_info,
        "preflight_error": native_error,
        "profile_passed": {profile: profile_passed},
        "conformance_complete": bool(profile == "mounted" and profile_passed and not frontier_open),
        "mandatory_missing_frontier": missing_mandatory,
        "umask": 0,
        "case_corpus_sha256": _json_hash(case_doc),
        "coverage_sha256": _json_hash(coverage_doc),
        "selected_case_count": sum(profile in case["profiles"] for case in cases.values()),
        "runtime": {"platform": platform.platform(), "python": sys.version.split()[0]},
        "case_results": records,
        "coverage_frontier": frontier,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = report_path.with_name(report_path.name + ".tmp")
    temp_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temp_path, report_path)
    return 0 if profile_passed else 1, report
