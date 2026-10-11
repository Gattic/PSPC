#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compress a PSP disc image into CSO v1, CSO v2, ZSO or DAX (the formats pspc_fs reads).

usage: mkcso.py [-f cso|cso2|zso|dax] [-b BLOCK] [-a ALIGN] [-l LEVEL] [--mix] in.iso out

CSO v1: raw deflate frames, index high bit = stored. ZSO: same layout with LZ4 blocks.
CSO v2: a frame stored at >= the frame size is raw, otherwise the index high bit picks LZ4
over deflate (--mix alternates the two, otherwise the smaller wins). DAX: 8 KiB zlib frames,
incompressible frames stored raw in NC areas.
The index shift (-a) is a minimum: an index entry holds 31 bits of file position >> shift, so
an output past 2 GB needs a shift of 1 or more, and the tool raises it as far as the image needs.
DAX can't hold an image of 4 GB or more (its header has a 32-bit size).
The output appears under its name only once it is complete.
LZ4 uses the 'lz4' module when installed, otherwise a slow pure-Python encoder.
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
import zlib

MAX_SHIFT = 20          # the most pspc_fs (and maxcso) accept
DAX_FRAME = 0x2000
DAX_LIMIT = 1 << 32     # DAX's header stores the image size in 32 bits

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


def ciso_worst_size(total, block, shift):
    """The largest a CSO/ZSO of `total` bytes can get: header, index, every frame stored raw (no
    frame is stored bigger, bar a last partial frame's few bytes of overhead) and padded to the
    alignment."""
    frames = (total + block - 1) // block
    return 24 + 4 * (frames + 1) + total + (frames + 1) * ((1 << shift) - 1) + block


def needed_shift(total, block):
    """The smallest index shift whose 31-bit entries reach the end of the worst-case output, or
    None if even MAX_SHIFT doesn't."""
    for shift in range(MAX_SHIFT + 1):
        if ciso_worst_size(total, block, shift) >> shift <= 0x7FFFFFFF:
            return shift
    return None


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
        if pos >> align > 0x7FFFFFFF:  # needed_shift rules this out; never write a wrong index
            raise RuntimeError('index shift %d too small at frame %d' % (align, f))
        index.append((pos >> align) | flag)
        dst.write(data)
        pos += len(data)
        pad()
    if pos >> align > 0x7FFFFFFF:
        raise RuntimeError('index shift %d too small for the end of the data' % align)
    index.append(pos >> align)
    dst.seek(24)
    dst.write(struct.pack('<%dI' % len(index), *index))


def write_dax(src, dst, level):
    fs = DAX_FRAME
    src.seek(0, 2)
    total = src.tell()
    src.seek(0)
    if total >= DAX_LIMIT:
        raise ValueError('DAX holds images under 4 GB (its header has a 32-bit size); use CSO or ZSO')
    frames = (total + fs - 1) // fs
    # The frames go to a scratch file first: the header in front of them depends on which frames
    # turn out incompressible.
    sizes, raw = [], []
    with tempfile.TemporaryFile() as spool:
        for f in range(frames):
            chunk = src.read(fs)
            z = zlib.compress(chunk, level)
            if len(z) >= len(chunk):
                z = chunk
                raw.append(f)
            spool.write(z)
            sizes.append(len(z))
        areas = []
        for f in raw:
            if areas and areas[-1][0] + areas[-1][1] == f:
                areas[-1][1] += 1
            else:
                areas.append([f, 1])
        pos = 32 + frames * 6 + len(areas) * 8
        offsets = []
        for n in sizes:
            offsets.append(pos)
            pos += n
        if offsets and offsets[-1] > 0xFFFFFFFF:
            raise ValueError('compressed DAX frames would start past 4 GB; use CSO or ZSO')
        dst.write(struct.pack('<4sIII16x', b'DAX\0', total, 1, len(areas)))
        dst.write(struct.pack('<%dI' % frames, *offsets))
        dst.write(struct.pack('<%dH' % frames, *sizes))
        for a in areas:
            dst.write(struct.pack('<II', a[0], a[1]))
        spool.seek(0)
        shutil.copyfileobj(spool, dst, 1 << 20)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('-f', '--format', choices=['cso', 'cso2', 'zso', 'dax'], default='cso')
    ap.add_argument('-b', '--block', type=int, default=2048, help='frame size (power of two >= 2048; DAX is always 8192)')
    ap.add_argument('-a', '--align', type=int, default=0,
                    help='minimum index shift (raised as far as the image needs)')
    ap.add_argument('-l', '--level', type=int, default=9, help='zlib level')
    ap.add_argument('--mix', action='store_true', help='CSO v2: alternate deflate and LZ4 frames')
    ap.add_argument('input')
    ap.add_argument('output')
    args = ap.parse_args()
    if args.block < 2048 or args.block & (args.block - 1):
        sys.exit('block size must be a power of two >= 2048')
    if not 0 <= args.align <= MAX_SHIFT:
        sys.exit('index shift must be 0 to %d' % MAX_SHIFT)
    total = os.path.getsize(args.input)
    # Everything that can be known before compressing is checked first.
    if args.format == 'dax':
        if total >= DAX_LIMIT:
            sys.exit('%s: DAX holds images under 4 GB (its header has a 32-bit size); use CSO or ZSO'
                     % args.input)
    else:
        if total // 2048 >= 0xFFFFFFFF:
            sys.exit('%s: too big for a CSO (8 TB or more)' % args.input)
        shift = needed_shift(total, args.block)
        if shift is None:
            sys.exit('%s: too big for a CSO index even with shift %d; use a larger -b' % (args.input, MAX_SHIFT))
        if args.align < shift:
            if args.align:
                print('index shift %d is too small for this image; using %d' % (args.align, shift), file=sys.stderr)
            args.align = shift
    # Written under a temporary name, renamed when complete.
    out_dir = os.path.dirname(os.path.abspath(args.output))
    fd, tmp = tempfile.mkstemp(prefix=os.path.basename(args.output) + '.', suffix='.part', dir=out_dir)
    try:
        with open(args.input, 'rb') as src, os.fdopen(fd, 'wb') as dst:
            if args.format == 'dax':
                write_dax(src, dst, args.level)
            else:
                write_ciso(src, dst, args.format, args.block, args.align, args.level, args.mix)
        os.replace(tmp, args.output)
    except BaseException as e:
        try:
            os.remove(tmp)
        except OSError:
            pass
        if isinstance(e, (ValueError, RuntimeError)):
            sys.exit('%s: %s' % (args.input, e))
        raise


if __name__ == '__main__':
    main()
