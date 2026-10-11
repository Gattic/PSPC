#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a sparse PSP disc image of several GB for testing large-image support.

usage: make_big_iso.py [--eboot FILE] [--probes FILE] out.iso

The image is an ISO 9660 volume with a PSP_GAME folder (PARAM.SFO, and SYSDIR/EBOOT.BIN when
--eboot is given, so it lists in the launcher and boots) and data files placed past 2, 4 and
8 GB (LAYOUT). Each data file holds markers, "NAME@0xOFFSET" padded with '.' to 64 bytes, at
known offsets; everything else reads as zeros. The file is sparse (FSCTL_SET_SPARSE on NTFS, a
hole past the end elsewhere), so an 8.8 GB image takes well under 1 MB of disk; it is removed
again if it didn't stay sparse.
tools/large_image_test.py builds one, boots it in pspc and checks what a guest program reads.
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
GB = 1 << 30

# name, byte offset in the image, size, offsets in the file that hold markers
LAYOUT = [
    ('SMALL.BIN', None, 4096, [0, 4096 - 64]),
    ('PAST2G.BIN', 2 * GB + GB // 4, 65536, [0, 65536 - 64]),
    ('PAST4G.BIN', 4 * GB + GB // 4, 65536, [0, 65536 - 64]),
    # 4 GB - 1 bytes from 4.5 GB: markers across the file's own 2 GB, past it, across the image's
    # 8 GB (3.5 GB into the file) and at the end.
    ('BIG.BIN', 4 * GB + GB // 2, 0xFFFFFFFF, [0, 0x7FFFFFE0, 0x80100000, 0xDFFFFFE0, 0xFFFFFFFF - 64]),
    ('TAIL.BIN', 8 * GB + 3 * GB // 4, 65536, [0, 65536 - 64]),
]


def marker(name, off):
    return ('%s@0x%x' % (name, off)).encode().ljust(64, b'.')


def make_sparse(path, size):
    """Creates `path` as an empty sparse file of `size` bytes. Refuses rather than allocate a
    dense multi-GB file where sparse files aren't available."""
    open(path, 'wb').close()
    if os.name == 'nt':
        import ctypes
        import ctypes.wintypes as wt
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.CreateFileW.restype = wt.HANDLE
        k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
        h = k32.CreateFileW(os.path.abspath(path), 0xC0000000, 0, None, 3, 0x80, None)
        if h in (None, wt.HANDLE(-1).value):
            raise OSError('cannot open %s' % path)
        ret = wt.DWORD()
        ok = k32.DeviceIoControl(wt.HANDLE(h), 0x900C4, None, 0, None, 0, ctypes.byref(ret), None)  # FSCTL_SET_SPARSE
        k32.CloseHandle(wt.HANDLE(h))
        if not ok:
            os.remove(path)
            raise OSError('%s: the file system has no sparse files' % path)
    # One byte written at the end: the gap stays a hole. (truncate() would fill it: the Windows
    # CRT extends a file by writing zeros.)
    with open(path, 'r+b') as f:
        f.seek(size - 1)
        f.write(b'\0')


def disk_usage(path):
    """Bytes `path` really occupies on disk (allocated, not its length)."""
    if os.name == 'nt':
        import ctypes
        import ctypes.wintypes as wt
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.GetCompressedFileSizeW.restype = wt.DWORD
        k32.GetCompressedFileSizeW.argtypes = [wt.LPCWSTR, ctypes.POINTER(wt.DWORD)]
        hi = wt.DWORD()
        lo = k32.GetCompressedFileSizeW(os.path.abspath(path), ctypes.byref(hi))
        return (hi.value << 32) | lo
    return os.stat(path).st_blocks * 512


def _both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def _both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


_DATE = bytes([126, 10, 11, 12, 0, 0, 0])  # 2026-10-11 12:00:00


def _record(name, lba, size, isdir):
    n = len(name)
    rec = bytearray(33 + n + ((n + 1) & 1))
    rec[0] = len(rec)
    rec[2:10] = _both32(lba)
    rec[10:18] = _both32(size)
    rec[18:25] = _DATE
    rec[25] = 2 if isdir else 0
    rec[28:32] = _both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def param_sfo(entries):
    """A PARAM.SFO of string (str) and int entries."""
    keys = sorted(entries)
    key_table = b''
    data_table = b''
    index = b''
    for k in keys:
        v = entries[k]
        if isinstance(v, int):
            fmt, raw, size = 0x0404, struct.pack('<I', v), 4
        else:
            b = v.encode() + b'\0'
            fmt, size = 0x0204, len(b)
            raw = b.ljust((len(b) + 3) & ~3, b'\0')
        index += struct.pack('<HHIII', len(key_table), fmt, size, len(raw), len(data_table))
        key_table += k.encode() + b'\0'
        data_table += raw
    key_table = key_table.ljust((len(key_table) + 3) & ~3, b'\0')
    kt = 20 + len(index)
    return struct.pack('<4sIIII', b'\0PSF', 0x101, kt, kt + len(key_table), len(keys)) + index + key_table + data_table


def build(out_path, eboot=None, probes=None, layout=LAYOUT, title='PSPC large image test', disc_id='PSPC00001'):
    """Writes the image. Returns {'sectors': total, 'files': {'/NAME': (lba, size)}} for every
    file (data files, /PROBES.TXT, /PSP_GAME/...)."""
    small = [('PSP_GAME/PARAM.SFO', param_sfo({
        'BOOTABLE': 1, 'CATEGORY': 'UG', 'DISC_ID': disc_id, 'DISC_NUMBER': 1, 'DISC_TOTAL': 1,
        'DISC_VERSION': '1.00', 'PARENTAL_LEVEL': 1, 'PSP_SYSTEM_VER': '1.00', 'REGION': 0x8000,
        'TITLE': title}))]
    if eboot is not None:
        small.append(('PSP_GAME/SYSDIR/EBOOT.BIN', eboot))
    if probes is not None:
        small.append(('PROBES.TXT', probes))
    # Directories (one sector each) from LBA 22, then the small files, then the data files.
    dirs = ['', 'PSP_GAME', 'PSP_GAME/SYSDIR']
    dir_lba = {d: 22 + i for i, d in enumerate(dirs)}
    files = {}
    nxt = 22 + len(dirs)
    for path, data in small:
        files[path] = (nxt, len(data), data)
        nxt += max(1, (len(data) + SECTOR - 1) // SECTOR)
    end = nxt * SECTOR
    for name, at, size, _ in layout:
        if at is None:
            at = end
        if at % SECTOR or at < end:
            raise ValueError('%s at %#x overlaps or is unaligned' % (name, at))
        files[name] = (at // SECTOR, size, None)
        end = at + size
    total = (end + SECTOR - 1) // SECTOR + 16  # a little slack after the last file
    make_sparse(out_path, total * SECTOR)
    with open(out_path, 'r+b') as f:
        def put(at, data):
            f.seek(at)
            f.write(data)
        for d in dirs:
            parent = d.rsplit('/', 1)[0] if '/' in d else ''
            recs = _record(b'\0', dir_lba[d], SECTOR, True) + _record(b'\1', dir_lba[parent], SECTOR, True)
            entries = []
            for sub in dirs:
                if sub and (sub.rsplit('/', 1)[0] if '/' in sub else '') == d:
                    entries.append((sub.rsplit('/', 1)[-1], _record(sub.rsplit('/', 1)[-1].encode(), dir_lba[sub], SECTOR, True)))
            for path, (lba, size, _) in files.items():
                folder, _, leaf = path.rpartition('/')
                if folder == d:
                    entries.append((leaf, _record((leaf + ';1').encode(), lba, size, False)))
            for _, rec in sorted(entries):
                recs += rec
            assert len(recs) <= SECTOR
            put(dir_lba[d] * SECTOR, recs)
        # Path tables (type L at 18 and 19, type M at 20 and 21).
        lt = mt = b''
        for i, d in enumerate(dirs):
            name = d.rsplit('/', 1)[-1].encode() if d else b'\0'
            parent = dirs.index(d.rsplit('/', 1)[0] if '/' in d else '') + 1 if d else 1
            pad = b'\0' if len(name) & 1 else b''
            lt += bytes([len(name), 0]) + struct.pack('<IH', dir_lba[d], parent) + name + pad
            mt += bytes([len(name), 0]) + struct.pack('>IH', dir_lba[d], parent) + name + pad
        for lba, t in ((18, lt), (19, lt), (20, mt), (21, mt)):
            put(lba * SECTOR, t)
        pvd = bytearray(SECTOR)
        pvd[0] = 1
        pvd[1:6] = b'CD001'
        pvd[6] = 1
        pvd[8:40] = b'PSP GAME'.ljust(32)
        pvd[40:72] = disc_id.encode().ljust(32)
        pvd[80:88] = _both32(total)
        pvd[120:124] = _both16(1)
        pvd[124:128] = _both16(1)
        pvd[128:132] = _both16(SECTOR)
        pvd[132:140] = _both32(len(lt))
        pvd[140:144] = struct.pack('<I', 18)
        pvd[144:148] = struct.pack('<I', 19)
        pvd[148:152] = struct.pack('>I', 20)
        pvd[152:156] = struct.pack('>I', 21)
        pvd[156:190] = _record(b'\0', dir_lba[''], SECTOR, True)
        pvd[881] = 1
        put(16 * SECTOR, bytes(pvd))
        put(17 * SECTOR, b'\xffCD001\x01'.ljust(SECTOR, b'\0'))
        for path, (lba, size, data) in files.items():
            if data is not None:
                put(lba * SECTOR, data)
        for name, _, size, marks in layout:
            lba = files[name][0]
            for m in marks:
                put(lba * SECTOR + m, marker(name, m))
    used = disk_usage(out_path)
    if used > (256 << 20):  # a few MB of metadata, markers and EBOOT; anything more isn't sparse
        os.remove(out_path)
        raise OSError('%s took %d MB of disk instead of staying sparse; removed' % (out_path, used >> 20))
    return {'sectors': total, 'files': {'/' + p: (lba, size) for p, (lba, size, _) in files.items()}}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--eboot', help='executable to put at PSP_GAME/SYSDIR/EBOOT.BIN')
    ap.add_argument('--probes', help='text file to put at /PROBES.TXT')
    ap.add_argument('output')
    args = ap.parse_args()
    eboot = open(args.eboot, 'rb').read() if args.eboot else None
    probes = open(args.probes, 'rb').read() if args.probes else None
    try:
        info = build(args.output, eboot, probes)
    except (OSError, ValueError) as e:
        sys.exit(str(e))
    print('%s: %d sectors (%.2f GB)' % (args.output, info['sectors'], info['sectors'] * SECTOR / GB))
    for path, (lba, size) in sorted(info['files'].items(), key=lambda kv: kv[1][0]):
        print('  %-28s LBA %#9x  %d bytes' % (path, lba, size))


if __name__ == '__main__':
    main()
