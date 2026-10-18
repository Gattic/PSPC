#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Launch-boundary tests; no downloads, display, games or system-library writes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/run_local_sdl.py"
LIBRARIES = ("libSDL2-2.0.so.0", "libSDL3.so.0")


class LocalSDLTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pspc local sdl ")
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.lib = self.build / "local-sdl/lib"
        self.lib.mkdir(parents=True)
        self.env = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "SDL_DYNAMIC_API", "SDL3_DYNAMIC_API"):
            self.env.pop(key, None)

    def libraries(self):
        # These placeholders are only for the command/environment boundary. --check
        # must reject them, not silently load the installed SDL instead.
        for name in LIBRARIES:
            (self.lib / name).write_bytes(b"not a shared library\n")

    def run_tool(self, *args, env=None):
        return subprocess.run([sys.executable, "-I", str(TOOL), "--build-dir", str(self.build), *args],
                              env=env or self.env, capture_output=True, text=True, timeout=10)

    def test_missing_library_fails_before_launch(self):
        for missing in LIBRARIES:
            self.libraries()
            (self.lib / missing).unlink()
            result = self.run_tool("--", sys.executable, "-c", "print('LAUNCHED')")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("pspc-local-sdl", result.stderr)
            self.assertIn(missing, result.stderr)
            self.assertNotIn("LAUNCHED", result.stdout)

    def test_environment_arguments_and_children(self):
        self.libraries()
        self.env["LD_LIBRARY_PATH"] = ":/existing/library/path::"
        code = ("import json,os,subprocess,sys; "
                "child=subprocess.check_output([sys.executable,'-c',"
                "'import os; print(os.environ[\"LD_LIBRARY_PATH\"])'],text=True).strip(); "
                "print(json.dumps([os.environ['LD_LIBRARY_PATH'],sys.argv[1:],child]))")
        result = self.run_tool("--", sys.executable, "-I", "-c", code, "a b", "$(not-a-shell)", "--flag")
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = f"{self.lib}:/existing/library/path"
        self.assertEqual(json.loads(result.stdout), [expected, ["a b", "$(not-a-shell)", "--flag"], expected])
        self.assertEqual(self.env["LD_LIBRARY_PATH"], ":/existing/library/path::")
        # Disabling is just not using the helper; no shell/global state was changed.
        direct = subprocess.check_output([sys.executable, "-c", "import os; print(os.environ['LD_LIBRARY_PATH'])"],
                                         env=self.env, text=True)
        self.assertEqual(direct.strip(), ":/existing/library/path::")

    def test_exit_status_is_preserved(self):
        self.libraries()
        result = self.run_tool("--", sys.executable, "-c", "raise SystemExit(37)")
        self.assertEqual(result.returncode, 37)

    def test_loader_overrides_are_rejected(self):
        self.libraries()
        for key in ("SDL_DYNAMIC_API", "SDL3_DYNAMIC_API", "LD_PRELOAD"):
            with self.subTest(key=key):
                env = dict(self.env, **{key: "/conflicting/library.so"})
                result = self.run_tool("--check", env=env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"Unset {key}", result.stderr)

    def test_invalid_library_does_not_fall_back(self):
        self.libraries()
        result = self.run_tool("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("local SDL:", result.stderr)

    def test_command_required_and_check_is_exclusive(self):
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertNotEqual(self.run_tool("--check", "--", "ignored").returncode, 0)

    def test_unrepresentable_library_path_is_rejected(self):
        self.build = self.build / "has:colon"
        result = self.run_tool("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LD_LIBRARY_PATH separator", result.stderr)

    def test_missing_command_is_explicit_failure(self):
        self.libraries()
        result = self.run_tool("--", str(self.build / "no-such-command"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no-such-command", result.stderr)


if __name__ == "__main__":
    unittest.main()
