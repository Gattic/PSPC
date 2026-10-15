#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise both test-tool entry points; launcher_paths checks the real runtime resolver."""
import io
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
autotests = runpy.run_path(str(ROOT / 'tools/run_autotests.py'))
smoke = runpy.run_path(str(ROOT / 'tools/smoke_games.py'))
resolve_firmware = autotests['resolve_firmware']


class FirmwareDiscoveryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='pspc firmware discovery λ ')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.pspc = self.root / 'bin/pspc'
        self.profile = self.root / 'data/firmware/4.01'
        self.flash0 = self.profile / 'flash0'
        self.flash1 = self.profile / 'flash1'
        self.override = self.root / 'explicit flash0'
        for directory in (self.flash0, self.override):
            font = directory / 'font/jpn0.pgf'
            font.parent.mkdir(parents=True)
            font.write_bytes(b'font preflight sentinel')
        self.calls = []

    def child(self, command, **kwargs):
        self.calls.append(list(command))
        if '--firmware-paths' in command:
            # Only the runtime child is substituted. Resolve/parse, filesystem preflight,
            # argument construction, parallel dispatch and reporting are the real code.
            flash0 = command[command.index('--flash0') + 1] if '--flash0' in command else self.flash0
            return subprocess.CompletedProcess(command, 0,
                json.dumps({'flash0': str(flash0), 'flash1': str(self.flash1)}), '')
        return subprocess.CompletedProcess(command, 124, b'', b'')

    def check_commands(self, override):
        queries = [cmd for cmd in self.calls if '--firmware-paths' in cmd]
        guests = [cmd for cmd in self.calls if '--firmware-paths' not in cmd]
        expected_query = [str(self.pspc), '--firmware-paths']
        if override:
            expected_query += ['--flash0', str(self.override)]
        self.assertEqual(queries, [expected_query], 'resolve/migrate once, before parallel guests')
        self.assertEqual(self.calls[0], expected_query)
        self.assertEqual(len(guests), 2)
        for command in guests:
            self.assertEqual(command[command.index('--config') + 1], 'none')
            self.assertEqual(command[command.index('--flash0') + 1], str(self.override if override else self.flash0))
            self.assertEqual(command[command.index('--flash1') + 1], str(self.flash1))

    def run_autotests(self, override=False):
        inputs = self.root / 'inputs'
        for name in ('font/fonttest', 'cpu/synthetic'):
            prx = inputs / (name + '.prx')
            prx.parent.mkdir(parents=True)
            prx.write_bytes(b'guest execution substituted by child stub')
            prx.with_suffix('.expected').write_text('')
        report = self.root / 'results.json'
        argv = ['run_autotests.py', '--pspc', str(self.pspc), '--tests', str(inputs),
                '--jobs', '2', '--json', str(report)]
        if override:
            argv += ['--flash0', str(self.override)]
        # The unrelated UMD corpus preparation is unnecessary for these two synthetic tests.
        with io.TextIOWrapper(io.BytesIO(), encoding='utf-8') as output, \
                patch.object(sys, 'stdout', output), patch.object(sys, 'argv', argv), \
                patch.dict(autotests['main'].__globals__, ROOT=self.root, prepare_fixtures=lambda _: self.root), \
                patch.object(subprocess, 'run', self.child):
            self.assertEqual(autotests['main'](), 0)
        results = json.loads(report.read_text())
        self.assertEqual(len(results), 2)
        self.assertTrue(all('skipped' not in result for result in results))
        self.check_commands(override)

    def run_smoke(self, override=False):
        games = self.root / 'games'
        games.mkdir()
        for name in ('first.pbp', 'second.pbp'):
            (games / name).write_bytes(b'guest execution substituted by child stub')
        argv = ['smoke_games.py', '--pspc', str(self.pspc), '--games', str(games),
                '--out', str(self.root / 'frames'), '--refs', str(self.root / 'refs')]
        if override:
            argv += ['--flash0', str(self.override)]
        with io.TextIOWrapper(io.BytesIO(), encoding='utf-8') as output, \
                patch.object(sys, 'stdout', output), patch.object(sys, 'argv', argv), \
                patch.dict(smoke['main'].__globals__, ROOT=self.root), \
                patch.object(subprocess, 'run', self.child):
            # No rendered frames from this stub: the smoke quality verdict must still fail.
            self.assertEqual(smoke['main'](), 1)
            output.flush()
            self.assertNotIn(b'no firmware', output.buffer.getvalue())
        self.check_commands(override)

    def test_autotests_use_selected_profile_and_do_not_skip_installed_font(self):
        self.run_autotests()

    def test_autotests_keep_explicit_override(self):
        self.run_autotests(override=True)

    def test_smoke_uses_selected_profile(self):
        self.run_smoke()

    def test_smoke_keeps_explicit_override(self):
        self.run_smoke(override=True)

    def test_resolution_failure_is_not_hidden_by_old_layout_fallback(self):
        old_font = self.pspc.parent / 'flash0/font/jpn0.pgf'
        old_font.parent.mkdir(parents=True)
        old_font.write_bytes(b'obsolete layout')
        with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'migration conflict')):
            with self.assertRaisesRegex(RuntimeError, 'migration conflict'):
                resolve_firmware(self.pspc)


if __name__ == '__main__':
    unittest.main()
