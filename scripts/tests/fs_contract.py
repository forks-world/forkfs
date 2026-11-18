#!/usr/bin/env python3
"""Thin CLI entry point for the fixed filesystem contract harness."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) == 3 and argv[0] == "--worker":
        from tests.fs_contract.runner import _worker_main
        return _worker_main(argv[1], argv[2])
    if len(argv) == 3 and argv[0] == "--preflight":
        from tests.fs_contract.runner import _preflight_main
        return _preflight_main(argv[1], argv[2])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("reference", "daemon", "mounted"))
    parser.add_argument("--cases", type=Path, default=ROOT / "tests/fs_contract/cases.json")
    parser.add_argument("--coverage", type=Path, default=ROOT / "tests/fs_contract/coverage.json")
    parser.add_argument("--daemon", type=Path)
    parser.add_argument("--reference-root", type=Path)
    parser.add_argument("--mountpoint", type=Path)
    parser.add_argument("--report", type=Path, default=Path("fs-contract-report.json"))
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="hard per-case worker deadline in seconds (default: 60)")
    parser.add_argument("--selftest", action="store_true", help="run infrastructure-only standard-library selftests")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or args.timeout > 3600:
        parser.error("--timeout must be in (0, 3600]")
    if args.selftest:
        from tests.fs_contract.test_harness import run_selftests
        return run_selftests()
    if args.profile is None:
        parser.error("--profile is required")
    try:
        from tests.fs_contract.runner import run_contract
        code, report = run_contract(args.cases, args.coverage, args.profile, args.report,
                                    daemon=args.daemon, reference_root=args.reference_root,
                                    mountpoint=args.mountpoint, timeout=args.timeout)
        print(json.dumps({"profile": args.profile, "profile_passed": report["profile_passed"],
                          "native_verified": report["native_verified"], "report": str(args.report)},
                         sort_keys=True))
        return code
    except Exception as exc:
        # A report is still emitted for schema/preflight failures. It never claims
        # conformance and identifies the gate as blocked.
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({"schema_version": 1, "profile": args.profile,
                                           "profile_passed": {args.profile or "unspecified": False},
                                           "conformance_complete": False, "native_verified": False,
                                           "status": "blocked", "error": f"{type(exc).__name__}: {exc}"},
                                          indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"fs contract harness blocked: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
