#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/mkcso.py (docs/large-iso-plan.md 2.3).

usage: test_mkcso.py [-v]   (PSPC_BIN = the folder with pspc_unittests.exe, default
                             build/RelWithDebInfo/bin; the round trip is skipped without it)

The index shift picker on sizes around 2, 4 and 8 GB; small images compressed with shifts 1 and 2
and read back sector by sector by PSPC (pspc_unittests' fs_external_images); and DAX refusing a
sparse 4 GB input at once, without compressing and without leaving an output behind.
"""
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # also when run with python -I
import make_big_iso  # noqa: E402
import mkcso  # noqa: E402

GB = 1 << 30
MKCSO = [sys.executable, '-I', str(HERE / 'mkcso.py')]


def unittests_exe():
    exe = pathlib.Path(os.environ.get('PSPC_BIN', HERE.parent / 'build' / 'RelWithDebInfo' / 'bin')) / 'pspc_unittests.exe'
    return exe if exe.exists() else None


class ShiftPicker(unittest.TestCase):
    def check(self, total, block):
        s = mkcso.needed_shift(total, block)
        self.assertIsNotNone(s)
        # Enough for the worst case, and the smallest that is.
        self.assertLessEqual(mkcso.ciso_worst_size(total, block, s) >> s, 0x7FFFFFFF)
        if s:
            self.assertGreater(mkcso.ciso_worst_size(total, block, s - 1) >> (s - 1), 0x7FFFFFFF)
        return s

    def test_sizes(self):
        for block in (2048, 16384):
            self.assertEqual(self.check(1 * GB, block), 0)
            self.assertEqual(self.check(2 * GB - 64 * 2**20, block), 0)
            self.assertEqual(self.check(3 * GB, block), 1)
            self.assertEqual(self.check(5 * GB, block), 2)
            self.assertEqual(self.check(8 * GB + GB // 2, block), 3)
            for edge in (2 * GB, 4 * GB, 8 * GB):
                for delta in (-(2**20), -1, 0, 1, 2**20):
                    self.check(edge + delta, block)

    def test_too_big(self):
        # 2^31 << 20 bytes is beyond any shift.
        self.assertIsNone(mkcso.needed_shift(1 << 52, 2048))


def sample_image(seed, size):
    """Mixed data: compressible text, random runs, zeros; stops mid-sector."""
    rnd = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        kind = rnd.randrange(3)
        n = rnd.randrange(100, 9000)
        if kind == 0:
            out += (b'UMD sector ISO9660 EBOOT.BIN ' * (n // 29 + 1))[:n]
        elif kind == 1:
            out += bytes(rnd.randrange(256) for _ in range(n))
        else:
            out += bytes(n)
    return bytes(out[:size])


class RoundTrip(unittest.TestCase):
    def test_shifts_read_back_by_pspc(self):
        exe = unittests_exe()
        if not exe:
            self.skipTest('no pspc_unittests.exe (set PSPC_BIN)')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            for shift in (1, 2):
                base = tmp / ('a%d' % shift)
                base.with_suffix('.iso').write_bytes(sample_image(shift, 300 * 1024 + 700))
                for fmt, ext in (('cso', '.cso'), ('cso2', '.cso2'), ('zso', '.zso')):
                    subprocess.run(MKCSO + ['-f', fmt, '-a', str(shift), str(base.with_suffix('.iso')),
                                            str(base.with_suffix(ext))], check=True)
                    self.assertEqual(base.with_suffix(ext).read_bytes()[21], shift)  # the header's shift
            (tmp / 'd.iso').write_bytes(sample_image(7, 200 * 1024))
            subprocess.run(MKCSO + ['-f', 'dax', str(tmp / 'd.iso'), str(tmp / 'd.dax')], check=True)
            self.assertEqual(sorted(p.name for p in tmp.glob('*.part')), [])
            env = dict(os.environ, PSPC_FS_IMAGE_DIR=str(tmp))
            r = subprocess.run([str(exe), 'fs_external_images'], env=env, stdout=subprocess.PIPE, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn('compared 7 external images', r.stdout)


class DaxLimit(unittest.TestCase):
    def test_refuses_4gb_at_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, 'big.iso')
            try:
                make_big_iso.make_sparse(src, 4 * GB)
            except OSError as e:
                self.skipTest(str(e))
            out = os.path.join(tmp, 'big.dax')
            start = time.time()
            r = subprocess.run(MKCSO + ['-f', 'dax', src, out], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('under 4 GB', r.stderr)
            self.assertLess(time.time() - start, 10)
            self.assertEqual(sorted(os.listdir(tmp)), ['big.iso'])


if __name__ == '__main__':
    unittest.main()
