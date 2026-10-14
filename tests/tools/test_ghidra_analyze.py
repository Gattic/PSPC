#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixture-only regression tests for Ghidra discovery and headless invocation.

No Ghidra, JDK, PSP SDK or firmware is required. The Linux CLI executes a
recording launcher; Windows selection is checked without executing a batch file.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/ghidra/ghidra_analyze.py"
spec = importlib.util.spec_from_file_location("ghidra_analyze", SCRIPT)
ghidra = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ghidra)


def elf_module(kernel=False):
    # An original PRX header with SceModuleInfo at the first segment's p_paddr.
    data = bytearray(86)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", data, 16, 0xFFA0)
    struct.pack_into("<I", data, 0x1C, 52)
    struct.pack_into("<I", data, 52 + 12, 84)
    struct.pack_into("<H", data, 84, 0x1000 if kernel else 0)
    return data


class HeadlessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pspc-ghidra-é ' $-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.install = self.root / "Ghidra install"
        self.support = self.install / "support"
        self.support.mkdir(parents=True)
        self.jdk = self.make_jdk("JDK 21")
        self.bin = self.root / "path-bin"
        self.bin.mkdir()
        self.record = self.root / "calls.jsonl"
        self.project = self.root / "project dir"
        self.output = self.root / "analysis output"
        self.user = self.root / "user module.prx"
        self.user.write_bytes(elf_module())
        self.env = {key: value for key, value in os.environ.items() if key not in
                    ("JAVA_HOME", "PSPC_JAVA_HOME", "GHIDRA_INSTALL_DIR")}
        self.env.update(GHIDRA_INSTALL_DIR=str(self.install), PSPC_JAVA_HOME=str(self.jdk),
                        PATH=str(self.bin), GHIDRA_TEST_RECORD=str(self.record))
        self.launcher = self.support / "analyzeHeadless"
        self.launcher.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "with open(os.environ['GHIDRA_TEST_RECORD'], 'a', encoding='utf-8') as f:\n"
            "    f.write(json.dumps({'argv': sys.argv[1:], 'java_home': os.environ['JAVA_HOME'],\n"
            "                        'path': os.environ['PATH']}) + '\\n')\n"
            "sys.exit(int(os.environ.get('GHIDRA_TEST_EXIT', '0')))\n",
            encoding="utf-8")
        self.launcher.chmod(0o755)

    def make_jdk(self, name, suffix=""):
        home = self.root / name
        (home / "bin").mkdir(parents=True)
        for tool in ("java", "javac"):
            executable = home / "bin" / (tool + suffix)
            executable.write_text("fixture executable\n", encoding="utf-8")
            executable.chmod(0o755)
        return home

    def cli(self, *options, inputs=None, env=None):
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--project-dir", str(self.project),
             "--out", str(self.output), *map(str, options),
             *map(str, inputs if inputs is not None else [self.user])],
            env=self.env if env is None else env, cwd=self.root,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)

    def calls(self):
        return [json.loads(line) for line in self.record.read_text(encoding="utf-8").splitlines()]

    @unittest.skipIf(os.name == "nt", "executes a POSIX launcher")
    def test_linux_cli_groups_modules_and_forwards_options(self):
        kernel = self.root / "kernel.prx"
        kernel.write_bytes(elf_module(kernel=True))
        peer = self.root / "peer.elf"
        peer.write_bytes(elf_module())
        raw = self.root / "raw ; $ payload.bin"
        raw.write_bytes(b"original raw fixture")
        result = self.cli("--ghidra", self.install, "--folder", "modules", "--project", "test project",
                          "--raw-base", "0x88fc0000", "--no-decomp", "--timeout", "17", "--max-cpu", "2",
                          inputs=[self.user, kernel, peer, raw])
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 3)
        for call in calls:
            args = call["argv"]
            self.assertEqual(args[:2], [str(self.project), "test project/modules"])
            self.assertEqual(call["java_home"], str(self.jdk))
            self.assertEqual(call["path"].split(os.pathsep)[0], str(self.jdk / "bin"))
            self.assertIn("-overwrite", args)
            self.assertEqual(args[args.index("-processor") + 1], "Allegrex:LE:32:default")
            self.assertEqual(args[args.index("-scriptPath") + 1], str(SCRIPT.parent))
            self.assertEqual(args[args.index("-preScript") + 1:args.index("-preScript") + 3],
                             ["PspcApplyNids.java", str(REPO / "data/nids.csv")])
            self.assertEqual(args[args.index("-postScript") + 1:args.index("-postScript") + 4],
                             ["PspcExportAnalysis.java", str(self.output), "nodecomp"])
            self.assertEqual(args[args.index("-analysisTimeoutPerFile") + 1], "17")
            self.assertEqual(args[args.index("-max-cpu") + 1], "2")
            for flag in ("-log", "-scriptlog"):
                self.assertEqual(Path(args[args.index(flag) + 1]).parent, self.project / "logs")
        user, kernel, raw_call = [call["argv"] for call in calls]
        self.assertEqual(user[user.index("-loader-imagebase") + 1], "08804000")
        self.assertEqual([user[i + 1] for i, arg in enumerate(user) if arg == "-import"],
                         [str(self.user), str(peer)])
        self.assertEqual(kernel[kernel.index("-loader-imagebase") + 1], "88000000")
        self.assertEqual(kernel[kernel.index("-loader") + 1], "PspElfLoader")
        self.assertEqual(raw_call[raw_call.index("-loader") + 1], "BinaryLoader")
        self.assertEqual(raw_call[raw_call.index("-loader-baseAddr") + 1], "0x88fc0000")
        self.assertEqual(raw_call[raw_call.index("-import") + 1], str(raw))

    @unittest.skipIf(os.name == "nt", "executes a POSIX launcher")
    def test_java_home_precedence(self):
        explicit = self.make_jdk("explicit JDK")
        standard = self.make_jdk("standard JDK")
        env = dict(self.env, JAVA_HOME=str(standard))
        result = self.cli("--java-home", explicit, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli(env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        del env["PSPC_JAVA_HOME"]
        result = self.cli(env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call["java_home"] for call in self.calls()],
                         list(map(str, [explicit, self.jdk, standard])))

    @unittest.skipIf(os.name == "nt", "POSIX PATH and symlink discovery")
    def test_path_jdk_resolves_alternatives_symlinks(self):
        alternatives = self.root / "alternatives"
        alternatives.mkdir()
        (alternatives / "javac").symlink_to(self.jdk / "bin/javac")
        (self.bin / "javac").symlink_to(alternatives / "javac")
        env = dict(self.env)
        del env["PSPC_JAVA_HOME"]
        result = self.cli(env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["java_home"], str(self.jdk))

    @unittest.skipIf(os.name == "nt", "POSIX executable permissions")
    def test_rejects_invalid_selected_jdk_without_falling_back(self):
        for defect in ("missing", "jre-only", "not-executable"):
            with self.subTest(defect=defect):
                home = self.make_jdk(defect)
                if defect == "missing":
                    (home / "bin/java").unlink()
                elif defect == "jre-only":
                    (home / "bin/javac").unlink()
                else:
                    (home / "bin/java").chmod(0o644)
                result = self.cli("--java-home", home)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("JDK", result.stderr)
                self.assertFalse(self.record.exists())
                self.assertFalse(self.project.exists())

    @unittest.skipIf(os.name == "nt", "POSIX PATH discovery")
    def test_jre_only_on_path_is_not_a_jdk(self):
        (self.bin / "java").symlink_to(self.jdk / "bin/java")
        env = dict(self.env)
        del env["PSPC_JAVA_HOME"]
        result = self.cli(env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("JDK", result.stderr)
        self.assertFalse(self.record.exists())

    @unittest.skipIf(os.name == "nt", "POSIX launcher checks")
    def test_missing_or_nonexecutable_launcher_is_explicit_error(self):
        self.launcher.chmod(0o644)
        for missing in (False, True):
            with self.subTest(missing=missing):
                if missing:
                    self.launcher.unlink()
                result = self.cli()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("analyzeHeadless", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertFalse(self.record.exists())

    def test_requires_ghidra_configuration(self):
        env = dict(self.env)
        del env["GHIDRA_INSTALL_DIR"]
        result = self.cli(env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GHIDRA_INSTALL_DIR", result.stderr)
        self.assertIn("--ghidra", result.stderr)
        self.assertFalse(self.record.exists())

    @unittest.skipIf(os.name == "nt", "POSIX shell quoting")
    def test_dry_run_is_shell_quoted_and_does_not_launch(self):
        result = self.cli("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        cmd = shlex.split(result.stdout.splitlines()[-1])
        self.assertEqual(cmd[0], str(self.launcher))
        self.assertEqual(cmd[cmd.index("-import") + 1], str(self.user))
        self.assertEqual(cmd[cmd.index("-postScript") + 2], str(self.output))
        self.assertFalse(self.record.exists())

    @unittest.skipIf(os.name == "nt", "executes a POSIX launcher")
    def test_headless_failure_is_propagated(self):
        result = self.cli(env=dict(self.env, GHIDRA_TEST_EXIT="23"))
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn("analyzeHeadless exited with 23", result.stderr)
        self.assertEqual(len(self.calls()), 1)

    def test_windows_dry_run_selects_batch_launcher_and_exe_jdk(self):
        (self.support / "analyzeHeadless.bat").write_text("fixture batch launcher\n")
        home = self.make_jdk("Windows JDK", suffix=".exe")
        argv = [str(SCRIPT), "--ghidra", str(self.install), "--java-home", str(home),
                "--project-dir", str(self.project), "--dry-run", str(self.user)]
        output = io.StringIO()
        with mock.patch.object(ghidra.os, "name", "nt"), mock.patch.object(sys, "argv", argv), \
                contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as exit_result:
            ghidra.main()
        self.assertEqual(exit_result.exception.code, 0)
        self.assertIn("analyzeHeadless.bat", output.getvalue())
        self.assertFalse(self.record.exists())

    def test_windows_jdk_installation_discovery(self):
        older = self.make_jdk("jdk-21.0.1", suffix=".exe")
        newer = self.make_jdk("jdk-21.0.2", suffix=".exe")
        with mock.patch.object(ghidra.os, "name", "nt"), mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(ghidra.glob, "glob", return_value=[str(older), str(newer)]):
            self.assertEqual(ghidra.find_java_home(None), str(newer))


if __name__ == "__main__":
    unittest.main()
