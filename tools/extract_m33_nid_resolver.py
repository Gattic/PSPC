#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 PSPC contributors
"""Extract M33 systemctrl's kernel NID resolver tables as facts.

systemctrl renames the old (1.50-era) kernel-library NIDs that modules import
to the NIDs the 4.01 Sony kernel actually exports.  It does this at module
load time by walking each import stub and translating its NID through a
per-library table of {old_nid, new_nid} pairs.  Those tables are static data
in systemctrl's .data, reached through a 26-entry directory of
{library_name, pairs_pointer, count}.

This tool locates that directory generically: it scans .data for a run of
such triples whose first pointer resolves to a library-name string in
.rodata and whose pairs pointer resolves to a table of 8-byte NID pairs, then
writes every pair to a CSV of facts (NID numbers only, no code).

Usage:
    python -I tools/extract_m33_nid_resolver.py private/fw/401/m33/systemctrl.prx
        [-o data/m33_nid_resolver.csv]

Output columns: library,old_nid,new_nid (both NIDs as 0x%08X).
"""

import argparse
import csv
import os
import re
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("prx", help="path to systemctrl.prx (plain, unpacked)")
    ap.add_argument("-o", "--out", default=os.path.join("data", "m33_nid_resolver.csv"))
    args = ap.parse_args()

    d = open(args.prx, "rb").read()
    if d[:4] != b"\x7fELF":
        sys.exit("%s: not an ELF/PRX" % args.prx)

    # Single PT_LOAD at file offset 0x60, virtual address 0: file = vaddr + off.
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    nph = struct.unpack_from("<H", d, 0x2C)[0]
    loads = []
    for i in range(nph):
        t, off, va, pa, fsz = struct.unpack_from("<5I", d, phoff + i * 32)
        if t == 1:
            loads.append((va, off, fsz))

    def f(va):
        for base, off, fsz in loads:
            if base <= va < base + fsz:
                return off + (va - base)
        return None

    def u32(va):
        o = f(va)
        return struct.unpack_from("<I", d, o)[0] if o is not None and o + 4 <= len(d) else None

    def cstr(va):
        o = f(va)
        if o is None:
            return None
        e = d.find(b"\0", o, o + 64)
        s = d[o:e if e >= 0 else o].decode("latin-1")
        return s if re.fullmatch(r"[A-Za-z0-9_]+", s or "") else None

    def is_pairs(va, cnt):
        if not (0 < cnt < 4096):
            return False
        o = f(va)
        if o is None or o + cnt * 8 > len(d):
            return False
        # Each old NID should be non-zero and distinct.
        olds = set()
        for k in range(cnt):
            old, new = struct.unpack_from("<2I", d, o + k * 8)
            if old == 0 or new == 0:
                return False
            olds.add(old)
        return len(olds) == cnt

    # Scan every word-aligned .data position for the directory: a run of >= 8
    # consecutive {name_ptr, pairs_ptr, count} triples that all validate.
    best = None
    data_segs = [(va, off, fsz) for va, off, fsz in loads]
    for base, off, fsz in data_segs:
        for pos in range(off, off + fsz - 12, 4):
            va = base + (pos - off)
            run = []
            p = va
            while True:
                name_ptr = u32(p)
                pairs_ptr = u32(p + 4)
                cnt = u32(p + 8)
                if name_ptr is None or cnt is None:
                    break
                lib = cstr(name_ptr)
                if lib is None or not is_pairs(pairs_ptr, cnt):
                    break
                run.append((lib, pairs_ptr, cnt))
                p += 12
            if len(run) >= 8 and (best is None or len(run) > len(best[1])):
                best = (va, run)
    if best is None:
        sys.exit("could not locate the NID resolver directory")

    dir_va, run = best
    rows = []
    for lib, pairs_ptr, cnt in run:
        o = f(pairs_ptr)
        for k in range(cnt):
            old, new = struct.unpack_from("<2I", d, o + k * 8)
            rows.append((lib, old, new))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["library", "old_nid", "new_nid"])
        for lib, old, new in rows:
            w.writerow([lib, "0x%08X" % old, "0x%08X" % new])
    libs = sorted({r[0] for r in rows})
    print("directory at vaddr 0x%08X: %d libraries, %d NID pairs -> %s"
          % (dir_va, len(libs), len(rows), args.out))
    changed = sum(1 for _, o, n in rows if o != n)
    print("  %d pairs rename the NID, %d are identity" % (changed, len(rows) - changed))
    for lib in libs:
        print("  %-28s %d" % (lib, sum(1 for r in rows if r[0] == lib)))


if __name__ == "__main__":
    main()
