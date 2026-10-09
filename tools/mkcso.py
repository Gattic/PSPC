#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compress a PSP disc image into CSO v1, CSO v2, ZSO or DAX (the formats pspc_fs reads).

usage: mkcso.py [-f cso|cso2|zso|dax] [-b BLOCK] [-a ALIGN] [-l LEVEL] [--mix] in.iso out

CSO v1: raw deflate frames, index high bit = stored. ZSO: same layout with LZ4 blocks.
CSO v2: a frame stored at >= the frame size is raw, otherwise the index high bit picks LZ4
over deflate (--mix alternates the two, otherwise the smaller wins). DAX: 8 KiB zlib frames,
incompressible frames stored raw in NC areas.
LZ4 uses the 'lz4' module when installed, otherwise a slow pure-Python encoder.
"""
import argparse
import struct
import sys
import zlib

try:
    import lz4.block as _lz4
except ImportError:
    _lz4 = None


def lz4_block(data):
    if _lz4:
        return _lz4.compress(data, store_size=False)
    n = len(data)
    out = bytearray()
    table = {}
    anchor = i = 0
    limit = n - 12

    def put_len(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    while i < limit:
        key = data[i:i + 4]
        cand = table.get(key, -1)
        table[key] = i
        if cand >= 0 and i - cand <= 65535:
            m = 4
            while i + m < n - 5 and data[cand + m] == data[i + m]:
                m += 1
            lit = i - anchor
            out.append((min(lit, 15) << 4) | min(m - 4, 15))
            if lit >= 15:
                put_len(lit - 15)
            out += data[anchor:i]
            out += struct.pack('<H', i - cand)
            if m - 4 >= 15:
                put_len(m - 4 - 15)
            i += m
            anchor = i
        else:
            i += 1
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        put_len(lit - 15)
    out += data[anchor:]
    return bytes(out)


def deflate_raw(data, level):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def write_ciso(src, dst, fmt, block, align, level, mix):
    src.seek(0, 2)
    total = src.tell()
    src.seek(0)
    frames = (total + block - 1) // block
    magic = b'ZISO' if fmt == 'zso' else b'CISO'
    ver = 2 if fmt == 'cso2' else 1
    dst.write(struct.pack('<4sIQIBB2x', magic, 0x18, total, block, ver, align))
    dst.write(b'\0' * 4 * (frames + 1))
    unit = 1 << align
    pos = dst.tell()

    def pad():
        nonlocal pos
        extra = (-pos) % unit
        dst.write(b'\0' * extra)
        pos += extra

    pad()
    index = []
    for f in range(frames):
        chunk = src.read(block)
        flag = 0
        if fmt == 'cso':
            data = deflate_raw(chunk, level)
            if len(data) >= len(chunk):
                data, flag = chunk, 0x80000000
        elif fmt == 'zso':
            data = lz4_block(chunk)
            if len(data) >= len(chunk):
                data, flag = chunk, 0x80000000
        else:
            d = deflate_raw(chunk, level)
            z = lz4_block(chunk)
            use_lz4 = (f % 2 == 1) if mix else len(z) < len(d)
            data, flag = (z, 0x80000000) if use_lz4 else (d, 0)
            stored = len(data) + (-len(data)) % unit
            if stored >= block and len(chunk) == block:
                data, flag = chunk, 0
        index.append((pos >> align) | flag)
        dst.write(data)
        pos += len(data)
        pad()
    index.append(pos >> align)
    dst.seek(24)
    dst.write(struct.pack('<%dI' % len(index), *index))


def write_dax(src, dst, level):
    fs = 0x2000
    src.seek(0, 2)
    total = src.tell()
    src.seek(0)
    frames = (total + fs - 1) // fs
    blobs, raw = [], []
    for f in range(frames):
        chunk = src.read(fs)
        z = zlib.compress(chunk, level)
        if len(z) >= len(chunk):
            blobs.append(chunk)
            raw.append(f)
        else:
            blobs.append(z)
    areas = []
    for f in raw:
        if areas and areas[-1][0] + areas[-1][1] == f:
            areas[-1][1] += 1
        else:
            areas.append([f, 1])
    pos = 32 + frames * 6 + len(areas) * 8
    offsets = []
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    dst.write(struct.pack('<4sIII16x', b'DAX\0', total, 1, len(areas)))
    dst.write(struct.pack('<%dI' % frames, *offsets))
    dst.write(struct.pack('<%dH' % frames, *[len(b) for b in blobs]))
    for a in areas:
        dst.write(struct.pack('<II', a[0], a[1]))
    for b in blobs:
        dst.write(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('-f', '--format', choices=['cso', 'cso2', 'zso', 'dax'], default='cso')
    ap.add_argument('-b', '--block', type=int, default=2048, help='frame size (power of two >= 2048; DAX is always 8192)')
    ap.add_argument('-a', '--align', type=int, default=0, help='index shift')
    ap.add_argument('-l', '--level', type=int, default=9, help='zlib level')
    ap.add_argument('--mix', action='store_true', help='CSO v2: alternate deflate and LZ4 frames')
    ap.add_argument('input')
    ap.add_argument('output')
    args = ap.parse_args()
    if args.block < 2048 or args.block & (args.block - 1):
        sys.exit('block size must be a power of two >= 2048')
    with open(args.input, 'rb') as src, open(args.output, 'wb') as dst:
        if args.format == 'dax':
            write_dax(src, dst, args.level)
        else:
            write_ciso(src, dst, args.format, args.block, args.align, args.level, args.mix)


if __name__ == '__main__':
    main()
