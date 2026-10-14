#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Large disc images end to end (docs/large-iso-plan.md 2.4).

usage: large_image_test.py [--pspc PATH] [--aot] [--keep] [--timeout S]

Builds an 8.75 GB sparse disc image (tools/make_big_iso.py) whose EBOOT.BIN is the guest program
tests/guest/large_image (built by tools/build_guest_tests.sh into build/guest-tests), boots it in
pspc, and compares what the program prints with lines worked out from the image file itself:
files past 2, 4 and 8 GB, seeks past 2 GB in a 4 GB - 1 file (sceIoLseek and sceIoLseek32), umd0:
sectors and /sce_lbn extents past 8 GB, and the last-sector devctl. Not part of the pspautotests
gate: no PSP recording exists for it. Skips (exit 0) when the guest program isn't built.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # also when run with python -I
import guest_tests  # noqa: E402
import make_big_iso  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
PRX = ROOT / 'build' / 'guest-tests' / 'large_image' / 'large_image.prx'
SECTOR = make_big_iso.SECTOR


def file_at(name):
    for n, at, size, _ in make_big_iso.LAYOUT:
        if n == name:
            return at, size
    raise KeyError(name)


def probes():
    """The commands disc0:/PROBES.TXT holds (see tests/guest/large_image/main.c)."""
    big_at, big = file_at('BIG.BIN')
    tail_at, _ = file_at('TAIL.BIN')
    past4_at, _ = file_at('PAST4G.BIN')
    big_end = big_at + big - 64  # BIG.BIN's last marker, past 8 GB in the image
    return [
        'stat disc0:/BIG.BIN',
        'stat disc0:/TAIL.BIN',
        'open disc0:/SMALL.BIN', 'read', 'seek 2 -64', 'read',
        'open disc0:/PAST2G.BIN', 'read', 'seek 2 -64', 'read',
        'open disc0:/PAST4G.BIN', 'read', 'seek 0 0xffc0', 'read',
        'open disc0:/BIG.BIN', 'read',
        'seek 0 0x7fffffe0', 'read',                     # across the file's own 2 GB
        'seek 0 0x80100000', 'read',
        'seek 1 %d' % ((big - 64) - (0x80100000 + 64)), 'read',  # forward to the end by SEEK_CUR
        'seek 1 %d' % -(big - 0x80100000), 'read',                # and back
        'seek 2 -64', 'read',
        'seek 0 %d' % (8 * make_big_iso.GB - big_at - 32), 'read',  # across the image's 8 GB
        'seek32 0 0x7fffffe0', 'read',
        'seek32 2 -64', 'read',                          # the int result of a position past 2 GB
        'seek 0 -5', 'seek 1 0',                         # before the start: -1, position kept
        'close',
        'open disc0:/TAIL.BIN', 'read', 'seek 2 -64', 'read', 'close',
        'umd %#x 0' % (past4_at // SECTOR),
        'umd %#x %#x' % (big_end // SECTOR, big_end % SECTOR),
        'umd %#x 0' % (tail_at // SECTOR),
        'lbn %#x 0x10000 0xffc0' % (tail_at // SECTOR),
        'lbn %#x 0x800 %#x' % (big_end // SECTOR, big_end % SECTOR),
        'lastlba',
    ]


def hex64(v):
    return '0x%016x' % (v & 0xFFFFFFFFFFFFFFFF)


def int32(v):
    return ((v + (1 << 31)) % (1 << 32)) - (1 << 31)


def show(data):
    return ' [%s]' % ''.join(chr(b) if 32 <= b < 127 else '?' for b in data[:64])


def expected(iso, info, commands):
    """What the guest program should print, from the image file (never from pspc)."""
    files = info['files']
    image_bytes = info['sectors'] * SECTOR
    out = []
    cur = None  # [first byte in the image, size, position]
    with open(iso, 'rb') as img:
        def at(offset, n):
            img.seek(offset)
            return img.read(max(0, n))

        def seek(whence, off):
            base = (0, cur[2], cur[1])[whence]
            if base + off < 0:
                return -1
            cur[2] = base + off
            return cur[2]

        for line in commands:
            p = line.split()
            op = p[0]
            if op == 'open':
                lba, size = files[p[1][len('disc0:'):]]
                cur = [lba * SECTOR, size, 0]
                res = ' ok'
            elif op == 'seek':
                res = ' ' + hex64(seek(int(p[1], 0), int(p[2], 0)))
            elif op == 'seek32':
                # An int offset in, the position back as an int.
                res = ' %d' % int32(seek(int(p[1], 0), int32(int(p[2], 0))))
            elif op == 'read':
                n = max(0, min(64, cur[1] - cur[2]))
                data = at(cur[0] + cur[2], n)
                cur[2] += n
                res = ' %d' % n + show(data)
            elif op == 'close':
                res = ' 0'
                cur = None
            elif op == 'stat':
                res = ' 0 ' + hex64(files[p[1][len('disc0:'):]][1])
            elif op == 'umd':
                sector, byte = int(p[1], 0), int(p[2], 0)
                res = ' %s 1' % hex64(sector) + show(at(sector * SECTOR + (byte & 2047), 64))
            elif op == 'lbn':
                start, size, byte = int(p[1], 0), int(p[2], 0), int(p[3], 0)
                size = min(size, image_bytes - start * SECTOR)
                n = max(0, min(64, size - byte))
                res = ' %d' % n + show(at(start * SECTOR + byte, n))
            elif op == 'lastlba':
                res = ' 0 0x%x' % (info['sectors'] - 1)
            else:
                raise ValueError(line)
            out.append(line + ' =>' + res)
    out.append('done')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / ('pspc.exe' if sys.platform == 'win32' else 'pspc')))
    ap.add_argument('--aot', action='store_true',
                    help='native code, built fresh for the run, instead of the interpreter')
    ap.add_argument('--keep', action='store_true', help='keep the image (build/large-image/large.iso)')
    ap.add_argument('--timeout', type=int, default=60)
    args = ap.parse_args()
    if not PRX.exists():
        print('SKIP: %s is not built (tools/build_guest_tests.sh, under Linux or WSL with pspdev)' % PRX)
        return 0
    work = ROOT / 'build' / 'large-image'
    work.mkdir(parents=True, exist_ok=True)
    iso = work / 'large.iso'
    commands = probes()
    info = make_big_iso.build(str(iso), eboot=PRX.read_bytes(), probes=('\n'.join(commands) + '\n').encode())
    want = expected(iso, info, commands)
    ms = pathlib.Path(tempfile.mkdtemp(prefix='pspc_large_ms_'))
    cmd = [os.path.abspath(args.pspc), '--config', 'none', '--no-plugins', '--headless', '--fast-time',
           '--timeout', str(args.timeout), '--log', 'warn', '--memstick', str(ms)]
    aot = None
    if args.aot:
        # pspc only runs native code that exists: build the program's (EBOOT.BIN's) first.
        aot = pathlib.Path(tempfile.mkdtemp(prefix='pspc_large_aot_'))
        err = guest_tests.build_native(PRX, aot, pathlib.Path(os.path.abspath(args.pspc)).parent.parent)
        if err:
            print('large image: ' + err)
            shutil.rmtree(aot, ignore_errors=True)
            iso.unlink(missing_ok=True)
            return 1
        cmd += ['--aot-dir', str(aot)]
    else:
        cmd.append('--interp')
    cmd.append(str(iso))
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout + 30)
        got = proc.stdout.decode('utf-8', 'replace').replace('\r\n', '\n').rstrip('\n').split('\n')
        err = proc.stderr.decode('utf-8', 'replace').strip().splitlines()
    except subprocess.TimeoutExpired:
        got, err = [], ['host timeout']
    finally:
        shutil.rmtree(ms, ignore_errors=True)
        if aot:
            shutil.rmtree(aot, ignore_errors=True)
        if not args.keep:
            iso.unlink(missing_ok=True)
    bad = [(i, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
    if len(want) != len(got):
        bad.append((min(len(want), len(got)), '%d lines' % len(want), '%d lines' % len(got)))
    print('large image (%.2f GB, %s): %d of %d lines match' % (info['sectors'] * SECTOR / make_big_iso.GB,
          'AOT' if args.aot else 'interpreter', len(want) - len(bad), len(want)))
    for i, w, g in bad[:10]:
        print('  line %d\n    expected: %s\n    got:      %s' % (i + 1, w, g))
    if bad and err:
        print('  stderr: ' + '\n          '.join(err[-8:]))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
