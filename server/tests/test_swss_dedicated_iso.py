# SPDX-License-Identifier: GPL-2.0-or-later
"""deploy/swss/swss-dedicated-iso.py: a copy of the playable disc with the dedicated executable written over the original's
sectors, nothing else changed, the old image never touched and no half-written image left behind."""

import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'deploy', 'swss', 'swss-dedicated-iso.py')

spec = importlib.util.spec_from_file_location('swss_dedicated_iso', TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

SECTOR = 2048
EBOOT_SIZE = 5000          # three sectors, the last one only partly the file's: the rest belongs to what follows it


def record(name, extent, size, is_dir=False):
    name = name.encode('latin-1')
    n = 33 + len(name)
    n += n % 2
    r = bytearray(n)
    r[0] = n
    struct.pack_into('<I', r, 2, extent)
    struct.pack_into('>I', r, 6, extent)
    struct.pack_into('<I', r, 10, size)
    struct.pack_into('>I', r, 14, size)
    r[25] = 2 if is_dir else 0
    r[32] = len(name)
    r[33:33 + len(name)] = name
    return bytes(r)


def directory(*records):
    return b''.join(records).ljust(SECTOR, b'\0')


def image(eboot, name='EBOOT.BIN;1'):
    """A small ISO 9660 image: /PSP_GAME/SYSDIR/EBOOT.BIN and /DATA.BIN, the executable at sector 21."""
    other = 21 + (len(eboot) + SECTOR - 1) // SECTOR
    pvd = bytearray(SECTOR)
    pvd[0:6] = b'\x01CD001'
    pvd[156:190] = record('\0', 18, SECTOR, True)[:34]
    pvd[156] = 34
    sectors = {
        16: bytes(pvd),
        18: directory(record('\0', 18, SECTOR, True), record('\1', 18, SECTOR, True),
                      record('PSP_GAME', 19, SECTOR, True), record('DATA.BIN;1', other, SECTOR)),
        19: directory(record('\0', 19, SECTOR, True), record('\1', 18, SECTOR, True), record('SYSDIR', 20, SECTOR, True)),
        20: directory(record('\0', 20, SECTOR, True), record('\1', 19, SECTOR, True), record(name, 21, len(eboot))),
        21: eboot.ljust((len(eboot) + SECTOR - 1) // SECTOR * SECTOR, b'\xaa'),    # the file's last sector ends in 0xaa
        other: bytes(range(256)) * 8,
    }
    out = bytearray((other + 1) * SECTOR)
    for lba, data in sectors.items():
        out[lba * SECTOR:lba * SECTOR + len(data)] = data
    return bytes(out)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class DedicatedIsoTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.old = bytes(i * 7 % 251 for i in range(EBOOT_SIZE))
        self.new = bytearray(self.old)
        self.new[1000:1004] = b'\xde\xad\xbe\xef'
        self.new[3000] ^= 0xff
        self.new = bytes(self.new)
        self.src = self.write('SWSS.iso', image(self.old))
        self.eboot = self.write('EBOOT.BIN', self.new)
        self.dst = os.path.join(self.dir, 'SWSS-dedicated.iso')

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def read(self, path):
        with open(path, 'rb') as f:
            return f.read()

    def test_the_executable_is_written_over_the_original_and_nothing_else_changes(self):
        old, new, diff = tool.make(self.src, self.dst, self.eboot)
        self.assertEqual((old, new, diff), (sha256(self.old), sha256(self.new), 5))
        want = bytearray(self.read(self.src))
        want[21 * SECTOR:21 * SECTOR + EBOOT_SIZE] = self.new         # the sector's tail (0xaa) and every other sector stay
        self.assertEqual(self.read(self.dst), bytes(want))
        with open(self.dst, 'rb') as f:
            extent, size = tool.find(f, 'PSP_GAME/SYSDIR/EBOOT.BIN')
            self.assertEqual(tool.read_file(f, extent, size), self.new)

    def test_the_playable_image_is_not_changed_and_no_temporary_file_is_left(self):
        before = self.read(self.src)
        tool.make(self.src, self.dst, self.eboot)
        self.assertEqual(self.read(self.src), before)
        self.assertEqual(sorted(os.listdir(self.dir)), ['EBOOT.BIN', 'SWSS-dedicated.iso', 'SWSS.iso'])

    def test_an_image_that_is_there_is_replaced_whole(self):
        # A running game keeps the old file open: the new one is another file, renamed into place.
        self.write('SWSS-dedicated.iso', b'old')
        tool.make(self.src, self.dst, self.eboot)
        self.assertEqual(len(self.read(self.dst)), len(self.read(self.src)))

    def test_an_executable_of_another_size_is_refused_and_nothing_is_made(self):
        for data in (self.new + b'\0', self.new[:-1]):
            with self.subTest(size=len(data)):
                with self.assertRaisesRegex(ValueError, 'in place'):
                    tool.make(self.src, self.dst, self.write('EBOOT.BIN', data))
        self.assertEqual(sorted(os.listdir(self.dir)), ['EBOOT.BIN', 'SWSS.iso'])

    def test_expected_hashes_are_checked_before_anything_is_written(self):
        tool.make(self.src, self.dst, self.eboot, sha256(self.old), sha256(self.new).upper())
        os.remove(self.dst)
        with self.assertRaisesRegex(ValueError, 'not the playable disc'):
            tool.make(self.src, self.dst, self.eboot, expect_old=sha256(self.new))
        with self.assertRaisesRegex(ValueError, 'EBOOT.BIN is'):
            tool.make(self.src, self.dst, self.eboot, expect_new=sha256(self.old))
        self.assertFalse(os.path.exists(self.dst))

    def test_the_playable_image_is_never_the_destination(self):
        with self.assertRaisesRegex(ValueError, 'another file'):
            tool.make(self.src, self.src, self.eboot)
        self.assertEqual(self.read(self.src), image(self.old))

    def test_a_file_that_is_not_an_iso_or_has_no_executable_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'not an ISO'):
            tool.make(self.write('junk.iso', os.urandom(40 * SECTOR)), self.dst, self.eboot)
        img = bytearray(image(self.old))
        img[20 * SECTOR:21 * SECTOR] = directory(record('\0', 20, SECTOR, True), record('\1', 19, SECTOR, True))
        with self.assertRaisesRegex(ValueError, 'not on the image'):
            tool.make(self.write('empty.iso', bytes(img)), self.dst, self.eboot)
        self.assertFalse(os.path.exists(self.dst))

    def test_names_match_without_case_or_version(self):
        for name in ('eboot.bin;1', 'EBOOT.BIN', 'Eboot.Bin;1'):
            with self.subTest(name=name):
                with open(self.write('v.iso', image(self.old, name)), 'rb') as f:
                    self.assertEqual(tool.find(f, 'PSP_GAME/SYSDIR/EBOOT.BIN'), (21, EBOOT_SIZE))

    def test_command_line(self):
        run = lambda *args: subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)  # noqa: E731
        p = run(self.src, self.dst, self.eboot, '--expect-new', sha256(self.new))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('executable was ' + sha256(self.old), p.stdout)
        self.assertIn('executable now ' + sha256(self.new), p.stdout)
        os.remove(self.dst)
        p = run(self.src, self.dst, self.eboot, '--expect-new', '0' * 64)
        self.assertEqual(p.returncode, 1)
        self.assertIn('swss-dedicated-iso:', p.stderr)
        self.assertFalse(os.path.exists(self.dst))


if __name__ == '__main__':
    unittest.main()
