#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Profile installer transactions with synthetic (not Sony) firmware."""
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/pspc_install_fw.py'
installer = runpy.run_path(str(SCRIPT))


def module():
    data = bytearray(256)
    data[:4] = b'\x7fELF'
    struct.pack_into('<I', data, 0x1c, 52)
    struct.pack_into('<H', data, 0x2c, 1)
    struct.pack_into('<I', data, 52 + 12, 128)
    data[132:141] = b'Synthetic'
    return data


class FirmwareManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pspc firmware tests ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        self.source = self.root / 'source'
        vsh = self.source / 'flash0/vsh/module/vshmain.prx'
        vsh.parent.mkdir(parents=True)
        vsh.write_bytes(module())
        registry = self.source / 'flash1/registry'
        registry.mkdir(parents=True)
        (registry / 'system.dreg').write_bytes(b'original settings')
        (registry / 'init.dat').write_bytes(b'initialization')
        self.args = ['--fw', str(self.source), '--data-dir', str(self.data),
                     '--pspc-dir', str(self.root / 'runtime')]

    def install(self, name, *args, expected=0):
        result = subprocess.run([sys.executable, '-I', str(SCRIPT), *self.args,
                                 '--profile', name, *args], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return self.data / 'firmware' / name

    def test_profiles_are_isolated_and_never_overwritten(self):
        first = self.install('4.01', '--no-aot')
        self.assertEqual((first / 'flash0/vsh/module/vshmain.prx').read_bytes(), module())
        self.assertFalse((first / 'flash1/registry/init.dat').exists())
        self.assertTrue((first / 'flash1/registry/init.dat.bak').exists())
        (first / 'flash1/registry/system.dreg').write_bytes(b'custom settings')
        second = self.install('6.61', '--no-aot')
        self.install('4.01', '--no-aot', expected=2)
        self.assertEqual((first / 'flash1/registry/system.dreg').read_bytes(), b'custom settings')
        self.assertEqual((second / 'flash1/registry/system.dreg').read_bytes(), b'original settings')
        self.assertFalse((self.data / 'flash0').exists())
        self.assertFalse(list((self.data / 'firmware').glob('.building-*')))

    def test_invalid_names_and_symlinks(self):
        for name in ('../outside', '.', 'a/b', 'x\\y', '-bad', 'bad.', 'nul', 'com1.txt', 'A', 'x' * 65):
            self.install(name, '--no-aot', expected=2)
        if sys.platform != 'win32':
            self.data.mkdir()
            (self.data / 'firmware').symlink_to(self.source, target_is_directory=True)
            self.install('4.01', '--no-aot', expected=2)
            self.assertFalse((self.source / '4.01').exists())

    def test_invalid_source_does_not_publish(self):
        (self.source / 'flash0/vsh/module/vshmain.prx').write_bytes(b'bad update')
        self.install('broken', '--no-aot', expected=1)
        self.assertEqual(list((self.data / 'firmware').iterdir()), [])

    def test_failed_and_cancelled_native_builds_do_not_publish(self):
        # Inject the compiler outcome, not the filesystem. The real AOT route is exercised by launcher_paths.
        for outcome in (7, KeyboardInterrupt()):
            argv = [str(SCRIPT), *self.args, '--profile', 'failed', '--cache', str(self.root / 'cache'),
                    '--work', str(self.root / 'work')]
            def build(*args):
                if isinstance(outcome, BaseException):
                    raise outcome
                return outcome
            with patch.object(sys, 'argv', argv), patch.dict(installer['main'].__globals__, build_native=build):
                if isinstance(outcome, BaseException):
                    with self.assertRaises(KeyboardInterrupt):
                        installer['main']()
                else:
                    self.assertEqual(installer['main'](), 7)
            self.assertEqual(list((self.data / 'firmware').iterdir()), [])

    def test_rebuild_preserves_flash_files(self):
        profile = self.install('4.01', '--no-aot')
        registry = profile / 'flash1/registry/system.dreg'
        registry.write_bytes(b'keep my settings')
        # No copying from --fw is permitted in rebuild mode.
        (self.source / 'flash0/vsh/module/vshmain.prx').write_bytes(b'bad source')
        argv = [str(SCRIPT), *self.args, '--profile', '4.01', '--rebuild', '--cache', str(self.root / 'cache'),
                '--work', str(self.root / 'work')]
        def build(args, dst0, *_):
            self.assertEqual(dst0, profile / 'flash0')
            return 0
        with patch.object(sys, 'argv', argv), patch.dict(installer['main'].__globals__, build_native=build):
            self.assertEqual(installer['main'](), 0)
        self.assertEqual(registry.read_bytes(), b'keep my settings')
        self.assertEqual((profile / 'flash0/vsh/module/vshmain.prx').read_bytes(), module())


if __name__ == '__main__':
    unittest.main()
