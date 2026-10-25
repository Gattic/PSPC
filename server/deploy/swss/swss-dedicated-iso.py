#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Makes the SWSS disc image a dedicated server runs from a copy of the playable one.

    python3 swss-dedicated-iso.py Games/SWSS.iso Games/SWSS-dedicated.iso EBOOT.BIN
        [--expect-old SHA256] [--expect-new SHA256]

The dedicated disc is the playable disc with another executable: a host that has no local player of its own, so the
game lists it with 0 players instead of 1. Only that file differs, and when it has the same size as the original the
image does not have to be rebuilt: the copy's sectors under PSP_GAME/SYSDIR/EBOOT.BIN are overwritten, which is all this
does (the ISO 9660 directories and every other file are not touched). The executable itself is not in this
repository; it comes from the machine that has the SWSS build.

The new image is written beside its destination and renamed into place once it has been read back, so a disc a running
game has open is never changed, and a failed run leaves nothing behind but the temporary file it removes. The sha256 of
the old and new executables are printed; --expect-old / --expect-new refuse a disc or an executable that is not the
one you meant."""

import argparse
import hashlib
import os
import shutil
import struct
import sys

SECTOR = 2048
EBOOT_PATH = 'PSP_GAME/SYSDIR/EBOOT.BIN'
CHUNK = 1 << 20


def entries(f, lba, size):
    """(name, extent, size, is_directory) of a directory's records, without '.' and '..' or file versions."""
    f.seek(lba * SECTOR)
    data = f.read(size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:                     # records do not cross sectors: the rest of this one is padding
            i = (i // SECTOR + 1) * SECTOR
            continue
        rec = data[i:i + n]
        i += n
        name = rec[33:33 + rec[32]]
        if name in (b'\x00', b'\x01'):
            continue
        extent, length = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        yield name.decode('latin-1').split(';')[0], extent, length, bool(rec[25] & 2)


def find(f, path):
    """(extent, size) of a file of the image, by its path (case does not matter)."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b'CD001':
        raise ValueError('not an ISO 9660 image')
    extent, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    parts = path.split('/')
    for i, part in enumerate(parts):
        for name, e, s, is_dir in entries(f, extent, size):
            if name.lower() == part.lower() and is_dir == (i < len(parts) - 1):
                extent, size = e, s
                break
        else:
            raise ValueError('%s is not on the image' % path)
    return extent, size


def read_file(f, extent, size):
    f.seek(extent * SECTOR)
    data = f.read(size)
    if len(data) != size:
        raise ValueError('the image ends inside %s' % EBOOT_PATH)
    return data


def make(src, dst, eboot_file, expect_old=None, expect_new=None):
    """Copies src to dst with the executable replaced. Returns (old sha256, new sha256, bytes that differ)."""
    if os.path.abspath(src) == os.path.abspath(dst):
        raise ValueError('the new image has to be another file than the playable one')
    with open(eboot_file, 'rb') as f:
        new = f.read()
    with open(src, 'rb') as f:
        extent, size = find(f, EBOOT_PATH)
        old = read_file(f, extent, size)
    old_sha, new_sha = hashlib.sha256(old).hexdigest(), hashlib.sha256(new).hexdigest()
    if expect_old and old_sha != expect_old.lower():
        raise ValueError('the disc\'s executable is %s, not the %s expected: this is not the playable disc' % (old_sha, expect_old))
    if expect_new and new_sha != expect_new.lower():
        raise ValueError('%s is %s, not the %s expected' % (eboot_file, new_sha, expect_new))
    if len(new) != size:
        raise ValueError('%s has %d bytes and the disc\'s executable %d: an image cannot be patched in place with another '
                         'size, use the whole dedicated image' % (eboot_file, len(new), size))
    tmp = dst + '.tmp'
    try:
        shutil.copyfile(src, tmp)
        with open(tmp, 'r+b') as f:
            f.seek(extent * SECTOR)
            f.write(new)
            f.flush()
            os.fsync(f.fileno())
        with open(tmp, 'rb') as f:
            e2, s2 = find(f, EBOOT_PATH)
            if (e2, s2) != (extent, size) or read_file(f, e2, s2) != new:
                raise ValueError('the new image does not read back as the executable that was written')
        os.replace(tmp, dst)
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise
    return old_sha, new_sha, sum(a != b for a, b in zip(old, new))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('playable', help='the playable SWSS image (not changed)')
    ap.add_argument('dedicated', help='the image to make')
    ap.add_argument('eboot', help='the dedicated executable, as many bytes as the playable disc\'s PSP_GAME/SYSDIR/EBOOT.BIN')
    ap.add_argument('--expect-old', metavar='SHA256', help='refuse unless the playable disc\'s executable has this sha256')
    ap.add_argument('--expect-new', metavar='SHA256', help='refuse unless the dedicated executable has this sha256')
    a = ap.parse_args()
    try:
        old, new, diff = make(a.playable, a.dedicated, a.eboot, a.expect_old, a.expect_new)
    except (OSError, ValueError) as e:
        print('swss-dedicated-iso: %s' % e, file=sys.stderr)
        return 1
    print('%s: %s\n  executable was %s\n  executable now %s (%d bytes differ)' % (a.dedicated, EBOOT_PATH, old, new, diff))
    return 0


if __name__ == '__main__':
    sys.exit(main())
