"""Fast infrastructure checks for the closed DSL and report gates."""
from __future__ import annotations

import unittest
import errno
import os
import subprocess
import tempfile
import time
from pathlib import Path
from unittest import mock

from . import runner
from .adapters import PosixAdapter, Unsupported, UntypedRpcFailure, _DarwinXattr
from .model import SchemaError, resolve_refs, safe_path, validate_case_file, validate_coverage


def _valid_case():
    return {"schema_version": 1, "cases": [{
        "id": "one", "families": ["paths"], "profiles": ["reference", "mounted"],
        "layers": ["posix"], "mandatory": True, "fixture": [],
        "steps": [{"op": "stat", "args": {"path": "f"}, "save": "before",
                   "expect": {"errno": 0, "type": "file"}},
                  {"op": "read", "args": {"path": "f"},
                   "expect": {"errno": "ENOENT", "unchanged": True}}]}]}


class HarnessModelTests(unittest.TestCase):
    def test_validates_closed_case_and_coverage_binding(self):
        cases = validate_case_file(_valid_case())
        coverage = {"schema_version": 1, "families": {"paths": {"case_ids": ["one"]}}}
        validate_coverage(coverage, cases)

    def test_unknown_operation_is_rejected(self):
        doc = _valid_case()
        doc["cases"][0]["steps"][0]["op"] = "eval"
        with self.assertRaises(SchemaError): validate_case_file(doc)

    def test_rename_extensions_reject_paths_outside_owned_root(self):
        for op in ("rename_exclusive", "rename_exchange"):
            for field in ("source", "target"):
                with self.subTest(op=op, field=field):
                    doc = _valid_case()
                    args = {"source": "from", "target": "to"}
                    args[field] = "../outside"
                    doc["cases"][0]["steps"] = [{"op": op, "args": args, "expect": {"errno": 0}}]
                    with self.assertRaisesRegex(SchemaError, "escapes the owned root"):
                        validate_case_file(doc)

    def test_forward_reference_is_rejected(self):
        doc = _valid_case()
        doc["cases"][0]["steps"].insert(0, {"op": "fstat", "args": {"handle": {"ref": "later", "field": "handle"}},
                                             "expect": {"errno": "EBADF", "unchanged": True}})
        with self.assertRaises(SchemaError): validate_case_file(doc)

    def test_coverage_missing_case_binding_is_rejected(self):
        cases = validate_case_file(_valid_case())
        coverage = {"schema_version": 1, "families": {"paths": {"case_ids": []}}}
        with self.assertRaises(SchemaError): validate_coverage(coverage, cases)

    def test_ref_resolution_preserves_binary_encoding_contract(self):
        saved = {"opened": {"handle": "opaque-handle"}}
        self.assertEqual(resolve_refs({"ref": "opened", "field": "handle"}, saved), "opaque-handle")

    def test_result_comparison_checks_bytes_inode_and_negative_identity(self):
        saved = {"old": {"inode": "opaque-id"}}
        self.assertEqual(runner._compare({"data_hex": "00ff", "inode": "new-id"},
                                         {"errno": 0, "data_hex": "00ff",
                                          "inode_not": {"ref": "old", "field": "inode"}}, saved), [])
        self.assertTrue(runner._compare({"data_hex": "00fe", "inode": "opaque-id"},
                                        {"errno": 0, "data_hex": "00ff",
                                         "inode_not": {"ref": "old", "field": "inode"}}, saved))
        self.assertTrue(runner._compare({"inode": "other"},
                                        {"errno": 0, "inode": {"ref": "old", "field": "inode"}}, saved))

    def test_directory_names_compare_as_exact_unordered_multisets(self):
        wanted = ["emoji-🙂", "line\nbreak", "quoted\"name"]
        self.assertEqual(runner._compare({"names": list(reversed(wanted))},
                                         {"errno": 0, "names": wanted}, {}), [])
        self.assertTrue(runner._compare({"names": wanted + [wanted[0]]},
                                        {"errno": 0, "names": wanted}, {}))

    def test_physical_dotdot_guard_preserves_raw_spelling(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "d").mkdir()
            (root / "d" / "f").write_bytes(b"x")
            self.assertEqual(safe_path(root, "d/../d/f"),
                             os.fsencode(str(root.resolve(strict=True)) + "/d/../d/f"))

    def test_safe_path_canonicalizes_owned_root_symlink_alias(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / "owned"
            (root / "d").mkdir(parents=True)
            (root / "d" / "f").write_bytes(b"x")
            alias = base / "owned-alias"
            os.symlink(root, alias, target_is_directory=True)
            self.assertEqual(safe_path(alias, "d/../d/f"),
                             os.fsencode(str(root.resolve(strict=True)) + "/d/../d/f"))

    def test_sequential_write_beyond_extent_bound_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            adapter = PosixAdapter(Path(tmp), "test")
            try:
                handle = adapter.execute("open", {"path": "f", "flags": ["write", "create"]})["handle"]
                self.assertEqual(
                    adapter.execute("seek", {"handle": handle, "displacement": runner.FILE_SIZE_LIMIT, "whence": "set"}),
                    {"offset": runner.FILE_SIZE_LIMIT},
                )
                with self.assertRaises(Unsupported):
                    adapter.execute("write_next", {"handle": handle, "data_hex": "ff"})
                self.assertEqual((Path(tmp) / "f").stat().st_size, 0)
            finally:
                adapter.close_all()

    def test_darwin_fingerprint_encodes_path_arguments_for_ctypes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "file").write_bytes(b"x")
            native = object.__new__(_DarwinXattr)
            native._flags = {"upsert": 1, "create": 3, "replace": 5}
            seen = []
            def path_call(path, *args):
                if not isinstance(path, bytes): raise TypeError("ctypes path argument is not bytes")
                seen.append(path)
                return 0
            native.listxattr = path_call
            native.getxattr = path_call
            native.setxattr = path_call
            native.removexattr = path_call
            native.flistxattr = lambda *args: 0
            native.fgetxattr = lambda *args: 0
            adapter = PosixAdapter(root, "apfs")
            adapter._xattr = native
            try:
                adapter.fingerprint()
                adapter._getxattr(str(root / "file"), "user.test")
                adapter._setxattr(str(root / "file"), "user.test", b"v", "upsert")
                adapter._removexattr(str(root / "file"), "user.test")
            finally:
                adapter.close_all()
            self.assertGreaterEqual(len(seen), 6)
            self.assertTrue(all(isinstance(path, bytes) for path in seen))

    def test_long_name_guard_does_not_fake_target_syscall_errno(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); long_name = "n" * 256
            adapter = runner.PosixAdapter(root, "test")
            try:
                with mock.patch("os.open", wraps=os.open) as target_open:
                    with self.assertRaises(OSError) as caught:
                        adapter.execute("create", {"path": long_name, "data_hex": "", "exclusive": True})
                    target_open.assert_called_once()
                self.assertEqual(caught.exception.errno, errno.ENAMETOOLONG)
            finally: adapter.close_all()

    def test_known_gap_only_labels_an_actual_failure(self):
        case = self._simple_case({"errno": 0}, known_gap=True)
        class PassAdapter:
            def __init__(self, *args, **kwargs): pass
            def fixture(self, item): pass
            def execute(self, op, args): return {}
            def close_all(self): pass
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "PosixAdapter", PassAdapter):
            result = runner._execute_case_inner(case, "reference", Path(tmp) / "owned", None, 1)
        self.assertEqual(result["status"], "pass")
        case = self._simple_case({"errno": "EEXIST", "unchanged": True}, known_gap=True)
        class WrongAdapter(PassAdapter):
            def execute(self, op, args): return {}
            def fingerprint(self, revision=None): return {"entries": [], "handles": []}
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "PosixAdapter", WrongAdapter):
            result = runner._execute_case_inner(case, "reference", Path(tmp) / "owned", None, 1)
        self.assertEqual(result["status"], "known_gap")

    def test_wrong_errno_data_and_fingerprint_change_fail(self):
        case = self._simple_case({"errno": "EEXIST", "unchanged": True})
        class ChangedAdapter:
            def __init__(self, *args, **kwargs): self.state = 0
            def fixture(self, item): pass
            def execute(self, op, args): self.state += 1; raise OSError(errno.EPERM, "wrong errno")
            def fingerprint(self, revision=None): return {"entries": [{"path": ".", "state": self.state}], "handles": []}
            def close_all(self): pass
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "PosixAdapter", ChangedAdapter):
            result = runner._execute_case_inner(case, "reference", Path(tmp) / "owned", None, 1)
        self.assertEqual(result["status"], "fail")
        self.assertTrue(result["steps"][0].get("atomicity_diff"))

    def test_contract_failure_survives_later_unsupported_fingerprint(self):
        case = self._simple_case({"errno": "EEXIST", "unchanged": True})
        class FailingAdapter:
            def __init__(self, *args, **kwargs): self.fingerprints = 0
            def fixture(self, item): pass
            def execute(self, op, args): raise OSError(errno.EPERM, "wrong errno")
            def fingerprint(self, revision=None):
                self.fingerprints += 1
                if self.fingerprints > 1: raise Unsupported("fingerprint unavailable")
                return {"entries": [], "handles": []}
            def close_all(self): pass
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "PosixAdapter", FailingAdapter):
            result = runner._execute_case_inner(case, "reference", Path(tmp) / "owned", None, 1)
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["steps"][0]["status"], "fail")
        self.assertIn("fingerprint_after_error", result["steps"][0])

    def test_untyped_rpc_and_unsupported_timeout_are_nonpassing(self):
        case = self._simple_case({"errno": "EIO", "unchanged": True})
        class UntypedAdapter:
            def __init__(self, *args, **kwargs): pass
            def fixture(self, item): pass
            def execute(self, op, args): raise UntypedRpcFailure(1, b"failure")
            def fingerprint(self, revision=None): return {"entries": [], "handles": [], "seq": 1}
            def close_all(self): pass
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "RpcAdapter", UntypedAdapter):
            result = runner._execute_case_inner(case, "daemon", Path(tmp) / "owned", Path("unused"), 1)
        self.assertEqual(result["status"], "fail")
        for error, expected in ((Unsupported("not supported"), "unsupported"), (TimeoutError("slow"), "blocked")):
            class ErrorAdapter:
                def __init__(self, *args, **kwargs): pass
                def fixture(self, item): pass
                def execute(self, op, args): raise error
                def close_all(self): pass
            with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner, "PosixAdapter", ErrorAdapter):
                result = runner._execute_case_inner(self._simple_case({"errno": 0}), "reference", Path(tmp) / "owned", None, 1)
            self.assertEqual(result["status"], expected)

    def test_mandatory_missing_frontier_fails_daemon_profile_and_empty_filter_errors(self):
        case = {"schema_version": 1, "cases": [{"id": "rpc_case", "families": ["rpc"],
                "profiles": ["daemon"], "layers": ["rpc"], "mandatory": True, "fixture": [],
                "steps": [{"op": "restart", "args": {}, "expect": {"errno": 0}}]}]}
        coverage = {"schema_version": 1, "families": {"rpc": {"case_ids": ["rpc_case"],
                    "missing_contracts": [{"id": "missing", "layer": "rpc", "profiles": ["daemon"],
                                           "mandatory": True, "reason": "not implemented"}]}}}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); case_path = root / "cases.json"; coverage_path = root / "coverage.json"
            report_path = root / "report.json"
            case_path.write_text(__import__("json").dumps(case), encoding="utf-8")
            coverage_path.write_text(__import__("json").dumps(coverage), encoding="utf-8")
            passed = {"case_id": "rpc_case", "family": ["rpc"], "profile_role": "daemon",
                      "layer": "rpc", "status": "pass", "steps": []}
            with mock.patch.object(runner, "_execute_case", return_value=passed):
                code, report = runner.run_contract(case_path, coverage_path, "daemon", report_path, Path("unused"))
            self.assertEqual(code, 1)
            self.assertFalse(report["profile_passed"]["daemon"])
            self.assertEqual(report["mandatory_missing_frontier"][0]["id"], "missing")

            case["cases"][0]["profiles"] = ["reference"]
            case["cases"][0]["layers"] = ["posix"]
            case_path.write_text(__import__("json").dumps(case), encoding="utf-8")
            with self.assertRaises(SchemaError):
                runner.run_contract(case_path, coverage_path, "daemon", report_path, Path("unused"))

    def test_real_sleeping_worker_is_killed_by_hard_deadline(self):
        real_popen = subprocess.Popen
        children = []
        def sleeper(*args, **kwargs):
            child = real_popen([os.sys.executable, "-c", "import time; time.sleep(30)"],
                               start_new_session=True, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            children.append(child)
            return child
        case = self._simple_case({"errno": 0})
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(runner.subprocess, "Popen", side_effect=sleeper):
            result = runner._execute_case(case, "reference", Path(tmp), None, 0.05)
        self.assertEqual(result["status"], "blocked")
        self.assertTrue(result.get("cleanup_pending"))
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll())

    @staticmethod
    def _simple_case(expect, known_gap=False):
        case = {"id": "simple", "families": ["family"], "profiles": ["reference"],
                "layers": ["posix"], "mandatory": True, "fixture": [],
                "steps": [{"op": "stat", "args": {"path": "file"}, "expect": expect}]}
        if known_gap: case["known_gap"] = {"issue": "test", "reason": "test"}
        return case


def run_selftests():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HarnessModelTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(run_selftests())
