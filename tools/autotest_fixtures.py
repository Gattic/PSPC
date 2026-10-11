# SPDX-License-Identifier: GPL-2.0-or-later
"""Test data some pspautotests expect but git can't carry, made by run_autotests.py before a run
(docs/autotests-plan.md):

- io/file/file stats its own file.c and expects the date it had on the author's disk
  (2013-01-21 00:11:54); git doesn't keep file dates. set_local_time() sets it as pspc reads it
  back (local time, converted with TzSpecificLocalTimeToSystemTime), so the result doesn't depend
  on the time zone.
- umd/io/umd_io lists disc0:/PSP_GAME and umd/raw_access reads its PARAM.SFO (the 472-byte one its
  source embeds), on a disc the suite doesn't ship (raw_access.iso is an older one).
  build_umd_test_disc() writes a minimal ISO9660 image laid out like a mastered UMD: the volume
  descriptor at 16, path tables at 18-21, one sector per directory from 22 (breadth first), then
  the files, PARAM.SFO first (LBN 26).
"""

import ctypes
import re
import struct
import zlib
from ctypes import wintypes

SECTOR = 2048


# ---- file dates

class _SYSTEMTIME(ctypes.Structure):
    _fields_ = [(n, wintypes.WORD) for n in ('wYear', 'wMonth', 'wDayOfWeek', 'wDay', 'wHour', 'wMinute', 'wSecond',
                                             'wMilliseconds')]


def set_local_time(path, year, month, day, hour, minute, second):
    """Sets a file's creation and last-write time to a local wall-clock time."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    local = _SYSTEMTIME(year, month, 0, day, hour, minute, second, 0)
    utc = _SYSTEMTIME()
    ft = wintypes.FILETIME()
    if not k32.TzSpecificLocalTimeToSystemTime(None, ctypes.byref(local), ctypes.byref(utc)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not k32.SystemTimeToFileTime(ctypes.byref(utc), ctypes.byref(ft)):
        raise ctypes.WinError(ctypes.get_last_error())
    # FILE_WRITE_ATTRIBUTES, share all, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS
    h = k32.CreateFileW(str(path), 0x100, 0x7, None, 3, 0x02000000, None)
    if h == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not k32.SetFileTime(wintypes.HANDLE(h), ctypes.byref(ft), None, ctypes.byref(ft)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k32.CloseHandle(wintypes.HANDLE(h))


# ---- the UMD test disc

def _both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def _both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


_DATE = bytes([113, 1, 21, 0, 11, 54, 0])  # 2013-01-21 00:11:54, GMT


def _dirrec(name, lba, size, isdir):
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


def _png1x1():
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\x00\x00\x00\x00')) + chunk(b'IEND', b''))


def raw_access_sfo(source_path):
    """The PARAM.SFO umd/raw_access compares against (dataRef[472] in its source)."""
    src = open(source_path, encoding='latin-1').read()
    body = src[src.index('dataRef[472]'):]
    body = body[body.index('{') + 1:body.index('};')]
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body))
    if len(data) != 472:
        raise ValueError('dataRef has %d bytes' % len(data))
    return data


def build_umd_test_disc(out_path, sfo):
    tree = {'PSP_GAME': {'ICON0.PNG': _png1x1(), 'PARAM.SFO': sfo, 'PIC1.PNG': _png1x1(), 'SYSDIR': {}, 'USRDIR': {}}}
    # Directories breadth first, in ISO9660 (sorted) order.
    dirs = [('', tree, None)]
    i = 0
    while i < len(dirs):
        _, node, _ = dirs[i]
        for k in sorted(node):
            if isinstance(node[k], dict):
                dirs.append((k, node[k], i))
        i += 1
    dir_lba = {i: 22 + i for i in range(len(dirs))}
    next_lba = 22 + len(dirs)
    files = []
    for di, (_, node, _) in enumerate(dirs):
        for k in sorted(node, key=lambda s: (s != 'PARAM.SFO', s)):
            if not isinstance(node[k], dict):
                files.append((di, k, node[k]))
    file_lba = {}
    for di, k, data in files:
        file_lba[(di, k)] = next_lba
        next_lba += max(1, (len(data) + SECTOR - 1) // SECTOR)
    total = next_lba
    img = bytearray(total * SECTOR)
    for di, k, data in files:
        img[file_lba[(di, k)] * SECTOR:file_lba[(di, k)] * SECTOR + len(data)] = data
    for di, (_, node, parent) in enumerate(dirs):
        recs = _dirrec(b'\x00', dir_lba[di], SECTOR, True)
        recs += _dirrec(b'\x01', dir_lba[parent if parent is not None else 0], SECTOR, True)
        for k in sorted(node):
            if isinstance(node[k], dict):
                ci = next(j for j, d in enumerate(dirs) if d[0] == k and d[2] == di)
                recs += _dirrec(k.encode(), dir_lba[ci], SECTOR, True)
            else:
                recs += _dirrec((k + ';1').encode(), file_lba[(di, k)], len(node[k]), False)
        img[dir_lba[di] * SECTOR:dir_lba[di] * SECTOR + len(recs)] = recs
    lt = bytearray()
    mt = bytearray()
    for di, (name, _, parent) in enumerate(dirs):
        nm = name.encode() if di else b'\x00'
        pn = (parent if parent is not None else 0) + 1
        pad = b'\x00' if len(nm) & 1 else b''
        lt += bytes([len(nm), 0]) + struct.pack('<IH', dir_lba[di], pn) + nm + pad
        mt += bytes([len(nm), 0]) + struct.pack('>IH', dir_lba[di], pn) + nm + pad
    for lba, t in ((18, lt), (19, lt), (20, mt), (21, mt)):
        img[lba * SECTOR:lba * SECTOR + len(t)] = t
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'PSP GAME'.ljust(32)
    pvd[40:72] = b'UMDTEST'.ljust(32)
    pvd[80:88] = _both32(total)
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[132:140] = _both32(len(lt))
    pvd[140:144] = struct.pack('<I', 18)
    pvd[144:148] = struct.pack('<I', 19)
    pvd[148:152] = struct.pack('>I', 20)
    pvd[152:156] = struct.pack('>I', 21)
    pvd[156:190] = _dirrec(b'\x00', 22, SECTOR, True)
    pvd[881] = 1
    img[16 * SECTOR:17 * SECTOR] = pvd
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    img[17 * SECTOR:18 * SECTOR] = term
    with open(out_path, 'wb') as f:
        f.write(img)
