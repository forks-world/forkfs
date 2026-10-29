"""End-to-end CLI coverage for developer_mount_soak compiler sysroot handling."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "bench" / "developer_mount_soak.py"


class DeveloperSoakNativeCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="developer soak native ")
        self.root = Path(self.temp.name)
        self.native = self.root / "native fixture"
        self.output = self.root / "output artifacts"
        self.native.mkdir()
        self.output.mkdir()
        self.bin_dir = self.root / "fake compiler bin"
        self.bin_dir.mkdir()
        self.compiler_log = self.root / "compiler argv.jsonl"
        self.real_compiler = shutil.which("cc")
        if self.real_compiler is None:
            self.skipTest("cc is required for the native CLI test")
        self.wrapper = self.bin_dir / "fake-cc"
        self.wrapper.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, sys\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['SOAK_COMPILER_LOG'], 'a', encoding='utf-8') as stream:\n"
            "    stream.write(json.dumps(args) + '\\n')\n"
            "if os.environ.get('SOAK_COMPILER_FAIL') == '1' and args != ['--version']:\n"
            "    print('controlled compiler stderr from native soak', file=sys.stderr)\n"
            "    sys.exit(29)\n"
            "forward = list(args)\n"
            "if '-isysroot' in forward:\n"
            "    at = forward.index('-isysroot')\n"
            "    del forward[at:at + 2]\n"
            "os.execv(os.environ['SOAK_REAL_COMPILER'], [os.environ['SOAK_REAL_COMPILER'], *forward])\n",
            encoding="utf-8",
        )
        self.wrapper.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update({
            "PATH": str(self.bin_dir) + os.pathsep + self.env.get("PATH", ""),
            "SOAK_COMPILER_LOG": str(self.compiler_log),
            "SOAK_REAL_COMPILER": self.real_compiler,
        })

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, *extra, env=None):
        command = [
            sys.executable, str(SCRIPT), "--native", str(self.native),
            "--output", str(self.output), "--rounds", "1", "--files", "1",
            "--deadline", "60", "--compiler", "fake-cc", *map(str, extra),
        ]
        return subprocess.run(command, env=env or self.env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=90, check=False)

    def compiler_argv(self):
        return [json.loads(line) for line in self.compiler_log.read_text(encoding="utf-8").splitlines()]

    def result_dir(self):
        matches = list(self.output.glob("forkfs-developer-mount-soak-*"))
        self.assertEqual(len(matches), 1)
        return matches[0]

    def assert_no_workspaces(self):
        self.assertEqual(list(self.native.glob(".forkfs-perf-*")), [])

    def test_sdk_path_with_spaces_reaches_every_compile_and_link(self):
        sdk = self.root / "SDK root with spaces"
        sdk.mkdir()
        sdk = sdk.resolve()
        result = self.invoke("--sysroot", sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.compiler_argv()
        version_calls = [argv for argv in commands if argv == ["--version"]]
        builds = [argv for argv in commands if argv != ["--version"]]
        self.assertEqual(len(version_calls), 1)
        self.assertEqual(len(builds), 12)
        compile_calls = [argv for argv in builds if "-c" in argv]
        link_calls = [argv for argv in builds if "-c" not in argv]
        self.assertEqual(len(compile_calls), 9)
        self.assertEqual(len(link_calls), 3)
        for argv in builds:
            self.assertEqual(argv.count("-isysroot"), 1)
            at = argv.index("-isysroot")
            self.assertEqual(argv[at + 1], str(sdk))
        summary = json.loads((self.result_dir() / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["outcome"], "ok")
        self.assertEqual(summary["compiler"]["sysroot"], str(sdk))
        self.assertEqual(summary["compiler"]["argv_flags"], ["-isysroot", str(sdk)])
        self.assertTrue(summary["workspaces_cleaned"])
        self.assert_no_workspaces()

    def test_no_sdk_preserves_compiler_argv_without_sysroot(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.compiler_argv()
        builds = [argv for argv in commands if argv != ["--version"]]
        self.assertEqual(len(builds), 12)
        self.assertTrue(all("-isysroot" not in argv for argv in commands))
        summary = json.loads((self.result_dir() / "summary.json").read_text(encoding="utf-8"))
        self.assertIsNone(summary["compiler"]["sysroot"])
        self.assertEqual(summary["compiler"]["argv_flags"], [])
        self.assertTrue(summary["workspaces_cleaned"])
        self.assert_no_workspaces()

    def test_invalid_sdk_is_rejected_before_fixture_creation(self):
        invalid_sdk = self.root / "missing SDK directory"
        result = self.invoke("--sysroot", invalid_sdk)
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid --sysroot", result.stderr)
        self.assertEqual(list(self.output.iterdir()), [])
        self.assertEqual(list(self.native.iterdir()), [])
        self.assertFalse(self.compiler_log.exists())

    def test_compiler_stderr_survives_failure_summary_and_cleanup(self):
        env = self.env.copy()
        env["SOAK_COMPILER_FAIL"] = "1"
        result = self.invoke(env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn("controlled compiler stderr from native soak", result.stderr)
        summary = json.loads((self.result_dir() / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["outcome"], "error")
        self.assertIn("controlled compiler stderr from native soak", summary["error"])
        self.assertTrue(summary["workspaces_cleaned"])
        self.assertEqual(summary["cleanup_errors"], [])
        self.assert_no_workspaces()


if __name__ == "__main__":
    unittest.main()
